/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * monitor.c -- the CRT described by a file: a strict loader, plus a guard on
 * real numbers.
 *
 * A pure module: no libdrm, no switchres, no dynamic allocation (bounded
 * stack buffers). The text read here is the text the daemon will hand as is
 * to switchres, line by line (monitor_band.text): there is a single parser,
 * and it is this one.
 *
 * Two distinct guards, in this order:
 *   1. the FIXED envelope of safety.h (H_BAND_*, V_*) -- never depends on the file;
 *   2. the band of the file -- it can only narrow the envelope.
 *
 * The loader refuses, with a named reason and the line number, any file that
 * switchres would accept or drop silently: switchres fails open and its
 * return code is 0 in all those cases.
 */

/* POSIX feature gate: no POSIX function here, but the template of safety.c
 * is kept so that both units compile the same way. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "monitor.h"
#include "modelines.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D(x) ((double)(x))

const char MONITOR_BUILTIN_TEXT[] =
    "monitor custom\n"
    "crt_range0 15625-16000, 50-80, 2.000, 4.700, 8.000, 0.064, 0.192, 1.024, 0, 0, 192, 288, 448, 576\n";

/* ------------------------------------------------------------------------- */
/* Parsing a crt_range line: sixteen fields, a closed grammar               */
/* ------------------------------------------------------------------------- */

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* Decimal = [0-9]+(\.[0-9]+)?, checked character by character before strtod:
 * no sign, no exponent, no hexadecimal, no nan/inf. The locale is never
 * touched. Returns 1 and advances *pp, or 0 without changing anything. */
static int scan_decimal(const char **pp, double *out)
{
    const char *p = *pp;
    char tok[64];
    size_t n = 0;
    char *end = NULL;
    double v;

    if (!is_digit(*p))
        return 0;
    while (is_digit(*p)) {
        if (n >= sizeof tok - 1) return 0;
        tok[n++] = *p++;
    }
    if (*p == '.') {
        if (n >= sizeof tok - 1) return 0;
        tok[n++] = *p++;
        if (!is_digit(*p)) return 0;
        while (is_digit(*p)) {
            if (n >= sizeof tok - 1) return 0;
            tok[n++] = *p++;
        }
    }
    tok[n] = '\0';
    errno = 0;
    v = strtod(tok, &end);
    if (end != tok + n || errno != 0 || !isfinite(v))
        return 0;
    *out = v;
    *pp = p;
    return 1;
}

/* Integer = [0-9]+, converted by strtol with checks on the end of the token,
 * on errno and on the range of an int. */
static int scan_int(const char **pp, int *out)
{
    const char *p = *pp;
    char tok[24];
    size_t n = 0;
    char *end = NULL;
    long v;

    if (!is_digit(*p))
        return 0;
    while (is_digit(*p)) {
        if (n >= sizeof tok - 1) return 0;
        tok[n++] = *p++;
    }
    tok[n] = '\0';
    errno = 0;
    v = strtol(tok, &end, 10);
    if (end != tok + n || errno != 0 || v < 0 || v > (long)INT_MAX)
        return 0;
    *out = (int)v;
    *pp = p;
    return 1;
}

/* The sixteen fields: "Hmin-Hmax, Vmin-Vmax, six decimals, six integers",
 * separated by a comma followed by optional spaces, with no space before the
 * comma nor around the dash -- the form switchres's sscanf reads. Parsing is
 * a single pass: the first fault decides the code. */
static enum monitor_err parse_fields(const char *s, struct monitor_band *b)
{
    const char *p = s;
    double d[10];
    int iv[6];
    int k;

    memset(d, 0, sizeof d);
    memset(iv, 0, sizeof iv);

    for (k = 0; k < 14; ++k) {
        if (*p == '\0')
            return MONITOR_ERR_FIELD_COUNT;          /* a field expected, the line ends */
        if (k < 2) {
            if (!scan_decimal(&p, &d[2 * k])) return MONITOR_ERR_FIELD_SYNTAX;
            if (*p != '-') return MONITOR_ERR_FIELD_SYNTAX;
            ++p;
            if (!scan_decimal(&p, &d[2 * k + 1])) return MONITOR_ERR_FIELD_SYNTAX;
        } else if (k < 8) {
            if (!scan_decimal(&p, &d[4 + (k - 2)])) return MONITOR_ERR_FIELD_SYNTAX;
        } else {
            if (!scan_int(&p, &iv[k - 8])) return MONITOR_ERR_FIELD_SYNTAX;
        }
        if (k < 13) {
            if (*p == '\0') return MONITOR_ERR_FIELD_COUNT;
            if (*p != ',') return MONITOR_ERR_FIELD_SYNTAX;
            ++p;
            while (*p == ' ' || *p == '\t') ++p;
        } else {
            if (*p == ',') return MONITOR_ERR_FIELD_COUNT;   /* more than sixteen fields */
            if (*p != '\0') return MONITOR_ERR_TRAILING;     /* anything after the sixteenth */
        }
    }

