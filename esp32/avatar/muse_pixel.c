// Copyright (c) 2026 miko-gadget-xiaozhi contributors
// SPDX-License-Identifier: MIT

/*
 * Miko, Mayar's mascot, as the board's 64x64 pixel-art avatar.
 *
 * A round, fuzzy blue plush: two bear ears, a curl of fur on top, big glossy
 * bead eyes with a white rim, pink felt cheeks, a tiny "w" mouth, stubby arms
 * and feet, and a cream belly patch with a blue "M". Cheerful and a bit shy.
 *
 * The body parts are analytic blobs so they can squash, breathe and wave; the
 * face, the curl and the effects are small stamped bitmaps. Everything is
 * drawn as palette indices into a 64x64 grid, which muse_pixel_scale() blows
 * up to the screen.
 *
 * ponytail: per-pixel maths is float, fine on the ESP32-S3's FPU (a few
 * thousand covered pixels a frame). A chip without one (ESP32-C6) would want
 * this in fixed point.
 */

#include "muse_pixel.h"

#include <math.h>
#include <string.h>

#define W MUSE_PX_W
#define H MUSE_PX_H
#define PI 3.14159265f

/* ---- palette ------------------------------------------------------------ */

enum {
    K_BG,
    K_LINE,
    K_FUR0,         /* darkest ... */
    K_FUR1,
    K_FUR2,
    K_FUR3,         /* ... highlight */
    K_BELLY,
    K_BELLY_SH,
    K_LETTER,
    K_EYE,
    K_WHITE,
    K_CHEEK,
    K_TONGUE,
    K_HEART,
    K_SHADOW,
    K_GLOW_LO,      /* from here on: follows the mode's accent */
    K_GLOW_HI,
    K_ACCENT,
    K_SPARK,
    K_COUNT,
};

static const uint32_t BASE[K_COUNT] = {
    [K_BG] = 0x000000,
    [K_LINE] = 0x0a1233,
    [K_FUR0] = 0x284a9c,
    [K_FUR1] = 0x3a66c4,
    [K_FUR2] = 0x4f82e0,
    [K_FUR3] = 0x7fa9f5,
    [K_BELLY] = 0xf4eccb,
    [K_BELLY_SH] = 0xd8cc9c,
    [K_LETTER] = 0x2f86d8,
    [K_EYE] = 0x0a0c18,
    [K_WHITE] = 0xffffff,
    [K_CHEEK] = 0xf0708a,
    [K_TONGUE] = 0xff8fa0,
    [K_HEART] = 0xff4f8b,
    [K_SHADOW] = 0x10131f,
};

static const uint32_t ACCENT[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = 0x9fc4ff,
    [MUSE_MODE_IDLE] = 0x6f9bff,
    [MUSE_MODE_LISTENING] = 0x4fd0ff,
    [MUSE_MODE_THINKING] = 0xd88bff,
    [MUSE_MODE_SPEAKING] = 0x63e6b0,
    [MUSE_MODE_ERROR] = 0xff5a5a,
    [MUSE_MODE_OFF] = 0x7a7fb8,
};

static uint16_t s_pal[K_COUNT], s_dim[K_COUNT];
static float s_acc[3];
static bool s_acc_set;

static uint8_t s_fb[W * H];
static uint8_t s_part[W * H];   /* which body part owns a cell, 0 = none */

enum { P_NONE, P_FOOT, P_EAR, P_BODY, P_ARM };  /* back to front */

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return ACCENT[mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE];
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int iround(float v)
{
    return (int)floorf(v + 0.5f);
}

static void set_colour(int k, float r, float g, float b)
{
#define C565(r, g, b) (uint16_t)((((int)(r) >> 3) << 11) | (((int)(g) >> 2) << 5) | ((int)(b) >> 3))
    s_pal[k] = C565(r, g, b);
    /* A cell's last screen row and column are drawn dimmer: the pixel grid. */
    s_dim[k] = C565(r * 0.7f, g * 0.7f, b * 0.7f);
#undef C565
}

