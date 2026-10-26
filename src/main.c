/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * main.c -- crt-bridge-rcv daemon entry point.
 *
 * Wires the daemon together:
 *   1. parse CLI flags (range-checked)
 *   1b. read the monitor file (--monitor, or the built-in 15 kHz band) BEFORE
 *      everything else: a missing, wrong or suspect file exits 3 before any
 *      queue, thread or image exists
 *   2. allocate the SPSC ring + frame_slot pool (SLOT_POOL_SIZE slots x
 *      MAX_FRAME_BYTES)
 *   3. allocate the jitter ring
 *   4. install SIGTERM/SIGINT handlers that flip shared.running = 0
 *   5. spawn blit thread first (does the modeset via switchres + DRM)
 *   6. spawn net thread (UDP/32100 listener + ACK pacer)
 *   7. join both threads
 *   8. on exit: dump jitter CSV (default /var/log/crt-bridge/jitter-<pid>.csv)
 *      and/or print the p50/p95/p99/p99.9 JSON one-liner to stdout
 *
 * Threats covered:
 *   - Tampering via CLI args                -> parse_int_range / parse_double_range
 *     reject out-of-range values with exit code 3 (config_rejected) and a
 *     stderr message
 *   - Info disclosure via CSV permissions   -> mkdir /var/log/crt-bridge mode 0750
 *     (systemd LogsDirectory= reinforces this when running under the unit)
 *   - DoS by crash loop                     -> handled by systemd Restart=on-failure
 *     RestartSec=2; daemon itself simply exits non-zero on init failure.
 *     A refused configuration exits 3, excluded from restarts by
 *     RestartPreventExitStatus=3; StartLimitIntervalSec/StartLimitBurst end
 *     a permanent failure in `failed`
 *   - Elevation of privilege (root daemon)  -> accepted
 */

#define _GNU_SOURCE
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "net.h"
#include "pcm.h"   /* sound output */
#include "input.h" /* Groovy input channel, UDP/32101 */
#include "fieldphase.h" /* 480i: on which field to place the flip */
#include "blit.h"
#include "jitter.h"
#include "spsc.h"
#include "proto.h"
#include "modelines.h"  /* REGIME_240P_SUPER_RES */
#include "monitor.h"    /* the CRT described by a file */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>  /* eventfd(2) -- wakes the blit thread */
#include <sys/socket.h>   /* shutdown(2) -- used by on_signal() to unblock recvmmsg */
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_W           320
#define DEFAULT_H           240
#define DEFAULT_REFRESH_HZ  60.0
#define DEFAULT_PORT        32100
#define DEFAULT_HEARTBEAT   2000   /* ms */
#define DEFAULT_NET_CPU     2
#define DEFAULT_BLIT_CPU    3
/* Idle screen. Ten seconds: well above the 2 s heartbeat_timeout and any
 * network hiccup -- the screen must not go dark in the middle of a game for
 * one lost datagram -- and well below the time it takes to mark a phosphor. */
#define DEFAULT_IDLE_AFTER  10     /* s; 0 disables */
/* Lower bound when the idle screen is enabled. With the emitter skipping
 * unchanged frames (GROOVY_FRAME_DUP), a still scene delivers a FULL frame
 * only once per second (GM_FULL_FRAME_PERIOD_NS in the emitter's libgm); in
 * between, the SPSC queue stays empty and last_content_ns does not move. At
 * --idle-after 1, the idle screen would be drawn and then vanish at every
 * forced full frame. Two seconds leave a whole period of margin. */
#define MIN_IDLE_AFTER      2      /* s; 1 is refused, see above */

/* SLOT_POOL_SIZE slots in the pool; the SPSC ring (header-only, capacity =
 * SPSC_CAPACITY) carries pointers, so the pool size is independent of the
 * ring depth.
 * NOTE: SLOT_POOL_SIZE >= SPSC_CAPACITY guards against slot-recycling races
 * -- the _Static_assert below enforces the invariant at compile time. */
#define SLOT_POOL_SIZE      16
_Static_assert(SLOT_POOL_SIZE >= SPSC_CAPACITY,
               "slot pool must hold at least one entry per SPSC slot in flight");

/* MAX_FRAME_BYTES is defined in net.h so every entry point producing an
 * expected-bytes value can clamp against the same upper bound. */

/* Process-wide shared state. Lives in BSS so signal handlers can reach it. */
static struct net_blit_shared g_shared;

/* The CRT. Written by main() before any thread is created, only read
 * afterwards (by the blit thread, through struct blit_thread_config). */
static struct monitor g_monitor;

