/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECEIVER_MODELINES_H
#define RECEIVER_MODELINES_H

#include <stdint.h>
#include "safety.h"

/*
 * modelines.h -- the pinned modelines, the single source of their timings.
 *
 * The daemon serves exactly three modes: 240p and 480i at 15 kHz, 480p at
 * 31 kHz. Their timings live here and nowhere else: blit.c and the monitor
 * check (monitor.c) read these constants; they never re-derive or hardcode
 * a timing value.
 *
 * Why 2560 pixels wide. blit.c magnifies the source horizontally by a
 * truncated integer (x_scale = fb_w / src_w). At fb_w=1280, a 512-wide
 * PlayStation frame truncates to x_scale=2 (1024 of 1280 px used, about
 * 20% black bars on each side). At fb_w=2560, the four common PlayStation
 * source widths land exactly: 256x10, 320x8, 512x5, 640x4.
 * The 2560 geometry is the validated 1280 geometry with every horizontal
 * value doubled, pclock included, so every blanking duration in
 * MICROSECONDS is UNCHANGED: hfreq, both regimes' vfreq and the analog
 * porch timings are bit-identical to the 1280 geometry. Only the pixel
 * COUNT doubles; the analog waveform does not change.
 * An earlier, independent 2560 candidate (41.5 MHz) is explicitly NOT
 * reused here: its blanking was too tight (hsync about 385 ns) and it does
 * not synchronize on the JVC.
 *
 * 480i. A square-pixel interlaced NTSC design (broadcast NTSC is
 * 15.734 kHz / 59.94 Hz fields). The pairing validated on the bench widens
 * the horizontal blanking beyond the broadcast standard to (a) land inside
 * the JVC safety envelope at this pclock and (b) share IDENTICAL pclock_hz
 * and h_total with the 240p entry. That sharing is the structural
 * precondition for the seamless 240p<->480i live toggle (13 MMIO registers
 * under CRTC_UPDATE_LOCK, INTERLACE_ENABLE double-buffered, V_UPDATE latch;
 * a 10-cycle endurance run was clean on the bench).
 *
 * Pixel clock matched to the console. The reference is the Mega Drive
 * NTSC, 59.922743 Hz, not a nominal 60.00 Hz. `m->clock` in blit.c's
 * pinned_modeline_to_drm() is truncated to whole kHz before it reaches
 * drmModeSetCrtc: sub-kHz precision never reaches the PLL. An earlier
 * pclock_hz (24271680, at 1280 wide) truncated to 24271 kHz and produced
 * 59.998319 Hz in 240p, a +0.126% drift against the console that saturated
 * the receiver queue in about 80 s. The clock below is a whole number of
 * kHz (no truncation loss) and produces 59.921687 Hz in 240p, -0.0018%
 * against the console. 480i absorbs a wider -0.19% error: a single shared
 * pclock cannot serve both regimes exactly at once, and 240p is the one
 * aligned.
 *
 * Do not change these constants without validating the modes again on the
 * CRT.
 */

/* -----------------------------------------------------------------------
 * 240p super-res, 2560 wide (the validated 1280 geometry, doubled).
 * Porches: fp 72 px = 1.485 us, sync 228 px = 4.703 us,
 * bp 228 px = 4.703 us -- the same DURATIONS as the 1280 geometry, which
 * is analog-correct (an older, tighter blanking does NOT synchronize on
 * the JVC).
 * pclock_hz doubled alongside h_active: hfreq 15,699.48 Hz and
 * vfreq 59.9217 Hz are unchanged by the widening (see the console-matched
 * pixel clock above).
 * Framebuffer: 2560x240x4 = 2,457,600 B, pitch 10,240 B.
 * ----------------------------------------------------------------------- */

