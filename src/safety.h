/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECEIVER_SAFETY_H
#define RECEIVER_SAFETY_H

/*
 * HARDWARE-SAFETY NOTICE:
 * H must be in [15.625, 16.0] kHz for the JVC DT-V1710CG low band,
 * or [27.0, 45.0] kHz for high band. The dead band [16.001, 26.999] kHz
 * is hardware-loss-grade -- the JVC deflection circuit will not sync and
 * risks damage under sustained signal.
 * This envelope is FIXED: the monitor file can only narrow it, never widen
 * it. Do not move these bounds unless the tube's own specification says so.
 */

#define H_BAND_LOW_MIN_HZ    15625U
#define H_BAND_LOW_MAX_HZ    16000U
#define H_BAND_HIGH_MIN_HZ   27000U
#define H_BAND_HIGH_MAX_HZ   45000U
#define V_MIN_HZ             50U
#define V_MAX_HZ             80U

/* Minimum interval between two successful drmModeSetCrtc calls: a 100 ms
 * floor, conservative against the timing of a CRT TV protection circuit.
 * There is no command-line override; one could be added if the hardware
 * ever shows a need.
 *
 * Single source of truth: defined here so the validate_modeline envelope
 * gate and the rate-limit floor live in the same hardware-safety header. */
#define MIN_MODESET_INTERVAL_NS 100000000ULL

/* Minimum time spent in a band, and a cap on band crossings (15 kHz <-> 31 kHz)
 * per 60 s window, both enforced AT THE RECEIVER, which protects the tube. A
 * request that comes too early is DELAYED, never refused; the 240p <-> 480i
 * switch is not a band crossing. This clock is separate from the 100 ms floor
 * above: the floor applies to every reprogramming, while the band rule keeps
 * its own band-entry time.
 *
 * Values set on 2026-10-18, after the first signal on the tube: the rule lets
 * intended bursts through (tests, game menus) and only slows a runaway. 1 s in
 * a band: the tube finishes its relay click. 12 per 60 s: only a sustained
 * rate of one switch every 5 s for a whole minute is slowed down. This is a
 * JUDGEMENT, not a measurement: the only observed fact is that on the first
 * signal the tube settled after one relay click, with no long black screen
 * and no rolling. The pure rule lives in bandrule.h. */
#define BAND_DWELL_MIN_NS           1000000000ULL
#define BAND_CROSSINGS_PER_MIN_MAX  12U

_Static_assert(BAND_DWELL_MIN_NS >= MIN_MODESET_INTERVAL_NS,
               "band dwell must not be shorter than the modeset floor");
_Static_assert(BAND_CROSSINGS_PER_MIN_MAX >= 1U,
               "band crossing cap must allow at least one crossing per minute");

_Static_assert(H_BAND_LOW_MAX_HZ < H_BAND_HIGH_MIN_HZ,
               "dead band must be non-empty");

typedef enum {
    MODELINE_ACCEPT             = 0,
    MODELINE_REJECT_H_LOW       = 1,
    MODELINE_REJECT_H_DEAD_BAND = 2,  /* hardware-loss-grade */
    MODELINE_REJECT_H_HIGH      = 3,
    MODELINE_REJECT_V           = 4,
    MODELINE_REJECT_BAND        = 5   /* inside the envelope, but in no usable band of the monitor file */
} modeline_result_t;

modeline_result_t validate_modeline(unsigned h_freq_hz, unsigned v_freq_hz);

/*
 * Structured rejection logger. Writes a single JSON-shape line to stderr
 * (always) and to syslog(LOG_DAEMON|LOG_ERR) when -DSAFETY_USE_SYSLOG=1.
 * Includes call_site, h_freq_hz, v_freq_hz, reason name, and a
 * CLOCK_MONOTONIC ns timestamp. No silent fallback: every rejection is logged.
 */
void safety_log_rejection(const char *call_site, unsigned h_freq_hz, unsigned v_freq_hz, modeline_result_t reason);

/*
 * Same line, same event name ("modeline_rejected"), same reason strings, but the
 * frequencies are REAL Hz printed with two decimals ("h_hz":15699.48). The
 * integer logger above would show 16000.9 Hz as 16000 -- the very truncation the
 * monitor guard (monitor.c) exists to remove.
 */
void safety_log_rejection_hz(const char *call_site, double h_hz, double v_hz, modeline_result_t reason);

/* Reason -> human-readable string. Exposed for tests and any caller logging. */
const char *safety_reason_str(modeline_result_t r);

#endif /* RECEIVER_SAFETY_H */