static void update_palette(muse_mode_t mode, float dt, float glow)
{
    float k = s_acc_set ? 1.0f - expf(-dt * 6.0f) : 1.0f;
    for (int i = 0; i < 3; i++) {
        s_acc[i] += ((float)((ACCENT[mode] >> (16 - 8 * i)) & 0xff) - s_acc[i]) * k;
    }
    s_acc_set = true;

    for (int i = 0; i < K_GLOW_LO; i++) {
        set_colour(i, (BASE[i] >> 16) & 0xff, (BASE[i] >> 8) & 0xff, BASE[i] & 0xff);
    }
    float r = s_acc[0], g = s_acc[1], b = s_acc[2];
    set_colour(K_GLOW_LO, r * 0.15f * glow, g * 0.15f * glow, b * 0.15f * glow);
    set_colour(K_GLOW_HI, r * 0.32f * glow, g * 0.32f * glow, b * 0.32f * glow);
    set_colour(K_ACCENT, r, g, b);
    set_colour(K_SPARK, (r + 255) / 2, (g + 255) / 2, (b + 255) / 2);
}

/* ---- drawing helpers ---------------------------------------------------- */

static void plot(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_fb[y * W + x] = c;
    }
}

/* Effects go around Miko, never over them. */
static void plot_bg(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H && s_part[y * W + x] == P_NONE && s_fb[y * W + x] != K_LINE) {
        s_fb[y * W + x] = c;
    }
}

static void plot_part(int x, int y, uint8_t c, uint8_t part)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_fb[y * W + x] = c;
        s_part[y * W + x] = part;
    }
}

/* Ordered-dither threshold in 0..1. */
static float dither(int x, int y)
{
    static const uint8_t M[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    return (M[(y & 3) * 4 + (x & 3)] + 0.5f) / 16.0f;
}

/* Position hash in 0..1. Callers pass body-relative cells so the fur travels with Miko. */
static float noise(int x, int y)
{
    uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (float)(h & 0xffff) / 65536.0f;
}

static uint8_t ink(char ch)
{
    switch (ch) {
    case 'k': return K_LINE;
    case 'w': return K_WHITE;
    case 'e': return K_EYE;
    case 'p': return K_CHEEK;
    case 't': return K_TONGUE;
    case 'h': return K_HEART;
    case 'a': return K_ACCENT;
    case 'f': return K_FUR2;
    case 'l': return K_FUR3;
    case 'm': return K_LETTER;
    default: return K_BG;
    }
}

/* Bitmap as one string, rows split by '/', '.' transparent, other letters per ink(). */
static void stamp(const char *art, int x0, int y0)
{
    int x = x0;
    for (; *art; art++) {
        if (*art == '/') {
            x = x0;
            y0++;
            continue;
        }
        if (*art != '.') {
            plot(x, y0, ink(*art));
        }
        x++;
    }
}

/* Fur tone for a surface facing (nx, ny), lit from the top left. (ax, ay) anchors the texture. */
static uint8_t fur_tone(float nx, float ny, int x, int y, int ax, int ay)
{
    float lit = 0.57f - 0.36f * nx - 0.46f * ny
              + (dither(x, y) - 0.5f) * 0.22f
              + (noise(x - ax, (y - ay + 64) / 2) - 0.5f) * 0.24f;     /* short vertical strands */
    return lit > 0.92f ? K_FUR3 : lit > 0.40f ? K_FUR2 : lit > 0.10f ? K_FUR1 : K_FUR0;
}

/* A furry ellipse, its long axis turned by `rot`. */
static void fur_blob(float cx, float cy, float rx, float ry, float rot, uint8_t part)
{
    float c = cosf(rot), s = sinf(rot), r = (rx > ry ? rx : ry) + 1.0f;
    int ax = (int)floorf(cx), ay = (int)floorf(cy);
    for (int y = (int)(cy - r); y <= (int)(cy + r); y++) {
        for (int x = (int)(cx - r); x <= (int)(cx + r); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float u = (dx * c + dy * s) / rx, v = (dy * c - dx * s) / ry;
            if (u * u + v * v <= 1.0f) {
                plot_part(x, y, fur_tone(dx / r, dy / r, x, y, ax, ay), part);
            }
        }
    }
}

typedef struct {
    float cx, cy;   /* body centre */
    float a, b;     /* body half width and half height */
} miko_t;

/* The body: a squarish ball, a touch wider at the base, with the belly patch. */
static void draw_body(const miko_t *m)
{
    int ax = (int)floorf(m->cx), ay = (int)floorf(m->cy);
    for (int y = (int)(m->cy - m->b - 2); y <= (int)(m->cy + m->b + 2); y++) {
        for (int x = (int)(m->cx - m->a * 1.15f - 2); x <= (int)(m->cx + m->a * 1.15f + 2); x++) {
            float dx = x + 0.5f - m->cx, dy = y + 0.5f - m->cy;
            float v = dy / m->b, u = dx / (m->a * (1.0f + 0.07f * v));
            float d = u * u + v * v - 0.55f * u * u * v * v;
            /* Tufts poke in and out along the edge. */
            if (d > 1.0f + (noise(x - ax, y - ay) - 0.5f) * 0.10f) {
                continue;
            }
            float bu = dx / (m->a * 0.59f), bv = (dy - m->b * 0.62f) / (m->b * 0.375f);
            float bd = bu * bu + bv * bv;
            uint8_t c;
            if (bd > 1.0f) {
                c = fur_tone(u * 0.9f, v * 0.9f, x, y, ax, ay);
            } else if (bd > 0.74f || (bu + bv > 0.55f && dither(x, y) < 0.4f)) {
                c = K_BELLY_SH;     /* felt edge, and shade on the lower right */
            } else {
                c = K_BELLY;
            }
            plot_part(x, y, c, P_BODY);
        }
    }
}

/* A dark line around the silhouette, and where a part overlaps the one behind it. */
static void draw_outline(void)
{
    static const int8_t N[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t mine = s_part[y * W + x];
            for (int k = 0; k < 4; k++) {
                int xx = x + N[k][0], yy = y + N[k][1];
                if ((unsigned)xx < W && (unsigned)yy < H && s_part[yy * W + xx] > mine) {
                    s_fb[y * W + x] = K_LINE;
                    break;
                }
            }
        }
    }
}