    b->h_min = d[0];  b->h_max = d[1];
    b->v_min = d[2];  b->v_max = d[3];
    b->hfp_us = d[4]; b->hsync_us = d[5]; b->hbp_us = d[6];
    b->vfp_ms = d[7]; b->vsync_ms = d[8]; b->vbp_ms = d[9];
    b->hsync_pol = iv[0]; b->vsync_pol = iv[1];
    b->prog_min = iv[2];  b->prog_max = iv[3];
    b->ilace_min = iv[4]; b->ilace_max = iv[5];
    return MONITOR_OK;
}

struct parse_state {
    struct monitor *mon;
    int seen_monitor;
    unsigned seen_index;      /* bit N = crt_rangeN already seen */
};

/* A line that is not empty, not a comment, ASCII, 255 bytes at most. */
static enum monitor_err parse_line(const char *ln, int lineno, struct parse_state *st)
{
    size_t klen = strcspn(ln, " \t");
    const char *rest = ln + klen;
    const char *val;

    if (klen == 0)
        return MONITOR_ERR_UNKNOWN_KEY;               /* indented line: switchres would trim it */

    if (klen == 7 && memcmp(ln, "monitor", 7) == 0) {
        if (st->seen_monitor)
            return MONITOR_ERR_MONITOR_DUP;
        val = rest + strspn(rest, " \t");
        if (strcmp(val, "custom") != 0)
            return MONITOR_ERR_MONITOR_NOT_CUSTOM;    /* arcade_15, "= custom", missing value... */
        st->seen_monitor = 1;
        return MONITOR_OK;
    }

    if (klen >= 9 && memcmp(ln, "crt_range", 9) == 0) {
        struct monitor_band *b;
        enum monitor_err fe;
        size_t i;
        int idx;

        for (i = 9; i < klen; ++i)
            if (!is_digit(ln[i]))
                return MONITOR_ERR_UNKNOWN_KEY;       /* crt_range=, crt_rangeX... */
        if (klen == 9)
            return MONITOR_ERR_UNKNOWN_KEY;           /* crt_range without an index */
        if (klen - 9 >= 2)
            return MONITOR_ERR_RANGE_INDEX;           /* crt_range10: index 10 does not exist */
        idx = ln[9] - '0';
        if ((st->seen_index >> idx) & 1u)
            return MONITOR_ERR_RANGE_DUP;
        if (st->mon->n >= MONITOR_MAX_BANDS)
            return MONITOR_ERR_RANGE_INDEX;           /* unreachable: ten indices, once each */

        val = rest + strspn(rest, " \t");
        b = &st->mon->band[st->mon->n];
        memset(b, 0, sizeof *b);
        fe = parse_fields(val, b);
        if (fe != MONITOR_OK)
            return fe;
        b->index = idx;
        b->line = lineno;
        memcpy(b->text, val, strlen(val) + 1);        /* val fits in 255 bytes: so does the line */
        st->seen_index |= 1u << idx;
        st->mon->n++;
        return MONITOR_OK;
    }

    return MONITOR_ERR_UNKNOWN_KEY;
}

/* ------------------------------------------------------------------------- */
/* Consistency rules                                                         */
/* ------------------------------------------------------------------------- */

/*
 * monitor_evaluate_range of switchres 2.2.2 (monitor.cpp), rule for rule,
 * units included (the V porches, written in ms, are divided by 1000 BEFORE
 * the evaluation), and the integer division by 2 of interlaced lines too.
 * Returns 1 if switchres would drop the band silently ("Error in monitor
 * range (ignoring)"), with return code 0.
 */