/* COUPLED TO THE EMITTER'S CLOCK SERVO (feed-forward per mode).
 * The rates derived here are COPIED into groovy_servo.h, in the emitter
 * (same derivation; this file is the SOURCE, groovy_servo.h points here):
 *   48480000 / (3088 * 262)       = 59.921687 Hz (240p, progressive)
 *   48480000 / (3088 * 525) * 2.0 = 59.807550 Hz (480i, PER FIELD)
 *   50350000 / (1600 * 525)       = 59.940476 Hz (480p, progressive)
 * If a constant below changes, change groovy_servo.h too: a test of the
 * emitter reads both files and requires the clock, the totals and the
 * three rates to be equal.
 * The emitter's start-up line "[groovy-servo] settings: ... ff_240p=
 * ff_480i= ff_480p=" shows the values in force in each session log. */
#define MODELINE_240P_SR_NTSC_H_ACTIVE     2560U
#define MODELINE_240P_SR_NTSC_H_BEGIN      2632U
#define MODELINE_240P_SR_NTSC_H_END        2860U
#define MODELINE_240P_SR_NTSC_H_TOTAL      3088U
#define MODELINE_240P_SR_NTSC_V_ACTIVE      240U
#define MODELINE_240P_SR_NTSC_V_BEGIN       244U
#define MODELINE_240P_SR_NTSC_V_END         247U
#define MODELINE_240P_SR_NTSC_V_TOTAL       262U
#define MODELINE_240P_SR_NTSC_PCLOCK_HZ 48480000U
#define MODELINE_240P_SR_NTSC_VFREQ_HZ      59.92
#define MODELINE_240P_SR_NTSC_INTERLACE       0U
#define MODELINE_240P_SR_NTSC_HSYNC           0U  /* -hsync */
#define MODELINE_240P_SR_NTSC_VSYNC           0U  /* -vsync */

/* -----------------------------------------------------------------------
 * 480i NTSC, 2560 wide (paired with 240p, doubled the same way).
 * h_active/h_begin/h_end/h_total and pclock_hz are IDENTICAL to the 240p
 * entry above by design (paired geometry -- see the header comment and
 * the CRITICAL INVARIANT asserts below). Field rate 59.8076 Hz, frame
 * rate 29.9038 Hz. A single 480-row framebuffer; the CRTC alternates even
 * and odd rows per field in hardware (verified on the bench: a coherent,
 * clean 480i picture).
 * pclock_hz doubled alongside the 240p entry (the CRITICAL INVARIANT
 * below requires both to move together): hfreq 15,699.48 Hz and field
 * rate 59.8076 Hz are unchanged. This regime absorbs the wider -0.19%
 * console error described in the header comment.
 * Framebuffer: 2560x480x4 = 4,915,200 B.
 * ----------------------------------------------------------------------- */
#define MODELINE_480I_NTSC_H_ACTIVE     2560U
#define MODELINE_480I_NTSC_H_BEGIN      2632U
#define MODELINE_480I_NTSC_H_END        2860U
#define MODELINE_480I_NTSC_H_TOTAL      3088U
#define MODELINE_480I_NTSC_V_ACTIVE      480U
#define MODELINE_480I_NTSC_V_BEGIN       488U
#define MODELINE_480I_NTSC_V_END         494U
#define MODELINE_480I_NTSC_V_TOTAL       525U
#define MODELINE_480I_NTSC_PCLOCK_HZ 48480000U
#define MODELINE_480I_NTSC_VFREQ_HZ      59.81
#define MODELINE_480I_NTSC_INTERLACE       1U
#define MODELINE_480I_NTSC_HSYNC           0U  /* -hsync */
#define MODELINE_480I_NTSC_VSYNC           0U  /* -vsync */

/* -----------------------------------------------------------------------
 * 480p, 1280 wide -- the third regime, at 31 kHz.
 *
 * Origin: the standard 640x480 VGA mode at 59.94 Hz (the kernel's DMT
 * mode), width DOUBLED, lines unchanged. Same porches, in time, as
 * `crt_range1` of hardware/monitors/jvc_dtv1710cg.ini: 0.636 / 3.813 /
 * 1.906 us horizontal, 0.318 / 0.064 / 1.048 ms vertical. Why 1280
 * pixels: the clock lands near the 48.48 MHz already proven, and a
 * 640-wide source maps at exactly x2 (320 at x4, 256 at x5).
 * The mode is set on the SAME 2560x480 buffer as the other two: the CRTC
 * scans the leftmost 1280 columns (buffer pitch unchanged).
 * Rate: 50,350,000 / (1600 * 525) = 59.940476 Hz, which the emitter's
 * servo carries (GROOVY_SERVO_CADENCE_480P_HZ, groovy_servo.h). Line rate:
 * 31,468.75 Hz.
 *
 * No 480p timing is computed on the fly: this constant is the only mode
 * served; the resolver (regime.h) picks a regime, never a timing. The
 * clock is a multiple of 1000 Hz (m->clock is truncated to kHz); it is
 * checked against switchres 2.2.2 off the bench.
 * ----------------------------------------------------------------------- */