/* An arm hangs from its shoulder; `swing` is 0 straight down, PI straight up, outward positive. */
static void draw_arm(float sx, float sy, int side, float swing)
{
    fur_blob(sx + side * sinf(swing) * 4.0f, sy + cosf(swing) * 4.0f, 3.4f, 5.8f, -side * swing, P_ARM);
}

/* ---- eyes: blinking and wandering gaze ---------------------------------- */

static uint32_t s_rng = 0x2545f491u;
static float s_last_t, s_next_blink = 1.5f, s_blink_at = -10.0f, s_next_gaze = 1.0f;
static float s_gx, s_gy, s_tgx, s_tgy;

static float rnd(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (float)(s_rng >> 8) / 16777216.0f;
}

/* Returns how shut the eyes are from blinking, 0..1. */
static float update_eyes(const muse_pose_t *p, float dt)
{
    if (p->t >= s_next_blink) {
        s_blink_at = p->t;
        s_next_blink = p->t + (rnd() < 0.2f ? 0.3f : 2.2f + rnd() * 3.0f);    /* sometimes twice */
    }
    if (p->t >= s_next_gaze) {
        s_next_gaze = p->t + 1.2f + rnd() * 2.4f;
        bool ahead = rnd() < 0.35f;
        s_tgx = ahead ? 0 : rnd() * 2 - 1;
        s_tgy = ahead ? 0 : (rnd() * 2 - 1) * 0.6f;
    }
    float tx = s_tgx, ty = s_tgy;
    if (p->mode == MUSE_MODE_LISTENING) {
        tx = ty = 0;
    } else if (p->mode == MUSE_MODE_THINKING) {
        tx = 0.2f + 0.8f * sinf(p->mode_t * 1.3f);
        ty = -1.0f;
    } else if (p->mode == MUSE_MODE_SPEAKING) {
        tx *= 0.3f;
        ty = 0;
    }
    float k = 1.0f - expf(-dt * 14.0f);
    s_gx += (tx - s_gx) * k;
    s_gy += (ty - s_gy) * k;

    float bt = (p->t - s_blink_at) / 0.16f;
    return bt < 0 || bt > 1 ? 0 : 1.0f - fabsf(bt * 2 - 1);
}

