# Waveshare ESP32-S3-Touch-AMOLED-1.43C Muse firmware

A port of the public [muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
(base commit `c9354b7`, ESP-IDF v6.0.1 only) for the Waveshare
ESP32-S3-Touch-AMOLED-1.43C board, with a panda avatar and Edge +
ElevenLabs text-to-speech. The current source has a 256×256 Studio Panda
renderer and a resilient Edge clock fallback; an ESP-IDF build and physical
audio check are still required before flashing it.

## Hardware

- ESP32-S3-PICO-1-N8R8, 8 MB flash + 8 MB PSRAM
- 1.43" 466×466 AMOLED, SH8601 QSPI (D0–D3 GPIO9–12, CS15, SCK14, RST13, TE8)
- CST820 touch @0x15 (SDA47/SCL48, RST16; INT17 wired, polled)
- ES8311 out + ES7210 dual-mic in + NS4150B amp (MCLK38/SCLK39/LRCK40/DSDIN41/DSOUT42, amp46, LDO18)
- Battery: ADC1 ch3/GPIO4, 12-bit, 12 dB, reading ×2 (200k/200k divider);
  charging = GPIO7 LOW; LED GPIO5; BOOT GPIO0
- No IMU/RTC/TF card. Partition layout: `partitions_muse_8mb.csv`
  (nvs 0x11000, ota_0 0x20000, ota_1 0x400000, prod_data 0x7E0000,
  prod_bak 0x7E1000) — 3.9 MB OTA slots.

## What is in this repo

`esp32/` mirrors the SDK tree — copy these files over a checkout of the
SDK at the base commit above. `docs/` holds the design notes.

**New files (written for this port):**

- `esp32/components/muse/boards/board_waveshare_s3_143c.c` — the board
  file: panel/touch/codec wiring, battery sensing, gap 8, CST820
  hibernate (`0xA5 <- 0x03`, resume must reset touch), touch clamp,
  BOOT hint bottom-center
- `esp32/components/muse/muse_edge_tts.{c,h}` + `muse_edge_tts_drm.{c,h}`
  — Microsoft Edge TTS (voice `zh-CN-XiaoxiaoNeural`), stream buffer and
  fetch-task stack in PSRAM
- `esp32/components/muse/muse_elevenlabs_tts.{c,h}` — direct ElevenLabs
  client (`eleven_flash_v2_5`, default voice Rachel), key from NVS only
- `esp32/components/muse/muse_tts_quota.{c,h}` — monthly quota routing:
  ElevenLabs first while quota lasts, Edge on exhaustion/failure;
  no key = Edge only
- `esp32/devices/sdkconfig.muse-waveshare-s3-143c` — board sdkconfig
  (CJK font on, 8 MB partitions)

**Modified upstream files:**

- `esp32/avatar/muse_pixel.c`, `esp32/components/muse/muse_pixel.h` —
  Studio Panda avatar: procedural 256×256 render grid, same 320 px
  on-screen size, 64 KB frame buffer in PSRAM; all eight poses preserved
- `esp32/components/muse/muse_ui.c` — avatar and reply layouts use fixed
  screen sizes independent of render-grid resolution
- `esp32/components/muse/muse_chat_session.cpp` — TTS routing hooks
- `esp32/components/muse/boards/muse_lcd_bands.c` — `wait_band` freeze fix
- `esp32/components/muse/{CMakeLists.txt,Kconfig,idf_component.yml}` —
  board wiring (choice, sources, SH8601 rule)
- `esp32/main/muse_glue.c`, `esp32/tools/board.sh` — glue + build shortcut
- `esp32/tools/muse/{anim.c,preview.c}` — host previews rendered at the
  real 320 px avatar canvas rather than a grid-derived size

`docs/BUILD_NOTES.md` is the original design note; a few of its TODOs
(gap 8, CST820 sleep, touch clamp) are already resolved in the as-built
board file. `docs/TTS_NOTES.md` preserves the tts2/tts3/tts4 build record
and describes the newer, unbuilt tts5 source candidate.
`docs/WUPSBR_COMPARISON.md` compares this port against the community
wupsbr port (reference only — not flashed wholesale).

## Build (ESP-IDF v6.0.1 only)

```bash
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6
cd ~/esp/esp-idf-v6 && ./install.sh esp32s3 && . ./export.sh
```

From the SDK's `esp32/` directory, after copying this repo's `esp32/`
files in:

```bash
./tools/board.sh waveshare-s3-143c flash-monitor /dev/cu.usbmodem1101
# or directly:
idf.py -B build-waveshare-s3-143c -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-waveshare-s3-143c/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-143c" \
  -p /dev/cu.usbmodem1101 flash monitor
```

## Host checks

The overlay has dependency-free host checks for the Edge token and retry
orchestration, plus sanitizer-backed rendering of all panda states:

```bash
python3 -m unittest discover -s tests -p 'test_*.py'
```

## Flash an app-only image (USB)

App-only writes preserve NVS (Wi-Fi, pairing, token, ElevenLabs key).
After a previous OTA the active slot is `ota_1` (0x400000), so a bare
write to `ota_0` (0x20000) will NOT boot unless `otadata` is erased
first, making the bootloader fall back to `ota_0`:

```bash
esptool.py --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 921600 \
  erase_region 0x1D000 0x2000
esptool.py --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 921600 \
  write_flash 0x20000 <app.bin>
```

A merged image written at 0x00 WIPES NVS — only for a factory reset.

## Provisioning (serial console, never in source)

The serial console ignores `\r`; only `\n` completes a line (in GNU
screen, finish with Ctrl-J, not Enter). No echo.

- `>sdk_token=<token>` — the device SDK token (max 96 chars)
- `>el_key=<key>` / `>el_key=` (clear) / `>el_key` (query → `@el_key set`
  or `@el_key (unset)`; the value is never printed or logged)

No token, key, SSID, or password appears anywhere in this repo — that is
deliberate and grep-verified. Tokens and keys go through the owner's own
serial console into board NVS only.

## Hardware validation still needed

The previously flashed tts4 build displayed replies but produced no sound.
The current source has not yet been built or flashed, so the following
changes are host-verified only.

**Addressed in current source:**

- Internal-heap exhaustion: the Edge stream buffer (32 KB) and fetch-task
  stack were moved to PSRAM in tts3; the board's free internal heap
  during a reply was only ~27 KB.
- An SNTP timeout no longer ends speech immediately. The client now attempts
  the Edge handshake, derives clock skew from a 403 `Date` header, and retries
  once. The retry orchestration and edge-tts 7.3.1 token vector are host-tested.

**Physical checks, in order:**

1. Query status and ensure the NVS speaker setting is on; `>speaker=1`
   and `>volume=80` set a known audible state.
2. Send the raw serial character `m` to play the embedded MP3 through
   the same decoder and speaker path. If this is silent, test the ES8311
   DAC, NS4150B amp and GPIO46 before changing TTS.
3. Trigger one reply and capture `edge_tts:` logs. Distinguish SNTP,
   TLS/DNS, HTTP 403 and no-audio failures rather than treating every
   text-only reply as the same bug.
4. Confirm `response` text, decoded MP3 and speaker output together on
   the physical board before publishing a new binary.

**Previous reference only:** the tts4 app image was 3,084,288 bytes,
SHA-256
`edaf27f94c5e9014c79edf852e8fdae00f99d891d26b5ff5eb4f2c203f3e1e2b`
and does not contain the 256×256 avatar or Edge clock fallback. No binary
for the current source is committed to this repository.

## License

Apache-2.0, same as the upstream muse-gadget-sdk (see `LICENSE`).
Derived files carry the same license; the upstream project is credited above.