#define MODELINE_480P_H_ACTIVE     1280U
#define MODELINE_480P_H_BEGIN      1312U
#define MODELINE_480P_H_END        1504U
#define MODELINE_480P_H_TOTAL      1600U
#define MODELINE_480P_V_ACTIVE      480U
#define MODELINE_480P_V_BEGIN       490U
#define MODELINE_480P_V_END         492U
#define MODELINE_480P_V_TOTAL       525U
#define MODELINE_480P_PCLOCK_HZ 50350000U
#define MODELINE_480P_VFREQ_HZ      59.94
#define MODELINE_480P_INTERLACE       0U
#define MODELINE_480P_HSYNC           0U  /* -hsync */
#define MODELINE_480P_VSYNC           0U  /* -vsync */

/* -----------------------------------------------------------------------
 * The pinned modeline struct. Header-light by design: it does NOT include
 * switchres_wrapper.h -- switchres is a runtime dependency of blit.c only.
 * blit.c bridges these fields to `sr_mode` at the call site
 * (sr_add_mode(width, height, vfreq, ...)).
 * ----------------------------------------------------------------------- */
struct pinned_modeline {
    int      width;            /* h_active */
    int      height;           /* v_active */
    double   vfreq_hz;         /* 59.92 (240p) or 59.81 (480i) -- see the
                                 * console-matched pixel clock above; this
                                 * field is a switchres registration hint
                                 * whose return geometry is discarded, and
                                 * m->vrefresh (rounds to 60 either way) --
                                 * not a real timing input to drmModeSetCrtc. */
    uint32_t pclock_hz;        /* radeon PLL: >= 10_000_000 */
    uint16_t h_begin, h_end, h_total;
    uint16_t v_begin, v_end, v_total;
    uint8_t  interlace;        /* 0 = progressive, 1 = INTERLACE */
    uint8_t  hsync, vsync;     /* sync polarity: 0=neg, 1=pos. arcade_15 NTSC = 0,0 */
};

static const struct pinned_modeline MODELINE_240P_SR_NTSC = {
    .width     = (int)MODELINE_240P_SR_NTSC_H_ACTIVE,
    .height    = (int)MODELINE_240P_SR_NTSC_V_ACTIVE,
    .vfreq_hz  = MODELINE_240P_SR_NTSC_VFREQ_HZ,
    .pclock_hz = (uint32_t)MODELINE_240P_SR_NTSC_PCLOCK_HZ,
    .h_begin   = (uint16_t)MODELINE_240P_SR_NTSC_H_BEGIN,
    .h_end     = (uint16_t)MODELINE_240P_SR_NTSC_H_END,
    .h_total   = (uint16_t)MODELINE_240P_SR_NTSC_H_TOTAL,
    .v_begin   = (uint16_t)MODELINE_240P_SR_NTSC_V_BEGIN,
    .v_end     = (uint16_t)MODELINE_240P_SR_NTSC_V_END,
    .v_total   = (uint16_t)MODELINE_240P_SR_NTSC_V_TOTAL,
    .interlace = (uint8_t)MODELINE_240P_SR_NTSC_INTERLACE,
    .hsync     = (uint8_t)MODELINE_240P_SR_NTSC_HSYNC,
    .vsync     = (uint8_t)MODELINE_240P_SR_NTSC_VSYNC,
};

