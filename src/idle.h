/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * idle.h -- what the tube shows when nothing arrives.
 *
 * Without this module the receiver did nothing with silence: empty queue, no
 * more flips, and the image of the last frame stayed on the scan forever. On
 * a cathode-ray tube, a still image held for hours burns the phosphor.
 *
 * This module draws the idle screen. It knows neither DRM nor the network: it
 * is given an XRGB8888 buffer and writes into it. That is what makes it
 * testable without hardware (receiver/test_idle.c).
 *
 * What protects the tube, and this is the point:
 *   - the background is black, so most of the surface does not light up;
 *   - the text is dim, not white;
 *   - the caller moves the block regularly (idle_place).
 */
#ifndef RECEIVER_IDLE_H
#define RECEIVER_IDLE_H

#include <stddef.h>
#include <stdint.h>

/* An 8x8 font. Every measure below follows from it. */
#define IDLE_GLYPH_W 8
#define IDLE_GLYPH_H 8

/* Line step, in font pixels: 8 for the glyph, 4 blank. */
#define IDLE_LINE_STEP 12

/* Beyond this, the caller should ask itself what it is displaying. */
#define IDLE_MAX_LINES 6

/* The grey of the text. 25 % brightness: readable on a tube, far from the
 * white that burns. A single component -- the idle screen is grey, not
 * coloured. */
#define IDLE_TEXT_LEVEL 0x40u

/* Clears the whole buffer to black, then draws the lines from (x, y).
 *
 *   dst      XRGB8888 buffer (bytes B, G, R, X)
 *   pitch    bytes per buffer row
 *   fb_w/h   buffer size, in pixels
 *   x_scale  horizontal stretch of each font pixel. In super-resolution the
 *            buffer is 2560 wide for 320 of content; without this stretch
 *            the text would be an unreadable thread. See idle_x_scale.
 *   y_scale  vertical stretch. 2 in 480i, 1 in 240p.
 *   x, y     top-left corner of the block, in buffer pixels
 *   lines    array of zero-terminated strings, n elements
 *   level    grey level applied to the three components
 *
 * Anything that overflows the buffer is clipped. The function never writes
 * outside [dst, dst + fb_h * pitch). Returns the number of glyphs drawn.
 */
int idle_render(void *dst, uint32_t pitch, int fb_w, int fb_h,
                int x_scale, int y_scale, int x, int y,
                const char *const *lines, int n, uint8_t level);

/* Clears the buffer to black, nothing else. This is the minimal version of
 * the idle screen: a black screen. */
void idle_render_black(void *dst, uint32_t pitch, int fb_h);

/* Size of the text block, to choose a position where it fits. */
void idle_text_extent(const char *const *lines, int n,
                      int x_scale, int y_scale, int *out_w, int *out_h);

/* Stretch to apply so that the text keeps the same apparent size whatever the
 * width of the buffer. The reference is 320: the content width around which
 * every 15 kHz regime of the receiver revolves. */
int idle_x_scale(int fb_w);
int idle_y_scale(int fb_h);

/* Chooses a position for the block, inside the margins, from a counter that
 * moves forward at each move. Deterministic: same counter, same position --
 * which makes the move testable.
 *
 * Writes *out_x / *out_y. If the block is larger than the buffer, the
 * position is 0 and rendering takes care of the clipping. */
void idle_place(uint64_t move_count, int fb_w, int fb_h, int text_box_w, int text_box_h,
                int *out_x, int *out_y);

/* IPv4 address of the first active interface that is not the loopback.
 * Cold path: getifaddrs, to be called once when the thread starts. Writes
 * "?" if nothing is found. Always terminates the string. */
void idle_local_ipv4(char *out, size_t out_sz);

#endif /* RECEIVER_IDLE_H */