typedef enum { EYE_OPEN, EYE_WIDE, EYE_HALF, EYE_SHUT, EYE_HAPPY, EYE_X } eye_t;

/* (x, y) is the top left of the eye's 7x7 box. */
static void draw_eye(int x, int y, eye_t kind)
{
    static const char *const ART[] = {
        [EYE_OPEN] = "..www../.weeew./wewweew/weweeew/weeeeew/.weeew./..www..",
        [EYE_WIDE] = "..www../.weeew./wewweew/wewweew/weweeew/weeeeew/.weeew./..www..",
        [EYE_HALF] = "......./......./kkkkkkk/weeeeew/weeeeew/.weeew./..www..",
        [EYE_SHUT] = "......./......./......./kk...kk/.kkkkk.",
        [EYE_HAPPY] = "......./..kkk../.kkkkk./kk...kk",
        [EYE_X] = "......./k...k../.k.k.../..k..../.k.k.../k...k..",
    };
    stamp(ART[kind], kind == EYE_X ? x + 1 : x, kind == EYE_WIDE ? y - 1 : y);
}

typedef enum { MOUTH_SMILE, MOUTH_O, MOUTH_HMM, MOUTH_TALK, MOUTH_GRIN, MOUTH_FLAT } mouth_t;

/* (x, y) is the top middle of the mouth. */
static void draw_mouth(int x, int y, mouth_t kind, float open)
{
    static const char *const ART[] = {
        [MOUTH_SMILE] = "k.k.k/.k.k.",
        [MOUTH_O] = "..k../.ktk./..k..",
        [MOUTH_HMM] = "...kk/kkk..",
        [MOUTH_GRIN] = "kkkkk/ktttk/.kkk.",
        [MOUTH_FLAT] = "...../.kkk.",
    };
    if (kind != MOUTH_TALK) {
        stamp(ART[kind], x - 2, y);
        return;
    }
    int h = 1 + iround(clampf(open, 0, 1) * 3.0f);
    for (int j = 0; j < h; j++) {
        for (int i = -2; i <= 2; i++) {
            bool corner = (h == 1 || (h >= 3 && (j == 0 || j == h - 1))) && (i == -2 || i == 2);
            if (!corner) {
                plot(x + i, y + j, h >= 2 && j == h - 1 && i > -2 && i < 2 ? K_TONGUE : K_LINE);
            }
        }
    }
}

/* ---- effects around Miko ------------------------------------------------ */

static void draw_glow(float cx, float cy, float radius, float strength)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float dx = (x + 0.5f - cx) / radius, dy = (y + 0.5f - cy) / radius;
            float v = strength * (1.0f - dx * dx - dy * dy), th = dither(x, y);
            if (v > 0.45f + th * 0.5f) {
                plot(x, y, K_GLOW_HI);
            } else if (v > th * 0.8f) {
                plot(x, y, K_GLOW_LO);
            }
        }
    }
}

static void draw_shadow(float cx, float half_w)
{
    for (int y = 58; y <= 60; y++) {
        float hw = y == 59 ? half_w : half_w * 0.7f;
        for (int x = iround(cx - hw); x <= iround(cx + hw); x++) {
            if (dither(x, y) > fabsf(x + 0.5f - cx) / hw * 0.8f) {
                plot(x, y, K_SHADOW);
            }
        }
    }
}

/* Dotted rings spreading out from the face, and sound arcs either side of it. */
static void draw_sound(float cx, float cy, float reach, float t, float level, float speed)
{
    for (int ring = 0; ring < 2; ring++) {
        float ph = t * speed + ring * 0.5f;
        ph -= floorf(ph);
        float r = 21.0f + ph * 10.0f;
        if (ph > 0.55f + level * 0.45f) {
            continue;   /* quiet rings die out sooner */
        }
        for (int i = ring; i < 56; i += 2) {
            float a = i * (2 * PI / 56);
            plot_bg(iround(cx + cosf(a) * r), iround(cy + sinf(a) * r * 0.94f), ph < 0.35f ? K_ACCENT : K_GLOW_HI);
        }
    }
    int arcs = 1 + (int)(level * 2.6f);
    for (int k = 0; k < arcs; k++) {
        float r = reach + 6.0f + k * 3.0f;
        for (int j = -2 - k; j <= 2 + k; j++) {
            int dx = iround(r - j * j * 1.5f / r), y = iround(cy + j);
            uint8_t c = k == 0 ? K_ACCENT : K_GLOW_HI;
            plot_bg(iround(cx + dx), y, c);
            plot_bg(iround(cx - dx) - 1, y, c);
        }
    }
}

