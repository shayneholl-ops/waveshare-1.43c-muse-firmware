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

/*
 * ElevenLabs text-to-speech, direct to api.elevenlabs.io. The API key
 * lives only in this board's NVS, entered by the user on the serial
 * console (">el_key=<key>"): it is never compiled into firmware, never
 * logged, and never printed back, and OTA preserves NVS so the key
 * survives updates. The voice/model are plain configuration, not
 * credentials; an empty voice ID uses the built-in default voice.
 *
 * Fetch model mirrors wupsbr's ElevenLabs client (one fetch task, MP3
 * streamed into a buffer, drained by the chat session task) and the
 * pump contract mirrors the Edge client: 1 = fetch finished with audio,
 * -1 = finished with no audio, 0 = still going. Only the chat session
 * task calls start/pump; only the fetch task touches the network.
 *
 * The key comes from muse_tts_quota. No key in NVS: this path is off
 * and replies fall back to Edge. The old proxy URL setting is dormant
 * and no longer consulted.
 */

typedef void (*muse_elevenlabs_tts_mp3_cb_t)(const uint8_t *data, size_t len);

/* True when an ElevenLabs API key is stored in NVS (the path can be attempted). */
bool muse_elevenlabs_tts_available(void);

/* Start fetching speech for one reply message. False when the path is
 * off, the text is empty, or the fetch task cannot be started; the
 * caller then falls back to Edge. Quota accounting is the caller's job. */
bool muse_elevenlabs_tts_start(int msg_index, const char *text, size_t len);

/* Drain newly arrived MP3 bytes through on_mp3. Reports completion only
 * once the fetch is done and the buffer is fully drained, so the audio
 * tail is never cut. After reporting 1 or -1 it returns 0 until the
 * next start(). */
int muse_elevenlabs_tts_pump(muse_elevenlabs_tts_mp3_cb_t on_mp3);

#ifdef __cplusplus
}
#endif
