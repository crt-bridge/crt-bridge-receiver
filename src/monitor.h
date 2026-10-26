/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECEIVER_MONITOR_H
#define RECEIVER_MONITOR_H

/*
 * monitor.h -- the CRT described by a file.
 *
 * A PURE module: no libdrm, no switchres, no dynamic allocation. It reads
 * the text of the monitor file (the crt_range grammar of switchres 2.2.2,
 * 16 fields) with a STRICT loader, and judges a mode (H, V) against the
 * fixed envelope of safety.h FIRST, THEN against the bands of the file.
 * The file can narrow the envelope, never widen it.
 *
 * The bounds of the envelope are never copied here: they come from
 * safety.h (H_BAND_*, V_*).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "safety.h"

#define MONITOR_MAX_BANDS       10
#define MONITOR_MAX_FILE_BYTES  4096
#define MONITOR_MAX_LINE_BYTES  255

struct monitor_band {
    double h_min, h_max, v_min, v_max;        /* Hz; V = field rate when interlaced */
    double hfp_us, hsync_us, hbp_us;           /* us */
    double vfp_ms, vsync_ms, vbp_ms;           /* ms, as written in the file */
    int    hsync_pol, vsync_pol;
    int    prog_min, prog_max, ilace_min, ilace_max;
    int    index;                              /* N of crt_rangeN */
    int    line;                               /* line in the file, from 1 */
    char   text[MONITOR_MAX_LINE_BYTES + 1];   /* validated value, handed as is to switchres */
};

struct monitor {
    unsigned n;                                /* number of bands, <= MONITOR_MAX_BANDS */
    struct monitor_band band[MONITOR_MAX_BANDS]; /* file order; bit i of a mask = band[i] */
    size_t file_bytes;
    int    from_file;                          /* 0: built-in text */
};

enum monitor_err {
    MONITOR_OK = 0,
    MONITOR_ERR_IO,
    MONITOR_ERR_EMPTY,
    MONITOR_ERR_TOO_BIG,
    MONITOR_ERR_LINE_TOO_LONG,
    MONITOR_ERR_CRLF,
    MONITOR_ERR_BOM,
    MONITOR_ERR_NUL,
    MONITOR_ERR_NON_ASCII,
    MONITOR_ERR_NO_MONITOR,
    MONITOR_ERR_MONITOR_DUP,
    MONITOR_ERR_MONITOR_NOT_CUSTOM,
    MONITOR_ERR_UNKNOWN_KEY,
    MONITOR_ERR_RANGE_INDEX,
    MONITOR_ERR_RANGE_DUP,
    MONITOR_ERR_FIELD_COUNT,
    MONITOR_ERR_FIELD_SYNTAX,
    MONITOR_ERR_TRAILING,
    MONITOR_ERR_NO_RANGE,
    MONITOR_ERR_MIN_GT_MAX,
    MONITOR_ERR_ZERO,
    MONITOR_ERR_ENVELOPE,
    MONITOR_ERR_DEAD_BAND,
    MONITOR_ERR_OVERLAP,
    MONITOR_ERR_NO_LOW_BAND,
    MONITOR_ERR_SWITCHRES_RULE,
    MONITOR_ERR_COUNT                           /* sentinel: number of codes */
};

struct monitor_error {
    enum monitor_err code;
    int line;                                   /* 0: file error, no line */
};

/* Built-in text: "monitor custom" then the 15 kHz band, equal to the low
 * band of the JVC monitor file. */
extern const char MONITOR_BUILTIN_TEXT[];

/* The loading functions return 0, or -1 with err filled in and out left
 * UNTOUCHED. err may be NULL. */
int  monitor_parse_text(const char *buf, size_t len, struct monitor *out, struct monitor_error *err);
int  monitor_load_file(const char *path, struct monitor *out, struct monitor_error *err);
int  monitor_load_builtin(struct monitor *out);
const char *monitor_err_str(enum monitor_err e);

