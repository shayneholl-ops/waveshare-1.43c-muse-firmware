# Waveshare ESP32-S3-Touch-AMOLED-1.43C board for muse-gadget-sdk

> **Historical design note.** This records the original board-port plan.
> The repository now includes the listed wiring edits, TTS providers,
> Studio Panda renderer and host checks. Use `README.md` for current setup
> and `TTS_NOTES.md` for the versioned build and hardware-validation status.

Authored from public sources only; nothing here was compiled (no toolchain
in this environment), and nothing here contains a secret or credential.
The SDK token is handled separately (see "Provisioning" below).

## Files in this drop

| File | Goes to in the SDK tree |
|---|---|
| `board_waveshare_s3_143c.c` | `esp32/components/muse/boards/board_waveshare_s3_143c.c` |
| `sdkconfig.muse-waveshare-s3-143c` | `esp32/devices/sdkconfig.muse-waveshare-s3-143c` |

## Required repo edits (not included as files — apply by hand)

These four small edits wire the new board into the SDK's existing
selection machinery. They mirror the 1.75C entries exactly.

### 1. `esp32/components/muse/Kconfig` — board choice

```kconfig
        config MUSE_BOARD_WAVESHARE_S3_143C
            bool "Waveshare ESP32-S3-Touch-AMOLED-1.43C"
            depends on IDF_TARGET_ESP32S3
```

And in `config MUSE_BOARD_ID`, next to the other Waveshare defaults:

```kconfig
        default "waveshare_s3_143c" if MUSE_BOARD_WAVESHARE_S3_143C
```

### 2. `esp32/components/muse/CMakeLists.txt` — board source

Add an `elseif` branch next to the other Waveshare boards (order among the
branches does not matter; the choice guarantees exactly one matches):

```cmake
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_143C)
        list(APPEND srcs "boards/board_waveshare_s3_143c.c" "boards/muse_lcd_bands.c")
```

### 3. `esp32/components/muse/idf_component.yml` — managed components

Add `"waveshare_s3_143c"` to the `esp_lvgl_adapter` rule list, and add a
rule for the panel driver (the component Waveshare's own BSP uses):

```yaml
  espressif/esp_lcd_sh8601:
    version: "^2.0.0"
    rules:
      - if: "$CONFIG{MUSE_BOARD_ID} == \"waveshare_s3_143c\""
```

`espressif/esp_codec_dev` needs no change: its rule already covers every
board (`MUSE_BOARD_ID != "none"`), and it supplies the ES8311/ES7210
drivers the board file uses. No vendor BSP component is needed: the panel,
touch and codecs are wired directly in the board file.

### 4. `esp32/tools/board.sh` — build shortcut (optional)

Add a case next to the other boards so the board builds in its own
`build-waveshare-s3-143c` directory:

```bash
  waveshare-s3-143c)
    TARGET=esp32s3
    DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-143c"
    # The S3's own USB-Serial/JTAG.
    PORTS="/dev/cu.usbmodem* /dev/ttyACM*"
    ;;
```

## Build and flash (ESP-IDF v6.0.1 only)

The SDK's README pins ESP-IDF v6.0.1 and no other version:

```bash
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6
cd ~/esp/esp-idf-v6 && ./install.sh esp32s3 && . ./export.sh
```

From the SDK's `esp32/` directory, after applying the edits above:

```bash
# With the board.sh case added:
./tools/board.sh waveshare-s3-143c flash-monitor /dev/cu.usbmodem1101

# Or directly with idf.py (same sdkconfig chain board.sh would use):
idf.py -B build-waveshare-s3-143c -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-waveshare-s3-143c/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-143c" \
  -p /dev/cu.usbmodem1101 flash monitor
```

`/dev/cu.usbmodem1101` is the board's macOS port from the user's flashing
spec (native USB-Serial/JTAG, no UART bridge). The same spec's merged-image
form still works: `idf.py merge-bin` (or `esptool.py merge_bin`) and then
`esptool.py --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 921600
write_flash 0x00 <merged.bin>`.

Do NOT flash with the StopWatch's 16 MB config: this board is 8 MB, and
the sdkconfig in this drop selects `partitions_muse_8mb.csv`, the exact
layout the lost firmware shipped (nvs 0x11000, ota_0/ota_1 3.9 MB each,
prod_data 0x7E0000, prod_bak 0x7E1000). Keeping that layout keeps OTA
compatibility with the original partitioning. Only the *contents* of
prod_data/prod_bak are lost, not the layout.

## Provisioning (later steps, not part of this board file)

- **SDK token:** a new token has already been stored via the secure entry
  (vault connector, host hatch.metaaivm.com). Apply it at build time in
  menuconfig (ESP32 Device SDK > Muse Gadgets SDK token) or after flashing
  over serial with `>sdk_token=` (the firmware answers `@sdk_token ok`
  and restarts with it). Never paste the token into source, sdkconfig, or
  chat. The old `mgst_` device token and VM binding are unrecoverable.
- **Pairing:** in the Muse app, turn on Settings > Devices > Developer
  mode, then Settings > Devices > Add Device. The device shows up as
  `MuseGadget-XXXXXX` (the old `MuseGadget-0E2EC8` name is not guaranteed
  to return). Re-enter the Wi-Fi credentials through the normal setup
  flow; the wiped NVS credentials are not recoverable.
- **First pairing confirms the token:** until then the token is stored
  but unverified.

## Deliberate design decisions

- **Direct driver wiring, not a vendor BSP.** No esp-bsp component exists
  for the 1.43C, so unlike `board_waveshare_s3_175c.c` (which delegates to
  `bsp_display_new()`/`bsp_touch_new()`), this file drives the SH8601
  panel, the CST820 touch and the ES8311/ES7210 codecs directly, using
  Waveshare's official BSP as the pin/init reference and the SDK's
  `board_vn_s3_183.c` as the codec-wiring reference.
- **Panel driver is SH8601, not CO5300.** Waveshare's 1.43C BSP drives the
  panel with `esp_lcd_sh8601` (QSPI, init commands copied verbatim). The
  lost firmware's logs said CO5300; the two are compatible, and the
  factory firmware on the board now also uses SH8601. If the panel stays
  dark, the CO5300 driver (`espressif/esp_lcd_co5300`, already a rule for
  other boards) is the first fallback — same QSPI wiring.
- **Touch is raw I2C + an LVGL indev callback, not esp_lcd_touch.** No
  CST820 esp_lcd_touch driver is wired in the SDK, so the file mirrors
  Waveshare's BSP read sequence (register 0x02 count/event, 0x03
  coordinates, both axes mirrored) instead of inventing a driver binding.
