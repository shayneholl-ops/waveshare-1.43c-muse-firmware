/* Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * ElevenLabs speech, direct to api.elevenlabs.io. The API key lives
 * only in this board's NVS (entered by the user on the serial console
 * with ">el_key=<key>", see muse_tts_quota): it is never compiled into
 * firmware, never logged, and never printed back. Fetch model follows
 * wupsbr's ElevenLabs client: one fetch task posts the text and
 * streams MP3 into a buffer; the chat session task drains it.
 * Differences, all deliberate:
 * - the request goes straight to api.elevenlabs.io with an xi-api-key
 *   header; the old proxy-URL path is dormant and no longer consulted
 *   (no Cloudflare Worker is involved anywhere);
 * - a quota/429 answer marks the month exhausted (muse_tts_quota) so
 *   later messages go straight to Edge;
 * - the fetch task syncs the wall clock first (shared with Edge), so
 *   the monthly rollover keeps working even when Edge never runs.
 */

#include "muse_elevenlabs_tts.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "muse_edge_tts.h"
#include "muse_tts_quota.h"

static const char *TAG = "el_tts";

#define EL_RX_BYTES (64 * 1024)
#define EL_READ_BYTES 2048
#define EL_TEXT_MAX 1024
#define EL_KEY_MAX 128
#define EL_URL_MAX 256
#define EL_FETCH_DEADLINE_S 60
/* Default voice when CONFIG_MUSE_ELEVENLABS_VOICE_ID is empty: Rachel,
 * a stock ElevenLabs voice that speaks Chinese text under the
 * multilingual Flash model. Overridable at build time via Kconfig. */
#define EL_VOICE_DEFAULT "21m00Tcm4TlvDq8ikWAM"

struct el_job_t {
    uint32_t job;
    char text[EL_TEXT_MAX];
    char key[EL_KEY_MAX];
};

static QueueHandle_t s_jobs;
static StreamBufferHandle_t s_rx;
static uint32_t s_job;
static uint32_t s_done_job;
static bool s_done;
static bool s_ok;
static bool s_got_audio;
static bool s_pending;

bool muse_elevenlabs_tts_available(void)
{
    return muse_tts_el_key()[0] != '\0';
}

static bool send_all(uint32_t job, const uint8_t *data, size_t len)
{
    while (len) {
        if (job != s_job) {
            return false;
        }
        size_t n = xStreamBufferSend(s_rx, data, len, pdMS_TO_TICKS(100));
        data += n;
        len -= n;
    }
    return true;
}

static bool body_says_quota(const char *body)
{
    /* Small case-insensitive substring scan for quota language. */
    static const char *words[] = { "quota", "credit", "exceed" };
    for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
        const char *p = body;
        size_t wl = strlen(words[w]);
        while (*p) {
            size_t k;
            for (k = 0; k < wl; k++) {
                if (tolower((unsigned char)p[k]) != words[w][k]) {
                    break;
                }
            }
            if (k == wl) {
                return true;
            }
            p++;
        }
    }
    return false;
}