/* Signal handler flips running flag. Keep async-signal-safe: only an atomic
 * store, no logging, no malloc. Blit thread observes within ~2 ms (its poll
 * timeout). Net thread is also forcibly woken via shutdown(net_sock_fd):
 * diagnosed on the bench, recvmmsg(MSG_WAITFORONE, timeout=100ms) ignores
 * its timeout when zero packets ever arrive AND SA_RESTART re-enters the
 * syscall after signal delivery, so the running-flag check alone never
 * fires during idle shutdown. shutdown() is AS-Safe per signal-safety(7) and
 * idempotent; net thread retains sole ownership of close(). */
static void on_signal(int sig)
{
    (void)sig;
    atomic_store(&g_shared.running, 0);
    int s = atomic_load(&g_shared.net_sock_fd);
    if (s >= 0) (void)shutdown(s, SHUT_RDWR);
}

/* ---------------------------------------------------------------------------
 * CLI parsing helpers -- strict bound-checking.
 * Both reject empty strings, trailing garbage, and out-of-range values.
 * ------------------------------------------------------------------------- */
static int parse_int_range(const char *s, int lo, int hi, int *out)
{
    if (!s || !*s) return -1;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || !end || *end != '\0') return -1;
    if (v < (long)lo || v > (long)hi) return -1;
    *out = (int)v;
    return 0;
}

static int parse_double_range(const char *s, double lo, double hi, double *out)
{
    if (!s || !*s) return -1;
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (errno != 0 || !end || *end != '\0') return -1;
    if (!(v >= lo && v <= hi)) return -1;
    *out = v;
    return 0;
}

/* A refused configuration is not fixed by restarting. Each refusal (option
 * out of range, unknown option, geometry too large for a slot) writes its
 * message, then this line, and exits with code 3, which the unit excludes
 * from restarts (RestartPreventExitStatus=3). Environment failures
 * (allocation, thread creation) stay at code 1. */
#define EXIT_CONFIG_REJECTED 3

static int config_rejected(void)
{
    fprintf(stderr,
            "{\"event\":\"daemon_exit\",\"code\":%d,\"reason\":\"config_rejected\"}\n",
            EXIT_CONFIG_REJECTED);
    return EXIT_CONFIG_REJECTED;
}

static void usage(const char *argv0)
{
    fprintf(stdout,
        "Usage: %s [options]\n"
        "  --listen-port PORT          UDP port (default %d)\n"
        "  --width N                   frame width  (default %d, range 16..1280)\n"
        "  --height N                  frame height (default %d, range 16..1024)\n"
        "  --refresh HZ                refresh rate (default %.1f, range 24..120)\n"
        "  --drm-device PATH           DRM card    (default /dev/dri/card0)\n"
        "  --jitter-csv PATH           CSV output  (default /var/log/crt-bridge/jitter-<pid>.csv)\n"
        "  --jitter-summary            print p50/p95/p99/p99.9 JSON to stdout at exit\n"
        "  --heartbeat-timeout-ms N    silence threshold (default %d, range 100..60000)\n"
        "  --net-cpu N                 net thread CPU affinity  (default %d)\n"
        "  --blit-cpu N                blit thread CPU affinity (default %d)\n"
        "  --audio-device PATH         PCM device (default /dev/snd/pcmC0D0p)\n"
        "  --audio-cpu N               audio thread CPU affinity (default: the net\n"
        "                              thread's CPU; -1 for no pinning)\n"
        "  --no-audio                  do not create the sound output at all\n"
        "  --audio-dump PATH           record what goes to the sound card\n"
        "                              (raw S16LE interleaved; for diagnosis)\n"
        "  --idle-after N              seconds of silence before the idle screen\n"
        "                              (default %d; 0 never goes dark; otherwise\n"
        "                              %d at least: the emitter's frame_dup sends\n"
        "                              only one full frame per second)\n"
        "  --idle-black                black idle screen, without the text block\n"
        "  --legacy-modeset            use the older per-frame modeset path (bisection\n"
        "                              escape hatch; default is the pinned dual-modeline\n"
        "                              super-res path)\n"
        "  --input-port PORT           Groovy input channel (default: video port + 1,\n"
        "                              so %d; 0 disables the thread)\n"
        "  --input-cpu N               input thread CPU affinity (default: the net\n"
        "                              thread's CPU; -1 for no pinning)\n"
        "  --no-input                  do not create the input thread at all\n"
        "  --field-phase off|a|b       480i: delays the first flip by ONE whole frame\n"
        "                              (a measuring instrument, not a fix; default off)\n"
        "  --wake-on-frame             wake the blit thread as soon as a frame arrives\n"
        "                              (eventfd; without it, the thread picks the frame\n"
        "                              up on its 2 ms step; default off)\n"
        "  --shed-window N             drop a frame when the last N displayed frames\n"
        "                              waited one vblank too many (240p only; 0..3600,\n"
        "                              requires --wake-on-frame; default 0, off)\n"
        "  --monitor PATH              monitor file (switchres crt_range grammar);\n"
        "                              missing or wrong: exit with code 3, before any image.\n"
        "                              Without the option: the built-in 15 kHz band\n"
        "  --check-monitor PATH        dry run of the monitor file: prints on standard\n"
        "                              output the bands understood and, for 240p, 480i,\n"
        "                              288p and 480p, the band and the mode that would come\n"
        "                              out (or the refusal); lights up nothing; exit 0 if\n"
        "                              the daemon would start with this file, 3 if not\n"
        "  --version                   print version and exit\n"
        "  --help                      this message\n",
        argv0, DEFAULT_PORT,
        DEFAULT_W, DEFAULT_H, DEFAULT_REFRESH_HZ,
        DEFAULT_HEARTBEAT, DEFAULT_NET_CPU, DEFAULT_BLIT_CPU,
        DEFAULT_IDLE_AFTER, MIN_IDLE_AFTER, DEFAULT_PORT + 1);
}

