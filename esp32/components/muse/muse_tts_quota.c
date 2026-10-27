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

#include "muse_tts_quota.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "tts_quota";

#define NVS_NS "muse_tts"
#define KEY_MONTH "el_month"
#define KEY_CHARS "el_chars"
#define KEY_PROXY "proxy"
#define KEY_EL "el_key"
#define EL_KEY_MAX 128
/* 2025-01-01 UTC: the clock must be past this to be trusted for months. */
#define TIME_VALID_AFTER_Q 1735689600LL
#define PROXY_MAX 256

static nvs_handle_t s_nvs;
static bool s_nvs_ok;
static bool s_nvs_tried;
static uint32_t s_mem_month;
static uint32_t s_mem_chars;
static char s_mem_proxy[PROXY_MAX];
static char s_mem_el_key[EL_KEY_MAX];

static uint32_t month_now(void)
{
    time_t now = time(NULL);
    if (now < TIME_VALID_AFTER_Q) {
        return 0;
    }
    struct tm tmv;
    gmtime_r(&now, &tmv);
    return (uint32_t)((tmv.tm_year + 1900) * 100 + (tmv.tm_mon + 1));
}

static void nvs_try_open(void)
{
    if (s_nvs_tried) {
        return;
    }
    s_nvs_tried = true;
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES &&
        err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init failed: %s, quota is RAM-only", esp_err_to_name(err));
        return;
    }
    if (nvs_open(NVS_NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed, quota is RAM-only");
        return;
    }
    s_nvs_ok = true;
}

static uint32_t read_u32(const char *key, uint32_t fallback)
{
    nvs_try_open();
    if (!s_nvs_ok) {
        return fallback;
    }
    uint32_t v = fallback;
    if (nvs_get_u32(s_nvs, key, &v) != ESP_OK) {
        return fallback;
    }
    return v;
}

static void write_u32(const char *key, uint32_t v)
{
    nvs_try_open();
    if (!s_nvs_ok) {
        return;
    }
    if (nvs_set_u32(s_nvs, key, v) == ESP_OK) {
        nvs_commit(s_nvs);
    }
}

/* Rolls the month over when the clock is valid and the stored month is
 * stale. Returns the month accounting currently applies to (0 when the
 * clock is invalid: keep counting against the stored month). */
static uint32_t current_month(void)
{
    uint32_t now = month_now();
    uint32_t stored = s_nvs_ok ? read_u32(KEY_MONTH, 0) : s_mem_month;
    if (now == 0) {
        return stored;
    }
    if (stored != now) {
        write_u32(KEY_MONTH, now);
        write_u32(KEY_CHARS, 0);
        s_mem_month = now;
        s_mem_chars = 0;
        return now;
    }
    return stored;
}

static uint32_t chars_used(void)
{
    nvs_try_open();
    if (s_nvs_ok) {
        return read_u32(KEY_CHARS, 0);
    }
    return s_mem_chars;
}

bool muse_tts_quota_allow(size_t chars)
{
    (void)current_month();
    return (uint64_t)chars_used() + chars <= MUSE_TTS_EL_MONTH_CHAR_BUDGET;
}

void muse_tts_quota_add(size_t chars)
{
    (void)current_month();
    uint32_t used = chars_used();
    uint32_t next = used + (uint32_t)chars;
    if (next < used) {
        next = MUSE_TTS_EL_MONTH_CHAR_BUDGET;
    }
    if (s_nvs_ok) {
        write_u32(KEY_CHARS, next);
    } else {
        s_mem_chars = next;
    }
}

void muse_tts_quota_exhaust(void)
{
    (void)current_month();
    if (s_nvs_ok) {
        write_u32(KEY_CHARS, MUSE_TTS_EL_MONTH_CHAR_BUDGET);
    } else {
        s_mem_chars = MUSE_TTS_EL_MONTH_CHAR_BUDGET;
    }
    ESP_LOGW(TAG, "ElevenLabs month marked exhausted, falling back to Edge");
}

bool muse_tts_quota_exhausted(void)
{
    (void)current_month();
    return chars_used() >= MUSE_TTS_EL_MONTH_CHAR_BUDGET;
}

