/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * crt-bridge-config -- read the settings a user put on the USB stick, judge
 * them, and write what the receiver daemon reads at start-up.
 *
 * Usage:
 *   crt-bridge-config --env-out PATH --report PATH
 *                     [--monitor-in PATH --monitor-out PATH]
 *                     [--settings-in PATH]
 *                     [--developer --builtin-monitor PATH]
 *
 * Inputs are optional: an option left out, or a file absent from the stick,
 * means "nothing there". Outputs are written atomically (a temporary file,
 * then rename), mode 0644:
 *   --monitor-out  monitor.ini, normalized (stickcfg_normalize);
 *   --env-out      HDA_VOLUME=... and DAEMON_OPTS=..., one variable per line;
 *   --report       one line per finding, also printed on standard output.
 *
 * Exit codes: 0 everything written; 1 an output could not be written;
 * 2 usage. A refused file is NOT an exit code: it goes to the report, and the
 * environment is composed so that the daemon decides. A refused monitor.ini
 * still passes --monitor, towards a path that does not exist: the daemon stops
 * (exit 3) instead of silently using the built-in 15 kHz band.
 *
 * Developer mode (--developer): the monitor file built into the image wins
 * (--builtin-monitor), a monitor.ini on the stick is ignored and said so, and
 * --jitter-summary is added.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "stickcfg.h"

#define PATH_BYTES 1100   /* stickcfg_path_ok caps paths at 1024 */

enum input { IN_ABSENT, IN_ERROR, IN_OK };

static void usage(const char *why)
{
    fprintf(stderr,
            "crt-bridge-config: %s\n"
            "usage: crt-bridge-config --env-out PATH --report PATH\n"
            "         [--monitor-in PATH --monitor-out PATH] [--settings-in PATH]\n"
            "         [--developer --builtin-monitor PATH]\n",
            why);
}

/* Reads at most cap bytes: a file larger than the bound is seen as cap bytes,
 * which the normalization refuses as too big. */
static enum input read_input(const char *path, unsigned char *buf, size_t cap, size_t *len,
                             char *why, size_t whycap)
{
    struct stat st;
    size_t n = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    *len = 0;
    if (fd < 0) {
        if (errno == ENOENT) return IN_ABSENT;
        snprintf(why, whycap, "cannot read (%s)", strerror(errno));
        return IN_ERROR;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(why, whycap, "not a regular file");
        close(fd);
        return IN_ERROR;
    }
    while (n < cap) {
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0) {
            if (errno == EINTR) continue;
            snprintf(why, whycap, "cannot read (%s)", strerror(errno));
            close(fd);
            return IN_ERROR;
        }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    *len = n;
    return IN_OK;
}

static int write_atomic(const char *path, const void *data, size_t len)
{
    char tmp[PATH_BYTES + 8];
    const unsigned char *p = data;
    size_t done = 0;
    int fd;

    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) {
        fprintf(stderr, "crt-bridge-config: path too long: %s\n", path);
        return -1;
    }
    /* O_EXCL after unlink: never write through something planted at tmp. */
    if (unlink(tmp) != 0 && errno != ENOENT) goto fail;
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) goto fail;
    if (fchmod(fd, 0644) != 0) goto fail_fd;
    while (done < len) {
        ssize_t w = write(fd, p + done, len - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            goto fail_fd;
        }
        done += (size_t)w;
    }
    if (fsync(fd) != 0) goto fail_fd;
    if (close(fd) != 0) goto fail_unlink;
    if (rename(tmp, path) != 0) goto fail_unlink;
    return 0;

fail_fd:
    close(fd);
fail_unlink:
    fprintf(stderr, "crt-bridge-config: cannot write %s (%s)\n", path, strerror(errno));
    unlink(tmp);
    return -1;
fail:
    fprintf(stderr, "crt-bridge-config: cannot write %s (%s)\n", path, strerror(errno));
    return -1;
}

static int output_path_ok(const char *p)
{
    return p[0] == '/' && strlen(p) <= 1024;
}

