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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Edge TTS voice for spoken replies. Override with
 * -DMUSE_EDGE_TTS_VOICE="zh-CN-YunjianNeural" (or any voice from
 * https://api.msedgeservices.com/tts/cognitiveservices/voices/list). */
#ifndef MUSE_EDGE_TTS_VOICE
#define MUSE_EDGE_TTS_VOICE "zh-CN-XiaoxiaoNeural"
#endif

/*
 * Fetch spoken audio for one reply message from Microsoft Edge's online
 * TTS service (free, no user key; see muse_edge_tts.c for the endpoint
 * and token details).
 *
 * Threading: ONLY the chat session task may call these. A background
 * fetch task does the network work and hands MP3 bytes to the session
 * task through a stream buffer, plus a done item through a queue. The
 * session task is the only task that touches the caller's MP3 buffer.
 *
 * muse_edge_tts_start() copies the text and returns false when it cannot
 * start (no memory or task spawn failure). The fetch task prefers SNTP for
 * wall time; if SNTP is unavailable, it recovers clock skew from Edge's
 * authenticated HTTP Date response and retries once. The caller falls back
 * to silent caption pacing only when both attempts produce no audio.
 *
 * muse_edge_tts_pump() drains newly arrived MP3 bytes through on_mp3 and
 * reports completion, but ONLY once the stream buffer is fully drained:
 *   1: fetch finished and at least one audio byte arrived (set mp3_ended)
 *  -1: fetch finished with no audio (fall back to silent caption pacing)
 *   0: still fetching, or nothing new, or already reported
 * Call it once per session loop iteration while a message is speaking.
 * After it reports 1 or -1 it returns 0 until the next start().
 */
typedef void (*muse_edge_tts_mp3_cb_t)(const uint8_t *data, size_t len);

bool muse_edge_tts_start(int msg_index, const char *text, size_t len);
int muse_edge_tts_pump(muse_edge_tts_mp3_cb_t on_mp3);

/*
 * Shared wall-clock gate, also used by the ElevenLabs path's monthly quota:
 * true once SNTP has synced the clock past TIME_VALID_AFTER. First use
 * starts SNTP and waits up to 12 s; call it from a fetch task, never from
 * the chat session task.
 */
bool muse_edge_tts_time_ready(void);

#ifdef __cplusplus
}
#endif