const char *muse_tts_proxy_url(void)
{
    nvs_try_open();
    if (s_nvs_ok) {
        size_t len = sizeof(s_mem_proxy);
        if (nvs_get_str(s_nvs, KEY_PROXY, s_mem_proxy, &len) == ESP_OK &&
            s_mem_proxy[0] != '\0') {
            return s_mem_proxy;
        }
    } else if (s_mem_proxy[0] != '\0') {
        return s_mem_proxy;
    }
    return CONFIG_MUSE_TTS_PROXY_URL;
}

bool muse_tts_proxy_set(const char *url)
{
    if (!url) {
        url = "";
    }
    if (strlen(url) >= PROXY_MAX) {
        return false;
    }
    nvs_try_open();
    if (s_nvs_ok) {
        esp_err_t err = url[0] ? nvs_set_str(s_nvs, KEY_PROXY, url)
                               : nvs_erase_key(s_nvs, KEY_PROXY);
        if (err != ESP_OK || nvs_commit(s_nvs) != ESP_OK) {
            return false;
        }
    }
    strlcpy(s_mem_proxy, url, sizeof(s_mem_proxy));
    return true;
}

bool muse_tts_proxy_console_command(const char *line)
{
    static const char *SET = "tts_proxy=";
    static const char *GET = "tts_proxy";
    if (strncmp(line, SET, strlen(SET)) == 0) {
        const char *url = line + strlen(SET);
        if (!muse_tts_proxy_set(url)) {
            printf("@tts_proxy error storage\n");
            fflush(stdout);
            return true;
        }
        ESP_LOGI(TAG, "proxy URL %s", url[0] ? "stored" : "cleared");
        printf("@tts_proxy %s\n", url[0] ? "ok" : "cleared");
        fflush(stdout);
        return true;
    }
    if (strcmp(line, GET) == 0) {
        const char *url = muse_tts_proxy_url();
        printf("@tts_proxy %s\n", url[0] ? url : "(unset)");
        fflush(stdout);
        return true;
    }
    return false;
}

/* ---- ElevenLabs API key (NVS only, never compiled in) ---- */

const char *muse_tts_el_key(void)
{
    nvs_try_open();
    if (s_nvs_ok) {
        size_t len = sizeof(s_mem_el_key);
        if (nvs_get_str(s_nvs, KEY_EL, s_mem_el_key, &len) == ESP_OK &&
            s_mem_el_key[0] != '\0') {
            return s_mem_el_key;
        }
    } else if (s_mem_el_key[0] != '\0') {
        return s_mem_el_key;
    }
    return "";
}

bool muse_tts_el_key_set(const char *key)
{
    if (!key) {
        key = "";
    }
    if (strlen(key) >= EL_KEY_MAX) {
        return false;
    }
    nvs_try_open();
    if (s_nvs_ok) {
        esp_err_t err = key[0] ? nvs_set_str(s_nvs, KEY_EL, key)
                               : nvs_erase_key(s_nvs, KEY_EL);
        if (err != ESP_OK || nvs_commit(s_nvs) != ESP_OK) {
            return false;
        }
    }
    strlcpy(s_mem_el_key, key, sizeof(s_mem_el_key));
    return true;
}

bool muse_tts_el_key_console_command(const char *line)
{
    static const char *SET = "el_key=";
    static const char *GET = "el_key";
    if (strncmp(line, SET, strlen(SET)) == 0) {
        const char *key = line + strlen(SET);
        /* Never echo or log the key itself: only its fate. */
        if (strlen(key) >= EL_KEY_MAX) {
            printf("@el_key too long\n");
            fflush(stdout);
            return true;
        }
        if (!muse_tts_el_key_set(key)) {
            printf("@el_key error storage\n");
            fflush(stdout);
            return true;
        }
        ESP_LOGI(TAG, "ElevenLabs key %s", key[0] ? "stored" : "cleared");
        printf("@el_key %s\n", key[0] ? "ok" : "cleared");
        fflush(stdout);
        return true;
    }
    if (strcmp(line, GET) == 0) {
        /* Query reports only presence, never the value. */
        printf("@el_key %s\n", muse_tts_el_key()[0] ? "set" : "(unset)");
        fflush(stdout);
        return true;
    }
    return false;
}
