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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Monthly ElevenLabs free-tier accounting, persisted in NVS so it survives
 * OTA updates. The free tier is 10,000 credits a month; the default Flash
 * model costs about 0.5 credits per character, so the month's budget is
 * 20,000 characters. Counting happens when a fetch starts (credits are
 * spent on request, so a failed fetch still counts -- the safe direction).
 * The month rolls over on the wall clock (SNTP); while the clock is not
 * yet valid, the stored month is kept and counting continues against it.
 * If NVS is unavailable, accounting falls back to RAM only for the boot.
 *
 * The ElevenLabs API key is deliberately NEVER compiled into firmware:
 * the firmware is built on a VM the user does not control, so a key in
 * source or Kconfig would pass through other hands and stay in the .bin.
 * Instead the key lives only in this board's NVS (same namespace as the
 * quota), entered by the user himself on the serial console
 * (">el_key=<key>" stores, ">el_key=" clears, ">el_key" reports only
 * set/unset, never the value). OTA preserves NVS, so changing or
 * clearing the key never needs a rebuild. No key in NVS: the
 * ElevenLabs path is off and replies use Edge only.
 *
 * The proxy URL is retained for compatibility (Kconfig
 * CONFIG_MUSE_TTS_PROXY_URL, serial ">tts_proxy=<url>") but is dormant:
 * the ElevenLabs client no longer consults it. It is not a credential
 * either way.
 */

#define MUSE_TTS_EL_MONTH_CHAR_BUDGET 20000u

/* Non-blocking. True when `chars` more characters fit in this month's
 * budget. Rolls the month over first, but only when the clock is valid. */
bool muse_tts_quota_allow(size_t chars);

/* Count `chars` against this month's budget (call when a fetch starts). */
void muse_tts_quota_add(size_t chars);

/* Mark this month exhausted (e.g. ElevenLabs answered 429 / quota error):
 * the ElevenLabs path stays off until the next month. */
void muse_tts_quota_exhaust(void);

/* True when this month is exhausted (budget spent or marked so). */
bool muse_tts_quota_exhausted(void);

/* Effective proxy URL: the console-stored value, else the Kconfig value,
 * else "". Never a credential. */
const char *muse_tts_proxy_url(void);

/* Store a proxy URL in NVS (NULL or "" clears it, back to Kconfig).
 * No restart is needed: the ElevenLabs client reads it on every start. */
bool muse_tts_proxy_set(const char *url);

/* Serial console command, chained with the SDK token command:
 * ">tts_proxy=<url>" stores, ">tts_proxy=" clears, ">tts_proxy" prints.
 * Retained but dormant: the ElevenLabs client no longer uses the URL.
 * Returns true when the line was this command. */
bool muse_tts_proxy_console_command(const char *line);

/* Effective ElevenLabs API key from NVS, else "". Never a build-time
 * value: there is no Kconfig for it on purpose. */
const char *muse_tts_el_key(void);

/* Store an ElevenLabs API key in NVS (NULL or "" clears it). No restart
 * is needed: the ElevenLabs client reads it on every start. The key is
 * never logged. */
bool muse_tts_el_key_set(const char *key);

/* Serial console command, chained with the SDK token command:
 * ">el_key=<key>" stores, ">el_key=" clears, ">el_key" reports only
 * "set" or "(unset)" -- the key value is never printed or logged.
 * Returns true when the line was this command. */
bool muse_tts_el_key_console_command(const char *line);

#ifdef __cplusplus
}
#endif
