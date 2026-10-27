# TTS for the rebuilt Muse gadget (Waveshare ESP32-S3-Touch-AMOLED-1.43C)

## Status: Edge TTS BUILT 2026-10-26 — awaiting OTA/flash + hardware test

Edge TTS is implemented, built, and verified on the host (see below). The
previous "blocked" status is SUPERSEDED: edge-tts 7.3.1's released v2
generator was ported to C and verified byte-for-byte against the Python
reference, and the firmware now syncs wall-clock time via SNTP. No code
was flashed or OTA'd from this VM — the board still runs the non-TTS
build until the user applies one of the images below.

Deliverables (in this directory; the known-good non-TTS
`muse-1.43c-merged.bin` was NOT overwritten — its SHA-256 still matches
the recorded value):

- `muse-1.43c-tts-merged.bin` — 3,215,360 bytes,
  SHA-256 `ae72e9f9712ba74fc0be9857172d5faf0ea27a6bdbeaff663718275d0bcc0b9e`.
  Full reflash image at offset `0x00`. **WIPES NVS — last resort only**
  (Wi-Fi + SDK token must be redone afterwards).
- `muse-1.43c-tts-app.bin` — 3,084,288 bytes (signed app-only image),
  SHA-256 `48acc84b1be13a6e7299b682e519cd569534e58ec6296c155f293fbe2c84ec3d`.
  **OTA candidate (preferred):** host at HTTPS and invoke `device.ota`
  with `{url, force: true}` (version gate: current build reports
  `999.0.0`, same as the running build, so `force: true` is required
  unless the version is bumped). NVS intact. Also flashable with esptool
  at `0x20000` (NVS intact).

Build verification (2026-10-26, `./tools/board.sh waveshare-s3-143c build`,
exit 0):

- Signed app 3,080,192 bytes by IDF's measure (file 3,084,288 with the
  signature sector) vs OTA slot `0x3E0000` (4,063,232 bytes):
  `0xEF000` bytes (24%) free. IDF's `check_sizes` passed.
- Partition table decoded from the built `partition-table.bin` is
  IDENTICAL to `partitions_muse_8mb.csv` (nvs `0x11000`/48K, otadata
  `0x1D000`/8K, phy `0x1F000`/4K, ota_0 `0x20000`/3968K,
  ota_1 `0x400000`/3968K, prod_data `0x7E0000`/4K,
  prod_bak `0x7E1000`/4K). Flash offsets unchanged: bootloader `0x0`,
  partition table `0x10000`, otadata `0x1D000`, app `0x20000`.