static bool el_fetch(const struct el_job_t *j)
{
    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return false;
    }
    cJSON_AddStringToObject(req, "text", j->text);
    if (CONFIG_MUSE_ELEVENLABS_MODEL[0] != '\0') {
        cJSON_AddStringToObject(req, "model_id", CONFIG_MUSE_ELEVENLABS_MODEL);
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return false;
    }
    /* Direct endpoint: the voice ID is part of the path, not the body. */
    const char *voice = CONFIG_MUSE_ELEVENLABS_VOICE_ID[0] != '\0'
                            ? CONFIG_MUSE_ELEVENLABS_VOICE_ID
                            : EL_VOICE_DEFAULT;
    char url[EL_URL_MAX];
    int url_len = snprintf(url, sizeof(url),
                           "https://api.elevenlabs.io/v1/text-to-speech/%s/"
                           "stream?output_format=mp3_44100_128",
                           voice);
    if (url_len < 0 || (size_t)url_len >= sizeof(url)) {
        cJSON_free(body);
        return false;
    }
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = EL_READ_BYTES;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (c) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_header(c, "Accept", "audio/mpeg");
        /* The key comes from the job (copied from NVS at start time);
         * it is never logged. */
        esp_http_client_set_header(c, "xi-api-key", j->key);
        int len = strlen(body);
        int64_t t0 = esp_timer_get_time();
        if (esp_http_client_open(c, len) != ESP_OK ||
            esp_http_client_write(c, body, len) != len) {
            ESP_LOGW(TAG, "el_tts: can't reach ElevenLabs");
        } else {
            esp_http_client_fetch_headers(c);
            int status = esp_http_client_get_status_code(c);
            static uint8_t buf[EL_READ_BYTES];
            if (status != 200) {
                int n = esp_http_client_read(c, (char *)buf, sizeof(buf) - 1);
                buf[n > 0 ? n : 0] = '\0';
                ESP_LOGW(TAG, "el_tts: ElevenLabs HTTP %d %s", status, (char *)buf);
                if (status == 401) {
                    ESP_LOGW(TAG, "el_tts: HTTP 401: bad key, re-enter with >el_key=");
                }
                if (status == 429 || body_says_quota((char *)buf)) {
                    muse_tts_quota_exhaust();
                }
            } else {
                size_t total = 0;
                int n;
                ok = true;
                while ((n = esp_http_client_read(c, (char *)buf, sizeof(buf))) > 0) {
                    if (!total) {
                        ESP_LOGI(TAG, "el_tts: first audio after %.2fs",
                                 (esp_timer_get_time() - t0) / 1e6);
                    }
                    total += n;
                    if (!send_all(j->job, buf, n)) {
                        ok = false;
                        break;
                    }
                    if (esp_timer_get_time() - t0 > (int64_t)EL_FETCH_DEADLINE_S * 1000000LL) {
                        ESP_LOGW(TAG, "el_tts: fetch deadline hit");
                        ok = false;
                        break;
                    }
                }
                ok = ok && n == 0 && total > 0 && esp_http_client_is_complete_data_received(c);
                ESP_LOGI(TAG, "el_tts: %u bytes of MP3%s", (unsigned)total, ok ? "" : ", incomplete");
            }
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    cJSON_free(body);
    return ok;
}

static void el_task(void *arg)
{
    (void)arg;
    static struct el_job_t j;
    for (;;) {
        if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* Sync the clock first (fetch task may block up to 12 s, same as
         * Edge): the monthly quota rollover depends on it, and Edge may
         * never run now that ElevenLabs is the primary path. */
        (void)muse_edge_tts_time_ready();
        bool ok = false;
        if (!muse_tts_quota_exhausted()) {
            ok = el_fetch(&j);
        }
        if (j.job == s_job) {
            s_ok = ok;
            s_done_job = j.job;
            s_done = true;
        }
    }
}

bool muse_elevenlabs_tts_start(int msg_index, const char *text, size_t len)
{
    (void)msg_index;
    const char *key = muse_tts_el_key();
    if (key[0] == '\0' || !text || len == 0) {
        return false;
    }
    if (!s_jobs) {
        s_jobs = xQueueCreateWithCaps(1, sizeof(struct el_job_t), MALLOC_CAP_SPIRAM);
        s_rx = xStreamBufferCreateWithCaps(EL_RX_BYTES, 1, MALLOC_CAP_SPIRAM);
        if (!s_jobs || !s_rx ||
            xTaskCreatePinnedToCoreWithCaps(el_task, "muse_el_tts", 8 * 1024, NULL, 4, NULL, 1,
                                            MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGE(TAG, "el_tts: no memory for the fetch task");
            return false;
        }
    }
    static struct el_job_t j;
    j.job = ++s_job;
    size_t copy = len < EL_TEXT_MAX - 1 ? len : EL_TEXT_MAX - 1;
    memcpy(j.text, text, copy);
    j.text[copy] = '\0';
    strlcpy(j.key, key, sizeof(j.key));
    xStreamBufferReset(s_rx);
    s_ok = false;
    s_done = false;
    s_got_audio = false;
    s_pending = true;
    if (xQueueSend(s_jobs, &j, 0) != pdTRUE) {
        s_pending = false;
        return false;
    }
    return true;
}

int muse_elevenlabs_tts_pump(muse_elevenlabs_tts_mp3_cb_t on_mp3)
{
    if (!s_pending) {
        return 0;
    }
    static uint8_t chunk[EL_READ_BYTES];
    for (;;) {
        size_t n = xStreamBufferReceive(s_rx, chunk, sizeof(chunk), 0);
        if (!n) {
            break;
        }
        s_got_audio = true;
        on_mp3(chunk, n);
    }
    if (!s_done || s_done_job != s_job || !xStreamBufferIsEmpty(s_rx)) {
        return 0;
    }
    s_pending = false;
    if (s_ok || s_got_audio) {
        return 1;
    }
    return -1;
}
