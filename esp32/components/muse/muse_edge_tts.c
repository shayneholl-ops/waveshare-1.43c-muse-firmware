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
 * Microsoft Edge online TTS client (edge-tts 7.3.1 wire protocol).
 *
 * Endpoint: wss://speech.platform.bing.com/consumer/speech/synthesize/
 *   readaloud/edge/v1?TrustedClientToken=<public constant>
 *   &ConnectionId=<uuid>&Sec-MS-GEC=<v2 token>&Sec-MS-GEC-Version=2-143.0.3650.75
 * The trusted client token is published in edge-tts itself; it is not a
 * user secret and no user key is involved anywhere here.
 *
 * Flow per message: TLS (certificate bundle) -> WebSocket upgrade ->
 * speech.config text message -> SSML text message -> binary audio
 * messages (2-byte big-endian header length, header block, MP3 payload;
 * verified against a live capture 2026-10-26) until a turn.end text
 * message. The fetch task streams MP3 bytes into a FreeRTOS stream
 * buffer; the session task drains it through muse_edge_tts_pump().
 *
 * Failure handling: ANY failure (no clock, TLS/WS error, 403, timeout,
 * malformed frame) ends the fetch and reports done with got_audio set
 * from the bytes actually delivered. The session task then either plays
 * the partial audio or falls back to silent caption pacing. Nothing here
 * can crash or stall a reply: the fetch has a 30 s overall deadline and
 * every socket operation carries the 10 s esp-tls network timeout.
 */

#include "muse_edge_tts.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "muse_edge_tts_drm.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "muse_edge_tts";

/* Public Edge client token, from edge-tts constants.py. Public, not secret. */
#define EDGE_HOST "speech.platform.bing.com"
#define EDGE_PATH "/consumer/speech/synthesize/readaloud/edge/v1"
#define EDGE_CLIENT_TOKEN "6A5AA1D4EAFF4E9FB37E23D68491D6F4"
#define EDGE_GEC_VERSION "2-143.0.3650.75"
#define EDGE_USER_AGENT \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " \
    "(KHTML, like Gecko) Chrome/143.0.0.0 Safari/537.36 Edg/143.0.0.0"

#define FETCH_DEADLINE_US (30 * 1000000LL)
#define SNTP_WAIT_MS 12000
#define TIME_VALID_AFTER 1704067200LL /* 2024-01-01: anything older is unset */
#define STREAM_BUF_BYTES (32 * 1024)
#define MSG_BUF_BYTES (16 * 1024)
#define HANDSHAKE_BUF_BYTES 4096

typedef struct {
    uint32_t gen;
    bool got_audio;
} done_t;

static StreamBufferHandle_t s_stream;
static QueueHandle_t s_done;
static volatile uint32_t s_gen;
static bool s_reported;
static bool s_sntp_started;
static int32_t s_skew_s;

/* ---- small helpers ---- */

static void hex_bytes(const uint8_t *in, size_t n, char *out, bool upper)
{
    static const char low[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *t = upper ? up : low;
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = t[(in[i] >> 4) & 0xf];
        out[i * 2 + 1] = t[in[i] & 0xf];
    }
    out[n * 2] = '\0';
}

/* JavaScript-style UTC date, exactly as edge-tts date_to_string() builds
 * it. newlib's default "C" locale gives English day/month names. */
static void js_date(char *out, size_t out_size)
{
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(out, out_size,
             "%a %b %d %Y %H:%M:%S GMT+0000 (Coordinated Universal Time)",
             &tmv);
}

/* Case-insensitive header lookup over a "\r\n"-separated block. Returns
 * the value start (after ':' and one optional space) and length, or NULL.
 * The block need not be NUL-terminated. */
