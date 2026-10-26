/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * stickcfg.c -- normalization of the files a user writes on the USB stick,
 * reading of settings.ini, and composition of the daemon environment.
 *
 * Pure module: no I/O, no dynamic allocation, bounded buffers only. See
 * stickcfg.h for the contract; crt-bridge-config.c does the file work.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "stickcfg.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Longest piece of a user value echoed into the report. */
#define ECHO_MAX 64

/* ------------------------------------------------------------------------- */
/* Normalization                                                             */
/* ------------------------------------------------------------------------- */

static enum stickcfg_norm_status refuse(struct stickcfg_norm *info, enum stickcfg_norm_status st,
                                        size_t offset, unsigned char byte)
{
    info->status = st;
    info->offset = offset;
    info->byte = byte;
    return st;
}

enum stickcfg_norm_status stickcfg_normalize(const unsigned char *in, size_t in_len,
                                             unsigned char *out, size_t *out_len,
                                             struct stickcfg_norm *info)
{
    size_t i = 0, o = 0, end = in_len;

    memset(info, 0, sizeof *info);
    info->status = STICKCFG_NORM_OK;
    *out_len = 0;

    if (in_len > STICKCFG_MAX_FILE_BYTES)
        return refuse(info, STICKCFG_NORM_TOO_BIG, STICKCFG_MAX_FILE_BYTES, 0);
    /* Notepad's "Unicode" is UTF-16: refused by name rather than as a NUL. */
    if (in_len >= 2 && ((in[0] == 0xFF && in[1] == 0xFE) || (in[0] == 0xFE && in[1] == 0xFF)))
        return refuse(info, STICKCFG_NORM_UTF16, 0, in[0]);
    if (in_len >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF) {
        info->bom = 1;
        i = 3;
    }
    /* Old DOS editors end a text file with Ctrl-Z; only the very last byte. */
    if (end > i && in[end - 1] == 0x1A) {
        info->eof_mark = 1;
        end--;
    }

    for (; i < end; i++) {
        unsigned char c = in[i];
        if (c == '\r') {
            out[o++] = '\n';
            if (i + 1 < end && in[i + 1] == '\n') {
                i++;
                info->crlf++;
            } else {
                info->cr++;
            }
            continue;
        }
        if (c == 0)
            return refuse(info, STICKCFG_NORM_NUL, i, c);
        if ((c < 0x20 && c != '\t' && c != '\n') || c == 0x7F)
            return refuse(info, STICKCFG_NORM_CONTROL, i, c);
        out[o++] = c;
    }
    *out_len = o;
    return STICKCFG_NORM_OK;
}

static void append_item(char *buf, size_t cap, size_t *len, const char *item)
{
    int w = snprintf(buf + *len, cap - *len, "%s%s", *len ? ", " : "", item);
    if (w < 0) return;
    if ((size_t)w >= cap - *len) *len = cap - 1;
    else *len += (size_t)w;
}

void stickcfg_norm_describe(const struct stickcfg_norm *info, char *buf, size_t cap)
{
    size_t len = 0;

    if (cap == 0) return;
    buf[0] = '\0';
    switch (info->status) {
    case STICKCFG_NORM_OK:
        if (info->crlf) append_item(buf, cap, &len, "CRLF converted");
        if (info->cr) append_item(buf, cap, &len, "CR converted");
        if (info->bom) append_item(buf, cap, &len, "BOM removed");
        if (info->eof_mark) append_item(buf, cap, &len, "end-of-file mark removed");
        if (len == 0) snprintf(buf, cap, "unchanged");
        break;
    case STICKCFG_NORM_TOO_BIG:
        snprintf(buf, cap, "larger than %d bytes", STICKCFG_MAX_FILE_BYTES);
        break;
    case STICKCFG_NORM_UTF16:
        snprintf(buf, cap, "UTF-16 text, save as UTF-8");
        break;
    case STICKCFG_NORM_NUL:
        snprintf(buf, cap, "NUL byte at offset %zu, save as UTF-8 text", info->offset);
        break;
    case STICKCFG_NORM_CONTROL:
        snprintf(buf, cap, "control character 0x%02X at offset %zu", (unsigned)info->byte, info->offset);
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Report                                                                    */
/* ------------------------------------------------------------------------- */

static const char TRUNCATED_LINE[] = "report: truncated\n";

void stickcfg_report_init(struct stickcfg_report *r)
{
    r->len = 0;
    r->truncated = 0;
    r->text[0] = '\0';
}

void stickcfg_report_line(struct stickcfg_report *r, const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    int w = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (w < 0) return;

    size_t n = strlen(line);
    /* The report is read on any machine: printable ASCII only, one line each. */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)line[i];
        if (c < 0x20 || c > 0x7E) line[i] = '?';
    }
    /* The tail of the buffer is kept for the one line saying lines were lost. */
    if (r->len + n + 2 > sizeof r->text - sizeof TRUNCATED_LINE) {
        if (!r->truncated) {
            memcpy(r->text + r->len, TRUNCATED_LINE, sizeof TRUNCATED_LINE);
            r->len += sizeof TRUNCATED_LINE - 1;
        }
        r->truncated = 1;
        return;
    }
    memcpy(r->text + r->len, line, n);
    r->len += n;
    r->text[r->len++] = '\n';
    r->text[r->len] = '\0';
}

