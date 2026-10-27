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

/*
 * Sec-MS-GEC v2 token generator for Microsoft Edge's online TTS service.
 *
 * Faithful C port of edge-tts 7.3.1's drm.py (MIT license,
 * https://github.com/rany2/edge-tts) — generate_sec_ms_gec_v2 and
 * its helpers). The token format is "<16 hex>-<64 hex>": a CS64 digest
 * followed by a SHA-256 digest of the same input string.
 *
 * The input string is the 5-minute-windowed Windows FILETIME tick count
 * (decimal, computed through the SAME float path as the Python reference:
 * double arithmetic, fmod, then %.0f) concatenated with the public Edge
 * client token constant. That constant is published in edge-tts itself;
 * it is not a user secret and must never be treated as one.
 *
 * The SHA-256 here is NOT the standard hash: msedge.dll's native token
 * hash pre-seeds the first block (words 0..13 zero, words 14..15 from the
 * first 7 input bytes, a tweaked round constant at index 14, and a
 * 57+len length field). The port below reproduces those quirks exactly;
 * do not "simplify" it to a standard SHA-256 or the token will not match.
 *
 * This file has no ESP-IDF dependencies so the generator can be unit
 * tested on a host (compile with a C99 compiler and -lm for fmod).
 */

#include "muse_edge_tts_drm.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Public Edge client token, from edge-tts constants.py. Public, not secret. */
#define EDGE_TTS_CLIENT_TOKEN "6A5AA1D4EAFF4E9FB37E23D68491D6F4"

#define WIN_EPOCH 11644473600.0 /* seconds between 1601 and 1970 */

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

/* Pre-seeded first-block constants from edge-tts drm.py. */
static const uint32_t sha256_iv[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};
static const uint32_t sha256_state0[8] = {
    0xfd9fbea2, 0x91243ce7, 0xbe499094, 0xcd2e1e0a,
    0x6c006f49, 0x04bd35ab, 0xfeb0a7e4, 0x03effbd2,
};
static const uint32_t sha256_sched14[14] = {
    0xeb24bf9e, 0x1558691f, 0xa0c09ee2, 0x0cfa32ba, 0xa622ee8c, 0xd1dd9491,
    0xc0dd25a0, 0xf9e70ab6, 0xf0c31573, 0x7058ce4e, 0x7188e606, 0x00e5ab33,
    0xd258b060, 0x72766564,
};
#define SHA256_ADJUSTMENT 0xc9dc06a7u
#define SHA256_SIGMA0_ADJ 0x059c0b80u
#define SHA256_SIGMA1_ADJ 0x000b92c0u

static const uint32_t cs64_coeff[14][2] = {
    {0xe82e91b1, 0xb32700d0}, {0x44de24c6, 0x88dff51d},
    {0x3beb65bc, 0x29514038}, {0xc89c5933, 0xb328cbdf},
    {0x8d1ec211, 0xd405b4d5}, {0x314ef5d5, 0x26e55f3c},
    {0xe92491b1, 0x607801c6}, {0xf0a5b9d5, 0x82b2c1fb},
    {0x06064cf3, 0xfa51a3da}, {0xda79fa60, 0x207cc464},
    {0xbed20560, 0x3c3c74f3}, {0x89f9c9f8, 0xd2d14b61},
    {0x35fa38c6, 0xbb04a2c1}, {0xcb49e3c6, 0xc5626167},
};
#define CS64_FACTOR 0x82893933u
#define CS64_ADJUSTMENT0 0xf2f90b1au
#define CS64_ADJUSTMENT1 0x0b1a0000u

static uint32_t rol16(uint32_t v)
{
    return (uint32_t)((v << 16) | (v >> 16));
}

static uint32_t sigma0(uint32_t v)
{
    return ROTR32(v, 7) ^ ROTR32(v, 18) ^ (v >> 3);
}

static uint32_t sigma1(uint32_t v)
{
    return ROTR32(v, 17) ^ ROTR32(v, 19) ^ (v >> 10);
}

static void sha256_rounds(uint32_t st[8], const uint32_t *w,
                          const uint32_t *k, int first_round)
{
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    uint32_t e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = first_round; i < 64; i++) {
        uint32_t s1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    st[0] = a;
    st[1] = b;
    st[2] = c;
    st[3] = d;
    st[4] = e;
    st[5] = f;
    st[6] = g;
    st[7] = h;
}

static void sha256_expand(uint32_t w[64], int first_word)
{
    for (int i = first_word; i < 64; i++) {
        w[i] = sigma1(w[i - 2]) + w[i - 7] + sigma0(w[i - 15]) + w[i - 16];
    }
}

