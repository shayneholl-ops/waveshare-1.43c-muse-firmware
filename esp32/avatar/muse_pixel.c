// Copyright (c) Meta Platforms, Inc. and affiliates.

/*
 * Studio Panda is a clean chibi panda built for the round AMOLED: a large
 * softly shaded head, compact body, small rounded ears and limbs, separated
 * eye patches, warm muzzle and restrained pink cheeks. The panda stays black
 * and white while the surrounding light carries each device state, keeping
 * the character recognizable in every animation without becoming a grey blob.
 */

#include "muse_pixel.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define W MUSE_PX_W
#define H MUSE_PX_H
#define TAU 6.283185307f
#define Q 12
#define ONE (1 << Q)
#define QF(value) ((int32_t)((value) * ONE))
#define LOGICAL_SIZE 128
#define RENDER_SCALE (W / LOGICAL_SIZE)

_Static_assert(W == H, "avatar grid must be square");
_Static_assert(W % LOGICAL_SIZE == 0, "avatar grid must scale 128 evenly");

enum {
    C_BG = 0,
    C_OUT,
    C_BLACK,
    C_BLACK_L,
    C_FUR_D,
    C_FUR,
    C_FUR_L,
    C_MUZZLE,
    C_EYE,
    C_PUPIL,
    C_SHINE,
    C_CHEEK,
    C_TONGUE,
    C_ACC_D,
    C_ACC,
    C_ACC_L,
    C_AURA,
    C_AURA_L,
    C_ERROR,
    C_HEART,
    C_SHADOW,
    C_COUNT,
};

typedef struct {
    float r;
    float g;
    float b;
} rgb_t;

typedef struct {
    uint32_t dark;
    uint32_t accent;
    uint32_t light;
} scheme_t;

static const scheme_t SCHEMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = {0x4258a8, 0xa9c0ff, 0xf3f6ff},
    [MUSE_MODE_IDLE] = {0x5533a8, 0xa77dff, 0xeee6ff},
    [MUSE_MODE_LISTENING] = {0x1466a8, 0x5cb8ff, 0xdff7ff},
    [MUSE_MODE_THINKING] = {0x8526a8, 0xe07bff, 0xffe8ff},
    [MUSE_MODE_SPEAKING] = {0x08765d, 0x6ff0bf, 0xe6fff6},
    [MUSE_MODE_ERROR] = {0x7a1830, 0xff5c5c, 0xffe0e0},
    [MUSE_MODE_OFF] = {0x282350, 0x7c72d0, 0xd8d4ff},
};

static const uint32_t FIXED[C_COUNT] = {
    [C_BG] = 0x000000,
    [C_OUT] = 0x08090d,
    [C_BLACK] = 0x17181d,
    [C_BLACK_L] = 0x343741,
    [C_FUR_D] = 0xd5d7d5,
    [C_FUR] = 0xf8f8f2,
    [C_FUR_L] = 0xffffff,
    [C_MUZZLE] = 0xfff3dc,
    [C_EYE] = 0xffffff,
    [C_PUPIL] = 0x090a0d,
    [C_SHINE] = 0xffffff,
    [C_CHEEK] = 0xff8fa3,
    [C_TONGUE] = 0xef617c,
    [C_ERROR] = 0xff4155,
    [C_HEART] = 0xff4f8b,
    [C_SHADOW] = 0x12141c,
};

static rgb_t s_accent[3];
static bool s_accent_init;
static uint16_t s_pal[C_COUNT];
static uint16_t s_pal_dim[C_COUNT];

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#ifdef CONFIG_SPIRAM
#define PIXEL_FB_ATTR __attribute__((section(".ext_ram.bss")))
#else
#define PIXEL_FB_ATTR
#endif
#else
#define PIXEL_FB_ATTR
#endif

static PIXEL_FB_ATTR uint8_t s_fb[W * H];