static int switchres_rejects(const struct monitor_band *b)
{
    double line_time, frame_time, vfp, vsync, vbp, blank;

    if (b->h_min < 14000.0 || b->h_min > 540672.0) return 1;
    if (b->h_max < 14000.0 || b->h_max < b->h_min || b->h_max > 540672.0) return 1;
    if (b->v_min < 40.0 || b->v_min > 200.0) return 1;
    if (b->v_max < 40.0 || b->v_max < b->v_min || b->v_max > 200.0) return 1;

    line_time = 1.0 / b->h_max * 1000000.0;           /* us */
    if (b->hfp_us <= 0.0 || b->hfp_us > line_time) return 1;
    if (b->hsync_us <= 0.0 || b->hsync_us > line_time) return 1;
    if (b->hbp_us <= 0.0 || b->hbp_us > line_time) return 1;

    vfp = b->vfp_ms / 1000.0;
    vsync = b->vsync_ms / 1000.0;
    vbp = b->vbp_ms / 1000.0;
    blank = vfp + vsync + vbp;
    frame_time = 1.0 / b->v_max * 1000.0;
    if (vfp <= 0.0 || vfp > frame_time) return 1;
    if (vsync <= 0.0 || vsync > frame_time) return 1;
    if (vbp <= 0.0 || vbp > frame_time) return 1;

    if (b->hsync_pol != 0 && b->hsync_pol != 1) return 1;
    if (b->vsync_pol != 0 && b->vsync_pol != 1) return 1;

    if (b->prog_min > 0 && b->prog_min < 128) return 1;
    if ((D(b->prog_min) + b->h_max * blank) * b->v_min > b->h_max) return 1;
    if (b->prog_max < b->prog_min) return 1;
    if ((D(b->prog_max) + b->h_max * blank) * b->v_min > b->h_max) return 1;

    if (b->ilace_min != 0) {
        if (b->ilace_min < b->prog_max) return 1;
        if (b->ilace_min < 128 * 2) return 1;
        if ((D(b->ilace_min / 2) + b->h_max * blank) * b->v_min > b->h_max) return 1;
        if (b->ilace_max < b->ilace_min) return 1;
        if ((D(b->ilace_max / 2) + b->h_max * blank) * b->v_min > b->h_max) return 1;
    } else if (b->ilace_max != 0) {
        return 1;
    }
    return 0;
}

/* Consistency of the whole, in this order: per band (ZERO, MIN_GT_MAX,
 * ENVELOPE, DEAD_BAND), then overlap, then one band at 15 kHz, then the
 * switchres rules. *line = line of the offending band, 0 for NO_LOW_BAND. */
static enum monitor_err check_bands(const struct monitor *m, int *line)
{
    unsigned i, j;
    int has_low = 0;

    for (i = 0; i < m->n; ++i) {
        const struct monitor_band *b = &m->band[i];
        *line = b->line;
        if (b->h_min <= 0.0 || b->h_max <= 0.0 || b->v_min <= 0.0 || b->v_max <= 0.0)
            return MONITOR_ERR_ZERO;
        if (b->h_min > b->h_max || b->v_min > b->v_max)
            return MONITOR_ERR_MIN_GT_MAX;
        /* A band fits ENTIRELY inside the fixed envelope of safety.h ... */
        if (b->h_min < D(H_BAND_LOW_MIN_HZ) || b->h_max > D(H_BAND_HIGH_MAX_HZ) ||
            b->v_min < D(V_MIN_HZ) || b->v_max > D(V_MAX_HZ))
            return MONITOR_ERR_ENVELOPE;
        /* ... and never touches the dead band (16,000 stays valid, so does 27,000). */
        if (b->h_max > D(H_BAND_LOW_MAX_HZ) && b->h_min < D(H_BAND_HIGH_MIN_HZ))
            return MONITOR_ERR_DEAD_BAND;
    }

    for (j = 1; j < m->n; ++j) {
        for (i = 0; i < j; ++i) {
            double lo = m->band[i].h_min > m->band[j].h_min ? m->band[i].h_min : m->band[j].h_min;
            double hi = m->band[i].h_max < m->band[j].h_max ? m->band[i].h_max : m->band[j].h_max;
            if (lo <= hi) {                           /* closed intervals: a single shared Hz is enough */
                *line = m->band[j].line;
                return MONITOR_ERR_OVERLAP;
            }
        }
    }

    for (i = 0; i < m->n; ++i)
        if (m->band[i].h_max <= D(H_BAND_LOW_MAX_HZ))
            has_low = 1;
    if (!has_low) {
        *line = 0;
        return MONITOR_ERR_NO_LOW_BAND;
    }

