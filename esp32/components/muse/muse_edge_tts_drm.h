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
 * Compute a Sec-MS-GEC v2 token for Microsoft Edge's online TTS service.
 *
 * out must hold at least 82 bytes ("<16 hex>-<64 hex>" + NUL). Returns
 * false if out is too small. unix_seconds is UTC seconds; skew_seconds
 * is added first (server-Date clock-skew correction, 0 when unknown).
 *
 * The embedded client token constant is public (published in edge-tts);
 * it is not a user secret.
 */
bool muse_edge_tts_token(char *out, size_t out_size, int64_t unix_seconds,
                         int32_t skew_seconds);

#ifdef __cplusplus
}
#endif