static const char *header_value(const uint8_t *block, size_t block_len,
                                const char *key, size_t *value_len)
{
    size_t key_len = strlen(key);
    size_t off = 0;
    while (off < block_len) {
        size_t line_end = off;
        while (line_end < block_len && block[line_end] != '\r' &&
               block[line_end] != '\n') {
            line_end++;
        }
        if (line_end - off > key_len &&
            block[off + key_len] == ':') {
            bool match = true;
            for (size_t i = 0; i < key_len; i++) {
                if (tolower(block[off + i]) != tolower((uint8_t)key[i])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                size_t v = off + key_len + 1;
                if (v < line_end && block[v] == ' ') {
                    v++;
                }
                *value_len = line_end - v;
                return (const char *)block + v;
            }
        }
        off = line_end;
        while (off < block_len &&
               (block[off] == '\r' || block[off] == '\n')) {
            off++;
        }
    }
    return NULL;
}

static bool header_equals(const uint8_t *block, size_t block_len,
                          const char *key, const char *want)
{
    size_t vlen = 0;
    const char *v = header_value(block, block_len, key, &vlen);
    return v != NULL && vlen == strlen(want) &&
           strncmp(v, want, vlen) == 0;
}

/* Days since 1970-01-01 (Howard Hinnant's civil algorithm). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* "Mon, 26 Oct 2026 17:02:21 GMT" -> unix seconds, or -1 on failure. */
static int64_t parse_rfc1123(const char *s, size_t len)
{
    char buf[64];
    if (len >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[4] = {0};
    if (sscanf(buf, "%*s %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm,
               &ss) != 6) {
        return -1;
    }
    int month = 0;
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon, months[i], 3) == 0) {
            month = i + 1;
            break;
        }
    }
    if (!month || day < 1 || day > 31 || year < 1970 || hh > 23 || mm > 59 ||
        ss > 60) {
        return -1;
    }
    return days_from_civil(year, month, day) * 86400 + hh * 3600 + mm * 60 +
           ss;
}

/* ---- clock ---- */

/* True when the wall clock is usable for the 5-minute token windows.
 * Starts SNTP once and waits up to SNTP_WAIT_MS on first use; never
 * blocks the session task (this runs on the fetch task). */
bool muse_edge_tts_time_ready(void)
{
    if (time(NULL) >= TIME_VALID_AFTER) {
        return true;
    }
    if (!s_sntp_started) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            s_sntp_started = true;
        } else {
            ESP_LOGE(TAG, "edge_tts: SNTP init failed: %s",
                     esp_err_to_name(err));
            return false;
        }
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_WAIT_MS)) != ESP_OK) {
        ESP_LOGE(TAG, "edge_tts: time sync timed out, staying text-only");
        return false;
    }
    return time(NULL) >= TIME_VALID_AFTER;
}

/* ---- TLS / WebSocket plumbing ---- */