static const uint8_t BAYER4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static inline float clampf(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

static inline float fracf(float value)
{
    return value - floorf(value);
}

static inline int iround(float value)
{
    return (int)floorf(value + 0.5f);
}

static inline rgb_t hex_rgb(uint32_t color)
{
    return (rgb_t){
        (float)((color >> 16) & 0xff),
        (float)((color >> 8) & 0xff),
        (float)(color & 0xff),
    };
}

static inline rgb_t mix(rgb_t a, rgb_t b, float amount)
{
    return (rgb_t){
        a.r + (b.r - a.r) * amount,
        a.g + (b.g - a.g) * amount,
        a.b + (b.b - a.b) * amount,
    };
}

static inline rgb_t scale_rgb(rgb_t color, float amount)
{
    return (rgb_t){
        color.r * amount,
        color.g * amount,
        color.b * amount,
    };
}

static inline uint16_t to565(rgb_t color)
{
    int r = (int)(color.r + 0.5f);
    int g = (int)(color.g + 0.5f);
    int b = (int)(color.b + 0.5f);
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    if (mode < 0 || mode >= MUSE_MODE_COUNT) {
        mode = MUSE_MODE_IDLE;
    }
    return SCHEMES[mode].accent;
}

static void update_palette(muse_mode_t mode, float dt, float fade)
{
    const scheme_t *target = &SCHEMES[mode];
    rgb_t next[3] = {
        hex_rgb(target->dark),
        hex_rgb(target->accent),
        hex_rgb(target->light),
    };
    float amount = s_accent_init ? 1.0f - expf(-dt * 7.0f) : 1.0f;
    for (int i = 0; i < 3; i++) {
        s_accent[i] = mix(s_accent[i], next[i], amount);
    }
    s_accent_init = true;

    rgb_t palette[C_COUNT];
    float fixed_fade = 0.18f + 0.82f * fade;
    for (int i = 0; i < C_COUNT; i++) {
        palette[i] = scale_rgb(hex_rgb(FIXED[i]), fixed_fade);
    }
    palette[C_BG] = hex_rgb(FIXED[C_BG]);
    palette[C_ACC_D] = scale_rgb(s_accent[0], fade);
    palette[C_ACC] = scale_rgb(s_accent[1], fade);
    palette[C_ACC_L] = scale_rgb(s_accent[2], fade);
    palette[C_AURA] = scale_rgb(s_accent[0], 0.31f * fade);
    palette[C_AURA_L] = scale_rgb(s_accent[1], 0.58f * fade);

    for (int i = 0; i < C_COUNT; i++) {
        s_pal[i] = to565(palette[i]);
        s_pal_dim[i] = to565(scale_rgb(palette[i], 0.72f));
    }
}

static inline void raw_px(int x, int y, uint8_t color)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_fb[y * W + x] = color;
    }
}

/* Direct detail marks use the original 128-unit design grid. Curves below
 * rasterize at the full render resolution instead of expanding old pixels. */
static inline void px(int x, int y, uint8_t color)
{
    int left = x * RENDER_SCALE;
    int top = y * RENDER_SCALE;
    for (int row = 0; row < RENDER_SCALE; row++) {
        for (int column = 0; column < RENDER_SCALE; column++) {
            raw_px(left + column, top + row, color);
        }
    }
}

