# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Fast host checks for the standalone Waveshare overlay."""

from pathlib import Path
import os
import re
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "esp32" / "components" / "muse"
AVATAR = ROOT / "esp32" / "avatar" / "muse_pixel.c"
AVATAR_HEADER = MUSE / "muse_pixel.h"
MUSE_UI = MUSE / "muse_ui.c"


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return result


class EdgeTtsTests(unittest.TestCase):
    def test_drm_token_matches_edge_tts_731_golden_vector(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            harness = tmp / "drm.c"
            harness.write_text(
                textwrap.dedent(
                    r"""
                    #include <assert.h>
                    #include <string.h>
                    #include "muse_edge_tts_drm.h"

                    int main(void)
                    {
                        char token[82];
                        assert(muse_edge_tts_token(token, sizeof(token),
                                                   1791888000LL, 0));
                        assert(strcmp(
                            token,
                            "492D7694A183A45E-"
                            "56E821F95D4579C5B7C7E33DC7303FF"
                            "379EC36D510213555BBBB9EB17EF15876") == 0);
                        return 0;
                    }
                    """
                )
            )
            binary = tmp / "drm"
            run(
                [
                    os.environ.get("CC", "cc"),
                    "-std=c11",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(MUSE),
                    str(harness),
                    str(MUSE / "muse_edge_tts_drm.c"),
                    "-lm",
                    "-o",
                    str(binary),
                ]
            )
            run([str(binary)])

    def test_sntp_failure_still_reaches_edge_date_retry(self):
        source = (MUSE / "muse_edge_tts.c").read_text()
        match = re.search(
            r"static bool fetch_audio_with_clock_fallback\(.*?\n\}\n",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(
            match,
            "missing host-testable Edge Date fallback orchestration",
        )

        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            harness = tmp / "retry.c"
            harness.write_text(
                textwrap.dedent(
                    r"""
                    #include <assert.h>
                    #include <stdbool.h>
                    #include <stdint.h>

                    typedef struct { int unused; } fetch_arg_t;
                    static const char *TAG = "test";
                    #define ESP_LOGW(tag, ...) ((void)(tag))

                    static int scenario;
                    static int calls;
                    static bool fetch_once(fetch_arg_t *arg, int64_t deadline_us,
                                           bool *got_audio, bool *retry)
                    {
                        (void)arg;
                        (void)deadline_us;
                        calls++;
                        *got_audio = false;
                        *retry = false;
                        if (scenario == 0 && calls == 1) {
                            *retry = true;
                        } else if (scenario == 0 || scenario == 2) {
                            *got_audio = true;
                        }
                        return *got_audio;
                    }
                    """
                )
                + match.group(0)
                + textwrap.dedent(
                    r"""
                    int main(void)
                    {
                        fetch_arg_t arg = {0};

                        scenario = 0;
                        calls = 0;
                        assert(fetch_audio_with_clock_fallback(&arg, 1, false));
                        assert(calls == 2);

                        scenario = 1;
                        calls = 0;
                        assert(!fetch_audio_with_clock_fallback(&arg, 1, false));
                        assert(calls == 1);

                        scenario = 2;
                        calls = 0;
                        assert(fetch_audio_with_clock_fallback(&arg, 1, true));
                        assert(calls == 1);
                        return 0;
                    }
                    """
                )
            )
            binary = tmp / "retry"
            run(
                [
                    os.environ.get("CC", "cc"),
                    "-std=c11",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(harness),
                    "-o",
                    str(binary),
                ]
            )
            run([str(binary)])


class AvatarTests(unittest.TestCase):
    def test_studio_panda_renders_every_state_with_clean_contrast(self):
        source = AVATAR.read_text()
        self.assertIn("Studio Panda", source)
        self.assertNotIn("Signal Dial", source)
        self.assertIn("#define MUSE_PX_W 256", AVATAR_HEADER.read_text())
        self.assertIn("static uint16_t s_map[MAP_MAX];", source)
        ui = MUSE_UI.read_text()
        self.assertIn("l->px = 256;", ui)
        self.assertIn("l->px = 128;", ui)
        self.assertNotIn("MINI_CELL_PX", ui)
        self.assertIn("s_canvas_px = s_w;", ui)

        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "muse_state.h").write_text(
                textwrap.dedent(
                    """
                    #pragma once
                    typedef enum {
                        MUSE_MODE_BOOT,
                        MUSE_MODE_IDLE,
                        MUSE_MODE_LISTENING,
                        MUSE_MODE_THINKING,
                        MUSE_MODE_SPEAKING,
                        MUSE_MODE_ERROR,
                        MUSE_MODE_OFF,
                        MUSE_MODE_COUNT
                    } muse_mode_t;
                    """
                )
            )
            harness = tmp / "avatar.c"
            harness.write_text(
                textwrap.dedent(
                    r"""
                    #include <assert.h>
                    #include <stdint.h>
                    #include <stdlib.h>
                    #include "muse_pixel.h"

                    #define RGB565(c) \
                        ((((c) >> 19 & 31) << 11) | \
                         (((c) >> 10 & 63) << 5) | ((c) >> 3 & 31))

                    static uint16_t frame[MUSE_CANVAS_PX * MUSE_CANVAS_PX];

                    static uint64_t hash_frame(void)
                    {
                        uint64_t h = 1469598103934665603ULL;
                        for (size_t i = 0; i < sizeof(frame) / sizeof(frame[0]); i++) {
                            h ^= frame[i];
                            h *= 1099511628211ULL;
                        }
                        return h;
                    }

                    static uint64_t render(muse_mode_t mode, float t, float level,
                                           float happy)
                    {
                        muse_pose_t pose = {
                            .mode = mode,
                            .t = t,
                            .mode_t = 1.0f,
                            .level = level,
                            .happy = happy,
                        };
                        muse_pixel_render(&pose);
                        muse_pixel_set_size(MUSE_CANVAS_PX);
                        muse_pixel_scale(frame, MUSE_CANVAS_PX, 0,
                                         MUSE_CANVAS_PX - 1, 0,
                                         MUSE_CANVAS_PX - 1);
                        return hash_frame();
                    }

                    int main(void)
                    {
                        uint64_t hashes[MUSE_MODE_COUNT + 1];
                        for (int mode = 0; mode < MUSE_MODE_COUNT; mode++) {
                            hashes[mode] = render((muse_mode_t)mode,
                                                  10.0f + mode, 0.72f, 0);
                            for (int prior = 0; prior < mode; prior++) {
                                assert(hashes[mode] != hashes[prior]);
                            }
                        }
                        hashes[MUSE_MODE_COUNT] =
                            render(MUSE_MODE_IDLE, 30.0f, 0, 1.0f);
                        assert(hashes[MUSE_MODE_COUNT] !=
                               hashes[MUSE_MODE_IDLE]);

                        render(MUSE_MODE_IDLE, 40.0f, 0, 0);
                        const uint16_t colors[] = {
                            RGB565(0xf8f8f2), RGB565(0x17181d),
                            RGB565(0xff8fa3),
                        };
                        for (size_t c = 0;
                             c < sizeof(colors) / sizeof(colors[0]); c++) {
                            size_t count = 0;
                            for (size_t i = 0;
                                 i < sizeof(frame) / sizeof(frame[0]); i++) {
                                count += frame[i] == colors[c];
                            }
                            assert(count >= 80);
                        }
                        return 0;
                    }
                    """
                )
            )
            binary = tmp / "avatar"
            run(
                [
                    os.environ.get("CC", "cc"),
                    "-std=c11",
                    "-O1",
                    "-g",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(tmp),
                    "-I",
                    str(MUSE),
                    str(harness),
                    str(AVATAR),
                    "-lm",
                    "-o",
                    str(binary),
                ]
            )
            run([str(binary)])


if __name__ == "__main__":
    unittest.main()