    for (i = 0; i < m->n; ++i) {
        if (switchres_rejects(&m->band[i])) {
            *line = m->band[i].line;
            return MONITOR_ERR_SWITCHRES_RULE;
        }
    }
    return MONITOR_OK;
}

/* ------------------------------------------------------------------------- */
/* Loading                                                                   */
/* ------------------------------------------------------------------------- */

static int fail(struct monitor_error *err, enum monitor_err code, int line)
{
    if (err != NULL) {
        err->code = code;
        err->line = line;
    }
    return -1;
}

int monitor_parse_text(const char *buf, size_t len, struct monitor *out, struct monitor_error *err)
{
    struct monitor tmp;
    struct parse_state st;
    size_t pos, i;
    int lineno;

    if (buf == NULL || out == NULL || len == 0)
        return fail(err, MONITOR_ERR_EMPTY, 0);
    if (len > MONITOR_MAX_FILE_BYTES)
        return fail(err, MONITOR_ERR_TOO_BIG, 0);
    if (len >= 3 && (unsigned char)buf[0] == 0xEFU && (unsigned char)buf[1] == 0xBBU &&
        (unsigned char)buf[2] == 0xBFU)
        return fail(err, MONITOR_ERR_BOM, 1);

    /* Bytes first: NUL, then \r (Windows line endings). */
    lineno = 1;
    for (i = 0; i < len; ++i) {
        if (buf[i] == '\0') return fail(err, MONITOR_ERR_NUL, lineno);
        if (buf[i] == '\n') ++lineno;
    }
    lineno = 1;
    for (i = 0; i < len; ++i) {
        if (buf[i] == '\r') return fail(err, MONITOR_ERR_CRLF, lineno);
        if (buf[i] == '\n') ++lineno;
    }

    memset(&tmp, 0, sizeof tmp);
    memset(&st, 0, sizeof st);
    st.mon = &tmp;
    tmp.file_bytes = len;
    tmp.from_file = 0;

    pos = 0;
    lineno = 0;
    while (pos < len) {
        char ln[MONITOR_MAX_LINE_BYTES + 1];
        size_t start = pos, llen;
        enum monitor_err pe;

        while (pos < len && buf[pos] != '\n') ++pos;
        llen = pos - start;
        ++lineno;
        if (pos < len) ++pos;                          /* skip the \n; the last line may lack one */

        if (llen > MONITOR_MAX_LINE_BYTES)
            return fail(err, MONITOR_ERR_LINE_TOO_LONG, lineno);
        if (llen == 0 || buf[start] == '#')
            continue;                                  /* empty, or a comment: non-ASCII allowed there */
        for (i = 0; i < llen; ++i)
            if ((unsigned char)buf[start + i] > 0x7FU)
                return fail(err, MONITOR_ERR_NON_ASCII, lineno);
        memcpy(ln, buf + start, llen);
        ln[llen] = '\0';
        if (strspn(ln, " \t") == llen)
            continue;                                  /* blank: switchres trims it too */

        pe = parse_line(ln, lineno, &st);
        if (pe != MONITOR_OK)
            return fail(err, pe, lineno);
    }

    if (!st.seen_monitor)
        return fail(err, MONITOR_ERR_NO_MONITOR, 0);
    if (tmp.n == 0)
        return fail(err, MONITOR_ERR_NO_RANGE, 0);
    {
        int bad_line = 0;
        enum monitor_err ce = check_bands(&tmp, &bad_line);
        if (ce != MONITOR_OK)
            return fail(err, ce, bad_line);
    }

    *out = tmp;
    return 0;
}

int monitor_load_file(const char *path, struct monitor *out, struct monitor_error *err)
{
    unsigned char buf[MONITOR_MAX_FILE_BYTES + 1];
    struct monitor tmp;
    struct monitor_error e = { MONITOR_OK, 0 };
    FILE *f;
    size_t n;
    int bad;

    if (path == NULL || path[0] == '\0' || out == NULL)
        return fail(err, MONITOR_ERR_IO, 0);
    f = fopen(path, "rb");
    if (f == NULL)
        return fail(err, MONITOR_ERR_IO, 0);
    n = fread(buf, 1, sizeof buf, f);                  /* one byte more than the maximum: TOO_BIG */
    bad = ferror(f);
    fclose(f);
    if (bad)
        return fail(err, MONITOR_ERR_IO, 0);            /* a directory opens under Linux and fails here */

    if (monitor_parse_text((const char *)buf, n, &tmp, &e) != 0) {
        if (err != NULL)
            *err = e;
        return -1;
    }
    tmp.from_file = 1;
    tmp.file_bytes = n;
    *out = tmp;
    return 0;
}

