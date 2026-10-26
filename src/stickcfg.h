/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECEIVER_STICKCFG_H
#define RECEIVER_STICKCFG_H

/*
 * stickcfg.h -- the settings files a user writes on the USB stick, often from
 * Windows: monitor.ini (the tube description) and settings.ini (named keys).
 *
 * Pure module: no I/O, no dynamic allocation, bounded buffers only. The tool
 * crt-bridge-config does the file work and calls these functions.
 *
 * The daemon stays strict about monitor.ini (a CR or a BOM is refused, exit 3).
 * Windows editors are tolerated HERE, at the copy from the stick to /run: CRLF
 * and lone CR become LF, a UTF-8 BOM and a final 0x1A are removed. Anything
 * that cannot be plain text is refused by name (UTF-16, NUL, other control
 * characters, more than 4096 bytes), never repaired.
 *
 * The environment handed to systemd is WRITTEN by compose, one variable per
 * line, from validated values only; nothing from the stick is copied into it.
 */

#include <stddef.h>

/* Same bound as MONITOR_MAX_FILE_BYTES of monitor.h. */
#define STICKCFG_MAX_FILE_BYTES 4096

/* --idle-after bounds: 0 (never), or MIN_IDLE_AFTER of main.c up to an hour.
 * The daemon judges the option again; a drift here makes it stop loudly. */
#define STICKCFG_IDLE_AFTER_MIN 2
#define STICKCFG_IDLE_AFTER_MAX 3600

#define STICKCFG_VOLUME_MAX     100

#define STICKCFG_REPORT_BYTES   8192

enum stickcfg_norm_status {
    STICKCFG_NORM_OK = 0,
    STICKCFG_NORM_TOO_BIG,      /* more than STICKCFG_MAX_FILE_BYTES */
    STICKCFG_NORM_UTF16,        /* starts with FF FE or FE FF */
    STICKCFG_NORM_NUL,          /* a NUL byte: UTF-16 without BOM, or binary */
    STICKCFG_NORM_CONTROL       /* any other control character */
};

struct stickcfg_norm {
    enum stickcfg_norm_status status;
    unsigned crlf;              /* CR LF pairs turned into LF */
    unsigned cr;                /* lone CR turned into LF */
    int bom;                    /* UTF-8 BOM removed */
    int eof_mark;               /* final 0x1A removed */
    size_t offset;              /* offending byte (NUL, CONTROL), from 0 */
    unsigned char byte;         /* its value */
};

/*
 * Normalize in[0..in_len) into out, which must hold in_len bytes (the output is
 * never longer). Tab and LF pass; CR LF and lone CR become LF; a leading UTF-8
 * BOM and a single final 0x1A are removed; bytes >= 0x80 pass (the daemon
 * judges them). On refusal *out_len is 0 and info says why.
 */
enum stickcfg_norm_status stickcfg_normalize(const unsigned char *in, size_t in_len,
                                             unsigned char *out, size_t *out_len,
                                             struct stickcfg_norm *info);

/* What was done ("CRLF converted, BOM removed", "unchanged"), or why the file
 * was refused ("UTF-16 text, save as UTF-8"). Always NUL-terminated. */
void stickcfg_norm_describe(const struct stickcfg_norm *info, char *buf, size_t cap);

/* The report: plain lines, each scrubbed to printable ASCII. A line that does
 * not fit is dropped, `truncated` is set, and one last line says so
 * ("report: truncated"). */
struct stickcfg_report {
    char   text[STICKCFG_REPORT_BYTES];
    size_t len;
    int    truncated;
};

void stickcfg_report_init(struct stickcfg_report *r);
void stickcfg_report_line(struct stickcfg_report *r, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* settings.ini, once judged. -1: key not set. */
struct stickcfg_settings {
    int  volume;                /* 0..100, HDA_VOLUME */
    int  idle_after;            /* 0, or IDLE_AFTER_MIN..IDLE_AFTER_MAX */
    int  idle_black;            /* idle_screen = black */
    int  no_audio;              /* audio = off */
    int  no_input;              /* inputs = off */
    char extra[STICKCFG_MAX_FILE_BYTES + 1]; /* validated tokens, single spaces */
};

void stickcfg_settings_init(struct stickcfg_settings *s);

/*
 * Read normalized settings text (LF only). Lines are `key = value`; `#` and `;`
 * start a comment line; a `[section]` header is reported and ignored (the keys
 * after it are still read); key names are case-insensitive. Every unknown key,
 * invalid value or malformed line becomes a "settings: ..." line in r and is
 * ignored; every accepted key too, so the report shows what was used.
 */
void stickcfg_parse_settings(const char *text, size_t len,
                             struct stickcfg_settings *s, struct stickcfg_report *r);

/*
 * Write the environment file text: "HDA_VOLUME=N\n" when the volume is set,
 * then always "DAEMON_OPTS=...\n" (possibly empty, so that it replaces the
 * image default). Options, in a fixed order: --jitter-summary (developer),
 * --monitor PATH (monitor_path != NULL), --idle-after N, --idle-black,
 * --no-audio, --no-input, then extra_options. s may be NULL.
 * Every value is checked against the charset [A-Za-z0-9 ._/=:,+-].
 * Returns 0, or -1 (out too small, or a forbidden character) with out[0] = 0.
 */
int stickcfg_compose_env(int developer, const char *monitor_path,
                         const struct stickcfg_settings *s,
                         char *out, size_t cap, size_t *out_len);

/*
 * Developer mode: the monitor file built into the image wins. Reports it, and
 * reports a monitor.ini on the stick as ignored when there is one. Returns
 * builtin, the path to pass as --monitor.
 */
const char *stickcfg_developer_monitor(int stick_has_file, const char *builtin,
                                       struct stickcfg_report *r);

/* 1 if p is usable as --monitor PATH: absolute, non-empty, no space, every
 * character in the environment charset. */
int stickcfg_path_ok(const char *p);

#endif /* RECEIVER_STICKCFG_H */