int main(int argc, char **argv)
{
    static unsigned char in[STICKCFG_MAX_FILE_BYTES + 1];
    static unsigned char norm[STICKCFG_MAX_FILE_BYTES + 1];
    static struct stickcfg_settings settings;
    static struct stickcfg_report report;
    static char env[2 * STICKCFG_MAX_FILE_BYTES];
    const char *mon_in = NULL, *mon_out = NULL, *set_in = NULL;
    const char *env_out = NULL, *rep_out = NULL, *builtin = NULL;
    const char *monitor_path = NULL;
    int developer = 0;
    char why[256], what[160];
    size_t len = 0, nlen = 0, elen = 0;
    struct stickcfg_norm info;
    enum input got;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char **slot = NULL;
        if (strcmp(a, "--developer") == 0) { developer = 1; continue; }
        if (strcmp(a, "--monitor-in") == 0) slot = &mon_in;
        else if (strcmp(a, "--monitor-out") == 0) slot = &mon_out;
        else if (strcmp(a, "--settings-in") == 0) slot = &set_in;
        else if (strcmp(a, "--env-out") == 0) slot = &env_out;
        else if (strcmp(a, "--report") == 0) slot = &rep_out;
        else if (strcmp(a, "--builtin-monitor") == 0) slot = &builtin;
        else { usage("unknown option"); return 2; }
        if (*slot) { usage("option given twice"); return 2; }
        if (i + 1 >= argc || argv[i + 1][0] == '\0') { usage("option without a value"); return 2; }
        *slot = argv[++i];
    }
    if (!env_out || !rep_out) { usage("--env-out and --report are required"); return 2; }
    if (!output_path_ok(env_out) || !output_path_ok(rep_out)) { usage("output paths must be absolute"); return 2; }
    if (mon_in && !mon_out) { usage("--monitor-in needs --monitor-out"); return 2; }
    /* --monitor-out goes into DAEMON_OPTS: it must survive systemd's split. */
    if (mon_out && !stickcfg_path_ok(mon_out)) { usage("bad --monitor-out (absolute, [A-Za-z0-9._/=:,+-])"); return 2; }
    if (developer != (builtin != NULL)) { usage("--developer and --builtin-monitor go together"); return 2; }
    if (builtin && !stickcfg_path_ok(builtin)) { usage("bad --builtin-monitor (absolute, [A-Za-z0-9._/=:,+-])"); return 2; }

    stickcfg_report_init(&report);
    stickcfg_settings_init(&settings);

    /* settings.ini */
    got = set_in ? read_input(set_in, in, sizeof in, &len, why, sizeof why) : IN_ABSENT;
    if (got == IN_ABSENT) {
        stickcfg_report_line(&report, "settings: none on stick, defaults used");
    } else if (got == IN_ERROR) {
        stickcfg_report_line(&report, "settings: refused: %s, defaults used", why);
    } else if (stickcfg_normalize(in, len, norm, &nlen, &info) != STICKCFG_NORM_OK) {
        stickcfg_norm_describe(&info, what, sizeof what);
        stickcfg_report_line(&report, "settings: refused: %s, defaults used", what);
    } else {
        stickcfg_norm_describe(&info, what, sizeof what);
        stickcfg_report_line(&report, "settings: read (%s)", what);
        stickcfg_parse_settings((const char *)norm, nlen, &settings, &report);
    }

    /* monitor.ini */
    if (developer) {
        struct stat st;
        int on_stick = mon_in && (stat(mon_in, &st) == 0 || errno != ENOENT);
        monitor_path = stickcfg_developer_monitor(on_stick, builtin, &report);
    } else {
        got = mon_in ? read_input(mon_in, in, sizeof in, &len, why, sizeof why) : IN_ABSENT;
        if (got == IN_OK && stickcfg_normalize(in, len, norm, &nlen, &info) == STICKCFG_NORM_OK) {
            stickcfg_norm_describe(&info, what, sizeof what);
            if (write_atomic(mon_out, norm, nlen) != 0) return 1;
            stickcfg_report_line(&report, "monitor: copied (%s)", what);
            monitor_path = mon_out;
        } else if (got == IN_ABSENT) {
            stickcfg_report_line(&report, "monitor: none on stick, built-in 15 kHz band");
        } else {
            if (got == IN_OK) {
                stickcfg_norm_describe(&info, what, sizeof what);
                stickcfg_report_line(&report, "monitor: refused: %s", what);
            } else {
                stickcfg_report_line(&report, "monitor: refused: %s", why);
            }
            /* The path passed to the daemon must not exist. */
            if (unlink(mon_out) != 0 && errno != ENOENT) {
                fprintf(stderr, "crt-bridge-config: cannot remove %s (%s)\n", mon_out, strerror(errno));
                return 1;
            }
            stickcfg_report_line(&report, "monitor: the receiver will stop (exit 3) rather than use the built-in band");
            monitor_path = mon_out;
        }
    }

    if (stickcfg_compose_env(developer, monitor_path, &settings, env, sizeof env, &elen) != 0) {
        fprintf(stderr, "crt-bridge-config: environment could not be composed\n");
        return 1;
    }
    for (size_t i = 0, start = 0; i < elen; i++) {
        if (env[i] == '\n') {
            stickcfg_report_line(&report, "env: %.*s", (int)(i - start), env + start);
            start = i + 1;
        }
    }
    if (write_atomic(env_out, env, elen) != 0) return 1;
    fputs(report.text, stdout);
    if (write_atomic(rep_out, report.text, report.len) != 0) return 1;
    return 0;
}