/* ------------------------------------------------------------------------- */
/* settings.ini                                                              */
/* ------------------------------------------------------------------------- */

enum key {
    KEY_VOLUME,
    KEY_IDLE_AFTER,
    KEY_IDLE_SCREEN,
    KEY_AUDIO,
    KEY_INPUTS,
    KEY_EXTRA_OPTIONS,
    KEY_COUNT
};

static const char *const KEY_NAMES[KEY_COUNT] = {
    "volume", "idle_after", "idle_screen", "audio", "inputs", "extra_options",
};

void stickcfg_settings_init(struct stickcfg_settings *s)
{
    s->volume = -1;
    s->idle_after = -1;
    s->idle_black = 0;
    s->no_audio = 0;
    s->no_input = 0;
    s->extra[0] = '\0';
}

static int is_blank(char c) { return c == ' ' || c == '\t'; }

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* Characters allowed in a DAEMON_OPTS token. */
static int token_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '/' || c == '=' || c == ':' || c == ',' || c == '+' || c == '-';
}

static void trim(const char **p, size_t *n)
{
    while (*n && is_blank((*p)[0])) { (*p)++; (*n)--; }
    while (*n && is_blank((*p)[*n - 1])) (*n)--;
}

static int eq_nocase(const char *p, size_t n, const char *word)
{
    if (n != strlen(word)) return 0;
    for (size_t i = 0; i < n; i++)
        if (lower(p[i]) != word[i]) return 0;
    return 1;
}

static int echo_len(size_t n) { return (int)(n > ECHO_MAX ? ECHO_MAX : n); }

/* Decimal digits only, at most max_digits of them. */
static int parse_uint(const char *p, size_t n, size_t max_digits, int *out)
{
    int v = 0;
    if (n == 0 || n > max_digits) return -1;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
    }
    *out = v;
    return 0;
}

/*
 * extra_options: long options only, each optionally followed by ONE value
 * (`--shed-window 2`). Every token stays within token_char, so nothing can
 * reach the environment but plain words; the daemon judges the options.
 * On success the tokens are written to out joined by single spaces.
 */
static int parse_extra(const char *v, size_t n, char *out, size_t cap, char *why, size_t whycap)
{
    char tmp[STICKCFG_MAX_FILE_BYTES + 1];
    size_t i = 0, o = 0;
    int may_take_value = 0;

    while (i < n) {
        while (i < n && is_blank(v[i])) i++;
        if (i >= n) break;
        size_t t = i;
        while (i < n && !is_blank(v[i])) i++;
        const char *tok = v + t;
        size_t tn = i - t;

        for (size_t k = 0; k < tn; k++) {
            if (!token_char(tok[k])) {
                snprintf(why, whycap, "character '%c' not allowed", tok[k]);
                return -1;
            }
        }
        if (tn >= 2 && tok[0] == '-' && tok[1] == '-') {
            if (tn == 2) {
                snprintf(why, whycap, "'--' alone is not an option");
                return -1;
            }
            may_take_value = memchr(tok, '=', tn) == NULL;
        } else if (tok[0] == '-') {
            snprintf(why, whycap, "'%.*s' is not a long option", echo_len(tn), tok);
            return -1;
        } else if (may_take_value) {
            may_take_value = 0;
        } else {
            snprintf(why, whycap, "'%.*s' does not start with '--'", echo_len(tn), tok);
            return -1;
        }
        if (o + (o ? 1 : 0) + tn + 1 > sizeof tmp || o + (o ? 1 : 0) + tn + 1 > cap) {
            snprintf(why, whycap, "too long");
            return -1;
        }
        if (o) tmp[o++] = ' ';
        memcpy(tmp + o, tok, tn);
        o += tn;
    }
    tmp[o] = '\0';
    memcpy(out, tmp, o + 1);
    return 0;
}