- **Battery is ADC + STAT, not a PMU.** There is no AXP2101/M5PM1 on this
  board, so `muse_pmu.c` (AXP2101-only) is deliberately unused.
  `read_power()` implements the official BSP's math: ADC1 channel 3
  (GPIO4), 12-bit, 12 dB attenuation, curve-fitting calibration, reading
  x2 (1/2 divider); STAT on GPIO7, LOW = charging. Percent is a
  resting-voltage curve (3.0 V empty to 4.2 V full), an estimate, not a
  fuel gauge.
- **USB presence is inferred, not sensed.** The S3's USB-Serial/JTAG block
  has no VBUS register, so `usb = charging` (STAT low means a charger is
  attached). This is stated in the code comment, not presented as a
  direct sense.
- **One button, no software power-off.** BOOT (GPIO0) talks; PWR is a
  physical power control, not a GPIO. `power_off` and `audio_power` are
  NULL on purpose: the amp enable is owned by the ES8311 driver through
  `pa_pin`, and no software power-off path is known.
- **CJK font is on.** The board's replies are Chinese, so
  `CONFIG_MUSE_CJK_FONT=y` is set (~850 KB flash; the 3.9 MB OTA slots
  still fit). Drop it if the build ever overflows a slot.
- **Light-sleep block copied from the StickS3 config.** Same SoC, same
  8 MB flash/PSRAM, no 32 kHz crystal; wake source is the BOOT button via
  `wait_buttons()`.

## Still to verify on real hardware (in rough order)

1. Panel lights with the SH8601 init; if not, try the CO5300 driver.
2. Image alignment: the gap is set to (6, 0) in both gap calls to match
   the init window's column-6 start, but the official BSP calls
   `set_gap(8, 0)`. If the image sits 2 px off, switch both to 8.
3. Touch: tap coordinates map correctly (mirroring), and the event filter
   (`event != 0x01`) behaves; the INT line (GPIO17) is wired but unused.
4. Audio: speaker out and both mics in; `mic_slot = -1` mixes both mics
   (as on the 1.75C); ES7210 gain steps (3 dB snap, 33 -> 34.5 dB).
5. Battery: ADC millivolts x2 against a meter; STAT polarity (LOW =
   charging); the < 500 mV no-battery heuristic with the cell unplugged.
6. Codec supply: LDO enable (GPIO18) polarity — if the codecs do not
   probe, flip `codec_power()` first.
7. Amp rail: confirm the NS4150B supply on the schematic and adjust
   `hw_gain.pa_voltage` (currently assumed 3.3 V) if it runs from VBAT.
8. Display bands: no half-drawn frames (the SPI bus must be set up from
   the UI-core task, per `muse_lcd_bands.h`).
9. BOOT hint position on screen (currently borrowed from the 1.75C rim
   layout) against the physical BOOT location.
10. Flash mode: dio/80 MHz per the flashing spec (the flash chip is
    QIO-capable; do not switch without a hardware check).
11. Idle current on battery: the CST820 is left scanning (no sleep
    command is in the official BSP); add one if idle draw is too high.
12. Size: the build must fit the 3.9 MB OTA slots with the CJK font on.

## Out of scope for this drop (later steps)

- Compiling and flashing (no toolchain here; first build happens on a
  machine with ESP-IDF v6.0.1).
- TTS provider wiring for Chinese voice replies: the playback path
  (16 kHz audio, MP3 decode, the `start_tts` hook in
  `muse_chat_session.cpp`) is public, but the TTS API call itself must
  be supplied — wire a Chinese-capable TTS API into `start_tts` and feed
  the returned MP3 to `tts_data()`. Voice input (mic -> server
  transcription -> reply text) needs no extra work.
- SDK token application and in-app pairing (see "Provisioning").
- Web-flasher / CI entries (`web-flasher/boards.json`,
  `.github/workflows/esp32.yml` matrix) for the 1.43C.
- Any StopWatch/1.43/1.43-B/1.75C pin, peripheral or config reuse beyond
  the structural patterns noted above.