/* Bit i = band[i] whose h_max <= H_BAND_LOW_MAX_HZ: the 15 kHz bands, the
 * only ones open to the 240p and 480i regimes. This is NOT the set of
 * bands usable in general: a regime's mask comes from monitor_regime_mask. */
unsigned monitor_low_bands_mask(const struct monitor *m);

/* Bit i = band[i] whose h_min >= H_BAND_HIGH_MIN_HZ: the 31 kHz bands. The
 * loader refuses any band that touches the dead band, so a band is either
 * entirely low or entirely high. */
unsigned monitor_high_bands_mask(const struct monitor *m);

/*
 * The mask of the bands a REGIME may see: 240p and 480i see the low bands
 * only; 480p sees the high bands only; any other regime, and a NULL m,
 * return 0. No regime sees both: the high band opens for 480p alone, never
 * for the legacy path nor for the 15 kHz regimes. `regime` is a value of
 * enum daemon_regime (modelines.h).
 */
unsigned monitor_regime_mask(const struct monitor *m, unsigned regime);

/*
 * The bands the receiver HANDS to switchres: those whose bit is set in
 * `usable`, in file order. idx_out (MONITOR_MAX_BANDS slots) receives their
 * indices in the band[] array; the return value is their count. The file
 * describes more than switchres receives: switchres sets a mode by itself
 * on the legacy start-up path, before the guard, so it must never know a
 * band the guard refuses. The caller renumbers these bands from
 * crt_range0. Bits beyond m->n are ignored; a NULL m returns 0.
 */
unsigned monitor_switchres_bands(const struct monitor *m, unsigned usable,
                                 unsigned idx_out[MONITOR_MAX_BANDS]);

/*
 * Guard. Order: the fixed envelope of safety.h first (on real numbers,
 * without rounding or truncation), then the PAIR (H, V) inside one band
 * whose bit is set in `usable`. band_out receives the index in the array
 * (-1 otherwise).
 */
modeline_result_t monitor_check(const struct monitor *m, double h_hz, double v_hz,
                                unsigned usable, int *band_out);

/*
 * Guard on the mode actually handed to the card: H = clock_khz * 1000 /
 * htotal, V = H / vtotal, x2 when interlaced, /2 when double-scanned. A
 * zero htotal or vtotal is refused without dividing. h_out / v_out /
 * band_out may be NULL.
 */
modeline_result_t monitor_check_mode(const struct monitor *m, uint32_t clock_khz,
                                     uint32_t htotal, uint32_t vtotal, int interlace,
                                     int doublescan, unsigned usable,
                                     double *h_out, double *v_out, int *band_out);

/* Log: one JSON line on stderr, without a fingerprint of the file. */
void monitor_log_loaded(const struct monitor *m, const char *path);
void monitor_log_rejected(const char *path, const struct monitor_error *e);

/*
 * Dry run: `crt-bridge-rcv --check-monitor`. PURE functions -- no I/O
 * outside the FILE received, no DRM, no thread, no socket, no log. One line
 * per fact, `key=value`, ASCII. monitor_report states the bands understood
 * (`usable=` under the low mask, `usable_480p=` under the 480p one), then,
 * for 240p, 480i, 288p and 480p, the band and the mode that would come out
 * under the mask of their regime, or the reason for the refusal. It returns
 * the number of PINNED modes (240p, 480i) the guard would refuse (0, 1 or
 * 2): a file the loader accepts but whose low band excludes a pinned mode
 * stops the daemon with code 3 at start-up, and the exit code of
 * --check-monitor must say so. 288p has no pinned mode. 480p has one (the
 * MODELINE_480P constant), judged under the high band only, but OUTSIDE
 * the count: a CRT without a high band keeps its 480p disabled and keeps
 * its daemon.
 * monitor_report_rejected states a refused file: the loader's reason and
 * the offending line.
 */
int  monitor_report(FILE *out, const struct monitor *m, const char *path);
void monitor_report_rejected(FILE *out, const char *path, const struct monitor_error *e);

#endif /* RECEIVER_MONITOR_H */