int monitor_load_builtin(struct monitor *out)
{
    return monitor_parse_text(MONITOR_BUILTIN_TEXT, strlen(MONITOR_BUILTIN_TEXT), out, NULL);
}

const char *monitor_err_str(enum monitor_err e)
{
    switch (e) {
    case MONITOR_OK:                    return "OK";
    case MONITOR_ERR_IO:                return "IO (file missing, unreadable or not a regular file)";
    case MONITOR_ERR_EMPTY:             return "EMPTY (no byte)";
    case MONITOR_ERR_TOO_BIG:           return "TOO_BIG (more than 4096 bytes)";
    case MONITOR_ERR_LINE_TOO_LONG:     return "LINE_TOO_LONG (more than 255 bytes)";
    case MONITOR_ERR_CRLF:              return "CRLF (Windows line endings: LF only)";
    case MONITOR_ERR_BOM:               return "BOM (UTF-8 byte order mark)";
    case MONITOR_ERR_NUL:               return "NUL (zero byte in file)";
    case MONITOR_ERR_NON_ASCII:         return "NON_ASCII (byte above 0x7F outside a comment)";
    case MONITOR_ERR_NO_MONITOR:        return "NO_MONITOR (no 'monitor custom' line)";
    case MONITOR_ERR_MONITOR_DUP:       return "MONITOR_DUP (monitor line twice)";
    case MONITOR_ERR_MONITOR_NOT_CUSTOM: return "MONITOR_NOT_CUSTOM (only 'monitor custom' is read)";
    case MONITOR_ERR_UNKNOWN_KEY:       return "UNKNOWN_KEY (only 'monitor' and 'crt_range0'..'crt_range9')";
    case MONITOR_ERR_RANGE_INDEX:       return "RANGE_INDEX (crt_range index above 9)";
    case MONITOR_ERR_RANGE_DUP:         return "RANGE_DUP (same crt_range index twice)";
    case MONITOR_ERR_FIELD_COUNT:       return "FIELD_COUNT (a crt_range line has exactly 16 fields)";
    case MONITOR_ERR_FIELD_SYNTAX:      return "FIELD_SYNTAX (field is not a plain decimal)";
    case MONITOR_ERR_TRAILING:          return "TRAILING (text after the 16th field)";
    case MONITOR_ERR_NO_RANGE:          return "NO_RANGE (no crt_range line)";
    case MONITOR_ERR_MIN_GT_MAX:        return "MIN_GT_MAX (a minimum above its maximum)";
    case MONITOR_ERR_ZERO:              return "ZERO (a frequency of zero)";
    case MONITOR_ERR_ENVELOPE:          return "ENVELOPE (band outside the fixed envelope of safety.h)";
    case MONITOR_ERR_DEAD_BAND:         return "DEAD_BAND (band touches the 16-27 kHz dead band)";
    case MONITOR_ERR_OVERLAP:           return "OVERLAP (two bands share a frequency)";
    case MONITOR_ERR_NO_LOW_BAND:       return "NO_LOW_BAND (no band within 15.625-16 kHz)";
    case MONITOR_ERR_SWITCHRES_RULE:    return "SWITCHRES_RULE (switchres would drop this band silently)";
    case MONITOR_ERR_COUNT:             break;
    }
    return "UNKNOWN";
}

unsigned monitor_low_bands_mask(const struct monitor *m)
{
    unsigned mask = 0, i;

    if (m == NULL)
        return 0;
    for (i = 0; i < m->n && i < MONITOR_MAX_BANDS; ++i)
        if (m->band[i].h_max <= D(H_BAND_LOW_MAX_HZ))
            mask |= 1u << i;
    return mask;
}

unsigned monitor_high_bands_mask(const struct monitor *m)
{
    unsigned mask = 0, i;

    if (m == NULL)
        return 0;
    for (i = 0; i < m->n && i < MONITOR_MAX_BANDS; ++i)
        if (m->band[i].h_min >= D(H_BAND_HIGH_MIN_HZ))
            mask |= 1u << i;
    return mask;
}