static void line(int x0, int y0, int x1, int y1, uint8_t color)
{
    x0 *= RENDER_SCALE;
    y0 *= RENDER_SCALE;
    x1 *= RENDER_SCALE;
    y1 *= RENDER_SCALE;
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        raw_px(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            return;
        }
        int twice = err * 2;
        if (twice >= dy) {
            err += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void thick_line(int x0, int y0, int x1, int y1, uint8_t color)
{
    line(x0, y0, x1, y1, color);
    if (abs(x1 - x0) >= abs(y1 - y0)) {
        line(x0, y0 + 1, x1, y1 + 1, color);
    } else {
        line(x0 + 1, y0, x1 + 1, y1, color);
    }
}

static void disc(int cx, int cy, int radius, uint8_t color)
{
    cx *= RENDER_SCALE;
    cy *= RENDER_SCALE;
    radius *= RENDER_SCALE;
    int radius2 = radius * radius;
    for (int y = -radius; y <= radius; y++) {
        int y2 = y * y;
        for (int x = -radius; x <= radius; x++) {
            if (x * x + y2 <= radius2) {
                raw_px(cx + x, cy + y, color);
            }
        }
    }
}

typedef struct {
    int32_t cx;
    int32_t cy;
    int32_t c;
    int32_t s;
    int32_t inner_rx;
    int32_t inner_ry;
    int32_t outer_rx;
    int32_t outer_ry;
    int bound;
} ellipse_q_t;

static int32_t ellipse_field(int32_t dx, int32_t dy, int32_t c, int32_t s,
                             int32_t inv_rx, int32_t inv_ry,
                             int32_t *u_out, int32_t *v_out)
{
    int32_t u = (int32_t)(((int64_t)dx * c + (int64_t)dy * s) >> Q);
    int32_t v = (int32_t)(((int64_t)dy * c - (int64_t)dx * s) >> Q);
    int32_t nu = (int32_t)(((int64_t)u * inv_rx) >> Q);
    int32_t nv = (int32_t)(((int64_t)v * inv_ry) >> Q);
    if (u_out) {
        *u_out = nu;
    }
    if (v_out) {
        *v_out = nv;
    }
    return (int32_t)(((int64_t)nu * nu + (int64_t)nv * nv) >> Q);
}

static uint8_t tone_for(int32_t u, int32_t v, int x, int y,
                        uint8_t shadow, uint8_t base, uint8_t light)
{
    int32_t lighting = -u - v;
    int threshold = BAYER4[y & 3][x & 3];
    if (lighting > QF(0.78f) ||
        (lighting > QF(0.52f) && threshold < 6)) {
        return light;
    }
    if (lighting < -QF(0.78f) ||
        (lighting < -QF(0.52f) && threshold > 9)) {
        return shadow;
    }
    return base;
}

static void ellipse(float cx, float cy, float rx, float ry, float angle,
                    float outline_px, uint8_t outline, uint8_t shadow,
                    uint8_t base, uint8_t light)
{
    cx *= RENDER_SCALE;
    cy *= RENDER_SCALE;
    rx *= RENDER_SCALE;
    ry *= RENDER_SCALE;
    outline_px *= RENDER_SCALE;
    rx = rx < 1.0f ? 1.0f : rx;
    ry = ry < 1.0f ? 1.0f : ry;
    ellipse_q_t shape = {
        .cx = QF(cx),
        .cy = QF(cy),
        .c = QF(cosf(angle)),
        .s = QF(sinf(angle)),
        .inner_rx = QF(1.0f / rx),
        .inner_ry = QF(1.0f / ry),
        .outer_rx = QF(1.0f / (rx + outline_px)),
        .outer_ry = QF(1.0f / (ry + outline_px)),
        .bound = iround(rx + ry + outline_px + 2.0f),
    };
    int center_x = iround(cx);
    int center_y = iround(cy);
    for (int y = center_y - shape.bound; y <= center_y + shape.bound; y++) {
        int32_t dy = y * ONE + ONE / 2 - shape.cy;
        for (int x = center_x - shape.bound; x <= center_x + shape.bound;
             x++) {
            int32_t dx = x * ONE + ONE / 2 - shape.cx;
            if (ellipse_field(dx, dy, shape.c, shape.s, shape.outer_rx,
                              shape.outer_ry, NULL, NULL) > ONE) {
                continue;
            }
            int32_t u;
            int32_t v;
            if (ellipse_field(dx, dy, shape.c, shape.s, shape.inner_rx,
                              shape.inner_ry, &u, &v) > ONE) {
                raw_px(x, y, outline);
                continue;
            }
            raw_px(x, y, tone_for(u, v, x, y, shadow, base, light));
        }
    }
}

static void flat_ellipse(float cx, float cy, float rx, float ry, float angle,
                         uint8_t color)
{
    ellipse(cx, cy, rx, ry, angle, 0.0f, color, color, color, color);
}

typedef struct {
    float next_blink;
    float blink_start;
    float next_gaze;
    float gaze_x;
    float gaze_y;
    float target_x;
    float target_y;
    float last_t;
} motion_t;

static motion_t s_motion = {
    .next_blink = 1.6f,
    .blink_start = -10.0f,
    .next_gaze = 1.1f,
};
static uint32_t s_rng = 0x9e3779b9u;

static float random_unit(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xffffff) / (float)0x1000000;
}

static float update_eyes(const muse_pose_t *pose, float dt)
{
    if (pose->t >= s_motion.next_blink) {
        s_motion.blink_start = pose->t;
        s_motion.next_blink =
            pose->t + (random_unit() < 0.18f ? 0.30f
                                             : 2.2f + random_unit() * 3.0f);
    }
    if (pose->t >= s_motion.next_gaze) {
        s_motion.next_gaze = pose->t + 1.2f + random_unit() * 2.4f;
        s_motion.target_x = random_unit() * 2.0f - 1.0f;
        s_motion.target_y = (random_unit() * 2.0f - 1.0f) * 0.55f;
    }

    float target_x = s_motion.target_x;
    float target_y = s_motion.target_y;
    if (pose->mode == MUSE_MODE_LISTENING ||
        pose->mode == MUSE_MODE_SPEAKING) {
        target_x = 0;
        target_y = 0;
    } else if (pose->mode == MUSE_MODE_THINKING) {
        target_x = 0.8f * sinf(pose->mode_t * 1.4f) + 0.1f;
        target_y = -0.8f;
    }
    float amount = 1.0f - expf(-dt * 14.0f);
    s_motion.gaze_x += (target_x - s_motion.gaze_x) * amount;
    s_motion.gaze_y += (target_y - s_motion.gaze_y) * amount;

    float blink_t = (pose->t - s_motion.blink_start) / 0.16f;
    if (blink_t < 0 || blink_t > 1) {
        return 0;
    }
    return 1.0f - fabsf(blink_t * 2.0f - 1.0f);
}

static void draw_orbit(int cx, int cy, float t, muse_mode_t mode, float level)
{
    int count = mode == MUSE_MODE_BOOT ? 9 : 16;
    float speed = mode == MUSE_MODE_THINKING ? 1.2f : 0.42f;
    for (int i = 0; i < count; i++) {
        float angle = t * speed + (float)i * TAU / count;
        float radius = 57.0f + 2.0f * sinf(t * 1.1f + i * 1.3f);
        int x = iround(cx + cosf(angle) * radius);
        int y = iround(cy + sinf(angle) * radius * 0.94f);
        uint8_t color = (i + (int)(t * 3.0f)) % 4 == 0 ? C_ACC_L
                                                        : C_AURA_L;
        px(x, y, color);
        if (level > 0.7f && (i & 3) == 0) {
            px(x + 1, y, color);
        }
    }
}

static void draw_side_waves(int cx, int cy, float t, float level)
{
    int waves = 1 + (int)(level * 2.8f);
    for (int side = -1; side <= 1; side += 2) {
        for (int wave = 0; wave < waves; wave++) {
            float radius = 39.0f + wave * 7.0f;
            float shimmer = 0.5f + 0.5f * sinf(t * 9.0f + wave * 1.4f);
            for (int point = -5 - wave; point <= 5 + wave; point++) {
                float y = point * 2.0f;
                float x = sqrtf(fmaxf(0.0f, radius * radius - y * y));
                int px_x = iround(cx + side * x);
                int px_y = iround(cy + y);
                if (wave == 0 || BAYER4[px_y & 3][px_x & 3] <
                                     4 + (int)(shimmer * 8.0f)) {
                    px(px_x, px_y, wave == 0 ? C_ACC : C_AURA_L);
                }
            }
        }
    }
}

static void draw_shadow(int cx, int cy, float width)
{
    int half = iround(width);
    for (int y = 0; y < 5; y++) {
        int row_half = half - abs(2 - y) * 3;
        for (int x = -row_half; x <= row_half; x++) {
            if (BAYER4[y & 3][x & 3] > abs(x) * 12 / (row_half + 1)) {
                px(cx + x, cy + y, C_SHADOW);
            }
        }
    }
}

typedef enum {
    EYES_NORMAL,
    EYES_WIDE,
    EYES_HAPPY,
    EYES_ERROR,
    EYES_CLOSED,
} eye_style_t;

static void draw_eye(float cx, float cy, eye_style_t style, float openness,
                     float gaze_x, float gaze_y)
{
    int x = iround(cx);
    int y = iround(cy);
    if (style == EYES_HAPPY) {
        thick_line(x - 4, y + 1, x, y - 2, C_EYE);
        thick_line(x, y - 2, x + 4, y + 1, C_EYE);
        return;
    }
    if (style == EYES_ERROR) {
        thick_line(x - 3, y - 3, x + 3, y + 3, C_EYE);
        thick_line(x + 3, y - 3, x - 3, y + 3, C_EYE);
        return;
    }
    if (style == EYES_CLOSED || openness < 0.28f) {
        thick_line(x - 4, y, x + 4, y, C_EYE);
        return;
    }

    float rx = style == EYES_WIDE ? 4.5f : 4.0f;
    float ry = style == EYES_WIDE ? 5.0f : 4.1f;
    flat_ellipse(cx, cy, rx, ry, 0, C_EYE);
    int pupil_x = x + iround(gaze_x * 1.4f);
    int pupil_y = y + iround(gaze_y * 1.2f);
    disc(pupil_x, pupil_y, style == EYES_WIDE ? 2 : 2, C_PUPIL);
    px(pupil_x - 1, pupil_y - 1, C_SHINE);
}

typedef enum {
    MOUTH_SMILE,
    MOUTH_LISTEN,
    MOUTH_THINK,
    MOUTH_SPEAK,
    MOUTH_GRIN,
    MOUTH_FLAT,
} mouth_style_t;

static void draw_nose_and_mouth(int cx, int cy, mouth_style_t style,
                                float level)
{
    px(cx - 2, cy, C_BLACK);
    px(cx - 1, cy, C_BLACK);
    px(cx, cy, C_BLACK);
    px(cx + 1, cy, C_BLACK);
    px(cx + 2, cy, C_BLACK);
    px(cx - 1, cy + 1, C_BLACK);
    px(cx, cy + 1, C_BLACK);
    px(cx + 1, cy + 1, C_BLACK);

    int mouth_y = cy + 4;
    switch (style) {
    case MOUTH_SMILE:
        line(cx, cy + 2, cx, mouth_y, C_BLACK);
        line(cx, mouth_y, cx - 4, mouth_y + 2, C_BLACK);
        line(cx, mouth_y, cx + 4, mouth_y + 2, C_BLACK);
        break;
    case MOUTH_LISTEN:
        flat_ellipse(cx, mouth_y + 1, 3.0f, 4.0f, 0, C_BLACK);
        flat_ellipse(cx, mouth_y + 1, 1.2f, 2.0f, 0, C_TONGUE);
        break;
    case MOUTH_THINK:
        line(cx - 4, mouth_y + 1, cx + 1, mouth_y + 1, C_BLACK);
        line(cx + 1, mouth_y + 1, cx + 3, mouth_y - 1, C_BLACK);
        break;
    case MOUTH_SPEAK: {
        float open = 3.0f + level * 4.0f;
        flat_ellipse(cx, mouth_y + 2, 4.5f, open, 0, C_BLACK);
        flat_ellipse(cx, mouth_y + 4, 2.6f, 1.8f, 0, C_TONGUE);
        break;
    }
    case MOUTH_GRIN:
        flat_ellipse(cx, mouth_y + 1, 7.0f, 4.0f, 0, C_BLACK);
        line(cx - 4, mouth_y, cx + 4, mouth_y, C_EYE);
        flat_ellipse(cx, mouth_y + 3, 3.0f, 1.4f, 0, C_TONGUE);
        break;
    case MOUTH_FLAT:
        thick_line(cx - 5, mouth_y + 1, cx + 5, mouth_y + 1, C_BLACK);
        break;
    }
}

static void draw_face(float head_x, float head_y, const muse_pose_t *pose,
                      float blink)
{
    float eye_y = head_y - 1.0f;
    float eye_dx = 13.0f;
    flat_ellipse(head_x - eye_dx, eye_y, 10.0f, 7.7f, 0.27f, C_BLACK);
    flat_ellipse(head_x + eye_dx, eye_y, 10.0f, 7.7f, -0.27f, C_BLACK);

    eye_style_t eyes = EYES_NORMAL;
    mouth_style_t mouth = MOUTH_SMILE;
    float openness = 1.0f - blink;
    if (pose->mode == MUSE_MODE_BOOT) {
        openness =
            clampf((pose->mode_t - 0.66f) / 0.28f, 0.0f, 1.0f);
    } else if (pose->mode == MUSE_MODE_LISTENING) {
        eyes = EYES_WIDE;
        mouth = MOUTH_LISTEN;
    } else if (pose->mode == MUSE_MODE_THINKING) {
        mouth = MOUTH_THINK;
    } else if (pose->mode == MUSE_MODE_SPEAKING) {
        mouth = MOUTH_SPEAK;
    } else if (pose->mode == MUSE_MODE_ERROR) {
        eyes = EYES_ERROR;
        mouth = MOUTH_FLAT;
    } else if (pose->mode == MUSE_MODE_OFF) {
        eyes = EYES_CLOSED;
        openness = 0;
    }
    if (pose->happy > 0.2f && pose->mode != MUSE_MODE_ERROR) {
        eyes = EYES_HAPPY;
        mouth = MOUTH_GRIN;
    }

    draw_eye(head_x - eye_dx, eye_y, eyes, openness, s_motion.gaze_x,
             s_motion.gaze_y);
    draw_eye(head_x + eye_dx, eye_y, eyes, openness, s_motion.gaze_x,
             s_motion.gaze_y);

    if (pose->mode == MUSE_MODE_LISTENING) {
        thick_line(iround(head_x - 20), iround(eye_y - 10),
                   iround(head_x - 12), iround(eye_y - 12), C_BLACK_L);
        thick_line(iround(head_x + 12), iround(eye_y - 12),
                   iround(head_x + 20), iround(eye_y - 10), C_BLACK_L);
    } else if (pose->mode == MUSE_MODE_THINKING) {
        thick_line(iround(head_x - 20), iround(eye_y - 10),
                   iround(head_x - 12), iround(eye_y - 11), C_BLACK_L);
        thick_line(iround(head_x + 12), iround(eye_y - 13),
                   iround(head_x + 20), iround(eye_y - 10), C_BLACK_L);
    }

    flat_ellipse(head_x, head_y + 12.0f, 10.0f, 7.0f, 0, C_MUZZLE);
    disc(iround(head_x - 23.0f), iround(head_y + 10.0f), 2, C_CHEEK);
    disc(iround(head_x + 23.0f), iround(head_y + 10.0f), 2, C_CHEEK);
    draw_nose_and_mouth(
        iround(head_x), iround(head_y + 8.0f), mouth,
        clampf(pose->level * 1.15f +
                   0.08f * (1.0f + sinf(pose->t * 21.0f)),
               0.0f, 1.0f));
}

static void draw_thought_dots(int cx, int cy, float t)
{
    int active = (int)(fracf(t * 1.4f) * 3.0f);
    for (int i = 0; i < 3; i++) {
        int radius = 2 + i;
        int x = cx + 34 + i * 8;
        int y = cy - 30 - i * 6 - (i == active ? 2 : 0);
        disc(x, y, radius, i == active ? C_ACC_L : C_ACC);
    }
}

static void draw_alert(int cx, int cy)
{
    for (int y = 0; y < 10; y++) {
        int half = y < 6 ? 2 : 1;
        for (int x = -half; x <= half; x++) {
            px(cx + x, cy + y, C_ERROR);
        }
    }
    disc(cx, cy + 14, 2, C_ERROR);
}

static void draw_heart(int cx, int cy)
{
    disc(cx - 3, cy - 2, 3, C_HEART);
    disc(cx + 3, cy - 2, 3, C_HEART);
    for (int y = 0; y < 7; y++) {
        int half = 6 - y;
        for (int x = -half; x <= half; x++) {
            px(cx + x, cy + y, C_HEART);
        }
    }
}

static void draw_hearts(int cx, int cy, float t, float amount)
{
    for (int i = 0; i < 2; i++) {
        float phase = fracf(t * 0.72f + i * 0.5f);
        if (phase > amount) {
            continue;
        }
        int x = iround(cx + (i ? 34.0f : -34.0f) +
                       sinf(phase * TAU + i) * 4.0f);
        int y = iround(cy - 18.0f - phase * 14.0f);
        draw_heart(x, y);
    }
}

static void draw_panda(const muse_pose_t *pose, float reveal, float fade)
{
    float level = clampf(pose->level, 0.0f, 1.0f);
    float happy = pose->mode == MUSE_MODE_ERROR
                      ? 0.0f
                      : clampf(pose->happy, 0.0f, 1.0f);
    float hop = happy * fabsf(sinf(pose->t * 8.5f)) * 6.0f;
    float bob = sinf(pose->t * 1.8f) * 1.2f;
    if (pose->mode == MUSE_MODE_LISTENING) {
        bob = sinf(pose->t * 3.0f) * 0.8f;
    } else if (pose->mode == MUSE_MODE_THINKING) {
        bob = sinf(pose->t * 2.2f) * 1.4f;
    } else if (pose->mode == MUSE_MODE_SPEAKING) {
        bob = sinf(pose->t * 5.0f) * 0.8f - level * 1.8f;
    }
    float shake = pose->mode == MUSE_MODE_ERROR && pose->mode_t < 0.6f
                      ? sinf(pose->t * 21.0f) * 3.0f
                      : 0.0f;
    float lean = pose->mode == MUSE_MODE_THINKING
                     ? sinf(pose->t * 1.25f) * 2.0f
                     : 0.0f;
    float squash = 0.40f + 0.60f * reveal;
    float breathe = sinf(pose->t * 2.0f + 0.7f) * 0.018f;
    float center_x = 64.0f + shake + lean;
    float center_y = 64.0f + bob - hop;
    float head_x = center_x + lean * 0.45f;
    float head_y = center_y - 15.0f;
    float body_y = center_y + 23.0f;

    draw_shadow(iround(center_x), iround(center_y + 55.0f),
                24.0f - hop * 1.1f);

    float body_rx = 24.0f * (1.0f + breathe) * squash;
    float body_ry = 27.0f * (1.0f - breathe) * squash;
    ellipse(center_x, body_y, body_rx, body_ry, 0, 2.0f, C_OUT, C_FUR_D,
            C_FUR, C_FUR_L);

    float feet_y = body_y + body_ry - 1.0f;
    ellipse(center_x - 13.0f, feet_y, 10.0f * squash, 5.5f * squash,
            -0.08f, 1.4f, C_OUT, C_OUT, C_BLACK, C_BLACK_L);
    ellipse(center_x + 13.0f, feet_y, 10.0f * squash, 5.5f * squash,
            0.08f, 1.4f, C_OUT, C_OUT, C_BLACK, C_BLACK_L);

    float left_x = center_x - 24.0f;
    float right_x = center_x + 24.0f;
    float arm_y = body_y - 2.0f;
    float left_angle = -0.22f;
    float right_angle = 0.22f;
    if (pose->mode == MUSE_MODE_LISTENING) {
        left_x = head_x - 32.0f;
        right_x = head_x + 32.0f;
        arm_y = head_y + 8.0f;
        left_angle = 0.55f;
        right_angle = -0.55f;
    } else if (pose->mode == MUSE_MODE_THINKING) {
        right_x = head_x + 18.0f;
        arm_y = head_y + 22.0f;
        right_angle = -1.05f;
    } else if (pose->mode == MUSE_MODE_SPEAKING) {
        float gesture = sinf(pose->t * 7.0f) * (0.2f + level * 0.35f);
        left_angle = -0.6f - gesture;
        right_angle = 0.6f - gesture;
        arm_y -= 3.0f;
    } else if (pose->mode == MUSE_MODE_OFF) {
        right_x = head_x + 32.0f;
        arm_y = head_y + 3.0f;
        right_angle = -1.35f + sinf(pose->t * 11.0f) * 0.18f * fade;
    }
    if (happy > 0.15f) {
        left_x = head_x - 31.0f;
        right_x = head_x + 31.0f;
        arm_y = head_y + 2.0f;
        float wiggle = sinf(pose->t * 14.0f) * 0.24f;
        left_angle = 0.95f + wiggle;
        right_angle = -0.95f - wiggle;
    }
    ellipse(left_x, arm_y, 7.0f * squash, 14.0f * squash, left_angle,
            1.5f, C_OUT, C_OUT, C_BLACK, C_BLACK_L);
    ellipse(right_x, arm_y, 7.0f * squash, 14.0f * squash, right_angle,
            1.5f, C_OUT, C_OUT, C_BLACK, C_BLACK_L);

    float head_rx = 34.0f * (1.0f + breathe) * squash;
    float head_ry = 28.0f * (1.0f - breathe) * squash;
    float ear_y = head_y - head_ry * 0.72f;
    float ear_dx = head_rx * 0.68f;
    ellipse(head_x - ear_dx, ear_y, 12.0f * squash, 12.0f * squash, 0,
            1.5f, C_OUT, C_OUT, C_BLACK, C_BLACK_L);
    ellipse(head_x + ear_dx, ear_y, 12.0f * squash, 12.0f * squash, 0,
            1.5f, C_OUT, C_OUT, C_BLACK, C_BLACK_L);
    ellipse(head_x, head_y, head_rx, head_ry, 0, 2.0f, C_OUT, C_FUR_D,
            C_FUR, C_FUR_L);

    if (reveal > 0.55f) {
        draw_face(head_x, head_y, pose, update_eyes(pose, 0.04f));
    }

    if (pose->mode == MUSE_MODE_THINKING) {
        draw_thought_dots(iround(head_x), iround(head_y), pose->t);
    }
    if (pose->mode == MUSE_MODE_ERROR) {
        draw_alert(iround(head_x + 40.0f), iround(head_y - 33.0f));
    }
    if (happy > 0) {
        draw_hearts(iround(head_x), iround(head_y), pose->t, happy);
    }
}

#define MAP_MAX 512
static uint16_t s_map[MAP_MAX];
static int s_size;

void muse_pixel_set_size(int pixels)
{
    s_size = pixels < 1 ? 1 : (pixels < MAP_MAX ? pixels : MAP_MAX);
    bool show_grid = s_size >= 3 * W;
    for (int i = 0; i < s_size; i++) {
        int cell = i * W / s_size;
        bool edge = show_grid && (i + 1) * W / s_size != cell;
        s_map[i] = (uint16_t)(cell | (edge ? 0x8000 : 0));
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0,
                      int y1)
{
    int count = x1 - x0 + 1;
    const uint16_t *xmap = &s_map[x0];
    const uint16_t *previous = NULL;
    uint16_t previous_map = 0;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        uint16_t mapped_y = s_map[y];
        if (previous && mapped_y == previous_map) {
            memcpy(dst, previous, (size_t)count * sizeof(uint16_t));
            continue;
        }
        const uint8_t *row = &s_fb[(mapped_y & 0x7fff) * W];
        if (mapped_y & 0x8000) {
            for (int i = 0; i < count; i++) {
                dst[i] = s_pal_dim[row[xmap[i] & 0x7fff]];
            }
        } else {
            for (int i = 0; i < count; i++) {
                uint16_t mapped_x = xmap[i];
                uint8_t color = row[mapped_x & 0x7fff];
                dst[i] =
                    mapped_x & 0x8000 ? s_pal_dim[color] : s_pal[color];
            }
        }
        previous = dst;
        previous_map = mapped_y;
    }
}

void muse_pixel_render(const muse_pose_t *pose)
{
    muse_mode_t mode = pose->mode;
    if (mode < 0 || mode >= MUSE_MODE_COUNT) {
        mode = MUSE_MODE_IDLE;
    }
    float dt = s_motion.last_t > 0
                   ? clampf(pose->t - s_motion.last_t, 0.0f, 0.2f)
                   : 0.04f;
    s_motion.last_t = pose->t;
    float fade =
        mode == MUSE_MODE_OFF
            ? clampf(1.0f - pose->mode_t / 1.35f, 0.0f, 1.0f)
            : 1.0f;
    update_palette(mode, dt, fade);
    memset(s_fb, C_BG, sizeof(s_fb));

    float reveal =
        mode == MUSE_MODE_BOOT
            ? clampf(pose->mode_t / 0.72f, 0.0f, 1.0f)
            : 1.0f;
    if (mode == MUSE_MODE_OFF) {
        reveal *= 0.72f + 0.28f * fade;
    }
    reveal = 1.0f - (1.0f - reveal) * (1.0f - reveal);

    int scene_x = 64;
    int scene_y = 65;
    float level = clampf(pose->level, 0.0f, 1.0f);
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) {
        draw_side_waves(scene_x, scene_y - 15, pose->t, level);
    } else {
        draw_orbit(scene_x, scene_y, pose->t, mode, level);
    }
    draw_panda(pose, reveal, fade);
}
