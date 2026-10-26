/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * idle.c -- the idle screen. See idle.h for the why.
 *
 * No allocation, no blocking I/O, except idle_local_ipv4, a cold path called
 * once when the flip thread starts.
 */
/* Feature gates: IFF_UP / IFF_LOOPBACK of <net/if.h> and getifaddrs sit
 * behind __USE_MISC, which strict -std=c11 does not enable. Same pattern as
 * blit.c and main.c. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "idle.h"

#include <ifaddrs.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>

/* ---------------------------------------------------------------------------
 * The font. 8x8, most significant bit on the left, one row per byte.
 *
 * Forty glyphs: the space, the ten digits, the twenty-six capitals, and three
 * punctuation marks. That is all it takes for an IP address, a time and two
 * words. An unknown character becomes a space.
 * ------------------------------------------------------------------------- */

#define GLYPH_SPACE 0
#define GLYPH_DOT  1
#define GLYPH_COLON 2
#define GLYPH_DASH  3
#define GLYPH_DIGIT0 4    /* 4..13  : '0'..'9' */
#define GLYPH_LETTER_A 14    /* 14..39 : 'A'..'Z' */
#define GLYPH_COUNT     40

static const uint8_t IDLE_FONT[GLYPH_COUNT][IDLE_GLYPH_H] = {
    /* space  */ { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* .      */ { 0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00 },
    /* :      */ { 0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00 },
    /* -      */ { 0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00 },

    /* 0 */ { 0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00 },
    /* 1 */ { 0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00 },
    /* 2 */ { 0x3C,0x66,0x06,0x0C,0x30,0x60,0x7E,0x00 },
    /* 3 */ { 0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00 },
    /* 4 */ { 0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00 },
    /* 5 */ { 0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00 },
    /* 6 */ { 0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00 },
    /* 7 */ { 0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x00 },
    /* 8 */ { 0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00 },
    /* 9 */ { 0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00 },

    /* A */ { 0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00 },
    /* B */ { 0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00 },
    /* C */ { 0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00 },
    /* D */ { 0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00 },
    /* E */ { 0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00 },
    /* F */ { 0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00 },
    /* G */ { 0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00 },
    /* H */ { 0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00 },
    /* I */ { 0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00 },
    /* J */ { 0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00 },
    /* K */ { 0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00 },
    /* L */ { 0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00 },
    /* M */ { 0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00 },
    /* N */ { 0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00 },
    /* O */ { 0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00 },
    /* P */ { 0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00 },
    /* Q */ { 0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00 },
    /* R */ { 0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00 },
    /* S */ { 0x3E,0x60,0x60,0x3C,0x06,0x06,0x7C,0x00 },
    /* T */ { 0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00 },
    /* U */ { 0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00 },
    /* V */ { 0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00 },
    /* W */ { 0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00 },
    /* X */ { 0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00 },
    /* Y */ { 0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00 },
    /* Z */ { 0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00 },
};

/* Lower case is accepted and folded to capitals: the font has a single case,
 * and dropping letters silently would be worse than enlarging them. */
static int glyph_index(char c)
{
    if (c >= '0' && c <= '9') return GLYPH_DIGIT0 + (c - '0');
    if (c >= 'A' && c <= 'Z') return GLYPH_LETTER_A  + (c - 'A');
    if (c >= 'a' && c <= 'z') return GLYPH_LETTER_A  + (c - 'a');
    if (c == '.') return GLYPH_DOT;
    if (c == ':') return GLYPH_COLON;
    if (c == '-') return GLYPH_DASH;
    return GLYPH_SPACE;
}

int idle_x_scale(int fb_w)
{
    /* 320 is the content width around which every 15 kHz regime of the
     * receiver revolves; it gives the text its apparent size, whatever the
     * real width of the buffer. */
    int s = fb_w / 320;
    return s < 1 ? 1 : s;
}

int idle_y_scale(int fb_h)
{
    /* Beyond 400 lines the mode is interlaced: double, or the text would be
     * half its apparent height. */
    return fb_h >= 400 ? 2 : 1;
}

void idle_text_extent(const char *const *lines, int n,
                      int x_scale, int y_scale, int *out_w, int *out_h)
{
    size_t longest_len = 0;
    int line_total = 0;

    if (lines && n > 0) {
        if (n > IDLE_MAX_LINES) n = IDLE_MAX_LINES;
        for (int i = 0; i < n; i++) {
            if (!lines[i]) continue;
            size_t len = strlen(lines[i]);
            if (len > longest_len) longest_len = len;
            line_total++;
        }
    }

    if (out_w) *out_w = (int)longest_len * IDLE_GLYPH_W * x_scale;
    /* The last line does not carry its line step. */
    if (out_h) *out_h = line_total > 0
        ? ((line_total - 1) * IDLE_LINE_STEP + IDLE_GLYPH_H) * y_scale
        : 0;
}

void idle_render_black(void *dst, uint32_t pitch, int fb_h)
{
    if (!dst || fb_h <= 0) return;
    memset(dst, 0, (size_t)fb_h * (size_t)pitch);
}

/* One font pixel, stretched into an x_scale by y_scale block. Clipped to the
 * buffer. */
static void fill_font_pixel(uint8_t *base, uint32_t pitch, int fb_w, int fb_h,
                 int px, int py, int x_scale, int y_scale, uint8_t level)
{
    for (int dy = 0; dy < y_scale; dy++) {
        int y = py + dy;
        if (y < 0 || y >= fb_h) continue;
        uint8_t *row_ptr = base + (size_t)y * (size_t)pitch;
        for (int dx = 0; dx < x_scale; dx++) {
            int x = px + dx;
            if (x < 0 || x >= fb_w) continue;
            uint8_t *p = row_ptr + (size_t)x * 4U;
            p[0] = level;   /* B */
            p[1] = level;   /* G */
            p[2] = level;   /* R */
            p[3] = 0;       /* X */
        }
    }
}

int idle_render(void *dst, uint32_t pitch, int fb_w, int fb_h,
                int x_scale, int y_scale, int x, int y,
                const char *const *lines, int n, uint8_t level)
{
    if (!dst || fb_w <= 0 || fb_h <= 0) return 0;
    if (x_scale < 1) x_scale = 1;
    if (y_scale < 1) y_scale = 1;

    idle_render_black(dst, pitch, fb_h);

    if (!lines || n <= 0) return 0;
    if (n > IDLE_MAX_LINES) n = IDLE_MAX_LINES;

    uint8_t *base = (uint8_t *)dst;
    int glyphs_drawn = 0;
    int line_slot = 0;

    for (int i = 0; i < n; i++) {
        if (!lines[i]) continue;
        int oy = y + line_slot * IDLE_LINE_STEP * y_scale;
        line_slot++;

        /* The whole line is off screen: nothing to draw, but the line step
         * has already moved forward, so the following lines stay aligned. */
        if (oy >= fb_h || oy + IDLE_GLYPH_H * y_scale <= 0) continue;

        for (const char *c = lines[i]; *c; c++) {
            int col = (int)(c - lines[i]);
            int ox = x + col * IDLE_GLYPH_W * x_scale;
            if (ox >= fb_w) break;                    /* the rest overflows */
            if (ox + IDLE_GLYPH_W * x_scale <= 0) continue;

            const uint8_t *g = IDLE_FONT[glyph_index(*c)];
            for (int gy = 0; gy < IDLE_GLYPH_H; gy++) {
                uint8_t bits = g[gy];
                if (!bits) continue;
                for (int gx = 0; gx < IDLE_GLYPH_W; gx++) {
                    if (!(bits & (uint8_t)(0x80u >> gx))) continue;
                    fill_font_pixel(base, pitch, fb_w, fb_h,
                         ox + gx * x_scale, oy + gy * y_scale,
                         x_scale, y_scale, level);
                }
            }
            glyphs_drawn++;
        }
    }

    return glyphs_drawn;
}

void idle_place(uint64_t move_count, int fb_w, int fb_h, int text_box_w, int text_box_h,
                int *out_x, int *out_y)
{
    int slack_x = fb_w - text_box_w;
    int slack_y = fb_h - text_box_h;

    if (slack_x < 0) slack_x = 0;
    if (slack_y < 0) slack_y = 0;

    /* A tiny linear congruential generator. Quality randomness is not the
     * goal: the block must not come back to the same place, and a test must
     * be able to replay the same sequence. */
    uint64_t s = move_count * 6364136223846793005ULL + 1442695040888963407ULL;
    uint32_t a = (uint32_t)(s >> 33);
    uint32_t b = (uint32_t)((s * 6364136223846793005ULL) >> 33);

    if (out_x) *out_x = slack_x ? (int)(a % (uint32_t)(slack_x + 1)) : 0;
    if (out_y) *out_y = slack_y ? (int)(b % (uint32_t)(slack_y + 1)) : 0;
}

void idle_local_ipv4(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '?';
    if (out_sz > 1) out[1] = '\0';
    else            out[0] = '\0';

    struct ifaddrs *if_list = NULL;
    if (getifaddrs(&if_list) != 0 || !if_list) return;

    for (struct ifaddrs *ifa = if_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        const struct sockaddr_in *sin =
            (const struct sockaddr_in *)(const void *)ifa->ifa_addr;
        char txt[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &sin->sin_addr, txt, sizeof txt)) continue;

        size_t n = strlen(txt);
        if (n + 1 > out_sz) continue;   /* does not fit: keep "?" */
        memcpy(out, txt, n + 1);
        break;
    }

    freeifaddrs(if_list);
}
