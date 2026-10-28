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

/* Host preview: renders one settled frame for every Muse state at the same
 * MUSE_CANVAS_PX size used by the device UI.
 */
#include <stdint.h>
#include <stdio.h>

#include "muse_pixel.h"

#define N MUSE_CANVAS_PX

static uint16_t s_buf[N * N];

static void save(const char *directory, const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/muse_%s.ppm", directory, name);
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

static void shot(const char *directory, const char *name, muse_pose_t pose)
{
    float end = pose.t;
    for (float t = end - 1.5f; t <= end; t += 0.04f) {
        muse_pose_t frame = pose;
        frame.t = t;
        frame.mode_t = pose.mode_t - (end - t);
        if (frame.mode_t < 0) {
            frame.mode_t = 0;
        }
        muse_pixel_render(&frame);
    }
    muse_pixel_set_size(N);
    muse_pixel_scale(s_buf, N, 0, N - 1, 0, N - 1);
    save(directory, name);
}

int main(int argc, char **argv)
{
    const char *directory = argc > 1 ? argv[1] : ".";
    shot(directory, "boot",
         (muse_pose_t){
             .mode = MUSE_MODE_BOOT,
             .t = 10.7f,
             .mode_t = 0.7f,
         });
    shot(directory, "idle",
         (muse_pose_t){
             .mode = MUSE_MODE_IDLE,
             .t = 20.3f,
             .mode_t = 5,
         });
    shot(directory, "listening",
         (muse_pose_t){
             .mode = MUSE_MODE_LISTENING,
             .t = 30.1f,
             .mode_t = 2,
             .level = 0.7f,
         });
    shot(directory, "thinking",
         (muse_pose_t){
             .mode = MUSE_MODE_THINKING,
             .t = 40.2f,
             .mode_t = 1,
         });
    shot(directory, "speaking",
         (muse_pose_t){
             .mode = MUSE_MODE_SPEAKING,
             .t = 50.25f,
             .mode_t = 1,
             .level = 0.6f,
         });
    shot(directory, "happy",
         (muse_pose_t){
             .mode = MUSE_MODE_IDLE,
             .t = 60.1f,
             .mode_t = 9,
             .happy = 1,
         });
    shot(directory, "off",
         (muse_pose_t){
             .mode = MUSE_MODE_OFF,
             .t = 90.0f,
             .mode_t = 0.6f,
         });
    shot(directory, "error",
         (muse_pose_t){
             .mode = MUSE_MODE_ERROR,
             .t = 80.0f,
             .mode_t = 2,
         });
    return 0;
}