- No credential strings in either binary: no `mgst_` + suffix anywhere
  (the bare literal `mgst_` appears once, as in the pre-TTS build — it
  is the SDK's token-prefix placeholder in code, not a token value), no
  Wi-Fi SSID/password strings. The Edge trusted client token
  `6A5AA1D4EAFF4E9FB37E23D68491D6F4` IS present once per binary: it is
  the public constant published in edge-tts itself, not a user secret,
  and no user key of any kind is in the build (the expected "No SDK
  token" build warning is unchanged — the SDK token lives in NVS and
  survives OTA).
- Board pins, display init, and battery logic untouched (ADC1_CH3/GPIO4
  x2, GPIO7 STAT LOW=charging). Only added files + the `start_tts`
  wiring described below.

## Why Edge is unblocked (what changed since the blocked note)

### 1. Sec-MS-GEC v2: ported from released code and verified byte-for-byte

- edge-tts **7.3.1** (release artifacts, not main-branch guesses) merged
  Sec-MS-GEC v2 support (PR #492). The generator was ported faithfully
  to C in `esp32/components/muse/muse_edge_tts_drm.c`
  (+ `muse_edge_tts_drm.h`): same float ticks path (double arithmetic,
  `fmod(t, 300.0)`, `*= 1e7`, `%.0f` — NOT recomputed with integer
  arithmetic, per the #490 warning), same custom SHA-256 first-block
  quirks (pre-seeded words, tweaked round constant, 57+len length
  field), same CS64 digest. Host test vs the Python 7.3.1 reference:
  **9/9 tokens identical**, including the golden vector from issue #490
  (window starting 2026-10-13T10:40:00Z →
  `492D7694A183A45E-56E821F95D4579C5B7C7E33DC7303FF379EC36D510213555BBBB9EB17EF15876`),
  same token inside a 300 s window, new token at the next window.
- The v2 token was also verified LIVE: a raw WebSocket handshake from
  this VM using a token from the C port's algorithm returned
  **HTTP 101 Switching Protocols** (a bad token gives 403), and real
  MP3 audio flowed back (see "live capture" below).

### 2. Wall-clock time: SNTP added

- `muse_edge_tts.c` starts SNTP once (`esp_netif_sntp_init` with the
  default `pool.ntp.org` server) and waits up to 12 s for sync on first
  TTS use, only when the clock is not already valid. If sync fails or
  times out, the message falls back to text-only — replies are never
  blocked waiting for time. No `sdkconfig` change was needed (SNTP/LWIP
  options and the certificate bundle were already enabled).

## Edge protocol implemented (edge-tts 7.3.1, verified against a live capture)

- Endpoint (NOT the `api.msedgeservices.com` endpoint from the old
  note — 7.3.1 still uses the Bing endpoint):
  `wss://speech.platform.bing.com/consumer/speech/synthesize/readaloud/edge/v1?TrustedClientToken=6A5AA1D4EAFF4E9FB37E23D68491D6F4&ConnectionId=<uuid-no-dashes>&Sec-MS-GEC=<v2-token>&Sec-MS-GEC-Version=2-143.0.3650.75`
- WebSocket handshake headers: `Origin:
  chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold`,
  `User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)
  AppleWebKit/537.36 (KHTML, like Gecko) Chrome/143.0.0.0 Safari/537.36
  Edg/143.0.0.0`, `Pragma: no-cache`, `Cache-Control: no-cache`,
  `Accept-Encoding: gzip, deflate, br, zstd`,
  `Accept-Language: en-US,en;q=0.9`, `Cookie: muid=<32-hex-UPPER>`.
  No subprotocol is sent (7.3.1 sends none). Transport is a minimal
  WebSocket client over `esp-tls` with the certificate bundle —
  `esp_websocket_client` does NOT exist in ESP-IDF v6.0.1, so no new
  component was added (`esp-tls`, `mbedtls`, `esp_netif` were already
  private requirements).
- Config message: `X-Timestamp:<JS-date>` +
  `Content-Type:application/json; charset=utf-8` + `Path:speech.config`
  + `{"context":{"synthesis":{"audio":{"metadataoptions":{"sentenceBoundaryEnabled":"true","wordBoundaryEnabled":"false"},"outputFormat":"audio-24khz-48kbitrate-mono-mp3"}}}}`
  (7.3.1 defaults: sentence boundaries on, word boundaries off).
- SSML message: `X-RequestId:<uuid-no-dashes>` +
  `Content-Type:application/ssml+xml` +
  `X-Timestamp:<JS-date>Z` (the trailing Z is Edge's own bug, kept
  deliberately) + `Path:ssml` + body
  `<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'><voice name='VOICE'><prosody pitch='+0Hz' rate='+0%' volume='+0%'>TEXT</prosody></voice></speak>`
  (`xml:lang='en-US'` is hardcoded even for Chinese voices, as in
  7.3.1; text prep: control chars 0–8, 11–12, 14–31 → space, then XML
  escape of `&` `<` `>` only, byte-wise so UTF-8 passes through).
- Audio arrives as binary WebSocket messages: 2-byte big-endian header
  length L, header block `msg[2:2+L]`, MP3 payload `msg[2+L:]`.
  Verified live 2026-10-26: L=128, headers
  `X-RequestId:… / Content-Type:audio/mpeg / X-StreamId:… /
  Path:audio`, payload starting with MP3 sync `FF F3` + `LAME`,
  720 MP3 bytes per 850-byte message. A final binary message with no
  Content-Type and no data is the expected end marker and is ignored.
  Text messages: `Path:turn.end` ends the fetch; `turn.start`,
  `response`, `audio.metadata` are ignored.
- On a handshake 403 with a `Date` header, the client parses the
  RFC1123 date, sets the clock skew (server − local), and retries once
  — mirroring `DRM.handle_client_response_error`.
- Voice is a single define in `muse_edge_tts.h`:
  `#define MUSE_EDGE_TTS_VOICE "zh-CN-XiaoxiaoNeural"` (overridable
  with `-DMUSE_EDGE_TTS_VOICE="..."`).
- Live-capture caveat (VM only, NOT a firmware problem): VM egress goes
  through a MITM proxy that rewrites the 101 response's `Connection`
  header to `close`, which makes aiohttp reject the handshake. A raw
  client ignoring that header got 101 + audio fine. The gadget on home
  Wi-Fi has no such proxy.

## Integration (what the code does)

Files (all under `esp32/components/muse/`, SDK checkout):

- `muse_edge_tts_drm.c/h` — the v2 token generator (host-testable, no
  IDF dependencies).
- `muse_edge_tts.c/h` — SNTP, TLS + WebSocket client, fetch task,
  stream buffer (32 KB) + done queue + generation counter.
- `muse_chat_session.cpp` — `start_tts()` now tries the speaking path
  first (text from `s_turn.texts + i * TEXT_MAX`, up to 1023 bytes);
  new `pump_tts()` called between `start_tts()` and `decode()` in the
  `P_WAIT_REPLY` loop; `CMakeLists.txt` adds both new `.c` files under
  `CONFIG_MUSE_HATCH` (both branches that compile
  `muse_chat_session.cpp`).

Proven from `esp32/components/muse/muse_chat_session.cpp` (unchanged
mechanics, now wired):

- Speaking setup: keep `m.tts = TTS_ACTIVE`, `s_turn.tts_msg = i`,
  `s_turn.silent = false`, `m.pcm_start = s_turn.pcm_out`,
  `m.pcm_frames = 0`, `s_turn.mp3_len = 0`, `s_turn.mp3_ended = false`,
  `s_turn.kbps = 0`, `s_turn.down_rate = 0`, `mp3dec_init(&s_turn.dec)`;
  `pump_tts()` passes MP3 to `tts_data()` as it arrives and sets
  `s_turn.mp3_ended` at the end. `decode()` plays it at the speaker's
  volume with captions following, and finishes the message once
  drained. `start_tts()` still returns early while `s_turn.tts_msg >=
  0`, so per-message drain/done handling lives in `pump_tts()`, not in
  `start_tts()`.
- Sample rate is NOT a constraint: `decode()` resamples whatever
  minimp3 reports (`info.hz`) to `MIC_RATE` (16 kHz) and mixes stereo
  to mono. `audio-24khz-48kbitrate-mono-mp3` needs no extra work.
- Buffer: `MP3_BUF` is 512 KB on this board (8 MB PSRAM,
  `CONFIG_MUSE_HATCH_COMPACT_BUFFERS` off) — about 85 seconds of 48 kbps
  speech. `tts_data()` drops bytes beyond it by design; very long
  replies will have their speech tail cut (captions are unaffected).
- Threading rule (kept): only the chat session task (`hatch_task`)
  touches `s_turn.mp3`. The fetch task writes ONLY to the stream buffer
  and the done queue; `hatch_task` drains via `pump_tts()`. A
  generation counter discards output from superseded fetches, and a
  done item is only applied after the stream buffer is fully drained
  (else the audio tail would be cut).
- Failure degrades to text-only, never crashes or blocks the on-screen
  reply: on fetch error / close / 30 s fetch deadline, if no audio
  arrived, `pump_tts()` reverts to silent pacing
  (`s_turn.silent = true`,
  `m.pcm_frames = m.len * MIC_RATE / TEXT_CHARS_PER_S`); if partial
  audio arrived, it sets `s_turn.mp3_ended` so `decode()` drains what
  is there. If `muse_edge_tts_start()` itself fails (no clock, no
  memory, task spawn failed) or the text is missing/empty, `start_tts()`
  takes the silent path immediately. Serial log lines are tagged
  `muse_edge_tts` with `edge_tts: ...` messages; a fetch failure logs
  `edge_tts: fetch failed` / `edge_tts: no audio` style lines.

## Token baking: NOT possible, and NOT needed

- **Not possible:** the SDK token lives in the Secure Vault (connector
  `custom.muse-gadget`) and cannot be read back by any tool. It must never
  be asked for, accepted, or recorded in chat or in any file. The build
  stays token-free for exactly this reason (the SDK's own "No SDK token"
  warning is expected).
- **Not needed:** the token (and Wi-Fi credentials) live in NVS, and NVS
  survives every update path except a full merged-image reflash:
  - **OTA (preferred):** `device.ota` takes `{url, force}` and runs
    `esp_https_ota`, which writes the new app into the *inactive* OTA
    slot and updates `otadata` only. It never erases NVS (the OTA code
    even writes a verify record *into* NVS). Wi-Fi + token survive.
  - **App-only esptool flash:** writing only the app binary at `0x20000`
    (the `flash_args` offset for `muse-gadget.bin`) leaves the NVS region
    at `0x11000` untouched. Wi-Fi + token survive.
  - **Full merged-image esptool reflash (last resort):** this DOES wipe
    NVS — proven byte-for-byte: the delivered
    `muse-1.43c-merged.bin` contains `0xFF` padding at offset `0x11000`
    (the NVS region), while the partition table at `0x10000` is intact.
    Flashing it at `0x00` therefore blanks NVS, and the user must redo
    the serial token step (`>sdk_token=` + Ctrl-J, NOT Enter) and
    re-enter Wi-Fi afterwards.

## OTA details (read from `esp32/main/ota.c` + `noise_control.cpp`)

- `device.ota` requires a non-empty `url` (HTTPS) and accepts optional
  `force` (boolean). The image is downloaded with `esp_https_ota`
  (TLS via the IDF certificate bundle) and the board reboots on success.
- **The OTA image must be the APP-ONLY binary, never the merged image.**
  `esp_https_ota` parses the app descriptor at the start of the download;
  a merged image starts with the bootloader, so it fails with "could not
  read image descriptor". The TTS OTA candidate is
  `muse-1.43c-tts-app.bin` (3,084,288 bytes, SHA-256
  `48acc84b1be13a6e7299b682e519cd569534e58ec6296c155f293fbe2c84ec3d`,
  copy of the build's signed `build-waveshare-s3-143c/muse-gadget.bin`).
  Record size + SHA-256 at build time for every future build the same
  way.
- **Version gate:** without `force`, OTA skips any image whose version is
  not newer than the running one. The current build reports version
  `999.0.0`, so an OTA of a same-version build MUST pass `force: true`,
  or the build must be stamped with `-DPROJECT_VER` newer than
  `999.0.0`. This is the single most likely "OTA did nothing" trap.
- After a successful OTA the next boot is a pending-verify boot: the
  board must reach the control session (i.e. come back online) or it
  rolls back to the previous image. A successful OTA is therefore only
  confirmed once the board reappears online with the new version.
- OTA is NOT attempted from this VM (no device here, no hosted URL).
  To use it, the parent must host the app-only `.bin` at an HTTPS URL
  the gadget can reach and invoke `device.ota` with `{url, force: true}`
  (or a bumped version).

## Concrete fallback proposal: Azure Speech F0 (REST) — retained, not needed

Edge is now built (see top), so this fallback is on standby only, for
the case that Edge fails on the real hardware. It needs one user-side
gate (an Azure resource + key), so it is proposed here, not silently
substituted.

- User-side gate first: create an Azure Speech resource on the **F0 free
  tier** (0.5 million neural characters/month free; then $15 per 1M
  characters). The key must go to the device the same way the SDK token
  did (serial console or build-time menuconfig) — never pasted in chat.
- Firmware side (same hook design as above, simpler transport):
  `POST https://<region>.tts.speech.microsoft.com/cognitiveservices/v1`
  with header `Ocp-Apim-Subscription-Key: <key>` and
  `X-Microsoft-OutputFormat: audio-24khz-48kbitrate-mono-mp3`, SSML body
  with voice `zh-CN-XiaoxiaoNeural`. The response body is MP3 → feed to
  `tts_data()` exactly as the Edge design describes. No WebSocket, no
  time-windowed token, no SNTP dependency — this is why it is the
  fallback: it sidesteps both Edge blockers.
- Same graceful failure rule: any TTS failure falls back to text-only
  captions; it must never crash the firmware or block the reply.
- When approved, the fallback build will deliver BOTH images, with sizes
  and SHA-256 recorded here, exactly as the Edge build above does.

## Flash steps (user's Mac, TTS build 2026-10-26)

- Preferred — OTA: host `muse-1.43c-tts-app.bin` at HTTPS, then
  `device.ota` with `{url, force: true}`. No USB, no serial, NVS intact.
- App-only esptool (NVS intact):
  `esptool.py --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 921600 --before default_reset --after hard_reset write_flash 0x20000 muse-1.43c-tts-app.bin`
- Full merged reflash (wipes NVS — last resort):
  `esptool.py --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 921600 --before default_reset --after hard_reset write_flash 0x00 muse-1.43c-tts-merged.bin`
  then redo `>sdk_token=` over serial (Ctrl-J, not Enter) and re-enter
  Wi-Fi in the app.

## How to test (once the TTS build is on the gadget)

1. Confirm the board comes back online as MuseGadget-0E2EC8 (OTA:
   also confirms the pending-verify boot passed; if it rolls back, the
   old version is still running).
2. Talk to the gadget (button talk / voice note). Expect: the reply text
   on screen AND the same reply spoken in Chinese (Xiaoxiao voice)
   through the speaker.
3. If speech is absent but captions still appear, the TTS fetch failed
   and fell back to text-only — check the serial log for the
   `muse_edge_tts` / `edge_tts:` error line (e.g. `edge_tts: TLS
   connect failed`, `edge_tts: time sync timed out`, `edge_tts: no
   audio`); the reply path itself is working.
4. Also verify the 12-item hardware checklist items that overlap audio:
   speaker output level, both microphones, and the battery reading
   (ADC x2) — unchanged by this work, but worth re-confirming after any
   reflash.

## v2 COMBINED BUILD 2026-10-27 UTC — SUPERSEDES the v1 Edge-only packages above

This build folds the wupsbr 1.43C board fixes AND dual-path TTS into our
own firmware (base: this SDK checkout + our authored board file).
wupsbr was used as reference only: its firmware was NOT flashed wholesale,
so this update stays OTA-compatible (NVS intact). The v1 packages
(`muse-1.43c-tts-app.bin` 48acc84b…ec3d,
`muse-1.43c-tts-merged.bin` ae72e9f9…0b9e) are VOID as of this build;
their files were left untouched (SHAs re-verified after this build).

### Deliverables (this directory)

- `muse-1.43c-tts2-app.bin` — 3,084,288 bytes (signed app-only image),
  SHA-256 `2157f7de8d3530e7e477d251bd9803d6e7ca6043b564512c2273e6228d520b07`.
  **OTA candidate (preferred):** host at HTTPS and invoke `device.ota`
  with `{url, force: true}` (version gate: build reports `999.0.0`,
  same as the running build). NVS intact. Also flashable with esptool
  at `0x20000` (NVS intact).
- `muse-1.43c-tts2-merged.bin` — 3,215,360 bytes,
  SHA-256 `0ce610a395102abe745f1d4e63cfd392b7c048216bc96202e27ed93f81de31c5`.
  Full reflash image at offset `0x00`. **WIPES NVS — last resort only**
  (verified: the NVS region at `0x11000` is all `0xFF` in this image).

### Board fixes (from wupsbr, via clone of wupsbr/waveshare-muse-gadget-sdk @ 2c64881)

- Display gap 6 → 8, both gap calls (`esp_lcd_panel_set_gap` and
  `muse_lcd_bands_set_gap`); the 2A window start (column 6) is the
  panel's GRAM window, not the driver gap.
- NS4150B `pa_voltage` 3.3 → 5.0 (volume mapping only, no hardware risk).
- `display_pause`: pause now also hibernates the CST820 (register
  `0xA5` ← `0x03`); resume resets the touch first (200/200/200 ms
  pulse, our existing timing), then resumes LVGL. Cuts idle current.
- BOOT hint moved to bottom-center `{ LV_ALIGN_CENTER, 0, 190 }`
  (eyeball-verify against the physical BOOT key after flashing).
- Brightness init `0x51`: `0xFF` → `0x00` (dark start, UI takes over).
- Touch clamp: coordinates now clamp to 465 (`LCD_WIDTH/HEIGHT - 1`)
  instead of 466.
- `wait_band` freeze fix, ADAPTED not wholesale: wupsbr's bands file is
  a full rewrite that drops the TE machinery our board file uses
  (`muse_lcd_bands_set_te`), so only the wait mechanism was ported
  into our bands file — `s_band_done` semaphore + `s_band_busy` flag,
  `lv_display_set_flush_wait_cb(s_disp, wait_band)` (blocking wait,
  100 ms re-checks), busy set on successful queue, cleared in the
  chunk-done ISR and in the send task's synchronous error path (so a
  failed piece cannot leave the wait stuck). Our TE code is untouched.

### Deliberately NOT adopted (same as the comparison verdict)

- wupsbr's battery percentage curve (ours stays: verified 88% @ 4084mV).
- wupsbr's USB inference (ours stays: `usb = charging`).
- `power_off` stays NULL (behavior change, not default).
- wupsbr's secrets mechanism stays OUT: no key is compiled into this
  firmware (see below).

### TTS: dual path, ElevenLabs primary, Edge fallback (user's routing rule)

- Primary: ElevenLabs via the user's own proxy
  (`muse_elevenlabs_tts.c`, fetch model follows wupsbr's streaming
  client: one fetch task, 64 KB PSRAM stream buffer, MP3 drained by the
  session task). The firmware posts only text (+ voice/model config)
  to the proxy URL and never sends an `xi-api-key` header; the key
  lives in the Worker's environment. Supports Chinese (voice/model
  come from Kconfig or the proxy's defaults).
- Fallback: Edge (existing code, untouched except that its SNTP gate
  `time_ready()` is now exported as `muse_edge_tts_time_ready()` so
  the quota month can roll over even when Edge never runs). The Edge
  v2 token generator (`muse_edge_tts_drm.c`) was NOT re-ported, so no
  re-verification against the Python reference was needed.
- Routing in `start_tts()`/`pump_tts()`: ElevenLabs is tried first
  while the month's free tier lasts; ANY ElevenLabs failure (network,
  proxy error, quota) retries the same message on Edge; only when
  Edge also fails does the message pace silently. Captions never block.
- Monthly quota (`muse_tts_quota.c`, NVS namespace `muse_tts`,
  survives OTA): 10,000 credits/month at Flash ≈ 0.5 credits/char →
  20,000 characters/month, counted when a fetch starts (conservative:
  a failed fetch still counts). Month rolls over on the SNTP wall
  clock; while the clock is invalid, counting continues against the
  stored month. A 429 or quota-worded error answer marks the month
  exhausted immediately. Once exhausted, the month stays on Edge until
  the next month.
- Proxy address, configurable two ways (Kconfig is only the default):
  build-time `CONFIG_MUSE_TTS_PROXY_URL` (default empty = ElevenLabs
  path off), or at runtime from the serial console, chained with the
  SDK token command: `>tts_proxy=<url>` stores (answers
  `@tts_proxy ok`), `>tts_proxy=` clears, `>tts_proxy` prints the
  current value. No restart needed; the next spoken reply uses it.
  The proxy URL is not a credential and is safe to keep in NVS.
- Proxy code: `~/workspace/firmware-rebuild/elevenlabs-proxy/`
  (`worker.js`, syntax-checked with node, + `README.md` with deploy
  steps, free-tier limits, and the keep-the-URL-private warning).
  The Worker passes upstream errors through so the gadget sees 429s.

### Build verification (2026-10-27 UTC, `./tools/board.sh waveshare-s3-143c build`)

- First attempt FAILED (ninja exit 1): the new ElevenLabs header used
  `uint8_t` without `#include <stdint.h>` (one root cause, two errors).
  Fixed; second attempt exit 0.
- Signed app 3,080,192 bytes by IDF's measure (file 3,084,288 with the
  signature sector) vs OTA slot `0x3E0000` (4,063,232 bytes):
  `0xEF000` bytes (24%) free. IDF's `check_sizes` passed.
- Partition table decoded from the built `partition-table.bin` is
  IDENTICAL to `partitions_muse_8mb.csv` (nvs `0x11000`/48K, otadata
  `0x1D000`/8K, phy `0x1F000`/4K, ota_0 `0x20000`/3968K,
  ota_1 `0x400000`/3968K, prod_data `0x7E0000`/4K,
  prod_bak `0x7E1000`/4K). Flash offsets unchanged: bootloader `0x0`,
  partition table `0x10000`, otadata `0x1D000`, app `0x20000`.
- No credential strings in either binary: only the bare `mgst_`
  placeholder (as in all prior builds, no suffix); no `xi-api-key`
  string at all; no Wi-Fi SSID; no `workers.dev` URL (Kconfig default
  is empty); no `sk-` strings. The expected "No SDK token" build
  warning is unchanged — the token lives in NVS and survives OTA.
- Host test suite: `python3 -m unittest discover -s tests` → 629 tests
  ran, OK (skipped=7, the documented input-missing skips), exit 0.

### NOT verified live (human steps remain)

- The ElevenLabs path has no key yet, so no real ElevenLabs call was
  made from firmware: compile + routing + quota + proxy code are done,
  end-to-end speech is not. Remaining steps, in order: (1) user
  creates an ElevenLabs free account/key; (2) sets the Worker secret
  and deploys the Worker; (3) sets the proxy URL on the gadget
  (`>tts_proxy=<url>`, Ctrl-J not Enter); (4) user picks the flash
  route (OTA vs esptool) for the tts2 packages; (5) spoken-reply test
  on hardware: screen text AND voice together (ElevenLabs first while
  the month lasts, Edge after it is spent); text-only means the
  speaking path fell back (check serial for `el_tts:` / `edge_tts:`).
- The 12-item hardware verification list (gap offset after the gap
  fix, touch mirroring, audio, battery, BOOT hint position, idle
  current with touch hibernate) is still open and should be re-run
  after flashing, since the board file changed.

---

## tts3 (2026-10-26 night PDT): Edge never spoke — root cause found and fixed; ElevenLabs goes direct with an NVS key

### What the user saw

After the tts2 OTA (2026-10-26 23:09 PDT, force: true, 999.0.0 both
sides), spoken replies showed text on screen but no voice. The board
stayed up continuously after the OTA reboot (uptime 124 s → 273 s, no
crash loop), health ok. At that point no ElevenLabs proxy was
configured, so all speech depended on the Edge path.

### Root cause: the Edge stream buffer could never be allocated (code bug, deterministic)

Chain, with file:line evidence (all paths under
`esp32/components/muse/` unless noted):

- `muse_chat_session.cpp` `start_tts()` (~line 1557): with no proxy
  configured, `muse_elevenlabs_tts_available()` is false, so the
  ElevenLabs branch is skipped cleanly and the message is NOT
  consumed; the Edge branch is reached.
- `muse_edge_tts.c` `muse_edge_tts_start()` (old line 772):
  `s_stream = xStreamBufferCreate(STREAM_BUF_BYTES, 1)` with
  `STREAM_BUF_BYTES` = 32 KB (line 76). On failure it returns false,
  and `start_tts()` then paces the message silently — text only.
- The killer fact: plain FreeRTOS allocations come STRICTLY from
  internal RAM. ESP-IDF's FreeRTOS heap (`components/freertos/
  heap_idf.c:43,47-55`): `portFREERTOS_HEAP_CAPS =
  (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`, and `pvPortMalloc()` is
  `heap_caps_malloc(size, portFREERTOS_HEAP_CAPS)` — no PSRAM
  fallback. (Plain `malloc()` DOES fall back to PSRAM above the 4 KB
  `SPIRAM_MALLOC_ALWAYSINTERNAL` threshold, which is why the fetch
  task's `malloc`'d frame/message buffers were never the problem.)
- The board's internal free heap during a voice turn is only ~27 KB
  (`device.health` `heap_free_internal`: 27,031 bytes at uptime 124 s,
  26,711 at 273 s). 32 KB > 27 KB, so the stream buffer creation
  fails on EVERY spoken reply, deterministically, regardless of
  network, clock, or speaker. Even the freer pre-OTA reading
  (35,523 bytes) could not have covered buffer + 12 KB fetch-task
  stack (44 KB+). This bug existed since the v1 Edge port and never
  showed up in host tests, which do not emulate ESP heap caps.
- The v2 ElevenLabs code already knew this: it uses
  `xStreamBufferCreateWithCaps(..., MALLOC_CAP_SPIRAM)` for its 64 KB
  buffer and a SPIRAM task stack. The v1-era Edge code predates that
  lesson.

### Ruled out (with evidence, not assumed)

- Routing: clean (see chain above); quota (`muse_tts_quota.c`) is
  only consulted on the ElevenLabs branch and never blocks Edge.
- SNTP time gate: `muse_edge_tts_time_ready()` (~line 216) is correct
  as written — `ESP_NETIF_SNTP_DEFAULT_CONFIG` sets `wait_for_sync`
  and `start`, the board creates a default Wi-Fi STA netif
  (`main/wifi_mgr.c:205`), and smooth-sync is off, so the 12 s
  `sync_wait` is a real wait, not an instant failure. A home-network
  SNTP block would still be possible, but it cannot be the whole
  story: the allocation failure fires before any network access.
- Edge v2 token generator (`muse_edge_tts_drm.c`): NOT re-ported in
  v2, unchanged from the v1 9/9-verified port.
- Audio chain: codec LDO GPIO18 active-high confirmed; amp enable
  GPIO46 driven HIGH by the ES8311 driver (`es8311.c:447-458`,
  `pa_reverted` unset = false); `pa_voltage` 5.0 only shifts the
  volume mapping by ~3.6 dB (`esp_codec_dev_vol.c:50-60`), it cannot
  silence output; volume default 70, not zero.
- OTA rollback: ruled out. `main/app.c` `ota_verify_task()` marks a
  PENDING_VERIFY image valid once the control WS is up, and the
  board is online and chatting (control WS up) with continuous
  uptime since the OTA reboot — no second reboot, no revert. The
  board IS running v2/v3-line code; the failure is a runtime bug
  in it, not old firmware.

### The fix (minimal, precedented) — `muse_edge_tts.c` only

- Stream buffer: `xStreamBufferCreate(STREAM_BUF_BYTES, 1)` →
  `xStreamBufferCreateWithCaps(STREAM_BUF_BYTES, 1, MALLOC_CAP_SPIRAM)`
  (PSRAM free ~2.9 MB).
- Fetch-task stack: `xTaskCreate(..., 12288, ...)` →
  `xTaskCreatePinnedToCoreWithCaps(..., 12288, ..., 1,
  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` — same pattern as the
  ElevenLabs fetch task and the 48 KB PSRAM chat-task stack. The
  12 KB stack alone would have fit, but moving it too leaves
  comfortable internal margin once TLS is up.
- Added `#include "esp_heap_caps.h"`. Nothing else in the Edge path
  changed (SNTP, DRM, fetch, decode, audio untouched).

### ElevenLabs goes direct (user's final decision: no Cloudflare, no proxy)

Routing (final): NVS holds an ElevenLabs key → ElevenLabs first
while the month's free tier lasts (quota module UNCHANGED: 10,000
credits/month, Flash ≈ 0.5 credit/char → 20,000 chars/month, NVS
counting, SNTP month rollover, 429/quota-worded answer marks the
month exhausted); exhausted or fetch failure → Edge direct; no key
in NVS → Edge direct only. Cloudflare is involved nowhere; the
Worker proxy (`elevenlabs-proxy/`) is superseded and left on disk
unused.

Key storage (the hard constraint: NEVER compiled into firmware —
the firmware is built on a VM, so a key in source/Kconfig would pass
through other hands and stay in the .bin):

- New serial console commands, same style as `>tts_proxy=` /
  `>sdk_token=`, dispatched in `main/muse_glue.c`
  `console_command()` alongside them:
  - `>el_key=<key>` stores the key in board NVS (namespace
    `muse_tts`, key `el_key`, same mechanism as the quota/proxy
    values); answers `@el_key ok`. `>el_key=` clears (answers
    `@el_key cleared`). `>el_key` reports ONLY `@el_key set` or
    `@el_key (unset)` — the value is never printed or logged.
  - Finish the line with Ctrl-J, not Enter (the console ignores CR,
    only LF completes a line); the console does not echo, so typing
    the key does not expose it. No restart, no rebuild needed;
    OTA preserves NVS, so the key survives updates and can be
    changed/cleared any time.
- Client change (`muse_elevenlabs_tts.c`): requests go direct to
  `https://api.elevenlabs.io/v1/text-to-speech/<voice_id>/stream?output_format=mp3_44100_128`
  with an `xi-api-key` header taken from the NVS key (copied into
  the fetch job at start time). Voice: `CONFIG_MUSE_ELEVENLABS_VOICE_ID`
  (build sdkconfig: empty) → built-in default Rachel
  `21m00Tcm4TlvDq8ikWAM`; model: `CONFIG_MUSE_ELEVENLABS_MODEL`
  = `eleven_flash_v2_5` (unchanged). Quota/error mapping unchanged:
  429 or quota-worded body → month exhausted; new: HTTP 401 logs
  `el_tts: HTTP 401: bad key, re-enter with >el_key=` (a bad key
  costs one fast failed attempt per message, then Edge speaks).
- `muse_elevenlabs_tts_available()` now means "an NVS key is set"
  (was: "a proxy URL is set"). Session routing in
  `muse_chat_session.cpp` needed NO logic change (it already tries
  ElevenLabs first, then Edge on any failure); only its comments
  were updated. The `tts_proxy` command and Kconfig option are
  retained but dormant: the client no longer consults the proxy URL.
- Docs updated in the same change: `muse_tts_quota.h`,
  `muse_elevenlabs_tts.h`, `components/muse/Kconfig` help texts.

### tts3 packages (supersede tts2; tts2 files untouched)

- OTA app package (preferred; also esptool-flashable at `0x20000`,
  NVS intact):
  `~/workspace/firmware-rebuild/build/muse-1.43c-tts3-app.bin` —
  3,084,288 bytes, SHA-256
  `8bc829d65f810023844b7c9544a88ea6d89ec7030f178ee3547f20d479abeaff`.
- Full package (last resort, wipes NVS — verified `0x11000` all
  `0xFF` layout as in v2):
  `~/workspace/firmware-rebuild/build/muse-1.43c-tts3-merged.bin` —
  3,215,360 bytes, SHA-256
  `36c321752aefee42e2597d1de3c2dcaeadbd602af97f3e3686e1b46d08924381`.
- tts2 SHAs re-verified unchanged after the tts3 build (app
  `2157f7de…520b07`, merged `0ce610a3…31c5`).

### Build verification (final tts3 build, `./tools/board.sh waveshare-s3-143c build`)

- Exit 0 (incremental rebuild; `muse_edge_tts.c`,
  `muse_elevenlabs_tts.c`, `muse_tts_quota.c`, `muse_chat_session.cpp`,
  `muse_glue.c` recompiled; ninja log confirms the Edge file
  rebuilt). First Edge-only tts3 intermediate (SHA `9399c61e…`)
  was superseded by this final build before delivery and never
  flashed; only the SHAs above are the deliverable.
- Signed app 3,080,192 bytes by IDF's measure (file 3,084,288 with
  the signature sector) vs OTA slot `0x3E0000` (4,063,232 bytes):
  `0xEF000` bytes (24%) free. IDF's `check_sizes` passed.
- Partition table decoded from the built `partition-table.bin` is
  IDENTICAL to the v2 layout (nvs `0x11000`/48K, otadata
  `0x1D000`/8K, phy `0x1F000`/4K, ota_0 `0x20000`/3968K,
  ota_1 `0x400000`/3968K, prod_data `0x7E0000`/4K,
  prod_bak `0x7E1000`/4K). Merged-image layout verified:
  merged[`0x20000`:] == app file, and the merged header region
  (bootloader/partition/otadata) is byte-identical to tts2's — only
  the app changed.
- No-key self-check on both tts3 binaries: `xi-api-key` appears
  only as the header field name (a constant, no value attached);
  no `workers.dev`, no Wi-Fi SSID, no `sk_…` key-like strings; only
  the bare `mgst_` placeholder (as in all prior builds). The key
  cannot be in the binaries: it never enters source, Kconfig, or
  the build — it is entered on the board itself. The expected "No
  SDK token" build warning is unchanged — the token lives in NVS
  and survives OTA.
- New code confirmed present in the built ELF: the direct
  `api.elevenlabs.io` URL format, the default voice ID, the `el_key=`
  console command, and the 401 hint log line.
- Host test suite: `python3 -m unittest discover -s tests` →
  629 tests ran, OK (skipped=7, the documented input-missing skips),
  exit 0 — run against the final tts3 source.

### Still to verify live (human steps, in order)

1. Flash route: OTA the tts3 APP package (force: true, versions are
   both 999.0.0), exactly as tts2 was OTA'd. OTA counts only when
   the board returns online as MuseGadget-0E2EC8.
2. Speak to the board with NO key set: screen text AND Edge
   (Xiaoxiao) voice together = the Edge bug is fixed. Text-only at
   this step means something else is also wrong — check serial for
   `edge_tts:` lines (in particular `edge_tts: no usable clock`
   vs fetch errors vs `stream buffer` allocation).
3. Optional ElevenLabs step: on the serial console type
   `>el_key=<the ElevenLabs key>` (Ctrl-J, not Enter; no echo),
   then `>el_key` should answer `@el_key set`. Speak again:
   ElevenLabs voice while the month lasts; after the month is
   spent (or on any ElevenLabs failure) it falls back to Edge
   automatically. `>el_key=` clears the key (back to Edge only).
4. The 12-item hardware verification list is still open and should
   be re-run after flashing, since the board file changed in v2.

## tts4 — panda avatar (2026-10-27)

The main-tile avatar is now a procedural panda, drawn by the same
`muse_pixel` pose/expression system (no static bitmap, so every mode
still animates). Built on the tts3 tree with no tts3 change reverted:
Edge PSRAM fix, ElevenLabs direct routing, and NVS `>el_key=` are all
still in this image. Nothing else changed: no TTS/Edge/quota/
partition/sdkconfig/build-config change, and `avatar/happy_anim.c`
(the jollybot.gif animation) is untouched.

### Files changed

- `avatar/muse_pixel.c` (only file with behaviour change):
  - Palette reworked to panda: near-white outline `0xf2f2f5`,
    white/grey fur ramp, warm-white face panel, mid-grey brows
    (read on both black patches and white face), panda black
    `0x1a1a1e` for ears, limbs, eye patches, nose, and mouth.
    Blush, tongue, shadow, heart, iris, shine, and the state-tinted
    rim light are unchanged.
  - New `draw_eye_patch` (tilted black ellipse around each eye,
    mirrored) and `draw_nose` (wide-topped black triangle); patches
    are drawn before the eyes, the nose before the mouth.
  - Eyes: bead eyes sit on the black patches with a 12-wide white
    halo backing so they read; happy/X/shut linework is white, not
    iris-coloured. Gaze factors doubled with the grid.
  - Ears: black circles on top of the head, drawn first in the
    avatar loop under the `M_BODY` mask so the outline pass rims
    them in light — that rim is the only reason black ears read
    against the black canvas.
  - Arms/feet solid panda black; face panel white with the grey
    tuck seam; body the same superellipse field, now white.
  - All geometry, stamps, and effect sizes doubled for the
    128 grid (see below); motion, aura/rings/sparkles/waves/
    thought-dots/hearts/alert, blink/gaze, and all modes
    (BOOT/IDLE/LISTENING/THINKING/SPEAKING/ERROR/OFF, happy > 0)
    are preserved.
- `components/muse/muse_pixel.h`: grid `MUSE_PX_W/H` 64 → 128, plus
  new `MUSE_CANVAS_PX` 320 (see below). Interfaces unchanged:
  `muse_pixel_render`, `muse_pixel_set_size`, `muse_pixel_scale`,
  `muse_pixel_accent` signatures are the same.
- `components/muse/muse_ui.c` (layout decoupling only, no behaviour
  change on this board):
  - Main canvas uses `MUSE_CANVAS_PX` (320) instead of
    `MUSE_PX_W * 5` (which only equalled 320 while the grid was 64
    wide), including the round-short branch. On-screen avatar size
    is unchanged; only the detail is denser.
  - Read mini stays 128 screen px (was `MUSE_PX_W * MINI_CELL_PX`
    at the 64 grid); its blank-rows term now scales by
    `l->px / MUSE_PX_W`, and `s_big_y` uses
    `s_canvas_px * ART_BLANK_ROWS / MUSE_PX_W` (the old
    `ART_BLANK_ROWS * (s_canvas_px / MUSE_PX_W)` truncated a
    half-cell at 2.5 screen px per grid cell).
  - `ART_BLANK_ROWS` 3 → 5: the shadow now ends near grid row 122
    of 128, so 5 grid rows sit below the art (was 3 of 64).
  - Heard mini, `s_cells`/`s_map`/`invalidate_muse`, and the
    `> s_w` canvas clamp need no change: they already scale with
    the grid (heard mini stays 256 screen px at both grids).

### Why 128, and where the memory went

The old grid was 64×64 blown up to a ~320 px canvas (~5 screen px
per cell — the blocky look). The panda is drawn on a 128×128 grid
on the same 320 px canvas (2.5 screen px per cell), so the same
on-screen avatar renders visibly finer. The frame and mask buffers
are 16 KB each at 128 (32 KB total); internal free heap during
voice turns is only ~27 KB, so they live in PSRAM via a guarded
`.ext_ram.bss` section attribute (ESP-IDF + `CONFIG_SPIRAM`, else
internal; host builds unaffected). Build map confirms:
`.ext_ram.bss 0x8000` for `muse_pixel.c.obj`. Buffers are only
touched from task context (render + LVGL flush), never an ISR.
`muse_ui.c` `s_cells` (32 KB) was already PSRAM-backed via
`MUSE_BIG_CAPS` and needed no change.

### What the panda looks like, per pose

White round head/body, black round ears with light rims, tilted
black eye patches with bead eyes (white halo + shine), small black
nose, smile, pink blush on both cheeks, black arms/feet. Poses:
BOOT opens big round eyes; IDLE blinks and breathes; LISTENING
raises both paws beside the face, brows up, O mouth; THINKING
one paw to the chin, brows angled, thought dots, HMM mouth;
SPEAKING talks with a tongue-visible open mouth plus sound waves;
ERROR shows white X eyes, a flat mouth, and the red `!` alert;
OFF shuts the eyes and fades; happy > 0 hops with arms up,
happy closed eyes, a grin, stronger blush, and floating hearts.
State rim light still tints with the mode accent.

Two defects found and fixed during host visual review (not present
in any shipped build before tts4, since the panda is new here):
the first patch size (rx 9.6, ry 7.0, tilt 0.4) merged at the
bottom into a solid bar that swallowed the nose — reduced to
rx 8.5, ry 5.5, tilt 0.3, which keeps the patches separate; and the
nose moved from `eye_y + 3` to `eye_y + 4` so its widest row no
longer bridges the patch bottoms. After the fix the nose is
distinct from the patches on every row in every pose.

### Packages (tts3 untouched)

- `build/muse-1.43c-tts4-app.bin` — 3,084,288 bytes,
  SHA-256 `edaf27f94c5e9014c79edf852e8fdae00f99d891d26b5ff5eb4f2c203f3e1e2b`
- `build/muse-1.43c-tts4-merged.bin` — 3,215,360 bytes,
  SHA-256 `2e7cb22abc6c0b2fb084c8439df43388ec620f27d3e617d8f4325a6bd3bd95c8`
- tts3 SHAs re-verified unchanged after packaging:
  app `8bc829d65f810023844b7c9544a88ea6d89ec7030f178ee3547f20d479abeaff`,
  merged `36c321752aefee42e2597d1de3c2dcaeadbd602af97f3e3686e1b46d08924381`.

### Verification

- `./tools/board.sh waveshare-s3-143c build` → exit 0. Signed app
  3,080,192 bytes, 24% headroom under the `0x3E0000` OTA slot
  (same as tts3). The "No SDK token" build warning is unchanged
  from prior builds — the pairing token lives in NVS and survives.
- Partition table dumped from the tts4 build is identical to the
  tts3 layout (nvs `0x11000`/48K, otadata `0x1D000`/8K, phy
  `0x1F000`/4K, ota_0 `0x20000`/3968K, ota_1 `0x400000`/3968K,
  prod_data `0x7E0000`/4K, prod_bak `0x7E1000`/4K). Merged-image
  layout verified: merged[`0x20000`:] == app file, and the merged
  header region (bootloader/partition/otadata) is byte-identical
  to tts3's — only the app changed.
- Host tests: `python3 -m unittest discover -s tests` → 629 ran,
  OK (skipped=7, the documented input-missing skips), exit 0 —
  identical to the tts3 baseline.
- Host visual review: the real `muse_pixel.c` compiled on the host
  and rendered for all 8 poses (BOOT/IDLE/LISTENING/THINKING/
  SPEAKING/ERROR/OFF + happy); every pose reads as the panda
  described above (ears, separate patches, nose, mouth, blush,
  limbs, and each pose's foreground elements: waves, thought
  dots, hearts, alert). The alert and thought dots were confirmed
  present in the framebuffer by direct inspection (they classify
  near grey in ASCII dumps when the host palette is mid-blend).
- No-key self-check on both tts4 binaries: `xi-api-key` only as
  the header field name; no `workers.dev`, no Wi-Fi SSID, no
  `sk_…` key-like strings; only the bare `mgst_` placeholder (as
  in all prior builds).

### Still to verify live (human steps)

Flash route: OTA failed 4× for tts3, so use the USB route that
worked for tts3 — `erase_region 0x1D000 0x2000`, then
`write_flash 0x20000 muse-1.43c-tts4-app.bin` (NVS at `0x11000`
preserved, so pairing/Wi-Fi/token survive). Confirm by looking at
the main tile: the avatar there should be the panda above (white
face, black ears and eye patches) instead of the old character.
Because this image also carries the tts3 voice fix, the same flash
covers both: speak to the board and check for Edge (Xiaoxiao)
voice alongside the screen text, per the tts3 live steps.