static void invalid(struct stickcfg_report *r, const char *key, const char *v, size_t vn, const char *hint)
{
    stickcfg_report_line(r, "settings: invalid value for %s: %.*s (%s), ignored", key, echo_len(vn), v, hint);
}

/* Returns 0 for on/text, 1 for off/black, -1 otherwise. */
static int parse_word(const char *v, size_t vn, const char *zero, const char *one)
{
    if (eq_nocase(v, vn, zero)) return 0;
    if (eq_nocase(v, vn, one)) return 1;
    return -1;
}

static void apply_key(enum key k, const char *v, size_t vn,
                      struct stickcfg_settings *s, struct stickcfg_report *r)
{
    int x = 0;
    char why[128];

    switch (k) {
    case KEY_VOLUME:
        if (parse_uint(v, vn, 3, &x) != 0 || x > STICKCFG_VOLUME_MAX) {
            invalid(r, "volume", v, vn, "0 to 100");
            return;
        }
        s->volume = x;
        stickcfg_report_line(r, "settings: volume = %d", x);
        return;
    case KEY_IDLE_AFTER:
        if (parse_uint(v, vn, 4, &x) != 0 ||
            (x != 0 && (x < STICKCFG_IDLE_AFTER_MIN || x > STICKCFG_IDLE_AFTER_MAX))) {
            char hint[64];
            snprintf(hint, sizeof hint, "0, or %d to %d seconds", STICKCFG_IDLE_AFTER_MIN, STICKCFG_IDLE_AFTER_MAX);
            invalid(r, "idle_after", v, vn, hint);
            return;
        }
        s->idle_after = x;
        stickcfg_report_line(r, "settings: idle_after = %d", x);
        return;
    case KEY_IDLE_SCREEN:
        x = parse_word(v, vn, "text", "black");
        if (x < 0) { invalid(r, "idle_screen", v, vn, "text or black"); return; }
        s->idle_black = x;
        stickcfg_report_line(r, "settings: idle_screen = %s", x ? "black" : "text");
        return;
    case KEY_AUDIO:
        x = parse_word(v, vn, "on", "off");
        if (x < 0) { invalid(r, "audio", v, vn, "on or off"); return; }
        s->no_audio = x;
        stickcfg_report_line(r, "settings: audio = %s", x ? "off" : "on");
        return;
    case KEY_INPUTS:
        x = parse_word(v, vn, "on", "off");
        if (x < 0) { invalid(r, "inputs", v, vn, "on or off"); return; }
        s->no_input = x;
        stickcfg_report_line(r, "settings: inputs = %s", x ? "off" : "on");
        return;
    case KEY_EXTRA_OPTIONS:
        if (parse_extra(v, vn, s->extra, sizeof s->extra, why, sizeof why) != 0) {
            stickcfg_report_line(r, "settings: invalid value for extra_options: %.*s (%s), nothing passed",
                                 echo_len(vn), v, why);
            return;
        }
        stickcfg_report_line(r, "settings: extra_options = %s", s->extra[0] ? s->extra : "(none)");
        return;
    case KEY_COUNT:
        break;
    }
}

void stickcfg_parse_settings(const char *text, size_t len,
                             struct stickcfg_settings *s, struct stickcfg_report *r)
{
    int seen[KEY_COUNT] = { 0 };
    size_t pos = 0;