static const struct pinned_modeline MODELINE_480I_NTSC = {
    .width     = (int)MODELINE_480I_NTSC_H_ACTIVE,
    .height    = (int)MODELINE_480I_NTSC_V_ACTIVE,
    .vfreq_hz  = MODELINE_480I_NTSC_VFREQ_HZ,
    .pclock_hz = (uint32_t)MODELINE_480I_NTSC_PCLOCK_HZ,
    .h_begin   = (uint16_t)MODELINE_480I_NTSC_H_BEGIN,
    .h_end     = (uint16_t)MODELINE_480I_NTSC_H_END,
    .h_total   = (uint16_t)MODELINE_480I_NTSC_H_TOTAL,
    .v_begin   = (uint16_t)MODELINE_480I_NTSC_V_BEGIN,
    .v_end     = (uint16_t)MODELINE_480I_NTSC_V_END,
    .v_total   = (uint16_t)MODELINE_480I_NTSC_V_TOTAL,
    .interlace = (uint8_t)MODELINE_480I_NTSC_INTERLACE,
    .hsync     = (uint8_t)MODELINE_480I_NTSC_HSYNC,
    .vsync     = (uint8_t)MODELINE_480I_NTSC_VSYNC,
};

static const struct pinned_modeline MODELINE_480P = {
    .width     = (int)MODELINE_480P_H_ACTIVE,
    .height    = (int)MODELINE_480P_V_ACTIVE,
    .vfreq_hz  = MODELINE_480P_VFREQ_HZ,
    .pclock_hz = (uint32_t)MODELINE_480P_PCLOCK_HZ,
    .h_begin   = (uint16_t)MODELINE_480P_H_BEGIN,
    .h_end     = (uint16_t)MODELINE_480P_H_END,
    .h_total   = (uint16_t)MODELINE_480P_H_TOTAL,
    .v_begin   = (uint16_t)MODELINE_480P_V_BEGIN,
    .v_end     = (uint16_t)MODELINE_480P_V_END,
    .v_total   = (uint16_t)MODELINE_480P_V_TOTAL,
    .interlace = (uint8_t)MODELINE_480P_INTERLACE,
    .hsync     = (uint8_t)MODELINE_480P_HSYNC,
    .vsync     = (uint8_t)MODELINE_480P_VSYNC,
};

/*
 * Regime-selector schema -- 5 entries: three populated (240p, 480i, 480p),
 * two reserved for PAL (480p was added BESIDE the first two). Values 0 and
 * 1 keep their identity: the MMIO code (mmio.h) and all the 15 kHz code
 * read them as they are; only the two PAL reserves, read by no code, moved
 * to 3 and 4.
 * REGIME_PAL_288P_SR_RESERVED / REGIME_PAL_576I_RESERVED are placeholders:
 * no modeline constants are defined for them yet (adding them later is a
 * table edit, not a refactor).
 */
enum daemon_regime {
    REGIME_240P_SUPER_RES        = 0,
    REGIME_480I                  = 1,
    REGIME_480P                  = 2,
    REGIME_PAL_288P_SR_RESERVED  = 3,
    REGIME_PAL_576I_RESERVED     = 4,
    REGIME_COUNT
};

/* -----------------------------------------------------------------------
 * Compile-time safety invariants.
 *
 * NOTE: these are expressed via the #define macros above (true C11 integer
 * constant expressions), NOT via MODELINE_*_NTSC.field member access.
 * Member access on a `static const struct` object is NOT an integer
 * constant expression in C (unlike C++ constexpr) -- GCC/Clang reject
 * `_Static_assert(MODELINE_240P_SR_NTSC.pclock_hz >= ..., ...)` with
 * "expression in static assertion is not an integer constant expression".
 * ----------------------------------------------------------------------- */

/* Test 1: 240p super-res hfreq must be inside the JVC low band. */
_Static_assert((MODELINE_240P_SR_NTSC_PCLOCK_HZ / MODELINE_240P_SR_NTSC_H_TOTAL) >= H_BAND_LOW_MIN_HZ &&
               (MODELINE_240P_SR_NTSC_PCLOCK_HZ / MODELINE_240P_SR_NTSC_H_TOTAL) <= H_BAND_LOW_MAX_HZ,
               "240p super-res hfreq out of JVC low band [H_BAND_LOW_MIN_HZ, H_BAND_LOW_MAX_HZ]");