/* ---------------------------------------------------------------------------
 * Slot pool allocator.
 *
 * Each slot owns a heap-allocated buf of MAX_FRAME_BYTES; the SPSC ring
 * itself is allocated separately and initialised via spsc_init (header-only).
 * Returns 0 on success, -1 on alloc failure (caller must call free_pool).
 * ------------------------------------------------------------------------- */
static int alloc_pool(struct frame_slot ***out_slots,
                      struct spsc_ring **out_ring,
                      size_t pool_size)
{
    struct frame_slot **slots = calloc(pool_size, sizeof(*slots));
    if (!slots) return -1;
    for (size_t i = 0; i < pool_size; ++i) {
        slots[i] = calloc(1, sizeof(struct frame_slot));
        if (!slots[i]) { *out_slots = slots; return -1; }
        slots[i]->buf = calloc(1, MAX_FRAME_BYTES);
        if (!slots[i]->buf) { *out_slots = slots; return -1; }
    }
    struct spsc_ring *ring = calloc(1, sizeof(*ring));
    if (!ring) { *out_slots = slots; return -1; }
    spsc_init(ring);

    *out_slots = slots;
    *out_ring  = ring;
    return 0;
}

static void free_pool(struct frame_slot **slots, size_t pool_size,
                      struct spsc_ring *ring)
{
    if (slots) {
        for (size_t i = 0; i < pool_size; ++i) {
            if (!slots[i]) continue;
            free(slots[i]->buf);
            free(slots[i]);
        }
        free(slots);
    }
    free(ring);
}