/* Pre-seeded first block: words 0..13 zero, 14..15 from data[0:7]. */
static void sha256_init(const uint8_t *data7, uint32_t st[8])
{
    uint32_t w[64];
    for (int i = 0; i < 14; i++) {
        w[i] = 0;
    }
    w[14] = ((uint32_t)data7[0] << 16) | ((uint32_t)data7[1] << 8) | data7[2];
    w[15] = ((uint32_t)data7[3] << 24) | ((uint32_t)data7[4] << 16) |
            ((uint32_t)data7[5] << 8) | data7[6];
    for (int i = 16; i < 30; i++) {
        uint32_t value = sha256_sched14[i - 16];
        /* i - 2 >= 14 always holds here; adjustment only at i == 16. */
        value += (i == 16) ? (sigma1(w[i - 2]) ^ SHA256_SIGMA1_ADJ)
                           : sigma1(w[i - 2]);
        if (i - 7 >= 14) {
            value += w[i - 7];
        }
        if (i - 15 >= 14) {
            /* Only i == 29 reaches this branch. */
            value += sigma0(w[i - 15]) ^ SHA256_SIGMA0_ADJ;
        }
        w[i] = value;
    }
    w[30] = sigma1(w[28]) + w[23] + sigma0(w[15]) + w[14] + SHA256_ADJUSTMENT -
            sha256_k[14];
    sha256_expand(w, 31);
    /* Block constants with K[14] replaced by the adjustment. */
    uint32_t kblk[64];
    for (int i = 0; i < 64; i++) {
        kblk[i] = sha256_k[i];
    }
    kblk[14] = SHA256_ADJUSTMENT;
    uint32_t work[8];
    for (int i = 0; i < 8; i++) {
        work[i] = sha256_state0[i];
    }
    sha256_rounds(work, w, kblk, 14);
    for (int i = 0; i < 8; i++) {
        st[i] = sha256_iv[i] + work[i];
    }
}

/* The msedge.dll token hash (NOT standard SHA-256: see header comment). */
static void sha256_token_digest(const uint8_t *data, size_t len,
                                uint8_t digest[32])
{
    /* Caller guarantees len >= 7. */
    uint32_t length = (uint32_t)(57 + len);
    size_t rest = len - 7;
    /* padded = data[7:] + 0x80 + zeros + 8-byte BE bit length. */
    uint32_t pad_zeros = (uint32_t)(((55u - length) % 64 + 64) % 64);
    size_t padded_len = rest + 1 + pad_zeros + 8;
    uint8_t block[64];
    uint32_t st[8];
    sha256_init(data, st);
    size_t off = 0;
    while (off < padded_len) {
        for (int i = 0; i < 64; i++) {
            size_t p = off + (size_t)i;
            uint8_t b;
            if (p < rest) {
                b = data[7 + p];
            } else if (p == rest) {
                b = 0x80;
            } else if (p < rest + 1 + pad_zeros) {
                b = 0x00;
            } else {
                /* 8-byte big-endian bit length, length * 8 bits. */
                size_t q = p - (rest + 1 + pad_zeros);
                uint64_t bits = (uint64_t)length * 8u;
                b = (uint8_t)(bits >> (56 - 8 * q));
            }
            block[i] = b;
        }
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)block[4 * i] << 24) |
                   ((uint32_t)block[4 * i + 1] << 16) |
                   ((uint32_t)block[4 * i + 2] << 8) | block[4 * i + 3];
        }
        sha256_expand(w, 16);
        uint32_t work[8];
        for (int i = 0; i < 8; i++) {
            work[i] = st[i];
        }
        sha256_rounds(work, w, sha256_k, 0);
        for (int i = 0; i < 8; i++) {
            st[i] += work[i];
        }
        off += 64;
    }
    for (int i = 0; i < 8; i++) {
        digest[4 * i] = (uint8_t)(st[i] >> 24);
        digest[4 * i + 1] = (uint8_t)(st[i] >> 16);
        digest[4 * i + 2] = (uint8_t)(st[i] >> 8);
        digest[4 * i + 3] = (uint8_t)st[i];
    }
}