/* Test 2: 480i hfreq must be inside the JVC low band. */
_Static_assert((MODELINE_480I_NTSC_PCLOCK_HZ / MODELINE_480I_NTSC_H_TOTAL) >= H_BAND_LOW_MIN_HZ &&
               (MODELINE_480I_NTSC_PCLOCK_HZ / MODELINE_480I_NTSC_H_TOTAL) <= H_BAND_LOW_MAX_HZ,
               "480i NTSC hfreq out of JVC low band [H_BAND_LOW_MIN_HZ, H_BAND_LOW_MAX_HZ]");

/* Test 3: 240p super-res pclock must clear the radeon HD 5xxx-7xxx PLL floor. */
_Static_assert(MODELINE_240P_SR_NTSC_PCLOCK_HZ >= 10000000U,
               "radeon HD 5xxx-7xxx PLL floor is ~10 MHz");

/* Tests 8/9: INTERLACE flag must be correctly keyed per regime. */
_Static_assert(MODELINE_480I_NTSC_INTERLACE == 1U,
               "480i NTSC modeline MUST have INTERLACE bit set");
_Static_assert(MODELINE_240P_SR_NTSC_INTERLACE == 0U,
               "240p super-res modeline MUST NOT have INTERLACE bit set");

/* CRITICAL INVARIANT: the paired 240p and 480i modelines MUST share
 * identical pclock_hz AND h_total. This is the structural condition for
 * the seamless 240p<->480i live toggle (13 MMIO registers rewritten under
 * CRTC_UPDATE_LOCK, no drmModeSetCrtc / PLL relock required), proven on
 * the bench. */
_Static_assert(MODELINE_240P_SR_NTSC_PCLOCK_HZ == MODELINE_480I_NTSC_PCLOCK_HZ,
               "240p/480i paired modelines must share identical pclock_hz for the seamless toggle");
_Static_assert(MODELINE_240P_SR_NTSC_H_TOTAL == MODELINE_480I_NTSC_H_TOTAL,
               "240p/480i paired modelines must share identical h_total for the seamless toggle");

/* Test 5: regime schema is a 5-entry schema (3 populated, 2 PAL reserved). */
_Static_assert(REGIME_COUNT == 5,
               "regime schema must have exactly 5 entries (240p, 480i, 480p + 2 PAL reserved)");

/* 480p fits in the tube's HIGH band, at a whole kHz, above the PLL floor,
 * progressive. */
_Static_assert((MODELINE_480P_PCLOCK_HZ / MODELINE_480P_H_TOTAL) >= H_BAND_HIGH_MIN_HZ &&
               (MODELINE_480P_PCLOCK_HZ / MODELINE_480P_H_TOTAL) <= H_BAND_HIGH_MAX_HZ,
               "480p hfreq out of JVC high band [H_BAND_HIGH_MIN_HZ, H_BAND_HIGH_MAX_HZ]");
_Static_assert(MODELINE_480P_INTERLACE == 0U,
               "480p modeline MUST NOT have INTERLACE bit set");
_Static_assert(MODELINE_480P_PCLOCK_HZ % 1000U == 0U,
               "480p pclock must be a whole number of kHz (m->clock is truncated to kHz)");
_Static_assert(MODELINE_480P_PCLOCK_HZ >= 10000000U,
               "radeon HD 5xxx-7xxx PLL floor is ~10 MHz");

/* NEGATIVE INVARIANT: the 240p/480i pair shares its clock and total width
 * because the MMIO toggle requires it (switching without reprogramming the
 * PLL). 480p goes through drmModeSetCrtc instead: it must never join that
 * pair by accident, or it would be taken for a candidate for the MMIO
 * toggle. */
_Static_assert(MODELINE_480P_H_TOTAL != MODELINE_240P_SR_NTSC_H_TOTAL,
               "480p h_total must differ from the 240p/480i MMIO pair's h_total");

#endif /* RECEIVER_MODELINES_H */
