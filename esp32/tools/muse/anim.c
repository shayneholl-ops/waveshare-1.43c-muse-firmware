/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

/* Host animation dump: plays each Muse animation through the real renderer at
 * the device frame rate and writes PPM frames (<dir>/<name>/NNN.ppm).
 * tools/muse/make_gifs.py turns those frames into one GIF per state.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#include "muse_pixel.h"

#define N MUSE_CANVAS_PX
#define DT 0.04f
#define WARMUP 1.5f

static uint16_t s_buf[N * N];

typedef struct {
    const char *name;
    muse_mode_t mode;
    float seconds;
    float start_mode_t;
    bool talky;
    bool pet;
} anim_t;

static const anim_t ANIMS[] = {
    {"boot", MUSE_MODE_BOOT, 3.0f, 0},
    {"idle", MUSE_MODE_IDLE, 6.0f, 5},
    {"listening", MUSE_MODE_LISTENING, 4.0f, 0, .talky = true},
    {"thinking", MUSE_MODE_THINKING, 4.0f, 0},
    {"speaking", MUSE_MODE_SPEAKING, 4.0f, 0, .talky = true},
    {"happy", MUSE_MODE_IDLE, 3.0f, 9, .pet = true},
    {"off", MUSE_MODE_OFF, 2.0f, 0},
    {"error", MUSE_MODE_ERROR, 3.0f, 0},
};

static float speech_level(float t)
{
    float syllable =
        fabsf(sinf(t * 6.3f)) * (0.55f + 0.45f * sinf(t * 1.7f + 1.0f));
    float gap = sinf(t * 0.9f) > -0.6f ? 1.0f : 0.15f;
    float value = syllable * gap;
    return value < 0 ? 0 : (value > 1 ? 1 : value);
}

static float happiness(float since_pet)
{
    float left = 1.6f - since_pet;
    if (since_pet < 0 || left <= 0) {
        return 0;
    }
    return left > 0.4f ? 1.0f : left / 0.4f;
}

static void save(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        return;
    }
    fprintf(file, "P6 %d %d 255\n", N, N);
    for (int i = 0; i < N * N; i++) {
        uint16_t color = s_buf[i];
        uint8_t rgb[3] = {
            (uint8_t)(((color >> 11) & 31) * 255 / 31),
            (uint8_t)(((color >> 5) & 63) * 255 / 63),
            (uint8_t)((color & 31) * 255 / 31),
        };
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}

int main(int argc, char **argv)
{
    const char *directory = argc > 1 ? argv[1] : ".";
    float base = 10.0f;
    muse_pixel_set_size(N);

    for (size_t index = 0; index < sizeof(ANIMS) / sizeof(ANIMS[0]);
         index++) {
        const anim_t *animation = &ANIMS[index];
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", directory, animation->name);
        mkdir(path, 0755);

        int frames = (int)(animation->seconds / DT + 0.5f);
        int warmup_frames = (int)(WARMUP / DT + 0.5f);
        for (int frame = -warmup_frames; frame < frames; frame++) {
            float elapsed = frame * DT;
            muse_pose_t pose = {
                .mode = animation->mode,
                .t = base + elapsed,
                .mode_t = animation->start_mode_t + elapsed,
                .level = animation->talky ? speech_level(elapsed) : 0,
                .happy = animation->pet ? happiness(elapsed - 0.5f) : 0,
            };
            if (pose.mode_t < 0) {
                pose.mode_t = 0;
            }
            muse_pixel_render(&pose);
            if (frame >= 0) {
                muse_pixel_scale(s_buf, N, 0, N - 1, 0, N - 1);
                snprintf(path, sizeof(path), "%s/%s/%03d.ppm", directory,
                         animation->name, frame);
                save(path);
            }
        }
        base += 100.0f;
    }
    return 0;
}