static void cs64_digest(const uint8_t *data, size_t len,
                        const uint8_t digest[32], uint32_t out[2])
{
    uint32_t key_a = (uint32_t)digest[0] | ((uint32_t)digest[1] << 8) |
                     ((uint32_t)digest[2] << 16) | ((uint32_t)digest[3] << 24);
    uint32_t key_b = (uint32_t)digest[4] | ((uint32_t)digest[5] << 8) |
                     ((uint32_t)digest[6] << 16) | ((uint32_t)digest[7] << 24);
    key_a |= 1;
    key_b |= 1;
    uint32_t factors[2] = {key_a * CS64_FACTOR, key_b * CS64_FACTOR};
    /* terms/offsets: 14 coefficients, then one pair per input word. */
    uint32_t terms[14 + 64];
    uint32_t offsets[14 + 64];
    int n = 0;
    for (int i = 0; i < 14; i++) {
        terms[n] = cs64_coeff[i][0] * factors[i % 2];
        offsets[n] = cs64_coeff[i][1];
        n++;
    }
    /* _words_le4_even(b"\x00" + data): LE words, zero-padded, even count. */
    size_t total = len + 1;
    size_t padded = (total + 3) & ~(size_t)3;
    int words = (int)(padded / 4);
    if (words % 2) {
        words++;
    }
    for (int i = 0; i < words; i++) {
        uint32_t word = 0;
        for (int k = 0; k < 4; k++) {
            size_t p = (size_t)i * 4 + (size_t)k;
            uint8_t b = 0;
            if (p == 0) {
                b = 0x00;
            } else if (p - 1 < len) {
                b = data[p - 1];
            }
            word |= (uint32_t)b << (8 * k);
        }
        uint32_t term = word * (i % 2 == 0 ? key_a : key_b);
        uint32_t offset =
            rol16(word) * (i % 2 == 0 ? 0xef0569fbu : 0xc31713dbu);
        if (i == 0) {
            term += CS64_ADJUSTMENT0 * factors[0];
            offset += CS64_ADJUSTMENT1;
        }
        terms[n] = term;
        offsets[n] = offset;
        n++;
    }
    uint32_t swap_acc = 0, swap_last = 0, state = 0, acc = 0;
    for (int i = 0; i + 1 < n; i += 2) {
        uint32_t x = terms[i], y = terms[i + 1];
        uint32_t t = rol16(offsets[i] + x);
        uint32_t q = t * 0x76f50f34u;
        swap_acc += rol16(q) * 0xc3efea97u + q;
        t = rol16(offsets[i + 1] + y);
        q = t * 0x1881ce3cu;
        swap_last = rol16(q) * 0x35bd1ec9u + q;
        swap_acc = swap_acc + swap_last;
        state = state * key_a + x;
        state = rol16(state) * 0xcf98b111u;
        state = rol16(state) * 0x87085b9fu;
        state = rol16(state) * 0x12ceb96du;
        state = rol16(state) * 0x257e1d83u;
        acc += state;
        t = state * key_b + y;
        t = rol16(t) * 0xa27416f5u;
        t = rol16(t) * 0xd38396ffu;
        state = rol16(rol16(t) * 0x7c932b89u) * 0xbfa49f69u;
        acc = acc + state;
    }
    out[0] = swap_last ^ state;
    out[1] = swap_acc ^ acc;
}

static const char hexd[] = "0123456789ABCDEF";

bool muse_edge_tts_token(char *out, size_t out_size, int64_t unix_seconds,
                         int32_t skew_seconds)
{
    /* "<16 hex>-<64 hex>" + NUL = 82 bytes. */
    if (out_size < 82) {
        return false;
    }
    /* Float path identical to the Python reference (see header comment). */
    double ticks = (double)(unix_seconds + (int64_t)skew_seconds);
    ticks += WIN_EPOCH;
    ticks -= fmod(ticks, 300.0);
    ticks *= 1e7; /* S_TO_NS / 100 */
    char ticks_str[32];
    snprintf(ticks_str, sizeof(ticks_str), "%.0f", ticks);
    char input[32 + 32 + 1];
    snprintf(input, sizeof(input), "%s%s", ticks_str, EDGE_TTS_CLIENT_TOKEN);
    size_t len = strlen(input);
    if (len < 7) {
        return false;
    }
    uint8_t digest[32];
    sha256_token_digest((const uint8_t *)input, len, digest);
    uint32_t cs64[2];
    cs64_digest((const uint8_t *)input, len, digest, cs64);
    /* cs64 is little-endian packed: out = LE bytes of (w0, w1) as hex. */
    for (int i = 0; i < 4; i++) {
        out[i * 2] = hexd[(cs64[0] >> (8 * i + 4)) & 0xf];
        out[i * 2 + 1] = hexd[(cs64[0] >> (8 * i)) & 0xf];
    }
    for (int i = 0; i < 4; i++) {
        out[8 + i * 2] = hexd[(cs64[1] >> (8 * i + 4)) & 0xf];
        out[8 + i * 2 + 1] = hexd[(cs64[1] >> (8 * i)) & 0xf];
    }
    out[16] = '-';
    for (int i = 0; i < 32; i++) {
        out[17 + i * 2] = hexd[(digest[i] >> 4) & 0xf];
        out[17 + i * 2 + 1] = hexd[digest[i] & 0xf];
    }
    out[81] = '\0';
    return true;
}