int main(int argc, char **argv)
{
    int     width        = DEFAULT_W;
    int     height       = DEFAULT_H;
    double  refresh      = DEFAULT_REFRESH_HZ;
    int     listen_port  = DEFAULT_PORT;
    int     heartbeat_ms = DEFAULT_HEARTBEAT;
    int     net_cpu      = DEFAULT_NET_CPU;
    int     blit_cpu     = DEFAULT_BLIT_CPU;
    int     want_summary = 0;
    int     legacy_modeset = 0;  /* --legacy-modeset opt-in */

    /* Sound. The device is NEVER opened at start-up -- pcm.c opens it only
     * when the first block of sound arrives. --no-audio therefore only
     * avoids creating the thread at all; it is not a safety net for
     * start-up, it is one for when in doubt. */
    const char *audio_device = NULL;
    /* -2 = not given: main() will use the net thread's CPU. -1 = no pinning,
     * on explicit request. >= 0 = that CPU. */
    int     audio_cpu        = -2;
    int     no_audio         = 0;
    int     idle_after_s     = DEFAULT_IDLE_AFTER;
    int     idle_black       = 0;
    uint8_t field_phase = (uint8_t)FIELD_PHASE_OFF;
    const char *audio_dump   = NULL;
    const char *drm_device = "/dev/dri/card0";
    const char *jitter_csv = NULL;

    /* Groovy input channel. The default derives from listen_port so that it
     * never collides with the video port: test tooling runs the video
     * channel on 32101, and the input port then moves along. Set after getopt_long since it depends on
     * listen_port, which may have changed. -2 = not given (like audio_cpu):
     * follows the net thread's CPU. */
    int     input_port = -1;    /* -1 = "use the derived default" */
    int     input_cpu  = -2;
    int     no_input   = 0;

    /* Waking the blit thread by eventfd. Off by default: the same binary
     * serves as the control. */
    int     wake_on_frame = 0;
    int     wake_fd       = -1;   /* -1 = off, never 0 (standard input) */
    /* Window of the adaptive frame dropping. 0 = off (default); N > 0
     * requires --wake-on-frame (checked after getopt, since options come in
     * any order). */
    int     shed_window   = 0;
    /* NULL = no --monitor: the built-in 15 kHz band. */
    const char *monitor_path = NULL;
    /* NULL = no --check-monitor. Otherwise: a dry run, handled right after
     * the options, which lights up nothing. */
    const char *check_path = NULL;

    static const struct option opts[] = {
        {"listen-port",          required_argument, 0, 'p'},
        {"width",                required_argument, 0, 'w'},
        {"height",               required_argument, 0, 'h'},
        {"refresh",              required_argument, 0, 'r'},
        {"drm-device",           required_argument, 0, 'd'},
        {"jitter-csv",           required_argument, 0, 'j'},
        {"jitter-summary",       no_argument,       0, 'J'},
        {"heartbeat-timeout-ms", required_argument, 0, 't'},
        {"net-cpu",              required_argument, 0, 'n'},
        {"blit-cpu",             required_argument, 0, 'b'},
        {"audio-device",         required_argument, 0, 'A'},
        {"audio-cpu",            required_argument, 0, 'C'},
        {"no-audio",             no_argument,       0, 'N'},
        {"audio-dump",           required_argument, 0, 'D'},
        {"idle-after",           required_argument, 0, 'I'},
        {"idle-black",           no_argument,       0, 'K'},
        {"legacy-modeset",       no_argument,       0, 'L'},
        {"input-port",           required_argument, 0, 'i'},
        {"input-cpu",            required_argument, 0, 'c'},
        {"no-input",             no_argument,       0, 'o'},
        {"field-phase",          required_argument, 0, 'F'},
        {"wake-on-frame",        no_argument,       0, 'W'},
        {"shed-window",          required_argument, 0, 'S'},
        {"monitor",              required_argument, 0, 'M'},
        {"check-monitor",        required_argument, 0, 'X'},
        {"version",             no_argument,       0, 'V'},
        {"help",                 no_argument,       0, 'H'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "p:w:h:r:d:j:Jt:n:b:A:C:ND:I:KLi:c:oF:WS:M:X:VH", opts, NULL)) != -1) {
        switch (c) {
        case 'p':
            if (parse_int_range(optarg, 1, 65535, &listen_port))      { fprintf(stderr, "bad --listen-port\n"); return config_rejected(); }
            break;
        case 'w':
            /* Ceiling 1280 -- bounds the RECEIVED wire frame at the emulator
             * core's NATIVE dimensions (320, 512, 640, 700...), NOT the
             * super-res framebuffer (2560 wide). 1280x480x4 ==
             * MAX_FRAME_BYTES still holds exactly (width*height*4 <=
             * MAX_FRAME_BYTES is still enforced by the height ceiling below)
             * -- this ceiling and the framebuffer width are two DECOUPLED
             * quantities that merely shared a number before the framebuffer
             * was widened; doubling one does not require doubling the other. */
            if (parse_int_range(optarg, 16, 1280, &width))            { fprintf(stderr, "bad --width\n"); return config_rejected(); }
            break;
        case 'h':
            /* Ceiling 1024 -- see --width above. */
            if (parse_int_range(optarg, 16, 1024, &height))           { fprintf(stderr, "bad --height\n"); return config_rejected(); }
            break;
        case 'r':
            if (parse_double_range(optarg, 24.0, 120.0, &refresh))    { fprintf(stderr, "bad --refresh\n"); return config_rejected(); }
            break;
        case 'd':
            drm_device = optarg;
            break;
        case 'j':
            jitter_csv = optarg;
            break;
        case 'J':
            want_summary = 1;
            break;
        case 't':
            if (parse_int_range(optarg, 100, 60000, &heartbeat_ms))   { fprintf(stderr, "bad --heartbeat-timeout-ms\n"); return config_rejected(); }
            break;
        case 'n':
            if (parse_int_range(optarg, 0, 255, &net_cpu))            { fprintf(stderr, "bad --net-cpu\n"); return config_rejected(); }
            break;
        case 'b':
            if (parse_int_range(optarg, 0, 255, &blit_cpu))           { fprintf(stderr, "bad --blit-cpu\n"); return config_rejected(); }
            break;
        case 'A':
            audio_device = optarg;
            break;
        case 'C':
            if (parse_int_range(optarg, -1, 255, &audio_cpu))      { fprintf(stderr, "bad --audio-cpu\n"); return config_rejected(); }
            break;
        case 'N':
            no_audio = 1;
            break;
        case 'D':
            audio_dump = optarg;
            break;
        case 'I':
            if (parse_int_range(optarg, 0, 86400, &idle_after_s) ||
                (idle_after_s != 0 && idle_after_s < MIN_IDLE_AFTER)) {
                fprintf(stderr, "bad --idle-after (0, or %d at least)\n", MIN_IDLE_AFTER);
                return config_rejected();
            }
            break;
        case 'K':
            idle_black = 1;
            break;
        case 'L':
            legacy_modeset = 1;
            break;
        case 'i':
            if (parse_int_range(optarg, 0, 65535, &input_port))       { fprintf(stderr, "bad --input-port\n"); return config_rejected(); }
            break;
        case 'c':
            if (parse_int_range(optarg, -1, 255, &input_cpu))         { fprintf(stderr, "bad --input-cpu\n"); return config_rejected(); }
            break;
        case 'o':
            no_input = 1;
            break;
        case 'F':
            if (field_phase_parse(optarg, &field_phase)) {
                fprintf(stderr, "bad --field-phase (expected: off|a|b)\n");
                return config_rejected();
            }
            break;
        case 'W':
            wake_on_frame = 1;
            break;
        case 'S':
            if (parse_int_range(optarg, 0, 3600, &shed_window))       { fprintf(stderr, "bad --shed-window (0..3600)\n"); return config_rejected(); }
            break;
        case 'M':
            /* There is no choosing between two monitor files: a plain refusal. */
            if (monitor_path) { fprintf(stderr, "bad --monitor (only once)\n"); return config_rejected(); }
            monitor_path = optarg;
            break;
        case 'X':
            if (check_path) { fprintf(stderr, "bad --check-monitor (only once)\n"); return config_rejected(); }
            check_path = optarg;
            break;
        case 'V':
            printf("crt-bridge-rcv 1.0.0\n");
            return 0;
        case 'H':
            usage(argv[0]);
            return 0;
        default:
            return config_rejected();
        }
    }

    /* The dry run is handled HERE, right after the options and before
     * everything else: no alloc_pool, no thread, no socket, no DRM device, no
     * log directory, no signal, no JSON line on stderr. It reads the file,
     * prints on standard output what would come out, and returns: 0 if the
     * daemon would start with this file, 3 if it would refuse it (file
     * refused, or a pinned mode excluded by the low band). Run on a receiver
     * in service, it does not disturb it. With --monitor, there is no
     * choosing between two files: a plain refusal, like any misused option. */
    if (check_path) {
        struct monitor chk;
        struct monitor_error cerr = { MONITOR_OK, 0 };

        if (monitor_path) {
            fprintf(stderr, "bad --check-monitor (incompatible with --monitor)\n");
            return config_rejected();
        }
        if (monitor_load_file(check_path, &chk, &cerr) != 0) {
            monitor_report_rejected(stdout, check_path, &cerr);
            return EXIT_CONFIG_REJECTED;
        }
        /* The file is read, but a pinned mode its low band excludes stops the
         * daemon with code 3 at start-up (boot_guard_pinned): the exit code
         * follows the daemon's decision, not only the loader's. The report is
         * written in both cases, stderr stays empty. */
        if (monitor_report(stdout, &chk, check_path) != 0)
            return EXIT_CONFIG_REJECTED;
        return 0;
    }

    /* Dropping without waking makes no sense: without the wake-up, the next
     * frame waits for the 2 ms step and dropping merely loses a frame. A
     * plain refusal, code 3, like any misused option. */
    if (shed_window > 0 && !wake_on_frame) {
        fprintf(stderr, "bad --shed-window (requires --wake-on-frame)\n");
        return config_rejected();
    }

    /* The monitor file is read HERE, after the options and BEFORE everything
     * else: no frame queue, no thread, no socket, no screen, no
     * daemon_mode_selected exist yet. A missing, unreadable, wrong or suspect
     * file stops the daemon with code 3 before any image could go out
     * (RestartPreventExitStatus=3: no restart). Without --monitor, the
     * built-in 15 kHz band: a failure there would be a defect of the code
     * itself, refused the same way. */
    if (monitor_path) {
        struct monitor_error merr = { MONITOR_OK, 0 };
        if (monitor_load_file(monitor_path, &g_monitor, &merr) != 0) {
            monitor_log_rejected(monitor_path, &merr);
            return config_rejected();
        }
    } else if (monitor_load_builtin(&g_monitor) != 0) {
        fprintf(stderr, "{\"event\":\"monitor_builtin_failed\"}\n");
        return config_rejected();
    }
    monitor_log_loaded(&g_monitor, monitor_path ? monitor_path : "");

    /* Allocate the slot pool and SPSC ring. */
    struct frame_slot **slots = NULL;
    struct spsc_ring   *ring  = NULL;
    if (alloc_pool(&slots, &ring, SLOT_POOL_SIZE) != 0) {
        fprintf(stderr, "{\"event\":\"alloc_pool_failed\"}\n");
        free_pool(slots, SLOT_POOL_SIZE, ring);
        return 1;
    }

    /* Populate shared state. */
    memset(&g_shared, 0, sizeof(g_shared));
    g_shared.spsc_ring     = ring;
    g_shared.slots         = slots;
    g_shared.slot_capacity = SLOT_POOL_SIZE;
    /* -1 = "no socket yet"; net thread publishes its fd here once
     * setup_socket() succeeds, on_signal() reads it to unblock recvmmsg. */
    atomic_store(&g_shared.net_sock_fd, -1);

    /* Validate the CLI-derived initial expected_frame_bytes against
     * MAX_FRAME_BYTES. With the CLI bounds (up to 1280x1024 RGB888, 3.75 MiB)
     * this can still exceed the slot buffer (1280x480x4, 2.34 MiB); the
     * configuration is refused rather than truncated. */
    uint64_t initial_bytes = (uint64_t)(unsigned)width *
                             (uint64_t)(unsigned)height * 3ULL;
    if (initial_bytes == 0 || initial_bytes > MAX_FRAME_BYTES) {
        fprintf(stderr,
                "{\"event\":\"bad_geometry\",\"w\":%d,\"h\":%d,"
                "\"bytes\":%llu,\"max\":%u}\n",
                width, height,
                (unsigned long long)initial_bytes,
                (unsigned)MAX_FRAME_BYTES);
        free_pool(slots, SLOT_POOL_SIZE, ring);
        return config_rejected();
    }
    atomic_store(&g_shared.expected_frame_bytes, (uint32_t)initial_bytes);

    /* Set BEFORE net_thread_start / blit_thread_start so both threads
     * observe the final value on their very first read -- no atomic
     * ordering race, this happens before either thread is spawned. */
    atomic_store(&g_shared.legacy_modeset, legacy_modeset);
    atomic_store(&g_shared.current_regime, (uint8_t)REGIME_240P_SUPER_RES);
    fprintf(stderr,
            "{\"event\":\"daemon_mode_selected\",\"legacy_modeset\":%d}\n",
            legacy_modeset);

    /* --- Sound output ------------------------------------------------------
     *
     * Created BEFORE the threads start, like legacy_modeset: both threads
     * read g_shared.audio without an atomic, and this write precedes their
     * creation, so there is no race.
     *
     * A creation failure is not fatal: the field stays NULL, net.c tests it,
     * and the daemon runs exactly as it would without sound. The rule holds:
     * a daemon that opens a sound device is a daemon that can fail at
     * start-up, so this one does not open it and cannot fail for that
     * reason.
     *
     * Priority 70: below the 80 of the blit thread. A missed frame is seen;
     * a micro-gap in the sound is heard but does not break the CRT's sync.
     * At priority 70 the thread cannot preempt the blit even if it lands on
     * its CPU. */
    /* --- Where the audio thread runs ----------------------------------------
     *
     * By default it follows the net thread's CPU. That is NOT because it
     * measures better: an A/B/C trial on three CPUs -- that of the network
     * interrupt, that of the net thread, that of the sound and GPU
     * interrupts -- gave indistinguishable results. Minimum fill in steady
     * state 31.5 / 31.6 / 31.8 ms, video latency 20,236 / 20,297 /
     * 20,345 us, and every zero-fill value turned out to be a re-priming,
     * present in all three runs.
     *
     * Two reasons to pin it anyway:
     *
     *   1. REPRODUCIBILITY. Left free, the kernel places it where it likes,
     *      and it did not pick the same CPU from one start to the next --
     *      CPU 1 then CPU 2, observed. A measurement made in those conditions
     *      compares with no other.
     *   2. PROTECTING THE VIDEO. At priority 70, this thread CAN preempt a
     *      hardware interrupt thread, which runs at 50 under PREEMPT_RT. That
     *      is exactly the mechanism of a 480i regression seen on the bench,
     *      where a blit burst preempted the network interrupt long enough for
     *      the card to lose datagrams. On the net thread's CPU, the only
     *      neighbour is a thread at priority 80 -- higher, then -- which the
     *      audio thread cannot disturb. It is the only one of the four CPUs
     *      that offers this guarantee.
     *
     * The blit CPU is excluded on principle: the CRT always wins, and it gets
     * no extra neighbour. */
    if (audio_cpu == -2)
        audio_cpu = net_cpu;

    /* Input channel. Same derivation rule as audio_cpu: -2 (not given)
     * follows the net thread's CPU. The port derives from listen_port + 1 to
     * match GroovyMAME's UDP_INPUT_PORT when the video port has its
     * production default (32100 -> 32101); a video port already at the
     * ceiling has no valid +1, so the thread is disabled rather than
     * overflowing silently. */
    if (input_cpu == -2)
        input_cpu = net_cpu;
    if (input_port < 0)
        input_port = (listen_port < 65535) ? listen_port + 1 : 0;

    if (!no_audio) {
        g_shared.audio = audio_out_create(audio_device, audio_cpu, 70,
                                          audio_dump);
        if (!g_shared.audio)
            fprintf(stderr, "{\"event\":\"audio_out_unavailable\"}\n");
    } else {
        fprintf(stderr, "{\"event\":\"audio_disabled_by_flag\"}\n");
    }

    atomic_store(&g_shared.running, 1);

    /* Allocate the jitter ring (~64K samples, about 18 minutes at 60 Hz). */
    struct jitter_ring *jr = jitter_create(65536);
    if (!jr) {
        fprintf(stderr, "{\"event\":\"jitter_create_failed\"}\n");
        free_pool(slots, SLOT_POOL_SIZE, ring);
        return 1;
    }

    /* Pre-create log directory (mode 0750). systemd's LogsDirectory=
     * does the same when running under the unit; this covers manual invocation. */
    if (mkdir("/var/log/crt-bridge", 0750) != 0 && errno != EEXIST) {
        /* Non-fatal: jitter export will retry against the user-supplied path. */
        fprintf(stderr, "{\"event\":\"mkdir_log_dir_warn\",\"errno\":%d}\n", errno);
    }

    /* Install signal handlers. SA_RESTART so blocked syscalls in the threads
     * don't error out before they observe shared.running == 0. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Block SIGPIPE -- UDP doesn't generate it, but a stray socket op shouldn't
     * kill the daemon if anything reaches for stderr/stdout under FIFO storms. */
    signal(SIGPIPE, SIG_IGN);

    /* The wake-up eventfd, created ONCE, before both threads, never on the
     * hot path. Non-blocking: the net thread must never wait on its write. A
     * failure is not fatal: the daemon says so and runs with the option off;
     * the blit thread then reports wake_on_frame 0 (flip_policy), the value
     * actually in effect. */
    if (wake_on_frame) {
        wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd < 0) {
            fprintf(stderr, "{\"event\":\"wake_fd_failed\",\"errno\":%d}\n", errno);
            wake_fd = -1;
        }
    }

    /* Spawn blit thread first -- it owns the modeset and DRM master.
     * blit_thread_start only CREATES the thread: it fails only on calloc or
     * pthread_create. A modeline rejection or a DRM failure happens later,
     * in the thread; it raises blit_fatal() and main() then exits with
     * code 1 -- with code 3 if the thread refused the configuration
     * (impossible mode, modeline outside the envelope). */
    struct blit_thread_config bcfg = {
        .drm_device          = drm_device,
        .width               = width,
        .height              = height,
        .refresh_hz          = refresh,
        .cpu_affinity        = blit_cpu,
        .sched_fifo_priority = 80,
        .idle_after_s        = idle_after_s,
        .idle_black          = idle_black,
        .field_phase         = field_phase,
        .wake_fd             = wake_fd,   /* -1 when the option is off */
        .shed_window         = shed_window,   /* 0 when dropping is off */
        .monitor             = &g_monitor,    /* never NULL */
        .monitor_from_file   = (monitor_path != NULL),
        .shared              = &g_shared,
        .jitter              = jr,
    };
    pthread_t blit_tid = 0;
    if (blit_thread_start(&bcfg, &blit_tid) != 0) {
        fprintf(stderr, "{\"event\":\"blit_thread_create_failed\"}\n");
        atomic_store(&g_shared.running, 0);
        if (wake_fd >= 0) close(wake_fd);
        jitter_destroy(jr);
        free_pool(slots, SLOT_POOL_SIZE, ring);
        return 1;
    }

    /* Spawn net thread. */
    struct net_thread_config ncfg = {
        .listen_port          = (uint16_t)listen_port,
        .busy_poll_us         = 50,
        .cpu_affinity         = net_cpu,
        .sched_fifo_priority  = 80,
        .heartbeat_timeout_ms = heartbeat_ms,
        .wake_fd              = wake_fd,   /* -1 when the option is off */
        .shared               = &g_shared,
    };
    pthread_t net_tid = 0;
    if (net_thread_start(&ncfg, &net_tid) != 0) {
        fprintf(stderr, "{\"event\":\"net_init_failed\"}\n");
        atomic_store(&g_shared.running, 0);
        pthread_join(blit_tid, NULL);
        if (wake_fd >= 0) close(wake_fd);
        jitter_destroy(jr);
        free_pool(slots, SLOT_POOL_SIZE, ring);
        return 1;
    }

    /* Input thread. Started AFTER net/blit, never before (it must never
     * preempt net/blit, neither at start-up nor by priority). A failure is
     * NOT fatal -- the exact pattern of the sound output's creation: logged,
     * and the daemon goes on without the input channel. */
    pthread_t input_tid = 0;
    int input_started = 0;
    if (!no_input && input_port > 0) {
        struct input_thread_config icfg = {
            .listen_port         = (uint16_t)input_port,
            .cpu_affinity        = input_cpu,
            .sched_fifo_priority = 70,
            .rescan_ms           = 1000u,
            .shared              = &g_shared,
        };
        if (input_thread_start(&icfg, &input_tid) == 0)
            input_started = 1;
        else
            fprintf(stderr, "{\"event\":\"input_init_failed\"}\n");   /* and go on */
    } else {
        fprintf(stderr, "{\"event\":\"input_disabled_by_flag\"}\n");
    }

    fprintf(stderr,
        "{\"event\":\"daemon_running\",\"pid\":%d,\"port\":%d,"
        "\"w\":%d,\"h\":%d,\"refresh\":%.1f,\"drm\":\"%s\","
        "\"net_cpu\":%d,\"blit_cpu\":%d}\n",
        (int)getpid(), listen_port, width, height, refresh, drm_device,
        net_cpu, blit_cpu);

    /* Main loop is just a join -- signals flip g_shared.running, threads exit. */
    pthread_join(net_tid, NULL);
    pthread_join(blit_tid, NULL);
    if (input_started) pthread_join(input_tid, NULL);

    /* After both threads: nobody writes or reads the eventfd any more. */
    if (wake_fd >= 0) {
        close(wake_fd);
        wake_fd = -1;
    }

    /* After the threads: nobody can call audio_out_push any more. */
    audio_out_destroy(g_shared.audio);
    g_shared.audio = NULL;

    /* ---------------------------------------------------------------------
     * Jitter export -- runs even when neither flag is given so that systemd
     * service crash investigations always have a CSV; --jitter-summary
     * additionally prints the parseable one-liner to stdout.
     * ------------------------------------------------------------------- */
    char path_buf[512];
    if (!jitter_csv) {
        snprintf(path_buf, sizeof path_buf,
                 "/var/log/crt-bridge/jitter-%d.csv", (int)getpid());
        jitter_csv = path_buf;
    }
    FILE *f = fopen(jitter_csv, "w");
    if (f) {
        (void)jitter_flush_csv(jr, f);
        fclose(f);
        if (chmod(jitter_csv, 0640) != 0) {
            /* Non-fatal: file exists with default umask; not a security gap
             * because LogsDirectory= already constrains the parent dir. */
        }
    } else {
        fprintf(stderr, "{\"event\":\"jitter_csv_open_failed\",\"path\":\"%s\",\"errno\":%d}\n",
                jitter_csv, errno);
    }

    if (want_summary) {
        double p50 = 0.0, p95 = 0.0, p99 = 0.0, p999 = 0.0;
        jitter_summary(jr, &p50, &p95, &p99, &p999);
        printf("{\"event\":\"jitter_summary\","
               "\"p50_ns\":%.0f,\"p95_ns\":%.0f,\"p99_ns\":%.0f,\"p999_ns\":%.0f}\n",
               p50, p95, p99, p999);
        fflush(stdout);
    }

    jitter_destroy(jr);
    free_pool(slots, SLOT_POOL_SIZE, ring);

    /* A configuration refusal in the blit thread comes before everything:
     * code 3, even if the same path raised blit_fatal().
     * Priority: 3 (configuration) > 1 (failure) > 0 (requested stop). */
    if (blit_config_rejected())
        return config_rejected();

    /* A stop caused by a failure (display, or the net thread dead on its
     * own) is not a clean stop: code 1, otherwise Restart=on-failure never
     * restarts. */
    if (blit_fatal() || net_fatal()) {
        /* Both causes named, even together; reason keeps the first one,
         * which tools and tests read. */
        const int b = blit_fatal() ? 1 : 0;
        const int n = net_fatal() ? 1 : 0;
        fprintf(stderr, "{\"event\":\"daemon_exit\",\"code\":1,\"reason\":\"%s\","
                        "\"blit\":%d,\"net\":%d}\n",
                b ? "blit_fatal" : "net_fatal", b, n);
        return 1;
    }
    return 0;
}