/* One mask PER regime, never the union of the two. */
unsigned monitor_regime_mask(const struct monitor *m, unsigned regime)
{
    if (m == NULL)
        return 0;
    if (regime == (unsigned)REGIME_240P_SUPER_RES || regime == (unsigned)REGIME_480I)
        return monitor_low_bands_mask(m);
    if (regime == (unsigned)REGIME_480P)
        return monitor_high_bands_mask(m);
    return 0;
}

unsigned monitor_switchres_bands(const struct monitor *m, unsigned usable,
                                 unsigned idx_out[MONITOR_MAX_BANDS])
{
    unsigned n = 0, i;

    if (m == NULL)
        return 0;
    for (i = 0; i < m->n && i < MONITOR_MAX_BANDS; ++i) {
        if (((usable >> i) & 1u) == 0u)
            continue;
        idx_out[n] = i;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Guard                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * monitor_check -- verdict on a point (H, V) in REAL Hz.
 *
 * Order: (1) the fixed envelope of safety.h, judged first and on its own,
 * without the file; (2) only then, the PAIR (H, V) inside one band whose bit
 * is set in `usable`. So the file can narrow the envelope, never widen it:
 * even a forged band array wider than the envelope lets nothing more
 * through. No conversion to integer, no rounding, no margin. The comparisons
 * are written so that NaN is refused.
 */
modeline_result_t monitor_check(const struct monitor *m, double h_hz, double v_hz,
                                unsigned usable, int *band_out)
{
    unsigned i, n;
    int h_seen = 0;

    if (band_out != NULL)
        *band_out = -1;

    if (!(h_hz >= D(H_BAND_LOW_MIN_HZ)))                       /* NaN too */
        return MODELINE_REJECT_H_LOW;
    if (h_hz > D(H_BAND_LOW_MAX_HZ) && h_hz < D(H_BAND_HIGH_MIN_HZ))
        return MODELINE_REJECT_H_DEAD_BAND;
    if (h_hz > D(H_BAND_HIGH_MAX_HZ))
        return MODELINE_REJECT_H_HIGH;
    if (!(v_hz >= D(V_MIN_HZ) && v_hz <= D(V_MAX_HZ)))         /* NaN too */
        return MODELINE_REJECT_V;

    if (m == NULL)
        return MODELINE_REJECT_BAND;
    n = m->n < MONITOR_MAX_BANDS ? m->n : MONITOR_MAX_BANDS;
    for (i = 0; i < n; ++i) {
        const struct monitor_band *b = &m->band[i];
        if (!((usable >> i) & 1u))
            continue;
        if (h_hz < b->h_min || h_hz > b->h_max)
            continue;
        h_seen = 1;
        if (v_hz >= b->v_min && v_hz <= b->v_max) {            /* the same band for H AND V */
            if (band_out != NULL)
                *band_out = (int)i;
            return MODELINE_ACCEPT;
        }
    }
    return h_seen ? MODELINE_REJECT_V : MODELINE_REJECT_BAND;
}

modeline_result_t monitor_check_mode(const struct monitor *m, uint32_t clock_khz,
                                     uint32_t htotal, uint32_t vtotal, int interlace,
                                     int doublescan, unsigned usable,
                                     double *h_out, double *v_out, int *band_out)
{
    double h = 0.0, v = 0.0;
    modeline_result_t r;

    if (htotal == 0U || vtotal == 0U) {
        if (h_out != NULL) *h_out = 0.0;
        if (v_out != NULL) *v_out = 0.0;
        if (band_out != NULL) *band_out = -1;
        return htotal == 0U ? MODELINE_REJECT_H_LOW : MODELINE_REJECT_V;   /* no division */
    }
    /* The clock is already truncated to kHz by pinned_modeline_to_drm: this
     * is the mode ACTUALLY handed to the card. Same rule as drm_mode_vrefresh. */
    h = D(clock_khz) * 1000.0 / D(htotal);
    v = h / D(vtotal);
    if (interlace)
        v *= 2.0;
    if (doublescan)
        v /= 2.0;
    if (h_out != NULL) *h_out = h;
    if (v_out != NULL) *v_out = v;
    r = monitor_check(m, h, v, usable, band_out);
    return r;
}

/* ------------------------------------------------------------------------- */
/* Log                                                                       */
/* ------------------------------------------------------------------------- */

/* A path made safe for a JSON line: anything outside [A-Za-z0-9_./-] becomes '?', bounded length. */
static void sanitize_path(const char *path, char *dst, size_t cap)
{
    size_t n = 0;

    if (path != NULL) {
        for (; *path != '\0' && n < cap - 1; ++path) {
            char c = *path;
            int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || is_digit(c) ||
                     c == '_' || c == '.' || c == '/' || c == '-';
            dst[n++] = ok ? c : '?';
        }
    }
    dst[n] = '\0';
}

void monitor_log_loaded(const struct monitor *m, const char *path)
{
    char p[128];
    unsigned mask, mask480p, i;

    if (m == NULL)
        return;
    sanitize_path(path, p, sizeof p);
    /* `usable_mask` keeps its original meaning (the low bands); the 480p mask
     * is added beside it, never in its place. */
    mask = monitor_regime_mask(m, (unsigned)REGIME_240P_SUPER_RES);
    mask480p = monitor_regime_mask(m, (unsigned)REGIME_480P);
    fprintf(stderr,
            "{\"event\":\"monitor_loaded\",\"source\":\"%s\",\"path\":\"%s\","
            "\"bytes\":%lu,\"n_bands\":%u,\"usable_mask\":%u,\"usable_mask_480p\":%u,\"bands\":[",
            m->from_file ? "file" : "builtin", p, (unsigned long)m->file_bytes, m->n, mask,
            mask480p);
    for (i = 0; i < m->n && i < MONITOR_MAX_BANDS; ++i) {
        const struct monitor_band *b = &m->band[i];
        fprintf(stderr,
                "%s{\"i\":%u,\"key\":\"crt_range%d\",\"line\":%d,"
                "\"h_min\":%.2f,\"h_max\":%.2f,\"v_min\":%.2f,\"v_max\":%.2f,"
                "\"usable\":%u,\"usable_480p\":%u}",
                i ? "," : "", i, b->index, b->line,
                b->h_min, b->h_max, b->v_min, b->v_max, (mask >> i) & 1u, (mask480p >> i) & 1u);
    }
    fprintf(stderr, "]}\n");
}

void monitor_log_rejected(const char *path, const struct monitor_error *e)
{
    char p[128];

    if (e == NULL)
        return;
    sanitize_path(path, p, sizeof p);
    fprintf(stderr,
            "{\"event\":\"monitor_rejected\",\"path\":\"%s\",\"reason\":\"%s\","
            "\"code\":%d,\"line\":%d}\n",
            p, monitor_err_str(e->code), (int)e->code, e->line);
}

/* ------------------------------------------------------------------------- */
/* Dry run                                                                   */
/* ------------------------------------------------------------------------- */

/* The first word of safety_reason_str: ACCEPT, H_LOW, H_DEAD_BAND, H_HIGH,
 * V_OUT_OF_RANGE or BAND. */
static void verdict_word(modeline_result_t r, char *dst, size_t cap)
{
    const char *s = safety_reason_str(r);
    size_t n = strcspn(s, " ");

    if (n >= cap)
        n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = '\0';
}

/*
 * One `source` line. `pinned` is the pinned mode of modelines.h for this source
 * (240p, 480i, 480p), NULL for the one that has none (288p): the report never
 * pretends a mode exists where the code knows none. `fatal` says whether refusing
 * a pinned mode stops the daemon (240p, 480i: yes; 480p: no, the regime is
 * disabled). `host` is the band that would take the timing with ALL bands open;
 * it is stated only when the verdict is not ACCEPT.
 */
static void report_source(FILE *out, const struct monitor *m, const char *name,
                          double h, double v, modeline_result_t r, int band,
                          const struct pinned_modeline *pinned, const char *regime,
                          int fatal)
{
    char word[24];

    verdict_word(r, word, sizeof word);
    fprintf(out, "source %s: h_hz=%.2f v_hz=%.2f verdict=%s", name, h, v, word);

    if (r == MODELINE_ACCEPT) {
        fprintf(out, " band=%d", band);
        if (pinned != NULL)
            fprintf(out, " mode=%dx%d%s regime=%s\n", pinned->width, pinned->height,
                    pinned->interlace ? "i" : "", regime);
        else
            fprintf(out, " mode=none (no pinned mode: band only)\n");
        return;
    }

    {
        int host = -1;
        const unsigned all = (1u << m->n) - 1u;

        if (monitor_check(m, h, v, all, &host) != MODELINE_ACCEPT)
            host = -1;
        if (host >= 0)
            fprintf(out, " host=%d", host);
        else
            fprintf(out, " host=none");
        if (pinned != NULL && fatal)
            fprintf(out, " mode=none (pinned mode refused: the daemon would stop with code 3)\n");
        else if (pinned != NULL)
            fprintf(out, " mode=none (regime %s disabled)\n", regime);
        else if (host >= 0)
            fprintf(out, " mode=none (band described, outside the mask of its regime)\n");
        else
            fprintf(out, " mode=none (no band of the file takes it)\n");
    }
}

int monitor_report(FILE *out, const struct monitor *m, const char *path)
{
    const unsigned mask = monitor_low_bands_mask(m);
    const unsigned mask_240p = monitor_regime_mask(m, (unsigned)REGIME_240P_SUPER_RES);
    const unsigned mask_480i = monitor_regime_mask(m, (unsigned)REGIME_480I);
    const unsigned mask_480p = monitor_regime_mask(m, (unsigned)REGIME_480P);
    unsigned i;
    double h = 0.0, v = 0.0;
    int band = -1;
    int refused_pinned = 0;
    modeline_result_t r;

    if (out == NULL || m == NULL)
        return 0;

    fprintf(out, "monitor: %s bytes=%lu bands=%u\n", path != NULL ? path : "",
            (unsigned long)m->file_bytes, m->n);
    for (i = 0; i < m->n && i < MONITOR_MAX_BANDS; ++i) {
        const struct monitor_band *b = &m->band[i];
        fprintf(out, "band %u: key=crt_range%d line=%d h=%.2f-%.2f v=%.2f-%.2f usable=%s usable_480p=%s\n",
                i, b->index, b->line, b->h_min, b->h_max, b->v_min, b->v_max,
                ((mask >> i) & 1u) ? "yes" : "no", ((mask_480p >> i) & 1u) ? "yes" : "no");
    }

    /* 240p and 480i: the pinned modes of modelines.h, judged as the daemon hands
     * them to the card (clock already in kHz), under the mask of their regime. */
    r = monitor_check_mode(m, MODELINE_240P_SR_NTSC.pclock_hz / 1000U,
                           MODELINE_240P_SR_NTSC.h_total, MODELINE_240P_SR_NTSC.v_total,
                           MODELINE_240P_SR_NTSC.interlace, 0, mask_240p, &h, &v, &band);
    report_source(out, m, "240p", h, v, r, band, &MODELINE_240P_SR_NTSC, "240p_super_res", 1);
    if (r != MODELINE_ACCEPT)
        ++refused_pinned;

    r = monitor_check_mode(m, MODELINE_480I_NTSC.pclock_hz / 1000U,
                           MODELINE_480I_NTSC.h_total, MODELINE_480I_NTSC.v_total,
                           MODELINE_480I_NTSC.interlace, 0, mask_480i, &h, &v, &band);
    report_source(out, m, "480i", h, v, r, band, &MODELINE_480I_NTSC, "480i", 1);
    if (r != MODELINE_ACCEPT)
        ++refused_pinned;

    /* 288p: the PAL reference timing, 15,625 Hz and 312 lines, under the low mask
     * (the PAL regime is reserved: no pinned mode, no mask of its own). */
    h = 15625.0;
    v = 15625.0 / 312.0;
    r = monitor_check(m, h, v, mask, &band);
    report_source(out, m, "288p", h, v, r, band, NULL, NULL, 0);

    /* 480p: the CONSTANT of modelines.h, judged under the high band only. A
     * refusal disables the regime, it does not stop the daemon: outside the count. */
    r = monitor_check_mode(m, MODELINE_480P.pclock_hz / 1000U,
                           MODELINE_480P.h_total, MODELINE_480P.v_total,
                           MODELINE_480P.interlace, 0, mask_480p, &h, &v, &band);
    report_source(out, m, "480p", h, v, r, band, &MODELINE_480P, "480p", 0);
    return refused_pinned;
}

void monitor_report_rejected(FILE *out, const char *path, const struct monitor_error *e)
{
    if (out == NULL || e == NULL)
        return;
    fprintf(out, "monitor: %s REFUSED reason=%s line=%d\n", path != NULL ? path : "",
            monitor_err_str(e->code), e->line);
}