static void draw_sparkles(float cx, float cy, float t, float speed, int count)
{
    for (int i = 0; i < count; i++) {
        float a = t * speed + i * (2 * PI / 6);
        int x = iround(cx + cosf(a) * (26.0f + sinf(i * 1.9f) * 2.0f));
        int y = iround(cy + sinf(a) * 24.0f);
        float tw = 0.5f + 0.5f * sinf(t * 5.0f + i * 1.7f);
        if (tw > 0.2f) {
            plot_bg(x, y, tw > 0.6f ? K_SPARK : K_ACCENT);
        }
        for (int k = 1; k <= (tw > 0.85f ? 2 : tw > 0.5f ? 1 : 0); k++) {
            plot_bg(x + k, y, K_ACCENT);
            plot_bg(x - k, y, K_ACCENT);
            plot_bg(x, y + k, K_ACCENT);
            plot_bg(x, y - k, K_ACCENT);
        }
    }
}

/* ---- frame -------------------------------------------------------------- */

void muse_pixel_render(const muse_pose_t *p)
{
    float dt = s_last_t > 0 ? clampf(p->t - s_last_t, 0, 0.2f) : 0.04f;
    s_last_t = p->t;

    muse_mode_t mode = p->mode < MUSE_MODE_COUNT ? p->mode : MUSE_MODE_IDLE;
    float t = p->t, mt = p->mode_t;
    float level = clampf(p->level, 0, 1);
    float happy = mode == MUSE_MODE_ERROR ? 0 : clampf(p->happy, 0, 1);
    /* Powering down: Miko waves, dozes off, and the glow fades. */
    float glow = mode == MUSE_MODE_OFF ? clampf(1.0f - mt / 1.3f, 0, 1) : 1.0f;
    /* Booting: Miko pops up from a squash, then opens their eyes. */
    float boot = mode == MUSE_MODE_BOOT ? clampf(mt / 1.4f, 0, 1) : 1.0f;
    float pop = mode == MUSE_MODE_BOOT ? clampf(mt / 0.6f, 0, 1) : 1.0f;

    update_palette(mode, dt, glow);
    float blink = update_eyes(p, dt);
    memset(s_fb, K_BG, sizeof(s_fb));
    memset(s_part, P_NONE, sizeof(s_part));

    /* ---- pose ---- */
    float bob = sinf(t * 1.8f), lean = 0;
    float hop = fabsf(sinf(t * 9.0f)) * 3.0f * happy;
    if (mode == MUSE_MODE_LISTENING) {
        bob = sinf(t * 3.0f) * 0.6f;
    } else if (mode == MUSE_MODE_THINKING) {
        bob = sinf(t * 2.4f) * 0.8f;
        lean = sinf(t * 1.3f) * 1.2f;
    } else if (mode == MUSE_MODE_SPEAKING) {
        bob = sinf(t * 5.0f) * 0.6f - level * 1.6f;
    } else if (mode == MUSE_MODE_ERROR) {
        bob = 1.0f;
        lean = mt < 0.6f ? sinf(t * 18.0f) * 1.3f : 0;
    }
    float squash = 1.0f - (1.0f - pop) * 0.4f + sinf(pop * PI) * 0.08f;
    float breath = sinf(t * 2.0f + 1.0f) * 0.03f;

    miko_t m;
    m.a = 16.3f * (1 + breath) * (2.0f - squash) + level * 0.6f;
    m.b = 17.5f * (1 - breath) * squash;
    m.cx = 31.5f + lean;                            /* centred on a cell, so the face is symmetric */
    m.cy = 52.5f - m.b + bob * 0.5f - hop;
    int icx = (int)floorf(m.cx);
    float top = m.cy - m.b;
    int ey = iround(m.cy - m.b * 0.33f);            /* eye line */

    /* ---- behind ---- */
    draw_glow(m.cx, m.cy - 1, 28.0f + level * 4.0f + sinf(t * 1.5f), (0.8f * boot + level * 0.4f));
    draw_shadow(m.cx, 13.0f - hop * 0.8f);

    /* ---- body ---- */
    float step = mode == MUSE_MODE_SPEAKING ? sinf(t * 5.0f) * 0.6f : 0;
    float feet_y = m.cy + m.b + 1.2f + hop * 0.3f;
    fur_blob(m.cx - 6.5f, feet_y + step, 4.3f, 3.6f, 0, P_FOOT);
    fur_blob(m.cx + 6.5f, feet_y - step, 4.3f, 3.6f, 0, P_FOOT);
    fur_blob(m.cx - m.a * 0.67f, m.cy - m.b * 0.9f, 4.6f, 4.6f, 0, P_EAR);
    fur_blob(m.cx + m.a * 0.67f, m.cy - m.b * 0.9f, 4.6f, 4.6f, 0, P_EAR);
    draw_body(&m);

    float sh = m.a * 0.92f, sy = m.cy + 0.5f;
    float swing_l = 0.55f + sinf(t * 1.8f + 0.6f) * 0.08f, swing_r = swing_l;
    if (happy > 0) {
        swing_l = swing_r = 2.6f + sinf(t * 14.0f) * 0.25f;      /* arms up, wiggling */
        sy -= 3.0f;
    } else if (mode == MUSE_MODE_LISTENING) {
        swing_l = swing_r = 2.5f;                                /* paws up by the ears */
        sy -= 4.0f;
    } else if (mode == MUSE_MODE_SPEAKING) {
        float w = sinf(t * 7.0f) * (0.2f + level * 0.5f);
        swing_l = 0.9f + w;
        swing_r = 0.9f - w;
    } else if (mode == MUSE_MODE_OFF) {
        swing_r = 2.5f + sinf(t * 12.0f) * 0.35f * glow;         /* waving goodbye */
    }
    draw_arm(m.cx - sh, sy, -1, swing_l);
    if (mode == MUSE_MODE_THINKING && happy <= 0) {
        fur_blob(m.cx + 6.5f, ey + 7.5f, 3.4f, 5.0f, 0.9f, P_ARM);  /* a paw to the chin */
    } else {
        draw_arm(m.cx + sh, mode == MUSE_MODE_OFF ? sy - 3.0f : sy, 1, swing_r);
    }
    draw_outline();

    /* The curl on top grows out of the head, and flops with the lean. */
    stamp("..kkk.../.klllk../klfkflk./kfk.kfk./.k.kffk./..kffk../..kffk..",
          icx - 3 + iround(lean * 0.4f), iround(top) - 7);

    /* ---- face ---- */
    eye_t eyes = blink > 0.66f ? EYE_SHUT : blink > 0.2f ? EYE_HALF : EYE_OPEN;
    mouth_t mouth = MOUTH_SMILE;
    float mouth_open = 0;
    switch (mode) {
    case MUSE_MODE_BOOT:
        eyes = mt < 0.9f ? EYE_SHUT : mt < 1.05f ? EYE_HALF : eyes;
        break;
    case MUSE_MODE_LISTENING:
        eyes = eyes == EYE_OPEN ? EYE_WIDE : eyes;
        mouth = MOUTH_O;
        break;
    case MUSE_MODE_THINKING:
        mouth = MOUTH_HMM;
        break;
    case MUSE_MODE_SPEAKING:
        mouth = MOUTH_TALK;
        mouth_open = level * 1.3f + 0.1f * (0.5f + 0.5f * sinf(t * 22.0f));   /* never quite still */
        break;
    case MUSE_MODE_ERROR:
        eyes = EYE_X;
        mouth = MOUTH_FLAT;
        break;
    case MUSE_MODE_OFF:
        eyes = mt > 0.8f ? EYE_SHUT : mt > 0.55f ? EYE_HALF : eyes;
        break;
    default:
        break;
    }
    if (happy > 0.2f) {
        eyes = EYE_HAPPY;
        mouth = MOUTH_GRIN;
    }

    bool look = eyes == EYE_OPEN || eyes == EYE_WIDE || eyes == EYE_HALF;
    int gx = look ? iround(s_gx) : 0, gy = look ? iround(s_gy * 0.8f) : 0;
    draw_eye(icx - 10 + gx, ey - 3 + gy, eyes);
    draw_eye(icx + 4 + gx, ey - 3 + gy, eyes);
    if (mode == MUSE_MODE_LISTENING) {              /* raised brows */
        stamp("kkk", icx - 8, ey - 7);
        stamp("kkk", icx + 6, ey - 7);
    } else if (mode == MUSE_MODE_THINKING) {        /* one cocked */
        stamp("kkk", icx - 8, ey - 5);
        stamp("..k/kk.", icx + 6, ey - 7);
    }
    stamp(".ppp./ppppp/.ppp.", icx - 13, ey + 3);
    stamp(".ppp./ppppp/.ppp.", icx + 9, ey + 3);
    draw_mouth(icx, ey + 1, mouth, mouth_open);

    stamp("mm...mm/mmm.mmm/mmmmmmm/mm.m.mm/mm...mm", icx - 3, iround(m.cy + m.b * 0.60f) - 2);

    /* ---- in front ---- */
    float speed = mode == MUSE_MODE_THINKING ? 2.8f : mode == MUSE_MODE_LISTENING ? 1.2f
                : mode == MUSE_MODE_SPEAKING ? 1.5f : 0.6f;
    draw_sparkles(m.cx, m.cy - 2, t, speed, (int)(6 * boot * glow));
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) {
        draw_sound(m.cx, ey + 1, m.a, t, level, mode == MUSE_MODE_LISTENING ? 0.9f : 0.6f);
    }
    if (mode == MUSE_MODE_THINKING) {               /* three thought dots stepping up */
        int on = (int)(t * 1.6f) % 3;
        for (int i = 0; i < 3; i++) {
            stamp(i == on ? "ww/ww" : "aa/aa", icx + 15 + i * 4, iround(top) - i * 3 - (i == on));
        }
    }
    if (happy > 0) {                                /* two hearts floating up */
        for (int i = 0; i < 2; i++) {
            float ph = t * 0.9f + i * 0.5f;
            ph -= floorf(ph);
            if (ph <= happy) {
                stamp(".h.h./hwhhh/hhhhh/.hhh./..h..",
                      icx + (i ? 15 : -20) + iround(sinf(ph * 2 * PI + i) * 2), iround(top - 2 - ph * 9));
            }
        }
    }
    if (mode == MUSE_MODE_ERROR) {
        stamp("aa/aa/aa/aa/../aa", icx + 19, iround(top) - 3);
    }
}

/* ---- scaling to the screen ---------------------------------------------- */

#define MAX_PX 512
static uint8_t s_cell[MAX_PX];      /* screen pixel -> grid cell */
static uint8_t s_edge[MAX_PX];      /* 1 on a cell's last screen pixel, when cells are big enough to show a grid */
static int s_size;

void muse_pixel_set_size(int px)
{
    s_size = px < MAX_PX ? px : MAX_PX;
    for (int i = 0; i < s_size; i++) {
        s_cell[i] = (uint8_t)(i * W / s_size);
        s_edge[i] = s_size >= 3 * W && (i + 1) * W / s_size != s_cell[i];
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        if (y > y0 && s_cell[y] == s_cell[y - 1] && s_edge[y] == s_edge[y - 1]) {
            memcpy(dst, dst - stride_px, n * sizeof(uint16_t));     /* same as the row above */
            continue;
        }
        const uint8_t *row = &s_fb[s_cell[y] * W];
        for (int i = 0; i < n; i++) {
            uint8_t c = row[s_cell[x0 + i]];
            dst[i] = s_edge[y] || s_edge[x0 + i] ? s_dim[c] : s_pal[c];
        }
    }
}