    while (pos < len) {
        size_t eol = pos;
        while (eol < len && text[eol] != '\n') eol++;
        const char *line = text + pos;
        size_t n = eol - pos;
        pos = eol + 1;

        trim(&line, &n);
        if (n == 0 || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            stickcfg_report_line(r, "settings: section header ignored: %.*s", echo_len(n), line);
            continue;
        }
        const char *eq = memchr(line, '=', n);
        if (!eq) {
            stickcfg_report_line(r, "settings: line without '=' ignored: %.*s", echo_len(n), line);
            continue;
        }
        const char *key = line;
        size_t kn = (size_t)(eq - line);
        const char *val = eq + 1;
        size_t vn = n - kn - 1;
        trim(&key, &kn);
        trim(&val, &vn);
        if (kn == 0) {
            stickcfg_report_line(r, "settings: line without a key ignored: %.*s", echo_len(n), line);
            continue;
        }
        int k = -1;
        for (int i = 0; i < KEY_COUNT; i++)
            if (eq_nocase(key, kn, KEY_NAMES[i])) k = i;
        if (k < 0) {
            stickcfg_report_line(r, "settings: unknown key: %.*s", echo_len(kn), key);
            continue;
        }
        if (seen[k])
            stickcfg_report_line(r, "settings: duplicate key: %s, the last valid value is used", KEY_NAMES[k]);
        seen[k] = 1;
        apply_key((enum key)k, val, vn, s, r);
    }
}

/* ------------------------------------------------------------------------- */
/* monitor.ini in developer mode                                             */
/* ------------------------------------------------------------------------- */

const char *stickcfg_developer_monitor(int stick_has_file, const char *builtin,
                                       struct stickcfg_report *r)
{
    /* The bench measures against the tube the image was built for: a file
     * left on the stick must not change it silently, nor pass unnoticed. */
    if (stick_has_file)
        stickcfg_report_line(r, "monitor: developer mode, stick file ignored");
    stickcfg_report_line(r, "monitor: developer mode, built-in file %s", builtin);
    return builtin;
}

/* ------------------------------------------------------------------------- */
/* Environment                                                               */
/* ------------------------------------------------------------------------- */

struct sink {
    char  *buf;
    size_t cap, len;
    int    bad;
};

static void put(struct sink *k, const char *s)
{
    size_t n = strlen(s);
    if (k->bad || k->len + n + 1 > k->cap) { k->bad = 1; return; }
    memcpy(k->buf + k->len, s, n);
    k->len += n;
    k->buf[k->len] = '\0';
}

/* One DAEMON_OPTS word (an option, or a run of them joined by spaces). */
static void put_opt(struct sink *k, const char *s, int *first)
{
    for (const char *p = s; *p; p++)
        if (!token_char(*p) && *p != ' ') k->bad = 1;
    if (!*first) put(k, " ");
    put(k, s);
    *first = 0;
}

int stickcfg_path_ok(const char *p)
{
    size_t n;
    if (!p || p[0] != '/') return 0;
    n = strlen(p);
    if (n > 1024) return 0;
    for (size_t i = 0; i < n; i++)
        if (!token_char(p[i])) return 0;
    return 1;
}

int stickcfg_compose_env(int developer, const char *monitor_path,
                         const struct stickcfg_settings *s,
                         char *out, size_t cap, size_t *out_len)
{
    struct sink k = { out, cap, 0, 0 };
    char num[32];
    int first = 1;

    *out_len = 0;
    if (cap == 0) return -1;
    out[0] = '\0';

    if (s && s->volume >= 0) {
        if (s->volume > STICKCFG_VOLUME_MAX) k.bad = 1;
        snprintf(num, sizeof num, "HDA_VOLUME=%d\n", s->volume);
        put(&k, num);
    }
    put(&k, "DAEMON_OPTS=");
    if (developer) put_opt(&k, "--jitter-summary", &first);
    if (monitor_path) {
        if (!stickcfg_path_ok(monitor_path)) k.bad = 1;
        put_opt(&k, "--monitor", &first);
        put_opt(&k, monitor_path, &first);
    }
    if (s) {
        if (s->idle_after >= 0) {
            if (s->idle_after != 0 &&
                (s->idle_after < STICKCFG_IDLE_AFTER_MIN || s->idle_after > STICKCFG_IDLE_AFTER_MAX))
                k.bad = 1;
            snprintf(num, sizeof num, "%d", s->idle_after);
            put_opt(&k, "--idle-after", &first);
            put_opt(&k, num, &first);
        }
        if (s->idle_black) put_opt(&k, "--idle-black", &first);
        if (s->no_audio) put_opt(&k, "--no-audio", &first);
        if (s->no_input) put_opt(&k, "--no-input", &first);
        if (s->extra[0]) put_opt(&k, s->extra, &first);
    }
    put(&k, "\n");

    if (k.bad) {
        out[0] = '\0';
        return -1;
    }
    *out_len = k.len;
    return 0;
}