static bool tls_write_all(esp_tls_t *tls, const uint8_t *data, size_t len)
{
    while (len > 0) {
        ssize_t n = esp_tls_conn_write(tls, data, len);
        if (n <= 0) {
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    return true;
}

/* Returns false on error/close (caller treats it as end of stream). */
static bool tls_read_exact(esp_tls_t *tls, uint8_t *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = esp_tls_conn_read(tls, buf, len);
        if (n <= 0) {
            return false;
        }
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

static bool ws_send(esp_tls_t *tls, uint8_t opcode, const uint8_t *payload,
                    size_t len)
{
    uint8_t mask[4];
    uint32_t r = esp_random();
    memcpy(mask, &r, sizeof(mask));
    uint8_t hdr[10];
    size_t hlen = 0;
    hdr[hlen++] = (uint8_t)(0x80 | opcode);
    if (len < 126) {
        hdr[hlen++] = (uint8_t)(0x80 | len);
    } else if (len < 65536) {
        hdr[hlen++] = (uint8_t)(0x80 | 126);
        hdr[hlen++] = (uint8_t)((len >> 8) & 0xff);
        hdr[hlen++] = (uint8_t)(len & 0xff);
    } else {
        hdr[hlen++] = (uint8_t)(0x80 | 127);
        for (int i = 7; i >= 0; i--) {
            hdr[hlen++] = (uint8_t)((len >> (8 * i)) & 0xff);
        }
    }
    if (!tls_write_all(tls, hdr, hlen) ||
        !tls_write_all(tls, mask, sizeof(mask))) {
        return false;
    }
    /* Mask in 4 KB chunks so long payloads need no extra buffer. */
    uint8_t chunk[4096];
    size_t off = 0;
    while (off < len) {
        size_t n = len - off < sizeof(chunk) ? len - off : sizeof(chunk);
        for (size_t i = 0; i < n; i++) {
            chunk[i] = payload[off + i] ^ mask[(off + i) % 4];
        }
        if (!tls_write_all(tls, chunk, n)) {
            return false;
        }
        off += n;
    }
    return true;
}

/* Reads one WebSocket frame's payload into frame (FRAME_BUF_BYTES max).
 * Sets *opcode (1 text, 2 binary, 0 continuation, 8 close, 9 ping,
 * 10 pong) and *fin. Returns false on error/close/timeout or an
 * oversize frame. Reassembly (fragmentation, control frames) is the
 * caller's job. */
#define FRAME_BUF_BYTES 4096
static bool ws_read_frame(esp_tls_t *tls, uint8_t *frame, size_t *frame_len,
                          uint8_t *opcode, bool *fin)
{
    uint8_t h[2];
    if (!tls_read_exact(tls, h, 2)) {
        return false;
    }
    *fin = (h[0] & 0x80) != 0;
    *opcode = h[0] & 0x0f;
    uint64_t len = h[1] & 0x7f;
    if (len == 126) {
        uint8_t ext[2];
        if (!tls_read_exact(tls, ext, 2)) {
            return false;
        }
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (!tls_read_exact(tls, ext, 8)) {
            return false;
        }
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | ext[i];
        }
    }
    bool masked = (h[1] & 0x80) != 0;
    uint8_t mask[4] = {0};
    if (masked && !tls_read_exact(tls, mask, 4)) {
        return false;
    }
    if (len > FRAME_BUF_BYTES) {
        ESP_LOGW(TAG, "edge_tts: WS frame too large (%" PRIu64 " bytes)",
                 len);
        return false;
    }
    if (!tls_read_exact(tls, frame, (size_t)len)) {
        return false;
    }
    if (masked) {
        for (uint64_t i = 0; i < len; i++) {
            frame[i] ^= mask[i % 4];
        }
    }
    *frame_len = (size_t)len;
    return true;
}

/* ---- the fetch ---- */

typedef struct {
    uint32_t gen;
    char *text; /* owned, freed on task exit */
    size_t len;
} fetch_arg_t;

static void fetch_done(fetch_arg_t *arg, bool got_audio)
{
    done_t d = {.gen = arg->gen, .got_audio = got_audio};
    xQueueSend(s_done, &d, 0);
}

static bool fetch_once(fetch_arg_t *arg, int64_t deadline_us,
                       bool *got_audio_out, bool *retry_403)
{
    *got_audio_out = false;
    *retry_403 = false;
    char token[82];
    if (!muse_edge_tts_token(token, sizeof(token), (int64_t)time(NULL),
                             s_skew_s)) {
        ESP_LOGE(TAG, "edge_tts: token build failed");
        return false;
    }
    uint8_t rb[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = esp_random();
        memcpy(rb + i, &r, 4);
    }
    char conn_id[33], muid[33], ws_key[32];
    hex_bytes(rb, 16, conn_id, false);
    hex_bytes(rb, 16, muid, true);
    size_t olen = 0;
    if (mbedtls_base64_encode((uint8_t *)ws_key, sizeof(ws_key), &olen, rb,
                              16) != 0) {
        return false;
    }

    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_tls_t *tls = esp_tls_init();
    if (tls == NULL) {
        ESP_LOGE(TAG, "edge_tts: TLS alloc failed");
        return false;
    }
    int rc = esp_tls_conn_new_sync(EDGE_HOST, strlen(EDGE_HOST), 443, &cfg,
                                   tls);
    if (rc != 1) {
        ESP_LOGE(TAG, "edge_tts: TLS connect failed");
        esp_tls_conn_destroy(tls);
        return false;
    }

    /* Handshake. */
    char req[1024];
    int req_len = snprintf(
        req, sizeof(req),
        "GET %s?TrustedClientToken=%s&ConnectionId=%s&Sec-MS-GEC=%s"
        "&Sec-MS-GEC-Version=%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold\r\n"
        "Pragma: no-cache\r\n"
        "Cache-Control: no-cache\r\n"
        "User-Agent: %s\r\n"
        "Accept-Encoding: gzip, deflate, br, zstd\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n"
        "Cookie: muid=%s\r\n\r\n",
        EDGE_PATH, EDGE_CLIENT_TOKEN, conn_id, token, EDGE_GEC_VERSION,
        EDGE_HOST, ws_key, EDGE_USER_AGENT, muid);
    if (req_len < 0 || (size_t)req_len >= sizeof(req)) {
        esp_tls_conn_destroy(tls);
        return false;
    }
    bool ok = tls_write_all(tls, (uint8_t *)req, (size_t)req_len);
    if (!ok) {
        ESP_LOGE(TAG, "edge_tts: handshake send failed");
        esp_tls_conn_destroy(tls);
        return false;
    }
    uint8_t hs[HANDSHAKE_BUF_BYTES];
    size_t hs_len = 0;
    bool complete = false;
    while (hs_len < sizeof(hs) - 1) {
        ssize_t n = esp_tls_conn_read(tls, hs + hs_len, 1);
        if (n <= 0) {
            break;
        }
        hs_len += (size_t)n;
        if (hs_len >= 4 &&
            memcmp(hs + hs_len - 4, "\r\n\r\n", 4) == 0) {
            complete = true;
            break;
        }
    }
    if (!complete) {
        ESP_LOGE(TAG, "edge_tts: handshake reply unreadable");
        esp_tls_conn_destroy(tls);
        return false;
    }
    hs[hs_len] = '\0';
    if (strstr((char *)hs, " 101 ") == NULL) {
        if (strstr((char *)hs, " 403 ") != NULL) {
            size_t vlen = 0;
            const char *date =
                header_value(hs, hs_len, "Date", &vlen);
            int64_t server = date ? parse_rfc1123(date, vlen) : -1;
            if (server > 0) {
                s_skew_s = (int32_t)(server - (int64_t)time(NULL));
                ESP_LOGW(TAG,
                         "edge_tts: 403, clock skew %d s, retrying once",
                         (int)s_skew_s);
                *retry_403 = true;
            } else {
                ESP_LOGE(TAG, "edge_tts: handshake 403 without usable Date");
            }
        } else {
            ESP_LOGE(TAG, "edge_tts: handshake rejected: %.40s", hs);
        }
        esp_tls_conn_destroy(tls);
        return false;
    }

    /* speech.config + SSML, exactly as edge-tts 7.3.1 sends them. */
    char date[64];
    js_date(date, sizeof(date));
    char config[512];
    int config_len = snprintf(
        config, sizeof(config),
        "X-Timestamp:%s\r\n"
        "Content-Type:application/json; charset=utf-8\r\n"
        "Path:speech.config\r\n\r\n"
        "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":"
        "{\"sentenceBoundaryEnabled\":\"true\",\"wordBoundaryEnabled\":"
        "\"false\"},\"outputFormat\":\"audio-24khz-48kbitrate-mono-mp3\"}"
        "}}}}\r\n",
        date);
    if (config_len < 0 || (size_t)config_len >= sizeof(config)) {
        esp_tls_conn_destroy(tls);
        return false;
    }
    ok = ws_send(tls, 1, (uint8_t *)config, (size_t)config_len);
    if (!ok) {
        ESP_LOGE(TAG, "edge_tts: speech.config send failed");
        esp_tls_conn_destroy(tls);
        return false;
    }
    /* Escape the text: control chars (except tab/LF/CR) -> space, then
     * & < > -> entities. Byte-wise, so UTF-8 passes through untouched. */
    size_t esc_cap = arg->len * 5 + 1;
    char *esc = malloc(esc_cap);
    if (esc == NULL) {
        esp_tls_conn_destroy(tls);
        return false;
    }
    size_t esc_len = 0;
    for (size_t i = 0; i < arg->len; i++) {
        uint8_t c = (uint8_t)arg->text[i];
        const char *rep = NULL;
        if (c == '&') {
            rep = "&amp;";
        } else if (c == '<') {
            rep = "&lt;";
        } else if (c == '>') {
            rep = "&gt;";
        } else if ((c <= 8) || (c >= 11 && c <= 12) || (c >= 14 && c <= 31)) {
            esc[esc_len++] = ' ';
            continue;
        }
        if (rep != NULL) {
            size_t rl = strlen(rep);
            memcpy(esc + esc_len, rep, rl);
            esc_len += rl;
        } else {
            esc[esc_len++] = c;
        }
    }
    esc[esc_len] = '\0';
    uint8_t rb2[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = esp_random();
        memcpy(rb2 + i, &r, 4);
    }
    char req_id[33];
    hex_bytes(rb2, 16, req_id, false);
    js_date(date, sizeof(date));
    size_t ssml_cap = esc_len + 512;
    char *ssml = malloc(ssml_cap);
    if (ssml == NULL) {
        free(esc);
        esp_tls_conn_destroy(tls);
        return false;
    }
    int ssml_len = snprintf(
        ssml, ssml_cap,
        "X-RequestId:%s\r\n"
        "Content-Type:application/ssml+xml\r\n"
        "X-Timestamp:%sZ\r\n"
        "Path:ssml\r\n\r\n"
        "<speak version='1.0' "
        "xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'>"
        "<voice name='%s'>"
        "<prosody pitch='+0Hz' rate='+0%%' volume='+0%%'>%s</prosody>"
        "</voice></speak>",
        req_id, date, MUSE_EDGE_TTS_VOICE, esc);
    free(esc);
    if (ssml_len < 0 || (size_t)ssml_len >= ssml_cap) {
        free(ssml);
        esp_tls_conn_destroy(tls);
        return false;
    }
    ok = ws_send(tls, 1, (uint8_t *)ssml, (size_t)ssml_len);
    free(ssml);
    if (!ok) {
        ESP_LOGE(TAG, "edge_tts: SSML send failed");
        esp_tls_conn_destroy(tls);
        return false;
    }

    /* Read frames until turn.end, close, error, deadline, or a newer
     * generation superseding this fetch. */
    uint8_t *msg = malloc(MSG_BUF_BYTES);
    uint8_t *frame = malloc(FRAME_BUF_BYTES);
    if (msg == NULL || frame == NULL) {
        free(msg);
        free(frame);
        esp_tls_conn_destroy(tls);
        return false;
    }
    size_t total_audio = 0;
    bool turn_end = false;
    while (!turn_end) {
        if (arg->gen != s_gen || esp_timer_get_time() > deadline_us) {
            break;
        }
        /* Reassemble one message, answering control frames as they come
         * without disturbing the fragment state. */
        size_t msg_len = 0;
        uint8_t msg_opcode = 0;
        bool first = true;
        bool msg_complete = false;
        bool stream_over = false;
        while (!msg_complete && !stream_over) {
            size_t frame_len = 0;
            uint8_t opcode = 0;
            bool fin = false;
            if (!ws_read_frame(tls, frame, &frame_len, &opcode, &fin)) {
                stream_over = true;
                break;
            }
            if (opcode == 8) {
                stream_over = true; /* close */
                break;
            }
            if (opcode == 9) { /* ping -> pong, same payload */
                ws_send(tls, 10, frame, frame_len);
                continue;
            }
            if (opcode == 10) { /* pong: ignore */
                continue;
            }
            if (first) {
                if (opcode != 1 && opcode != 2) {
                    stream_over = true; /* continuation first: violation */
                    break;
                }
                msg_opcode = opcode;
                first = false;
            } else if (opcode != 0) {
                stream_over = true; /* new message mid-fragment: violation */
                break;
            }
            if (frame_len > MSG_BUF_BYTES - msg_len) {
                ESP_LOGW(TAG, "edge_tts: message too large, ending fetch");
                stream_over = true;
                break;
            }
            memcpy(msg + msg_len, frame, frame_len);
            msg_len += frame_len;
            msg_complete = fin;
        }
        if (stream_over || msg_len == 0) {
            if (stream_over) {
                break;
            }
            continue;
        }
        /* One complete message in msg[0:msg_len]. */
        if (msg_opcode == 1) {
            /* Text: headers, blank line, JSON body. */
            size_t sep = 0;
            bool found = false;
            for (size_t i = 0; i + 3 < msg_len; i++) {
                if (memcmp(msg + i, "\r\n\r\n", 4) == 0) {
                    sep = i;
                    found = true;
                    break;
                }
            }
            size_t hdr_len = found ? sep : msg_len;
            if (header_equals(msg, hdr_len, "Path", "turn.end")) {
                turn_end = true;
            }
            /* turn.start / response / audio.metadata: nothing to do. */
        } else {
            /* Binary: [BE16 header length][headers][MP3 payload]. */
            if (msg_len < 2) {
                continue;
            }
            uint16_t hlen = (uint16_t)((msg[0] << 8) | msg[1]);
            if ((size_t)hlen + 2 > msg_len) {
                ESP_LOGW(TAG, "edge_tts: short binary message, ignored");
                continue;
            }
            const uint8_t *hdr = msg + 2;
            size_t payload_off = 2 + (size_t)hlen;
            size_t payload_len = msg_len - payload_off;
            if (!header_equals(hdr, hlen, "Path", "audio")) {
                continue;
            }
            size_t ct_len = 0;
            const char *ct = header_value(hdr, hlen, "Content-Type", &ct_len);
            if (ct != NULL && !(ct_len == 10 && strncmp(ct, "audio/mpeg", 10) == 0)) {
                ESP_LOGW(TAG, "edge_tts: unexpected audio Content-Type");
                continue;
            }
            if (ct == NULL && payload_len != 0) {
                ESP_LOGW(TAG, "edge_tts: audio without Content-Type, ignored");
                continue;
            }
            if (payload_len == 0) {
                continue; /* end marker */
            }
            /* Stream it; if the session task is not draining, wait a
             * little, then drop the rest of this chunk (minimp3 resyncs
             * at the next frame). */
            size_t sent = 0;
            while (sent < payload_len) {
                if (arg->gen != s_gen) {
                    break;
                }
                size_t n = xStreamBufferSend(
                    s_stream, msg + payload_off + sent, payload_len - sent,
                    pdMS_TO_TICKS(2000));
                if (n == 0) {
                    ESP_LOGW(TAG, "edge_tts: stream buffer full, dropping %u bytes",
                             (unsigned)(payload_len - sent));
                    break;
                }
                sent += n;
            }
            total_audio += sent;
        }
    }
    *got_audio_out = total_audio > 0;
    free(msg);
    free(frame);
    ws_send(tls, 8, NULL, 0); /* best-effort close */
    esp_tls_conn_destroy(tls);
    return true;
}

static void fetch_task(void *arg_v)
{
    fetch_arg_t *arg = (fetch_arg_t *)arg_v;
    int64_t deadline_us = esp_timer_get_time() + FETCH_DEADLINE_US;
    bool got_audio = false;
    if (!muse_edge_tts_time_ready()) {
        ESP_LOGE(TAG, "edge_tts: no usable clock, staying text-only");
    } else {
        /* fetch_once reports a 403-with-Date retry through retry even
         * when it returns false, so check the flag, not the result. */
        bool retry = false;
        fetch_once(arg, deadline_us, &got_audio, &retry);
        if (retry) {
            /* One retry with the server-Date skew, as edge-tts does. */
            bool retry_audio = false;
            bool retry_again = false;
            fetch_once(arg, deadline_us, &retry_audio, &retry_again);
            got_audio = retry_audio;
        }
    }
    /* Report exactly once, even on failure, so the session task never
     * waits on a dead fetch. Stale generations are discarded by pump(). */
    if (arg->gen == s_gen) {
        fetch_done(arg, got_audio);
    }
    ESP_LOGI(TAG, "edge_tts: fetch done, %s",
             got_audio ? "audio received" : "no audio");
    free(arg->text);
    free(arg);
    vTaskDelete(NULL);
}

/* ---- session-task API ---- */

bool muse_edge_tts_start(int msg_index, const char *text, size_t len)
{
    (void)msg_index;
    if (text == NULL || len == 0) {
        return false;
    }
    if (s_stream == NULL) {
        /* Stream buffer MUST live in PSRAM: plain FreeRTOS allocations
         * (xStreamBufferCreate) come strictly from internal RAM
         * (heap_idf.c portFREERTOS_HEAP_CAPS = MALLOC_CAP_INTERNAL |
         * MALLOC_CAP_8BIT, no PSRAM fallback), and this board only has
         * ~27 KB internal free during a voice turn -- less than the
         * 32 KB buffer. With the plain call the buffer creation fails
         * every time and Edge TTS silently never starts. The ElevenLabs
         * client already uses SPIRAM caps for its stream buffer. */
        s_stream = xStreamBufferCreateWithCaps(STREAM_BUF_BYTES, 1,
                                               MALLOC_CAP_SPIRAM);
        s_done = xQueueCreate(4, sizeof(done_t));
        if (s_stream == NULL || s_done == NULL) {
            return false;
        }
    }
    fetch_arg_t *arg = malloc(sizeof(*arg));
    if (arg == NULL) {
        return false;
    }
    arg->text = malloc(len + 1);
    if (arg->text == NULL) {
        free(arg);
        return false;
    }
    memcpy(arg->text, text, len);
    arg->text[len] = '\0';
    arg->len = len;
    /* New generation supersedes any older fetch still running; its
     * output is discarded by the generation checks. */
    s_gen++;
    arg->gen = s_gen;
    s_reported = false;
    xStreamBufferReset(s_stream);
    xQueueReset(s_done);
    /* Fetch-task stack also in PSRAM (same reason as the stream buffer:
     * 12 KB would still fit today, but leaves almost no internal margin
     * once TLS is up; the ElevenLabs fetch task already runs its stack
     * from PSRAM, and the chat task's 48 KB stack is PSRAM too). */
    if (xTaskCreatePinnedToCoreWithCaps(fetch_task, "muse_edge_tts", 12288,
                                        arg, 4, NULL, 1,
                                        MALLOC_CAP_SPIRAM |
                                            MALLOC_CAP_8BIT) != pdPASS) {
        free(arg->text);
        free(arg);
        return false;
    }
    ESP_LOGI(TAG, "edge_tts: fetching speech for %u bytes", (unsigned)len);
    return true;
}

int muse_edge_tts_pump(muse_edge_tts_mp3_cb_t on_mp3)
{
    if (s_stream == NULL || s_reported) {
        return 0;
    }
    uint8_t buf[2048];
    size_t n;
    while ((n = xStreamBufferReceive(s_stream, buf, sizeof(buf), 0)) > 0) {
        if (on_mp3 != NULL) {
            on_mp3(buf, n);
        }
    }
    /* Only finalize once the buffer is fully drained. */
    while (true) {
        done_t d;
        if (xQueuePeek(s_done, &d, 0) != pdTRUE) {
            return 0;
        }
        if (d.gen != s_gen) {
            xQueueReceive(s_done, &d, 0); /* stale, discard */
            continue;
        }
        if (xStreamBufferBytesAvailable(s_stream) > 0) {
            return 0; /* more bytes to drain first */
        }
        xQueueReceive(s_done, &d, 0);
        s_reported = true;
        return d.got_audio ? 1 : -1;
    }
}
