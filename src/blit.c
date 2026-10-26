/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * blit.c -- DRM/KMS blit thread.
 *
 * Where the daemon meets the hardware. Start-up sequence:
 *
 *   switchres_setup (sr_init -> ten crt_rangeN reset to "auto" -> one
 *     crt_rangeN per band of the monitor file -> sr_set_option("api","drmkms")
 *     -> sr_set_monitor("custom") LAST: it applies the ranges already set
 *     right away) -> sr_init_disp(NULL,NULL)
 *     -> sr_switch_to_mode(320,240,60,0,&result) (legacy path only)
 *     -> modeset_guard(REAL drmModeModeInfo)   [the single point of passage]
 *     -> sr_deinit
 *     -> open("/dev/dri/card0") -> daemon takes DRM master
 *     -> pick connector+CRTC -> alloc 2 dumb buffers (XRGB8888) + addFB
 *     -> drmModeSetCrtc(fb[0])
 *     -> page-flip loop:
 *          spsc_try_consume(slot)
 *          blit RGB888 -> XRGB8888 into fb[next]
 *          drmModePageFlip(DRM_MODE_PAGE_FLIP_EVENT)
 *          poll(drm_fd, 2ms); drmHandleEvent on POLLIN
 *          page_flip_handler:  record jitter delta + atomic ACK signal
 *
 * Anti-patterns:
 *   - DRM atomic commit API          : NEVER (switchres uses legacy path)
 *   - page-flip while one is pending : NEVER (returns EBUSY storm)
 *   - silent fallback on rejection   : NEVER (hardware-loss-grade abort)
 */

/* Feature gates: _GNU_SOURCE for cpu_set_t / pthread_setaffinity_np;
 * _POSIX_C_SOURCE for clock_gettime under -std=c11. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "blit.h"
#include "idle.h"
#include "safety.h"
#include "monitor.h"    /* the CRT described by a file, monitor_check_mode */
#include "jitter.h"
#include "spsc.h"
#include "shed.h"     /* the adaptive shed */
#include "bandrule.h" /* the band rule: minimum time and ceiling */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <limits.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "modelines.h"  /* MODELINE_240P_SR_NTSC + MODELINE_480I_NTSC + enum daemon_regime; MODELINE_480P */
#include "regime.h"     /* regime_name, regime_crosses_band, regime_mmio_toggle_ok */
#include "mmio.h"       /* Seamless 240p<->480i toggle: 13-register CRTC poke */
#include "fieldphase.h" /* 480i: which field the switch lands on */

/* True when the blit thread stopped the daemon on a display failure.
 * Deliberately outside struct net_blit_shared: that struct is defined twice
 * (net.h and below), and a field added to only one of them would make the
 * two copies disagree. */
static atomic_int s_blit_fatal;

int blit_fatal(void)
{
    return atomic_load(&s_blit_fatal);
}

/* True when the blit thread rejected the requested configuration (a mode
 * switchres cannot make, a modeline rejected by the safety envelope, an
 * hfreq outside the band). main() then exits with code 3, which the unit
 * excludes from restarts: restarting does not repair a configuration.
 * Deliberately outside struct net_blit_shared: that struct is defined twice
 * (net.h and below), and a field added to only one of them would make the
 * two copies disagree. */
static atomic_int s_config_rejected;

int blit_config_rejected(void)
{
    return atomic_load(&s_config_rejected);
}

/* The 480p STATE, a value of `enum regime480p_state` (regime.h, six
 * values). R480P_ENABLED: its frozen mode was judged and accepted at
 * start-up by boot_guard_pinned (site boot_480p). Any other state: the 480p
 * regime alone is disabled (no high band in the monitor file, a high band
 * that does not admit the constant, switchres refusing); the daemon runs,
 * 240p and 480i too, and a 480p SWITCHRES is served as 480i (fallback).
 * Zero = R480P_NOT_JUDGED: the blit thread has judged nothing yet, which
 * is not a refusal. Written by the blit thread at start-up; read by the
 * network thread. Deliberately outside struct net_blit_shared: that struct
 * is defined twice (net.h and below), and a field added to only one of
 * them would make the two copies disagree. */
static atomic_int s_regime_480p_state;

int blit_regime_480p_state(void)
{
    return atomic_load(&s_regime_480p_state);
}

/* The return-to-idle FLAG: raised by the blit thread when it has just put
 * the CRT back to 240p because the receiver is idle, read and cleared in
 * one step by the network thread (blit_idle_return_take), which then knows
 * the game may have resumed without announcing its regime again. Outside
 * struct net_blit_shared for the same reason as the 480p state: that struct
 * is defined twice (net.h and below), and a field added to only one of
 * them would make the two copies disagree. */
static atomic_int s_idle_returned;

int blit_idle_return_take(void)
{
    return atomic_exchange(&s_idle_returned, 0);
}

/* The two reasons for a band crossing, written under the `reason` key of
 * band_crossing_begin (the bench tools read named fields). */
#define CROSSING_REASON_REQUEST      "request"
#define CROSSING_REASON_SESSION_END  "session_end"

/* "No wait target": outside the three served regimes. */
#define WAIT_TARGET_NONE  ((uint8_t)0xFFu)

/* -----------------------------------------------------------------------------
 * Shared types from net.h.
 *
 * These mirror the contract of net.h. They are defined here ONLY when net.h
 * has not been included yet, so that:
 *   (a) blit.c compiles cleanly in isolation;
 *   (b) a translation unit that #includes net.h then blit.h gets the
 *       canonical definitions from net.h and skips this block.
 * ---------------------------------------------------------------------------*/
#ifndef RECEIVER_NET_H

struct frame_slot {
    uint32_t frame_id;
    uint32_t expected_bytes;
    uint32_t received_bytes;
    /* LZ4 -- the flip thread does not use these, but they MUST be here: the
     * layout must be identical to net.h, otherwise every field that follows
     * is read at the wrong offset. */
    uint8_t  compressed;
    /* Decompression time of the image in us, set by the network thread,
     * read here for the latency CSV. SAME POSITION as in net.h, right after
     * `compressed`, in the padding (offset 14). */
    uint16_t decomp_us;
    uint32_t clear_bytes;
    uint16_t last_vsync;
    uint8_t  field;
    uint8_t  is_field;       /* kept in sync with net.h canonical definition */
    uint8_t *buf;            /* heap-allocated, size = max_frame_bytes */

    /* Latency instrumentation. Kept in sync with net.h canonical
     * definition -- same field order/types (manual-sync pattern). */
    uint64_t first_byte_ns;
    uint64_t ready_ns;
};

/* Kept in sync with net.h canonical definition. */
struct sr_mode_request {
    uint16_t h_active;
    uint16_t h_begin;
    uint16_t h_end;
    uint16_t h_total;
    uint16_t v_active;
    uint16_t v_begin;
    uint16_t v_end;
    uint16_t v_total;
    double   pclock;
    uint8_t  interlace;
    uint8_t  regime;         /* decided by the network thread, copy of net.h */
    uint8_t  _pad[6];
    double   refresh_hz;
    uint64_t monotonic_ns_received;
};

struct net_blit_shared {
    void               *spsc_ring;
    struct frame_slot **slots;
    size_t              slot_capacity;

    _Atomic uint32_t    expected_frame_bytes;

    /* Kept in sync with net.h canonical definition. */
    _Atomic uint32_t    h_active_announced;
    _Atomic uint32_t    v_active_announced;

    _Atomic uint8_t     ack_ready;
    _Atomic uint32_t    ack_frame_echo;
    _Atomic uint16_t    ack_vcount_echo;
    _Atomic uint32_t    ack_frame;
    _Atomic uint16_t    ack_vcount;
    _Atomic uint8_t     ack_flags;

    _Atomic int         running;

    /* Runtime CMD_SWITCHRES cross-thread surface. */
    _Atomic uint64_t          switchres_seq;
    struct sr_mode_request    request;
    _Atomic uint8_t           synced;
    _Atomic uint64_t          last_modeset_ns;

    /* UDP socket fd for SIGTERM shutdown(). */
    _Atomic int         net_sock_fd;

    /* Kept in sync with net.h canonical definition -- same field
     * order/types as the real struct so the byte layout matches across
     * TUs (manual-sync pattern). */
    _Atomic int         legacy_modeset;
    _Atomic uint8_t     current_regime;
};

#define FLAG_VRAM_READY      0x01
#define FLAG_VRAM_END_FRAME  0x02
#define FLAG_VRAM_SYNCED     0x04
/* Kept in sync with FLAG_VGA_F1 of proto.h (canonical source).
 * blit.c is compiled as its own translation unit and does not include
 * proto.h; this manual-sync duplicate is the same pattern as the three
 * flags above. */
#define FLAG_VGA_F1          0x20

#endif /* RECEIVER_NET_H */

/* -----------------------------------------------------------------------------
 * switchres C wrapper API (libswitchres.so 2.2.2).
 *
 * Hand-prototyped so that blit.c does not need the switchres dev headers.
 * libswitchres provides these symbols as extern "C".
 * ---------------------------------------------------------------------------*/
typedef struct sr_mode {
    int      width, height, refresh;
    double   vfreq, hfreq;
    uint64_t pclock;
    int      hbegin, hend, htotal;
    int      vbegin, vend, vtotal;
    int      interlace, doublescan;
    int      hsync, vsync;
    int      is_refresh_off, is_stretched;
    double   x_scale, y_scale, v_scale;
    int      id;
} sr_mode;

extern void sr_init(void);
extern void sr_set_monitor(const char *preset);
extern void sr_set_option(const char *key, const char *value);
extern int  sr_init_disp(const char *device, void *callback);
extern int  sr_switch_to_mode(int width, int height, double refresh, int flags, sr_mode *out);
extern void sr_deinit(void);

/* Runtime modeline computation. Signatures checked against
 * switchres_wrapper.h of switchres v2.2.2:
 *
 *   MODULE_API int sr_add_mode(int, int, double, int, sr_mode*);  // w, h, refresh, flags, out
 *   MODULE_API int sr_get_mode(int, sr_mode*);                    // by id, NOT by w/h/r
 *
 * Use sr_add_mode with flags=0 (FLUSH). Measured: rc=1 with the daemon
 * holding DRM master; coexists cleanly with switchres's drmModeAttachMode
 * side effect on the same fd.
 *
 * Compute-only fallback (DONT_FLUSH) is `flags = 1<<16` per
 * switchres_defines.h SR_MODE_DONT_FLUSH. */
extern int  sr_add_mode(int width, int height, double refresh, int flags, sr_mode *out);
extern int  sr_get_mode(int id, sr_mode *out);

/* Registration flag asking for an INTERLACED mode. Copied from
 * switchres_wrapper.h 2.2.2 (`#define SR_MODE_INTERLACED 1<<0`): this file
 * does not include the switchres header, the API is redeclared by hand.
 * With a TWO-band monitor file, the height no longer selects interlace
 * (the best match wins, and the 31 kHz band satisfies 480 lines in
 * progressive at 100 MHz): 480i asks for it explicitly. */
#define SR_MODE_INTERLACED (1 << 0)

/* -----------------------------------------------------------------------------
 * Internals
 * ---------------------------------------------------------------------------*/

#define NUM_FBS 2

/* ---------------------------------------------------------------------------
 * Latency instrumentation: decompose the time a frame spends in the daemon
 * between the three stages the two threads own (reassembly, SPSC queue,
 * scanout wait), plus the queue depth observed at consumption.
 * Pre-allocated ring, no malloc/printf/syscall on the hot path -- same
 * discipline as jitter.c. Lives entirely alongside jitter.{c,h}; that
 * format stays untouched.
 * ------------------------------------------------------------------------- */
#define LAT_RING_CAP 524288u   /* ~2.4 h at 60 Hz -- covers a 2 h session */

struct lat_sample {
    uint64_t flip_ts_ns;
    uint32_t frame_id;
    uint32_t reasm_ns;     /* ready_ns      - first_byte_ns */
    uint32_t queue_ns;     /* consume_ns    - ready_ns      */
    uint32_t scanout_ns;   /* flip_ts_ns    - consume_ns    */
    uint32_t spsc_depth;   /* spsc_count() right after the pop */
    uint32_t skipped;      /* stale images dropped for this slot */
    /* The parts that could only be bounded before, measured here. CSV
     * contract shared with the bench's run-report tool, word for word:
     *   vbl_seq    : the `frame` received by page_flip_handler; 0 on the
     *                simulated path (test_mode_no_drm), which has no vblank;
     *   flipreq_ns : return of a successful drmModePageFlip - consume_ns,
     *                that is the pixel conversion and the flip call;
     *                scanout_ns - flipreq_ns is the wait for the vblank;
     *   decomp_us  : LZ4 decompression of the image (net.c), included in
     *                reasm_ns; 0 when uncompressed;
     *   shed       : images shed by the adaptive shed (--shed-window) since
 *                the previous image shown, 0 or 1; already counted in skipped. */
    uint32_t vbl_seq;
    uint32_t flipreq_ns;
    uint16_t decomp_us;
    uint16_t shed;
};

/* 524 288 samples of 48 bytes (24 MiB) instead of 32 (16 MiB): the price of
 * the four extra columns, allocated once at thread entry. */
_Static_assert(sizeof(struct lat_sample) == 48,
               "struct lat_sample changed size -- review the cost of "
               "the latency ring (LAT_RING_CAP samples)");

_Static_assert((LAT_RING_CAP & (LAT_RING_CAP - 1u)) == 0u,
               "LAT_RING_CAP must be a power of two (mask indexing below)");

struct blit_internal {
    struct blit_thread_config cfg;

    int                       drm_fd;
    uint32_t                  crtc_id;
    uint32_t                  conn_id;
    /* Position of crtc_id in drmModeRes.crtcs, resolved once by
     * pick_crtc_connector; -1 if not found. This is the pipe index that
     * drmWaitVBlank expects in DRM_VBLANK_HIGH_CRTC_MASK -- without it, the
     * two waits of --field-phase queried pipe 0, which crtc_id does not
     * necessarily name. */
    int                       crtc_index;
    drmModeModeInfo           mode;             /* derived from sr_mode result */

    uint32_t                  fb_id[NUM_FBS];
    uint32_t                  handle[NUM_FBS];
    void                     *fb_map[NUM_FBS];
    size_t                    fb_size;
    uint32_t                  fb_pitch;
    uint32_t                  fb_w;             /* modeline width (may exceed cfg.width if super-res) */
    uint32_t                  fb_h;             /* modeline height */

    int                       next_fb;
    int                       flip_pending;

    /* Page flips refused in a row, EBUSY aside, on both call sites (main
     * loop and idle). Reset by any drmModePageFlip that returns 0. Private
     * to the blit thread: never in the structs that are defined twice.
     * flip_test_*: test hooks CRT_BRIDGE_TEST_FAIL_FLIP / _FLIP_N, read once
     * and only under test_mode_no_drm (see blit_thread_main). */
    unsigned                  flip_fail_run;
    int                       flip_test_fail_all;
    unsigned                  flip_test_fail_n;
    unsigned                  flip_test_k;

    uint64_t                  last_flip_ns;
    uint32_t                  current_frame_id;
    uint16_t                  current_vcount;
    /* is_field of the slot THAT PRODUCED the frame, captured once at the
     * consumption point (before any branch), NOT a mirror of the CRTC
     * hardware state. bi->current_regime (below) is only written at the end
     * of blit_regime_toggle, after the hardware switch really happened --
     * the modeset throttle (MIN_MODESET_INTERVAL_NS, safety.h) can delay
     * that write by up to 100 ms while ni->announced_regime (net.c) switches
     * synchronously. field_flag_for reads THIS field, not current_regime,
     * to stay correct during that window. */
    uint8_t                   current_is_field;

    /* 480i switch phase.
     * field_phase  : the mode asked on the command line (copy of cfg).
     * phase_locked : 1 as soon as the phase of the session is set. The
     *   re-phasing happens ONCE per entry into 480i, never per frame: 0x06
     *   pairs use two numbers each, so their opening parity is constant for
     *   the whole session. Deciding again at every flip would delay EVERY
     *   image on the old 0x07 path (one number per frame, alternating
     *   parity) -- a defect made up from nothing. Reset at every regime
     *   change (blit_regime_toggle). */
    uint8_t                   field_phase;
    uint8_t                   phase_locked;

    /* --- Idle screen ---------------------------------------------------
     *
     * Without it, empty queue = no more flips = the image of the last frame
     * stays on the scan forever, and on a CRT that leaves a mark.
     *
     * idle_flip is the flag that matters: an idle flip must feed NEITHER the
     * jitter, NOR the latency, NOR the acknowledgement. One flip per second
     * would inject one-second gaps into the statistic that the 99th
     * percentile is drawn from. */
    uint64_t                  idle_after_ns;      /* 0 = disabled */
    uint64_t                  last_content_ns;    /* last real frame */
    int                       idle_active;
    int                       idle_return_failed; /* the return to 240p failed
                              * (guard or call) during THIS idle period; no second attempt before
                              * the next idle period -- no series of returns on the CRT. Reset on
                              * entering idle. Private to the blit thread. */
    int                       idle_flip;          /* the flip in flight is an idle flip */
    uint64_t                  idle_last_draw_ns;
    uint64_t                  idle_next_move_ns;
    uint64_t                  idle_move_count;
    int                       idle_x, idle_y;
    char                      idle_ip[24];

    /* Runtime modeset state.
     *
     * test_mode_no_drm: CRT_BRIDGE_NO_DRM=1 short-circuit. Set once at thread
     *   entry from getenv. When set, blit_init skips DRM open + drmSetMaster +
     *   alloc_fb + drmModeSetCrtc + sr_init/sr_init_disp; blit_runtime_modeset
     *   skips drmModeSetCrtc + alloc_fb + drmModeRmFB. The state machine
     *   (synced atomic, last_modeset_ns, switchres_seq observation,
     *   modeset_guard calls) runs in full so test fixtures can drive it.
     *
     * pending_request / pending_seq: last-write-wins coalescing buffer.
     *   When a CMD_SWITCHRES arrives inside the rate-limit window, we stash
     *   the latest request here and consume only the most-recent one when
     *   the window expires. pending_seq == 0 means "no pending request".
     *
     * coalesced_count: telemetry -- number of times pending_request was
     *   overwritten before firing.
     *
     * last_rate_limited_seq: deduplicates the runtime_modeset_rate_limited
     *   log line to one emission per pending_seq value. */
    int                       test_mode_no_drm;
    struct sr_mode_request    pending_request;
    uint64_t                  pending_seq;
    uint64_t                  coalesced_count;
    uint64_t                  last_rate_limited_seq;

    /* Super-resolution dual-modeline state.
     *
     * mode_240p_super_res / mode_480i: cached drmModeModeInfo for each
     * registered regime, built directly from the locked constants in
     * modelines.h (NOT from switchres's computed geometry -- see
     * register_dual_modelines() for rationale). Populated once at boot
     * by register_dual_modelines(); read by blit_init_default() (initial
     * scan-out) and blit_regime_toggle() (runtime toggle fast-path).
     *
     * current_regime: blit-thread-local mirror of
     * cfg.shared->current_regime (enum daemon_regime). Kept for cheap
     * same-thread reads; the shared atomic is the cross-thread source of
     * truth net.c reads.
     *
     * fb_dump_path: test hook. Cached once from
     * getenv("CRT_BRIDGE_FB_DUMP_PATH") at thread entry (NULL in prod).
     * See the dump call site in blit_thread_main's page-flip loop. */
    drmModeModeInfo           mode_240p_super_res;
    drmModeModeInfo           mode_480i;
    /* The third frozen mode, MODELINE_480P (1280x480, progressive, 31 kHz),
     * built like the other two by pinned_modeline_to_drm. It scans out of
     * the SAME 2560x480 buffer: the CRTC scans only 1280 of its columns. Set
     * even when 480p is disabled (the guard decides whether it serves, not
     * its presence). */
    drmModeModeInfo           mode_480p;
    uint8_t                   current_regime;
    const char               *fb_dump_path;

    /* Band crossing (240p/480i <-> 480p) being measured. Private to the
     * blit thread, never in the structs that are defined twice.
     * crossing_await_first: armed by blit_regime_toggle after a crossing,
     * disarmed by the FIRST page flip that follows (real or simulated),
     * which writes band_crossing_first_image. crossing_t0_ns: the instant of
     * band_crossing_begin; crossing_setcrtc_ns: the duration of the call
     * that set the mode again; crossing_from/to: the two regimes (enum
     * daemon_regime). drop_before_ns: the instant the call that set the
     * mode again ended; an image whose ready_ns precedes it is acknowledged
     * WITHOUT being shown (it was composed for the old regime). Stays 0
     * outside a crossing: nothing is dropped then. crossing_dropped: the
     * number of images dropped that way since this crossing, reported by
     * band_crossing_first_image. */
    int                       crossing_await_first;
    uint64_t                  crossing_t0_ns;
    uint64_t                  crossing_setcrtc_ns;
    uint8_t                   crossing_from;
    uint8_t                   crossing_to;
    uint64_t                  drop_before_ns;
    uint32_t                  crossing_dropped;

    /* The band rule: how long the CRT stays in a band, how many crossings
     * per minute. Private to the blit thread, never in the structs that are
     * defined twice. Set at thread entry (blit_thread_main) with the
     * constants of safety.h; only the test hooks, read under
     * test_mode_no_drm, shorten it. A clock DISTINCT from the 100 ms floor
     * (last_modeset_ns): two rules, two instants.
     * last_deferred_seq: deduplicates band_crossing_deferred, one line per
     * pending_seq (same pattern as last_rate_limited_seq, separate counter). */
    struct bandrule           rule;
    uint64_t                  last_deferred_seq;

    /* The wait imposed by the band rule. Private to the blit thread.
     * band_wait: raised at the FIRST deferral of a request that crosses the
     * band (band_wait_begin: buffers zeroed, synced at 0), lowered when the
     * pending request is consumed -- by the crossing, by a cancellation
     * (back to the current regime) or by a newer request that does not
     * cross (240p <-> 480i). While it is raised, every consumed image is
     * acknowledged without being shown: the session lives, the screen stays
     * black. wait_dropped: the number of images acknowledged that way during
     * the wait, carried into crossing_dropped when the request goes out
     * (band_crossing_first_image.dropped counts the wait AND the cut after). */
    int                       band_wait;
    uint32_t                  wait_dropped;

    /* The target of the band wait, kept when band_wait_begin opens it for a
     * request that crosses: band_crossing_cancelled says what the wait was
     * aiming at when it ends without crossing. WAIT_TARGET_NONE outside such
     * a wait (and after a failed crossing, crossing_hold_black: a request
     * that lifts that black cancels nothing). Private to the blit thread. */
    uint8_t                   wait_target;

    /* The reason of the crossing being written, read by band_crossing_begin
     * under the `reason` key: "request" (a request from the emitter, the
     * default) or "session_end" (the return to 240p on entering idle). Set
     * around the call to blit_regime_toggle by blit_idle_step, set back to
     * "request" right after. Private to the blit thread. */
    const char               *crossing_reason;

    /* Test hook CRT_BRIDGE_TEST_FAIL_CROSSING: when it is 1, every call that
     * would cross the band is simulated as a FAILURE (read once, only under
     * test_mode_no_drm, see blit_thread_main). Private to the blit thread. */
    int                       crossing_test_fail;

    /* Latency instrumentation.
     *
     * lat_ring: calloc'd ONCE at blit thread entry (never on the hot path).
     * NULL if the allocation failed -- lat_ring_record() tests the pointer
     * and no-ops, the daemon stays fully functional.
     *
     * cur_*: captured once per consumed slot, right after spsc_try_consume,
     * BEFORE pixel conversion -- read by lat_ring_record() at EITHER
     * completion path (real page_flip_handler or test_mode_no_drm's
     * simulated flip), whichever fires for that slot.
     *
     * lat_win_*: 1-second aggregate window state (min/max/sum), reset after
     * each emitted {"event":"latency_1s",...} line. No sort, no allocation. */
    struct lat_sample        *lat_ring;
    size_t                     lat_ring_head;      /* monotonic write counter */
    size_t                     lat_ring_written;    /* tracks ring wrap, mirrors jitter.c */
    uint64_t                   cur_first_byte_ns;
    uint64_t                   cur_ready_ns;
    uint64_t                   cur_consume_ns;
    uint32_t                   cur_spsc_depth;
    /* Captured for the slot in flight, read by lat_ring_record: decomp_us
     * of the slot at consumption, flipreq_ns on the return of a successful
     * drmModePageFlip (or just before the simulated completion), shed =
     * pending_shed at consumption (adaptive shed, below). */
    uint16_t                   cur_decomp_us;
    uint32_t                   cur_flipreq_ns;
    uint16_t                   cur_shed;
    uint64_t                   lat_win_start_ns;
    uint32_t                   lat_win_n;
    uint64_t                   lat_total_min, lat_total_max, lat_total_sum;
    uint32_t                   lat_queue_min, lat_queue_max;
    uint64_t                   lat_queue_sum;
    uint64_t                   lat_reasm_sum;
    uint32_t                   lat_depth_max;
    const char               *lat_csv_path;

    /* Queue policy.
     * queue_fifo : 1 if CRT_BRIDGE_QUEUE_POLICY=fifo (an A/B switch without
     * a rebuild, like CRT_BRIDGE_LATENCY_CSV / CRT_BRIDGE_FB_DUMP_PATH; read
     * once at thread entry, never on the hot path). Default 0 = the "newest
     * wins" policy.
     * cur_skipped : images dropped to reach the current slot -- captured at
     * the same instant as the other cur_*, read by lat_ring_record.
     * lat_win_skipped / lat_skipped_total : 1 s window aggregate and total. */
    int                        queue_fifo;
    uint32_t                   cur_skipped;
    uint64_t                   lat_win_skipped;
    uint64_t                   lat_skipped_total;

    /* The adaptive shed (shed.h). Private to the blit thread, never in the
     * structs that are defined twice.
     * shed          : the window; window = 0 when shedding is off, and then
     *                 nothing below moves.
     * shed_armed    : set by page_flip_handler (real DRM path, 240p);
     *                 holds for the SINGLE consumption that follows the
     *                 event, and drops back to 0 there, image taken or not.
     * carry_skipped : images not shown (skipped by the queue + the shed
     *                 one) carried over to the next image shown.
     * pending_shed  : 1 if an image was shed since the last image shown;
     *                 becomes the `shed` column of the CSV. */
    struct shed_state          shed;
    int                        shed_armed;
    uint32_t                   carry_skipped;
    uint16_t                   pending_shed;

    /* After-the-fact check of the field bit.
     * field_win_f0/f1 count, over the current window, how many completions
     * saw FLAG_VGA_F1 clear (f0) or set (f1) -- recomputed by
     * field_flag_for(), never read back from the wire. Reset with the rest
     * of the window (same pattern as lat_win_skipped) and published in the
     * same latency_1s line. A healthy second of 480i gives two close
     * values; f1 == 0 over an interlaced session is the signature of a bit
     * stuck dead. */
    uint32_t                   field_win_f0;
    uint32_t                   field_win_f1;

    /* --- Field phase measurement: ON WHICH FIELD the flip lands.
     *
     *  flip_seq_s0/s1 : parity of the vblank sequence returned by the flip
     *      handler. In interlaced mode, DRM halves framedur_ns
     *      (DRM_MODE_FLAG_INTERLACE), which suggests one vblank per FIELD --
     *      not verified on this kernel, hence seq_d_min/max.
     *  seq_d_min/max : sequence gap between two consecutive flips. On the
     *      paired 0x06 path there is ONE flip per pair, so two fields: the
     *      gap is 2 if the counter counts fields, 1 if it counts images.
     *      THIS ONE NUMBER decides whether a one-field delay can be
     *      expressed with the sequence counter -- the field phase depends
     *      on it.
     *  seq_d_rejected : gaps left out of d_min/d_max because implausible
     *      (more than SEQ_D_PLAUSIBLE_MAX fields between two flips) after
     *      folding to 24 bits -- the radeon/DCE6 vblank counter is a 24-bit
     *      counter (max_vblank_count), and gaps of 2^24 + 1 at wrap-around
     *      were measured on the bench.
     *  ilace_b0/b1 : bit 0 of CRTC_INTERLACE_STATUS read at the flip.
     *  ilace_k0/k1 : the relation (bit0 XOR (sequence & 1)). If the register
     *      tells the truth, it is CONSTANT over a session (only one of the
     *      two values moves); if both rise together, the register says
     *      nothing usable and the matter is closed.
     *
     * Same one-second window, same reset and same log line as
     * field_win_f0/f1. */
    uint32_t flip_seq_s0, flip_seq_s1;
    uint32_t seq_d_min, seq_d_max;
    uint32_t seq_d_rejected;
    uint32_t ilace_b0, ilace_b1;
    uint32_t ilace_k0, ilace_k1;
    uint32_t last_flip_seq;
    uint8_t  have_last_flip_seq;
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Saturating delta: later - earlier, clamped to [0, UINT32_MAX]. Redirects a
 * negative delta (daemon restart, slot recycle, clock not yet initialised)
 * to 0 rather than wrapping. Explicit cast on the narrowing return -- the
 * Makefile builds with -Wconversion. */
static uint32_t sat_delta_ns(uint64_t later, uint64_t earlier)
{
    if (later <= earlier) {
        return 0u;
    }
    uint64_t d = later - earlier;
    return (d > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)d;
}

/* field_flag_for -- the formula is written ONCE, called at BOTH completion
 * sites (page_flip_handler and its test_mode_no_drm mirror) AND by
 * lat_ring_record for the field.f0/f1 check -- a diverging copy would give
 * a false diagnosis during the very session that decides the phase,
 * without any automated test noticing (the protocol tests check the bit on
 * the wire, not these counters).
 *
 * GroovyMAME computes field(n) = F ^ ((n - G) mod 2) where F is this bit
 * and G our ack_frame (drawnogpu.cpp); our decoder of interlaced 0x06
 * reads field(n) = n mod 2. The two only agree if F = G mod 2 -- this is
 * forced, not a matter of style.
 *
 * A blind toggle (^= 1 per flip) would be WRONG here: in 480i one flip is
 * worth TWO GroovyMAME frames, so G keeps a constant parity, and an
 * alternating F would swap the two fields of every other pair. On our own
 * 0x07 traffic (one flip per frame), the parity of ack_frame toggles at
 * every flip -- both readings agree, it is the same rule.
 *
 * The first parameter used to be bi->current_regime, the HARDWARE mirror
 * of the CRTC (written once, at the end of blit_regime_toggle, AFTER the
 * switch really happened). The modeset throttle (MIN_MODESET_INTERVAL_NS =
 * 100 ms, safety.h) can delay that write by several frames at 60 Hz while
 * ni->announced_regime (net.c) switches SYNCHRONOUSLY as soon as
 * CMD_SWITCHRES is received. During that window, a frame truly in the NEW
 * regime (correctly paired on the network side) could still be
 * acknowledged with the parity of the OLD regime. The parameter now
 * carries `is_field` -- captured once per frame, at the slot consumption
 * point (bi->current_is_field, before any branch), from the value net.c
 * already set synchronously (struct frame_slot.is_field, set by net.c in
 * its 0x06 and 0x07 cases). Immune to the delay of the deferred hardware
 * switch.
 *
 * 240p: no field, the bit stays clear (is_field == 0). NO allocation / NO
 * blocking I/O -- callable from page_flip_handler (hot-path budget). */
static uint8_t field_flag_for(uint8_t is_field, uint32_t frame_id)
{
    return (is_field && (frame_id & 1u))
        ? (uint8_t)FLAG_VGA_F1 : (uint8_t)0u;
}

/* Vblank sequence gap beyond which a measurement is no longer one: 64
 * fields, that is more than one second between two flips in 480i -- more
 * than the full-frame period of the emitter's frame duplication
 * (GM_FULL_FRAME_PERIOD_NS, 1 s). Above it, it is a residual wrap-around
 * or a gap, counted in seq_d_rejected rather than kept in d_min/d_max. */
#define SEQ_D_PLAUSIBLE_MAX 64u

/* lat_ring_record -- sample capture + 1 s aggregate. Called from BOTH
 * flip-completion paths (page_flip_handler = real DRM, blit_thread_main's
 * test_mode_no_drm branch = simulated) with the same bi->cur_* fields
 * captured once at spsc_try_consume time. NO allocation, NO syscall, NO
 * sort -- a single fprintf fires at most once per second (same acceptance
 * as the daemon's existing on-event logging). */
static void lat_ring_record(struct blit_internal *bi, uint64_t flip_ts_ns,
                            uint32_t seq, uint8_t have_seq)
{
    if (!bi->lat_ring) {
        return;   /* calloc failed at thread entry -- instrumentation disabled */
    }

    uint32_t reasm_ns   = sat_delta_ns(bi->cur_ready_ns,   bi->cur_first_byte_ns);
    uint32_t queue_ns   = sat_delta_ns(bi->cur_consume_ns, bi->cur_ready_ns);
    uint32_t scanout_ns = sat_delta_ns(flip_ts_ns,         bi->cur_consume_ns);

    /* Recomputes the SAME formula as page_flip_handler/test_mode_no_drm
     * (field_flag_for, a single definition): lat_ring_record is called
     * BEFORE the expression is computed at the acknowledgement site itself,
     * and does not receive it as a parameter. bi->current_is_field and
     * bi->current_frame_id are already set at this point (at the slot
     * consumption point, before the two completion branches) -- no longer
     * bi->current_regime (a hardware mirror that may lag), see the comment
     * of field_flag_for. */
    uint8_t field_bit_here = field_flag_for(bi->current_is_field, bi->current_frame_id);

    size_t idx = bi->lat_ring_head & (size_t)(LAT_RING_CAP - 1u);
    bi->lat_ring[idx].flip_ts_ns = flip_ts_ns;
    bi->lat_ring[idx].frame_id   = bi->current_frame_id;
    bi->lat_ring[idx].reasm_ns   = reasm_ns;
    bi->lat_ring[idx].queue_ns   = queue_ns;
    bi->lat_ring[idx].scanout_ns = scanout_ns;
    bi->lat_ring[idx].spsc_depth = bi->cur_spsc_depth;
    bi->lat_ring[idx].skipped    = bi->cur_skipped;
    bi->lat_ring[idx].vbl_seq    = have_seq ? seq : 0u;
    bi->lat_ring[idx].flipreq_ns = bi->cur_flipreq_ns;
    bi->lat_ring[idx].decomp_us  = bi->cur_decomp_us;
    bi->lat_ring[idx].shed       = bi->cur_shed;
    bi->lat_ring_head++;
    bi->lat_ring_written++;

    /* 1-second aggregate window. total_ns is derived here (sum of three
     * saturating uint32_t, so uint64_t avoids any overflow concern) and again
     * at CSV-write time -- never stored in the ring itself. */
    uint64_t total_ns = (uint64_t)reasm_ns + (uint64_t)queue_ns + (uint64_t)scanout_ns;

    if (bi->lat_win_n == 0u) {
        bi->lat_win_start_ns = flip_ts_ns;
        bi->lat_total_min    = total_ns;
        bi->lat_total_max    = total_ns;
        bi->lat_queue_min    = queue_ns;
        bi->lat_queue_max    = queue_ns;
        bi->lat_depth_max    = bi->cur_spsc_depth;
        bi->lat_total_sum    = 0u;
        bi->lat_queue_sum    = 0u;
        bi->lat_reasm_sum    = 0u;
        bi->lat_win_skipped  = 0u;
        bi->field_win_f0     = 0u;
        bi->field_win_f1     = 0u;
        bi->flip_seq_s0      = 0u;
        bi->flip_seq_s1      = 0u;
        bi->seq_d_min        = UINT32_MAX;
        bi->seq_d_max        = 0u;
        bi->seq_d_rejected   = 0u;
        bi->ilace_b0         = 0u;
        bi->ilace_b1         = 0u;
        bi->ilace_k0         = 0u;
        bi->ilace_k1         = 0u;
    } else {
        if (total_ns < bi->lat_total_min) bi->lat_total_min = total_ns;
        if (total_ns > bi->lat_total_max) bi->lat_total_max = total_ns;
        if (queue_ns  < bi->lat_queue_min) bi->lat_queue_min = queue_ns;
        if (queue_ns  > bi->lat_queue_max) bi->lat_queue_max = queue_ns;
        if (bi->cur_spsc_depth > bi->lat_depth_max) bi->lat_depth_max = bi->cur_spsc_depth;
    }
    bi->lat_total_sum += total_ns;
    bi->lat_queue_sum += (uint64_t)queue_ns;
    bi->lat_reasm_sum += (uint64_t)reasm_ns;
    bi->lat_win_skipped += (uint64_t)bi->cur_skipped;
    if (field_bit_here) {
        bi->field_win_f1++;
    } else {
        bi->field_win_f0++;
    }

    /* Pure instrumentation: this MEASURES the field phase, it does not
     * correct it. None of these lines touches the flip.
     *
     * Kept HERE, after the window reset above and at the same place as
     * field_win_f0/f1 -- not in page_flip_handler before the call, as it
     * used to be: incrementing BEFORE lat_ring_record let the
     * `lat_win_n == 0` block erase the contribution of the first flip of
     * EVERY window (s0 + s1 was n - 1, never n). have_seq is 0 on the
     * simulated path (test_mode_no_drm), which has no vblank sequence: the
     * phase counters stay at zero there, as before.
     * last_flip_seq/have_last_flip_seq are NOT window counters: they carry
     * across seconds. */
    if (have_seq) {
        if (seq & 1u) {
            bi->flip_seq_s1++;
        } else {
            bi->flip_seq_s0++;
        }
        if (bi->have_last_flip_seq) {
            /* The radeon/DCE6 vblank counter is a 24-bit counter
             * (max_vblank_count): without folding, its wrap-around gives a
             * gap of 2^24 + 1, measured on the bench and kept in d_max,
             * where it polluted the window. So we fold to 24 bits, then set
             * aside any implausible gap -- residual wrap-around or a hole,
             * not a measurement -- in a separate counter, so that it stays
             * visible. */
            uint32_t d = (seq - bi->last_flip_seq) & 0x00FFFFFFu;
            if (d <= SEQ_D_PLAUSIBLE_MAX) {
                if (d < bi->seq_d_min) bi->seq_d_min = d;
                if (d > bi->seq_d_max) bi->seq_d_max = d;
            } else {
                bi->seq_d_rejected++;
            }
        }
        bi->last_flip_seq      = seq;
        bi->have_last_flip_seq = 1u;
        if (bi->current_is_field && mmio_available()) {
            /* One uncached 32-bit read per flip (30 per second on the
             * paired path), guarded by the interlaced regime AND by MMIO
             * availability. Precedent: mmio_seamless_toggle already reads
             * CRTC_DOUBLE_BUFFER_CONTROL after each poke. */
            uint32_t st = mmio_interlace_status();
            if (st != 0xFFFFFFFFu) {
                uint32_t b = st & 1u;
                if (b) bi->ilace_b1++; else bi->ilace_b0++;
                if (b ^ (seq & 1u)) bi->ilace_k1++; else bi->ilace_k0++;
            }
        }
    }
    bi->lat_win_n++;

    if (flip_ts_ns - bi->lat_win_start_ns >= 1000000000ull) {
        fprintf(stderr,
                "{\"event\":\"latency_1s\",\"n\":%" PRIu32 ","
                "\"total_us\":{\"min\":%" PRIu64 ",\"avg\":%" PRIu64 ",\"max\":%" PRIu64 "},"
                "\"queue_us\":{\"min\":%" PRIu32 ",\"avg\":%" PRIu64 ",\"max\":%" PRIu32 "},"
                "\"reasm_us\":{\"avg\":%" PRIu64 "},\"depth_max\":%" PRIu32 ","
                "\"skipped\":%" PRIu64 ",\"skipped_total\":%" PRIu64 ","
                "\"field\":{\"f0\":%" PRIu32 ",\"f1\":%" PRIu32 "}"
                ",\"flip\":{\"s0\":%" PRIu32 ",\"s1\":%" PRIu32 ",\"d_min\":%" PRIu32 ",\"d_max\":%" PRIu32 ",\"d_rej\":%" PRIu32 "}"
                ",\"ilace\":{\"b0\":%" PRIu32 ",\"b1\":%" PRIu32 ",\"k0\":%" PRIu32 ",\"k1\":%" PRIu32 "}}\n",
                bi->lat_win_n,
                bi->lat_total_min / 1000u,
                bi->lat_total_sum / (uint64_t)bi->lat_win_n / 1000u,
                bi->lat_total_max / 1000u,
                bi->lat_queue_min / 1000u,
                bi->lat_queue_sum / (uint64_t)bi->lat_win_n / 1000u,
                bi->lat_queue_max / 1000u,
                bi->lat_reasm_sum / (uint64_t)bi->lat_win_n / 1000u,
                bi->lat_depth_max,
                bi->lat_win_skipped,
                bi->lat_skipped_total,
                bi->field_win_f0,
                bi->field_win_f1,
                bi->flip_seq_s0,
                bi->flip_seq_s1,
                (bi->seq_d_min == UINT32_MAX) ? 0u : bi->seq_d_min,
                bi->seq_d_max,
                bi->seq_d_rejected,
                bi->ilace_b0,
                bi->ilace_b1,
                bi->ilace_k0,
                bi->ilace_k1);
        bi->lat_win_n       = 0u;
        bi->lat_total_sum   = 0u;
        bi->lat_queue_sum   = 0u;
        bi->lat_reasm_sum   = 0u;
        bi->lat_win_skipped = 0u;
        bi->field_win_f0    = 0u;
        bi->field_win_f1    = 0u;
        bi->flip_seq_s0      = 0u;
        bi->flip_seq_s1      = 0u;
        bi->seq_d_min        = UINT32_MAX;
        bi->seq_d_max        = 0u;
        bi->seq_d_rejected   = 0u;
        bi->ilace_b0         = 0u;
        bi->ilace_b1         = 0u;
        bi->ilace_k0         = 0u;
        bi->ilace_k1         = 0u;
    }
}

/* Convert sr_mode -> drmModeModeInfo for legacy drmModeSetCrtc. */
static void sr_mode_to_drm(const sr_mode *s, drmModeModeInfo *m)
{
    memset(m, 0, sizeof *m);
    m->clock       = (uint32_t)(s->pclock / 1000U);          /* kHz */
    m->hdisplay    = (uint16_t)s->width;
    m->hsync_start = (uint16_t)s->hbegin;
    m->hsync_end   = (uint16_t)s->hend;
    m->htotal      = (uint16_t)s->htotal;
    m->vdisplay    = (uint16_t)s->height;
    m->vsync_start = (uint16_t)s->vbegin;
    m->vsync_end   = (uint16_t)s->vend;
    m->vtotal      = (uint16_t)s->vtotal;
    m->vrefresh    = (uint32_t)(s->vfreq + 0.5);
    m->flags       = (uint32_t)(
        (s->hsync     ? DRM_MODE_FLAG_PHSYNC    : DRM_MODE_FLAG_NHSYNC) |
        (s->vsync     ? DRM_MODE_FLAG_PVSYNC    : DRM_MODE_FLAG_NVSYNC) |
        (s->interlace ? DRM_MODE_FLAG_INTERLACE : 0u)                  |
        (s->doublescan? DRM_MODE_FLAG_DBLSCAN   : 0u));
    snprintf(m->name, sizeof m->name, "%dx%d@%u",
             s->width, s->height, (unsigned)m->vrefresh);
}

/* Convert a locked pinned_modeline (modelines.h) directly into a
 * drmModeModeInfo -- deliberately NOT via switchres/sr_mode. modelines.h's
 * header banner is explicit: blit.c includes that header and reads these
 * constants directly, it does NOT re-derive or hardcode any timing value.
 * switchres's own modeline math (sr_add_mode) is not guaranteed to
 * reproduce the exact register values validated on the hardware
 * (h_total=1544, originally pclock_hz=24271680) -- those numbers were
 * validated on the JVC CRT with a standalone libdrm tool, bypassing
 * switchres entirely. This helper is the single point where modelines.h's
 * locked fields become the drmModeModeInfo actually handed to
 * drmModeSetCrtc.
 *
 * pclock_hz was later retuned to 24240000 (alignment on the console
 * frequency, a latency correction) -- the geometry measured on the bench
 * (h_total, v_total, porches) is unchanged and remains the reference; only
 * the pixel clock moved, per modelines.h's PCLOCK REACCORD banner. No code
 * in this function changed. */
static void pinned_modeline_to_drm(const struct pinned_modeline *pm,
                                    drmModeModeInfo *m)
{
    memset(m, 0, sizeof *m);
    m->clock       = pm->pclock_hz / 1000U;   /* Hz -> kHz, matches sr_mode_to_drm convention */
    m->hdisplay    = (uint16_t)pm->width;
    m->hsync_start = pm->h_begin;
    m->hsync_end   = pm->h_end;
    m->htotal      = pm->h_total;
    m->vdisplay    = (uint16_t)pm->height;
    m->vsync_start = pm->v_begin;
    m->vsync_end   = pm->v_end;
    m->vtotal      = pm->v_total;
    m->vrefresh    = (uint32_t)(pm->vfreq_hz + 0.5);
    m->flags       = (uint32_t)(
        (pm->hsync     ? DRM_MODE_FLAG_PHSYNC    : DRM_MODE_FLAG_NHSYNC) |
        (pm->vsync     ? DRM_MODE_FLAG_PVSYNC    : DRM_MODE_FLAG_NVSYNC) |
        (pm->interlace ? DRM_MODE_FLAG_INTERLACE : 0u));
    snprintf(m->name, sizeof m->name, "%dx%d@%u",
             pm->width, pm->height, (unsigned)m->vrefresh);
}

/* The SINGLE point where switchres is configured: the USABLE bands of the
 * same file as the guard, never a hard-coded preset. The file describes
 * every band; switchres only receives those of the given mask, renumbered
 * from crt_range0. Reason: at --legacy-modeset start-up, switchres sets its
 * first mode itself before the guard judges it; a 31 kHz band it knew
 * about could therefore reach the CRT. Order (checked off the bench
 * against switchres 2.2.2):
 *   1. sr_init() reads a switchres.ini found in ./, ./ini/ or /etc/: a
 *      possible stray. The ten slots crt_range0..9 are therefore reset to
 *      "auto" (skipped by set_preset) before ours are written.
 *   2. one crt_range<k> line per given band, k from 0 to given-1, with
 *      band.text -- the VERY text the strict loader validated, never read
 *      again from disk: a single parser, no race between validation and
 *      switchres. The loader requires a low band, so given >= 1.
 *   3. "api" = drmkms.
 *   4. sr_set_monitor("custom") LAST: display_manager::set_monitor runs
 *      set_preset() right away, the ranges must already be set.
 * Called before sr_init_disp, in place of the three earlier calls (sr_init,
 * preset, api). The log says how many bands the file describes and how many
 * are given to switchres.
 *
 * The mask of given bands is a PARAMETER, chosen by the caller -- never
 * computed here. The --legacy-modeset path passes the low-band mask only
 * (switchres sets a mode there before the guard and must never know about
 * 31 kHz); the default path adds the high bands if and only if 480p is
 * enabled. */
static void switchres_setup(const struct monitor *m, unsigned usable)
{
    char key[16];
    unsigned given_idx[MONITOR_MAX_BANDS];
    const unsigned given = monitor_switchres_bands(m, usable, given_idx);

    sr_init();
    for (int k = 0; k < MONITOR_MAX_BANDS; k++) {
        snprintf(key, sizeof key, "crt_range%d", k);
        sr_set_option(key, "auto");
    }
    for (int k = 0; k < (int)given; k++) {
        snprintf(key, sizeof key, "crt_range%d", k);
        sr_set_option(key, m->band[given_idx[k]].text);
    }
    sr_set_option("api", "drmkms");
    sr_set_monitor("custom");
    fprintf(stderr,
            "{\"event\":\"switchres_configured\",\"monitor\":\"custom\","
            "\"described\":%u,\"given\":%u}\n", m->n, given);
}

/* The SINGLE point of passage: no mode goes to the card (drmModeSetCrtc
 * with a mode, MMIO switch) without being judged here, in this function.
 * It judges the drmModeModeInfo REALLY handed over (clock already truncated
 * to kHz), in real numbers, without rounding or margin, and as an (H, V)
 * pair inside one band of the file. The mask of usable bands is RECEIVED
 * from the caller, per regime -- never computed here, never the union of
 * both bands. 240p and 480i pass the low-band mask (monitor_low_bands_mask,
 * or monitor_regime_mask of their regime); 480p passes the mask of the high
 * band alone (monitor_regime_mask(..., REGIME_480P)). The --legacy-modeset
 * path (blit_init_legacy, blit_runtime_modeset) passes ONLY the low mask:
 * the 31 kHz band stays unusable there, otherwise a CMD_SWITCHRES at
 * 31.4 kHz forged on the wire would be programmed with no test pattern and
 * nobody watching.
 *
 * At --legacy-modeset start-up, switchres sets its first mode itself
 * (sr_switch_to_mode) before this guard judges it: so it has only ever
 * known the usable bands, because switchres_setup only gives it those of
 * the same mask. This guard remains the judge of what comes out.
 *
 * A refusal is logged (modeline_rejected, in real Hz); the caller decides
 * what follows. h_out / v_out / band_out may be NULL; band_out is the index
 * in the file's table, -1 on refusal. */
static modeline_result_t modeset_guard(const struct blit_internal *bi, const char *site,
                                       const drmModeModeInfo *m, unsigned usable,
                                       double *h_out, double *v_out, int *band_out)
{
    double h = 0.0, v = 0.0;
    int band = -1;
    modeline_result_t r = monitor_check_mode(
        bi->cfg.monitor, m->clock, m->htotal, m->vtotal,
        (m->flags & DRM_MODE_FLAG_INTERLACE) != 0,
        (m->flags & DRM_MODE_FLAG_DBLSCAN) != 0,
        usable, &h, &v, &band);
    if (r != MODELINE_ACCEPT) {
        char where[48];
        snprintf(where, sizeof where, "blit.c::%s", site);
        safety_log_rejection_hz(where, h, v, r);
    }
    if (h_out)    *h_out = h;
    if (v_out)    *v_out = v;
    if (band_out) *band_out = band;
    return r;
}

/* The two pinned modes of modelines.h are judged at start-up, BEFORE any
 * image and BEFORE any drmModeSetCrtc, on the REAL constants
 * (pinned_modeline_to_drm is pure: it does not touch DRM), even under
 * CRT_BRIDGE_NO_DRM whose synthetic modes have no timing. A file whose low
 * band excludes 15 699.5 Hz therefore stops the daemon here:
 * s_config_rejected, then -2 (exit code 3). Both modes are judged even if
 * the first is refused: the log says everything the file excludes. The
 * third mode (480p) is judged too, under its own mask, and its refusal
 * does not kill the daemon (see the block below). */
static int boot_guard_pinned(struct blit_internal *bi)
{
    static const struct {
        const char                         *site;
        const struct pinned_modeline      *pm;
        unsigned                            regime;
    } pinned[2] = {
        { "boot_240p", &MODELINE_240P_SR_NTSC, (unsigned)REGIME_240P_SUPER_RES },
        { "boot_480i", &MODELINE_480I_NTSC,    (unsigned)REGIME_480I           },
    };
    int refused = 0;

    for (int i = 0; i < 2; i++) {
        drmModeModeInfo mode;
        double h = 0.0, v = 0.0;
        int band = -1;

        pinned_modeline_to_drm(pinned[i].pm, &mode);
        if (modeset_guard(bi, pinned[i].site, &mode,
                          monitor_regime_mask(bi->cfg.monitor, pinned[i].regime),
                          &h, &v, &band) != MODELINE_ACCEPT) {
            refused = 1;
            continue;
        }
        fprintf(stderr,
                "{\"event\":\"modeset_guard_accepted\",\"site\":\"%s\","
                "\"h_hz\":%.2f,\"v_hz\":%.2f,\"band\":%d}\n",
                pinned[i].site, h, v, band);
    }

    /* The third frozen mode, MODELINE_480P, is judged here under the mask
     * of its OWN regime alone (the high band) -- and this judgement is NEVER
     * fatal. Accepted: 480p is enabled (s_regime_480p_state =
     * R480P_ENABLED, regime_enabled). Refused: the 480p regime alone is
     * disabled (regime_disabled) and the daemon runs, 240p and 480i too.
     * With no high band in the file (the mask is empty) there is nothing to
     * judge: the guard is not called, it would write a modeline_rejected at
     * every start of a CRT without 31 kHz.
     * The state is not reset to "disabled" on entry -- it is
     * R480P_NOT_JUDGED (zero) until here, and only the VERDICT is written.
     * Under --legacy-modeset no regime resolver runs and the legacy path
     * never gives the high band -- so 480p is NOT judged: the guard is not
     * called at site boot_480p, no regime_enabled is written, the state is
     * R480P_LEGACY and the log says so (regime_disabled, reason
     * legacy_modeset). */
    if (atomic_load(&bi->cfg.shared->legacy_modeset)) {
        atomic_store(&s_regime_480p_state, R480P_LEGACY);
        fprintf(stderr,
                "{\"event\":\"regime_disabled\",\"regime\":\"480p\","
                "\"reason\":\"legacy_modeset\"}\n");
    } else if (monitor_regime_mask(bi->cfg.monitor, (unsigned)REGIME_480P) == 0u) {
        atomic_store(&s_regime_480p_state, R480P_NO_HIGH_BAND);
        fprintf(stderr,
                "{\"event\":\"regime_disabled\",\"regime\":\"480p\","
                "\"reason\":\"no_high_band\"}\n");
    } else {
        drmModeModeInfo mode;
        double h = 0.0, v = 0.0;
        int band = -1;

        pinned_modeline_to_drm(&MODELINE_480P, &mode);
        if (modeset_guard(bi, "boot_480p", &mode,
                          monitor_regime_mask(bi->cfg.monitor, (unsigned)REGIME_480P),
                          &h, &v, &band) != MODELINE_ACCEPT) {
            atomic_store(&s_regime_480p_state, R480P_GUARD_REFUSED);
            fprintf(stderr,
                    "{\"event\":\"regime_disabled\",\"regime\":\"480p\","
                    "\"reason\":\"guard_refused\"}\n");
        } else {
            atomic_store(&s_regime_480p_state, R480P_ENABLED);
            fprintf(stderr,
                    "{\"event\":\"regime_enabled\",\"regime\":\"480p\","
                    "\"h_hz\":%.2f,\"v_hz\":%.2f,\"band\":%d}\n", h, v, band);
        }
    }

    if (refused) {
        atomic_store(&s_config_rejected, 1);   /* exit code 3, no restart */
        return -2;
    }
    return 0;
}

/* Register both super-res modelines at boot.
 *
 * sr_add_mode is NOT a gate. On the stock kernel switchres can create no
 * mode (the drmkms backend lacks CUSTOM_VIDEO_CAPS_ADD): it only looks for
 * an EXISTING mode of the connector that lies inside a given band, and its
 * answer is never read back. With the JVC EDID forced on DVI-I-1 that is
 * 640x240 at 15 655 Hz; on any other connector (another output, another
 * card, a virtual machine) no existing mode may lie in the 15 kHz band, and
 * rc != 1. That says nothing about the tube nor about the monitor file, only
 * about the connector's mode list -- boot_guard_pinned has already judged
 * the frozen modes, and boot_default judges the first one again before it
 * reaches the card. A refusal is therefore journalled
 * (switchres_no_existing_mode) and the daemon goes on: neither exit code 1
 * nor exit code 3. Only a failed sr_init_disp (no display at all) stays
 * fatal. The --legacy-modeset CLI flag (read in main.c) routes the daemon
 * to blit_init_legacy() instead of this path -- a bisection escape hatch.
 *
 * sr_add_mode(w, h, refresh, flags, out) is called for BOTH regimes (flags
 * = FLUSH, per the switchres_wrapper.h signature -- flags is a switchres
 * behavior bitmask). With a TWO-band monitor file, the height no longer
 * selects interlace -- the best match wins, and the 31 kHz band satisfies
 * 480 lines in progressive at 100 MHz. 480i is therefore asked for with
 * SR_MODE_INTERLACED, 240p keeps 0. With a single band, the height was
 * enough. The drmModeModeInfo actually cached for drmModeSetCrtc use is
 * built SEPARATELY, directly from the locked modelines.h constants via
 * pinned_modeline_to_drm() -- see that function's banner for why
 * switchres's own computed geometry is not trusted for the real scan-out
 * mode.
 *
 * Defense-in-depth: drmSetMaster re-grabbed after sr_deinit (amdgpu strips
 * master when switchres opens its fd).
 */
static int register_dual_modelines(struct blit_internal *bi)
{
    sr_mode r240, r480;
    memset(&r240, 0, sizeof r240);
    memset(&r480, 0, sizeof r480);

    /* switchres receives the low bands, plus the high bands if and only if
     * 480p is enabled (the guard judged it at start-up). A disabled 480p
     * leaves switchres no 31 kHz band. */
    unsigned given_mask = monitor_low_bands_mask(bi->cfg.monitor);
    if (blit_regime_480p_state() == R480P_ENABLED)
        given_mask |= monitor_regime_mask(bi->cfg.monitor, (unsigned)REGIME_480P);
    switchres_setup(bi->cfg.monitor, given_mask);
    int disp_idx = sr_init_disp(NULL, NULL);
    if (disp_idx < 0) {
        fprintf(stderr,
                "{\"event\":\"modeline_registration_failed\","
                "\"reason\":\"sr_init_disp_failed\",\"rc\":%d}\n", disp_idx);
        sr_deinit();
        return -1;
    }

    /* No existing connector mode in the given bands: journalled, never
     * fatal (see the banner). The file and the tube are not in question,
     * the connector's mode list is. */
    if (sr_add_mode(MODELINE_240P_SR_NTSC.width,
                    MODELINE_240P_SR_NTSC.height,
                    MODELINE_240P_SR_NTSC.vfreq_hz,
                    0 /* SR_MODE flags: 0 = FLUSH */,
                    &r240) != 1) {
        fprintf(stderr,
                "{\"event\":\"switchres_no_existing_mode\","
                "\"regime\":\"240p_super_res\",\"width\":%d,\"height\":%d}\n",
                MODELINE_240P_SR_NTSC.width, MODELINE_240P_SR_NTSC.height);
    }
    if (sr_add_mode(MODELINE_480I_NTSC.width,
                    MODELINE_480I_NTSC.height,
                    MODELINE_480I_NTSC.vfreq_hz,
                    SR_MODE_INTERLACED /* FLUSH + interlace asked for */,
                    &r480) != 1) {
        fprintf(stderr,
                "{\"event\":\"switchres_no_existing_mode\","
                "\"regime\":\"480i\",\"width\":%d,\"height\":%d}\n",
                MODELINE_480I_NTSC.width, MODELINE_480I_NTSC.height);
    }
    /* 480p, registered after the other two, flag 0 (progressive). When
     * switchres CREATED a mode for the request (rc = 1, the size asked
     * for), it must be PROGRESSIVE and its line rate at least that of the
     * high band (H_BAND_HIGH_MIN_HZ, hfreq in real Hz); a mismatch disables
     * 480p alone -- neither s_config_rejected nor an error: 240p and 480i
     * are still served. The mode served remains the frozen constant.
     *
     * On the stock kernel switchres cannot CREATE any mode: the drmkms
     * backend lacks CUSTOM_VIDEO_CAPS_ADD (test_kernel_user_modes() fails,
     * the drmModeAttachMode switch has no effect). sr_add_mode then returns,
     * with rc = 1, the EXISTING connector mode closest to the request --
     * here the only mode of the EDID, 640x240 progressive at 15 655 Hz, for
     * any request (reproduced off the bench against switchres 2.2.2). Its
     * line rate says nothing about 480p; judging it cut 480p at every
     * start-up. This mode is recognised by its size, different from the
     * request: it is logged (switchres_returned_existing_mode). When no
     * existing mode lies in the given bands at all (rc != 1), it is the same
     * doctrine as 240p and 480i: logged (switchres_no_existing_mode), 480p
     * left as the guard set it. In both cases the guard (boot_guard_pinned),
     * which already judged the constant, remains the only judge. */
    if (blit_regime_480p_state() == R480P_ENABLED) {
        sr_mode r480p;
        memset(&r480p, 0, sizeof r480p);
        const int rc480p = sr_add_mode(MODELINE_480P.width,
                                       MODELINE_480P.height,
                                       MODELINE_480P.vfreq_hz,
                                       0 /* progressive, FLUSH */,
                                       &r480p);
        if (rc480p != 1) {
            fprintf(stderr,
                    "{\"event\":\"switchres_no_existing_mode\","
                    "\"regime\":\"480p\",\"width\":%d,\"height\":%d}\n",
                    MODELINE_480P.width, MODELINE_480P.height);
        } else if (r480p.width != MODELINE_480P.width ||
                   r480p.height != MODELINE_480P.height) {
            fprintf(stderr,
                    "{\"event\":\"switchres_returned_existing_mode\",\"regime\":\"480p\","
                    "\"asked_width\":%d,\"asked_height\":%d,\"got_width\":%d,"
                    "\"got_height\":%d,\"interlace\":%d,\"hfreq_hz\":%.1f}\n",
                    MODELINE_480P.width, MODELINE_480P.height,
                    r480p.width, r480p.height, r480p.interlace, r480p.hfreq);
        } else if (r480p.interlace != 0 ||
                   r480p.hfreq < (double)H_BAND_HIGH_MIN_HZ) {
            atomic_store(&s_regime_480p_state, R480P_SWITCHRES_REFUSED);
            fprintf(stderr,
                    "{\"event\":\"regime_disabled\",\"regime\":\"480p\","
                    "\"reason\":\"switchres_refused\",\"rc\":%d,"
                    "\"interlace\":%d,\"hfreq_hz\":%.1f}\n",
                    rc480p, r480p.interlace, r480p.hfreq);
        }
    }
    sr_deinit();

    /* Defense: re-grab DRM master after switchres tore it down. */
    if (drmSetMaster(bi->drm_fd) < 0) {
        fprintf(stderr,
                "{\"event\":\"drmsetmaster_failed_post_dual_register\","
                "\"errno\":%d}\n", errno);
        return -1;
    }

    /* The ACTUAL scan-out drmModeModeInfo: built from the locked constants,
     * NOT from r240/r480's switchres-computed geometry (see banner above). */
    pinned_modeline_to_drm(&MODELINE_240P_SR_NTSC, &bi->mode_240p_super_res);
    pinned_modeline_to_drm(&MODELINE_480I_NTSC,    &bi->mode_480i);
    /* 480p, same construction, alongside. switchres was asked for it above
     * (check of its interlace and line rate when it created one); the mode
     * served remains the frozen constant, judged by boot_guard_pinned. */
    pinned_modeline_to_drm(&MODELINE_480P,         &bi->mode_480p);

    /* Defense check: 480i MUST have INTERLACE flag, 240p MUST NOT. Sourced
     * from modelines.h's own .interlace field via pinned_modeline_to_drm,
     * so this is deterministic (not dependent on switchres's own guess). */
    if ((bi->mode_480i.flags & DRM_MODE_FLAG_INTERLACE) == 0) {
        fprintf(stderr,
                "{\"event\":\"modeline_registration_failed\","
                "\"regime\":\"480i\",\"reason\":\"interlace_flag_missing\"}\n");
        return -1;
    }
    if ((bi->mode_240p_super_res.flags & DRM_MODE_FLAG_INTERLACE) != 0) {
        fprintf(stderr,
                "{\"event\":\"modeline_registration_failed\","
                "\"regime\":\"240p_super_res\",\"reason\":\"interlace_flag_unexpected\"}\n");
        return -1;
    }

    bi->current_regime = (uint8_t)REGIME_240P_SUPER_RES;
    /* count: the two frozen modes the daemon serves (240p and 480i), plus
     * 480p when the guard enabled it -- whatever switchres found on the
     * connector. */
    fprintf(stderr,
            "{\"event\":\"modeline_registration_succeeded\",\"count\":%d,"
            "\"default_regime\":\"240p_super_res\"}\n",
            2 + (blit_regime_480p_state() == R480P_ENABLED ? 1 : 0));
    return 0;
}

/* Walk DRM resources, pick first CONNECTED connector with a working encoder.
 * Also resolves the index of the chosen CRTC in res->crtcs (*crtc_index_out,
 * -1 if not found): this is the pipe index that drmWaitVBlank expects. */
static int pick_crtc_connector(int fd, uint32_t *crtc_out, uint32_t *conn_out,
                               int *crtc_index_out)
{
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        return -1;
    }
    int found = -1;
    *crtc_index_out = -1;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) {
            continue;
        }
        if (c->connection == DRM_MODE_CONNECTED && c->encoder_id) {
            drmModeEncoder *e = drmModeGetEncoder(fd, c->encoder_id);
            if (e && e->crtc_id) {
                *crtc_out = e->crtc_id;
                *conn_out = c->connector_id;
                for (int k = 0; k < res->count_crtcs; k++) {
                    if (res->crtcs[k] == e->crtc_id) {
                        *crtc_index_out = k;
                        break;
                    }
                }
                found = 0;
            }
            if (e) {
                drmModeFreeEncoder(e);
            }
        }
        drmModeFreeConnector(c);
        if (found == 0) {
            break;
        }
    }
    drmModeFreeResources(res);
    return found;
}

/* Allocate one dumb buffer + drmModeAddFB + mmap. Returns 0 on success. */
static int alloc_fb(int fd, int w, int h,
                    uint32_t *fb_id, uint32_t *handle,
                    void **map, size_t *size, uint32_t *pitch)
{
    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof creq);
    creq.width  = (uint32_t)w;
    creq.height = (uint32_t)h;
    creq.bpp    = 32U;

    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        return -1;
    }
    *handle = creq.handle;
    *size   = creq.size;
    *pitch  = creq.pitch;

    if (drmModeAddFB(fd, (uint32_t)w, (uint32_t)h, 24, 32,
                     creq.pitch, creq.handle, fb_id) < 0) {
        return -2;
    }

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof mreq);
    mreq.handle = creq.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        return -3;
    }

    *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                fd, (off_t)mreq.offset);
    if (*map == MAP_FAILED) {
        /* A failed buffer is NULL, never MAP_FAILED -- blit_cleanup only
         * releases non-null entries. */
        *map = NULL;
        return -4;
    }
    memset(*map, 0, creq.size); /* black */
    return 0;
}

/* Bounds: the blit is limited by min(slot_pixels, fb_pixels) and by
 * pitch * height destination bytes (fb_pitch is what mmap gave us; the
 * destination row stride may exceed w*4 due to driver alignment).
 *
 * Best integer fit: switchres on arcade_15 + the JVC envelope produces
 * fb_w=640 for the entire 240p hfreq class (15.625-15.750 kHz) regardless of
 * source dims. Pixel-perfect requires integer scaling, so we pick the largest
 * N>=1 such that src_w*N <= fb_w and center the result. Source widths that
 * divide fb_w cleanly (256->2x, 320->2x, 640->1x) render fullscreen; others
 * (gridlee 341, etc.) render with symmetric black bands. Pre-clear ensures
 * the bands stay black across geometry changes. Pixel-perfect fullscreen
 * for non-divisor widths would need bypassing switchres or a switchres
 * patch; not done. */
static void blit_rgb888_to_xrgb8888(const uint8_t *src,
                                    void *dst,
                                    int src_w, int src_h,
                                    int src_pixels,
                                    uint32_t pitch,
                                    int fb_w, int fb_h)
{
    int x_scale = src_w > 0 ? fb_w / src_w : 1;
    if (x_scale < 1) x_scale = 1;
    int draw_w = src_w * x_scale;
    if (draw_w > fb_w) draw_w = fb_w;
    int x_offset = (fb_w - draw_w) / 2;

    int draw_h = src_h < fb_h ? src_h : fb_h;
    int y_offset = (fb_h - draw_h) / 2;

    memset(dst, 0, (size_t)fb_h * pitch);

    int max_src_pixels = src_w * draw_h;
    int n = src_pixels < max_src_pixels ? src_pixels : max_src_pixels;
    int rows = n / src_w;

    for (int sy = 0; sy < rows; sy++) {
        const uint8_t *srow = src + (size_t)sy * (size_t)src_w * 3U;
        uint8_t *drow = (uint8_t *)dst
                      + (size_t)(y_offset + sy) * pitch
                      + (size_t)x_offset * 4U;
        for (int sx = 0; sx < src_w; sx++) {
            int dx = sx * x_scale;
            if (dx + x_scale > draw_w) break;
            uint8_t r = srow[sx * 3 + 0];
            uint8_t g = srow[sx * 3 + 1];
            uint8_t b = srow[sx * 3 + 2];
            uint8_t *block = drow + (size_t)dx * 4U;
            for (int rep = 0; rep < x_scale; rep++) {
                block[rep * 4 + 0] = b;
                block[rep * 4 + 1] = g;
                block[rep * 4 + 2] = r;
                block[rep * 4 + 3] = 0;
            }
        }
    }
}


/* --- The guard that was missing --------------------------------------------
 *
 * struct frame_slot is defined TWICE: here and in the other file (net.h is
 * canonical; the blit.c copy is the active one here, because blit.c does
 * not include net.h). Adding a field to one without the other gives no
 * warning: the two units read the same memory with two different layouts.
 *
 * What happened when `compressed` and `clear_bytes` were added to net.h
 * alone: the flip thread read `buf` at the wrong offset, got a bogus
 * pointer, and the kernel killed the daemon -- "segfault at 34800", that is
 * NULL + 320*224*3, the exact size of one Sonic 2 frame.
 *
 * Both definitions are now checked against the SAME numbers. A divergence
 * breaks the build instead of breaking the bench. Changing one number
 * requires changing both, which is exactly the reminder wanted. */
_Static_assert(sizeof(struct frame_slot) == 48,
               "frame_slot changed size -- net.h and blit.c must be "
               "changed TOGETHER (see the comment above)");
_Static_assert(offsetof(struct frame_slot, buf) == 24,
               "the offset of frame_slot.buf moved -- this is EXACTLY the "
               "defect that once killed the daemon");
_Static_assert(offsetof(struct frame_slot, ready_ns) == 40,
               "the offset of frame_slot.ready_ns moved");
_Static_assert(offsetof(struct frame_slot, decomp_us) == 14,
               "the offset of frame_slot.decomp_us moved -- it must stay "
               "in the padding after `compressed`, at the same position as net.h");

/* Same guard for struct sr_mode_request, also defined TWICE (net.h is
 * canonical, the blit.c copy is always active). The `regime` field takes
 * the first padding byte, at offset 25, in both files. A divergence breaks
 * the build, not the bench. */
_Static_assert(sizeof(struct sr_mode_request) == 48,
               "sr_mode_request changed size -- net.h and blit.c must be "
               "changed TOGETHER (see the comment above)");
_Static_assert(offsetof(struct sr_mode_request, regime) == 25,
               "the offset of sr_mode_request.regime moved -- net.h and blit.c "
               "must be changed TOGETHER (see the comment above)");

/* The end of a band-crossing measurement: the FIRST page flip after the
 * mode change (real, timestamp of the DRM event; or simulated, now_ns()).
 * One line per crossing, never per image: the flag drops here, so nothing
 * costs on the hot path in steady state. `dropped` is the number of images
 * CONSUMED then acknowledged without being shown (the wait imposed by the
 * rule, plus the cut after the crossing): a minimum, because the images
 * that the "newest wins" queue skipped do not pass through here. Both
 * completion sites call this function, with the same expression. */
static void crossing_note_first_image(struct blit_internal *bi, uint64_t first_image_ns)
{
    if (!bi->crossing_await_first) {
        return;
    }
    bi->crossing_await_first = 0;
    fprintf(stderr,
            "{\"event\":\"band_crossing_first_image\",\"from\":\"%s\",\"to\":\"%s\","
            "\"t0_ns\":%llu,\"setcrtc_ns\":%llu,\"first_image_ns\":%llu,"
            "\"frame_id\":%u,\"dropped\":%u}\n",
            regime_name(bi->crossing_from), regime_name(bi->crossing_to),
            (unsigned long long)bi->crossing_t0_ns,
            (unsigned long long)bi->crossing_setcrtc_ns,
            (unsigned long long)first_image_ns,
            (unsigned)bi->current_frame_id,
            (unsigned)bi->crossing_dropped);
}

/* An image composed for the old regime, arrived before the reprogramming
 * ended: acknowledged, neither converted nor shown. The emitter waits for
 * the acknowledgement of each image; the CRT must never show an image of
 * the old mode stretched over the new one. The same four writes as the
 * simulated completion and page_flip_handler (the field parity leaves with
 * the acknowledgement, in the same step), but from the slot itself: no
 * page flip happens, flip_pending does not move. */
static void blit_ack_without_display(struct blit_internal *bi, const struct frame_slot *slot)
{
    uint8_t field_bit = field_flag_for(slot->is_field, slot->frame_id);

    atomic_store(&bi->cfg.shared->ack_frame,  slot->frame_id);
    atomic_store(&bi->cfg.shared->ack_vcount, slot->last_vsync);
    atomic_store(&bi->cfg.shared->ack_flags,
                 (uint8_t)(FLAG_VRAM_READY | FLAG_VRAM_END_FRAME | field_bit));
    atomic_store(&bi->cfg.shared->ack_ready,  (uint8_t)1);
    /* An image of real traffic, even dropped, proves a session is alive --
     * idle (blit_idle_step) must not draw an idle screen over the black of a
     * crossing. */
    bi->last_content_ns = now_ns();
    if (bi->band_wait) {
        bi->wait_dropped++;
    } else {
        bi->crossing_dropped++;
    }
}

/* The first deferral of a request that crosses the band: the screen turns
 * black (both buffers zeroed, as before a crossing) and synced drops to 0
 * (the same step as blit_regime_toggle before a mode change). The wait can
 * last seconds; the old image, composed for the old regime, is no longer
 * shown. While band_wait is raised, the loop acknowledges images without
 * showing them. One call per wait: the flag is the guard. No flip is
 * drained here: nothing is reprogrammed, only the buffer contents change. */
static void band_wait_begin(struct blit_internal *bi)
{
    for (int i = 0; i < NUM_FBS; i++) {
        if (bi->fb_map[i]) {
            memset(bi->fb_map[i], 0, bi->fb_size);
        }
    }
    atomic_store(&bi->cfg.shared->synced, 0u);
    bi->band_wait    = 1;
    bi->wait_dropped = 0u;
}

/* A band crossing that failed (guard refusal, or the call failed): the
 * hardware stayed in the old mode, while the network thread has already
 * announced the requested regime. Images of the requested regime must not
 * be converted into the old mode (the top half of a 480-line test pattern
 * would show cropped at 15 kHz): the screen stays black and the loop
 * acknowledges without showing -- the same state as the wait imposed by
 * the rule (band_wait) -- until the next consumed request. Unlike
 * band_wait_begin, synced stays at 1: the receiver is ready for the current
 * band, and a synced at 0 with no planned end would block an emitter that
 * honours it. A crossing whose measurement was still armed
 * (crossing_await_first) is disarmed: the measurement would only close on
 * the image after the wait. */
static void crossing_hold_black(struct blit_internal *bi)
{
    for (int i = 0; i < NUM_FBS; i++) {
        if (bi->fb_map[i]) {
            memset(bi->fb_map[i], 0, bi->fb_size);
        }
    }
    atomic_store(&bi->cfg.shared->synced, 1u);
    bi->band_wait            = 1;
    bi->wait_dropped         = 0u;
    bi->wait_target          = WAIT_TARGET_NONE;   /* this black waits for no target */
    bi->crossing_await_first = 0;
}

/* The call that was to cross the band failed AFTER it was sent to the card
 * (or was simulated failing: CRT_BRIDGE_TEST_FAIL_CROSSING). A failure is
 * not guaranteed to leave the CRT untouched: the radeon driver is not
 * atomic and turns the DAC off before reprogramming the mode. The attempt
 * therefore counts as a crossing, for the band rule (last crossing,
 * ceiling ring) as for the 100 ms floor: an oscillating emitter no longer
 * re-runs drmModeSetCrtc at every SWITCHRES. The band_crossing_failed event
 * is a failure in the series for the bench report. Returns -1: the request
 * is consumed, the daemon stays alive. */
static int crossing_failed(struct blit_internal *bi, uint8_t have, uint8_t want,
                           uint64_t t0, int err)
{
    fprintf(stderr,
            "{\"event\":\"band_crossing_failed\",\"from\":\"%s\",\"to\":\"%s\","
            "\"t0_ns\":%llu,\"errno\":%d}\n",
            regime_name(have), regime_name(want),
            (unsigned long long)t0, err);
    bandrule_note_crossing(&bi->rule, t0);
    atomic_store(&bi->cfg.shared->last_modeset_ns, now_ns());
    crossing_hold_black(bi);
    return -1;
}

/* DRM page-flip event callback. Runs in drmHandleEvent caller context (the
 * blit thread). NO allocation / NO blocking I/O -- hot-path budget. */
static void page_flip_handler(int fd,
                              unsigned int frame,
                              unsigned int sec,
                              unsigned int usec,
                              void *data)
{
    (void)fd;
    struct blit_internal *bi = (struct blit_internal *)data;

    /* Idle flip: it showed no frame. Recording it would skew the jitter (a
     * one-second gap), the latency (timestamps of the previous frame) and
     * the acknowledgement (a frame_id already acknowledged). Settle the
     * flip and leave. */
    if (bi->idle_flip) {
        bi->idle_flip    = 0;
        bi->flip_pending = 0;
        bi->last_flip_ns = 0;   /* the next real flip starts afresh */
        return;
    }

    /* CLOCK_MONOTONIC since kernel 4.15. */
    uint64_t ts_ns = (uint64_t)sec * 1000000000ULL + (uint64_t)usec * 1000ULL;
    uint64_t delta = bi->last_flip_ns ? (ts_ns - bi->last_flip_ns) : 0ULL;
    bi->last_flip_ns = ts_ns;

    /* The first image after a band crossing. */
    crossing_note_first_image(bi, ts_ns);

    if (delta) {
        jitter_record(bi->cfg.jitter, ts_ns, delta, frame);
    }

    /* Real DRM completion path. ts_ns is CLOCK_MONOTONIC from the DRM
     * event, directly comparable to now_ns(). The vblank sequence goes
     * along with the timestamp -- the phase counters (flip_seq_*, seq_d_*,
     * ilace_*) are kept INSIDE lat_ring_record, after its window reset, not
     * here before the call (otherwise the first flip of each second was
     * erased). */
    lat_ring_record(bi, ts_ns, (uint32_t)frame, 1u);

    /* The excess of the image just shown, on the real DRM path only: the
     * simulated path (test_mode_no_drm) has no vblank grid, it never gets
     * here. Progressive 240p only; in 480i (or on an interlaced mode), the
     * window starts again from zero. Period and vblank come from the active
     * mode; a zero clock gives a zero period, hence a zero excess. Integer
     * arithmetic, no call. */
    if (bi->shed.window > 0u) {
        if (bi->current_regime == (uint8_t)REGIME_240P_SUPER_RES &&
            !bi->current_is_field &&
            !(bi->mode.flags & DRM_MODE_FLAG_INTERLACE)) {
            uint32_t vb_lines = (bi->mode.vtotal > bi->mode.vdisplay)
                ? (uint32_t)(bi->mode.vtotal - bi->mode.vdisplay) : 0u;
            uint64_t excess_ns = shed_excess(
                ts_ns, bi->cur_ready_ns,
                shed_mode_ns(bi->mode.htotal, bi->mode.vtotal, bi->mode.clock),
                shed_mode_ns(bi->mode.htotal, vb_lines, bi->mode.clock),
                SHED_MARGIN_NS);
            bi->shed_armed = shed_note(&bi->shed, excess_ns);
        } else {
            shed_reset(&bi->shed);
            bi->shed_armed = 0;
        }
    }

    bi->flip_pending = 0;

    /* FLAG_VGA_F1 carries the parity of the frame published in this SAME
     * acknowledgement (see field_flag_for above for the full derivation).
     * Computed and published in the same atomic step as
     * ack_frame/ack_vcount -- GroovyMAME combines the three in a single
     * formula (drawnogpu.cpp); desynchronising them would produce a wrong
     * field choice without any of the three values being wrong on its own.
     * bi->current_is_field (not bi->current_regime) -- see the comment of
     * field_flag_for. */
    uint8_t field_bit = field_flag_for(bi->current_is_field, bi->current_frame_id);

    /* Signal net thread to send an ACK for the frame just shown. */
    atomic_store(&bi->cfg.shared->ack_frame,  bi->current_frame_id);
    atomic_store(&bi->cfg.shared->ack_vcount, bi->current_vcount);
    atomic_store(&bi->cfg.shared->ack_flags,
                 (uint8_t)(FLAG_VRAM_READY | FLAG_VRAM_END_FRAME | field_bit));
    atomic_store(&bi->cfg.shared->ack_ready,  (uint8_t)1);
}

/* -----------------------------------------------------------------------------
 * blit_idle_step -- one idle step. Called when the queue is empty.
 *
 * Does nothing until the silence has lasted idle_after_ns. Then redraws
 * once per second and flips. The text block moves every IDLE_MOVE_NS,
 * because a still text leaves a mark too.
 * -------------------------------------------------------------------------- */
#define IDLE_DRAW_NS 1000000000ULL   /* one redraw per second: the clock moves on */
#define IDLE_MOVE_NS 20000000000ULL  /* the block moves every 20 s */

/* Threshold of consecutive page flip failures, EBUSY aside, before exiting
 * with code 1. 60 = one second at 60 Hz, chosen by the maintainer. An
 * EACCES (DRM master lost), an EINVAL or an ENODEV (radeon reset) do not
 * pass; EBUSY does: it is never counted. */
#define FLIP_FAIL_FATAL 60u

/* A page flip refused, EBUSY aside, at either site: `site` is "blit"
 * (blit_thread_main, drmModePageFlip_err line) or "idle" (blit_idle_step,
 * idle_flip_err line). At the threshold, the fatal step: s_blit_fatal then
 * running = 0; main() exits with code 1. Never exit() from a thread. */
static void flip_failed(struct blit_internal *bi, int err, const char *site)
{
    fprintf(stderr, "{\"event\":\"%s\",\"errno\":%d}\n",
            strcmp(site, "idle") == 0 ? "idle_flip_err" : "drmModePageFlip_err",
            err);
    if (++bi->flip_fail_run >= FLIP_FAIL_FATAL) {
        fprintf(stderr,
                "{\"event\":\"page_flip_fatal\",\"run\":%u,\"errno\":%d,"
                "\"site\":\"%s\"}\n", bi->flip_fail_run, err, site);
        atomic_store(&s_blit_fatal, 1);
        atomic_store(&bi->cfg.shared->running, 0);
    }
}

/* blit_regime_toggle is defined below: the return to 240p on idle calls it. */
static int blit_regime_toggle(struct blit_internal *bi,
                              const struct sr_mode_request *req);

static void blit_idle_step(struct blit_internal *bi, uint64_t now)
{
    /* The idle STATE is kept on real DRM as in test mode; only drawing the
     * idle screen and the flip stay specific to real DRM (the
     * `test_mode_no_drm` exit below). Before, test mode left at once and no
     * idle entry existed off the hardware: a test of the return to 240p
     * could not see it. */
    if (bi->idle_after_ns == 0ULL || bi->flip_pending)
        return;
    if (bi->last_content_ns == 0ULL)
        bi->last_content_ns = now;          /* first round: start from here */
    if (now - bi->last_content_ns < bi->idle_after_ns)
        return;

    int entering = 0;
    if (!bi->idle_active) {
        entering              = 1;
        bi->idle_active       = 1;
        bi->idle_return_failed = 0;
        bi->idle_last_draw_ns = 0ULL;
        bi->idle_next_move_ns = 0ULL;
        shed_reset(&bi->shed);              /* idle breaks the series */
        bi->shed_armed        = 0;
        fprintf(stderr,
                "{\"event\":\"idle_enter\",\"silence_s\":%llu}\n",
                (unsigned long long)((now - bi->last_content_ns) / 1000000000ULL));
    }

    /* The return to 240p. A receiver in 480p that goes idle puts the CRT
     * back to 15 kHz through the NORMAL crossing path: blit_regime_toggle
     * judges 240p with the guard under its mask, zeroes the buffers, writes
     * band_crossing_begin; the 100 ms floor and the band rule are checked
     * HERE before the call, as blit_check_pending_switchres does for a
     * request. Refused by either, nothing happens and the next round (the
     * queue is empty, this step comes again) retries -- no sleep and no
     * timer in this thread. A request already waiting (pending_seq) is not
     * overtaken. A failure (guard, call) is not retried during this idle
     * period: no series of returns on the CRT. The decision COMES BEFORE the
     * test-mode exit below: that is what a test sees. */
    if (!bi->idle_return_failed &&
        regime_idle_return_wanted(atomic_load(&bi->cfg.shared->current_regime),
                                  bi->idle_active,
                                  atomic_load(&bi->cfg.shared->legacy_modeset))) {
        const uint64_t last_ms = atomic_load(&bi->cfg.shared->last_modeset_ns);
        if (bi->pending_seq == 0 &&
            (last_ms == 0 || now - last_ms >= MIN_MODESET_INTERVAL_NS) &&
            bandrule_allows(&bi->rule, now)) {
            struct sr_mode_request ret;
            memset(&ret, 0, sizeof ret);
            ret.regime                 = (uint8_t)REGIME_240P_SUPER_RES;
            ret.monotonic_ns_received  = now;
            /* As blit_check_pending_switchres does when a request goes out: the
             * black of a failed crossing (crossing_hold_black), if any, ends here. */
            bi->band_wait        = 0;
            bi->crossing_dropped = bi->wait_dropped;
            bi->wait_dropped     = 0u;
            bi->wait_target      = WAIT_TARGET_NONE;
            bi->crossing_reason  = CROSSING_REASON_SESSION_END;
            const int rc = blit_regime_toggle(bi, &ret);
            bi->crossing_reason  = CROSSING_REASON_REQUEST;
            if (rc == 0 &&
                atomic_load(&bi->cfg.shared->current_regime) == (uint8_t)REGIME_240P_SUPER_RES) {
                atomic_store(&s_idle_returned, 1);   /* read by the network thread (resume) */
                return;     /* the idle screen is drawn on the next round, at 15 kHz */
            }
            bi->idle_return_failed = 1;
        }
    }

    if (bi->test_mode_no_drm)
        return;                             /* no drawing and no flip without DRM */
    if (!entering && now - bi->idle_last_draw_ns < IDLE_DRAW_NS)
        return;
    bi->idle_last_draw_ns = now;

    int fb_w    = (int)bi->fb_w;
    int fb_h    = bi->mode.vdisplay ? (int)bi->mode.vdisplay : (int)bi->fb_h;
    int x_scale = idle_x_scale(fb_w);
    int y_scale = idle_y_scale(fb_h);
    void *dst   = bi->fb_map[bi->next_fb];
    if (!dst) return;

    if (bi->cfg.idle_black) {
        /* The minimal version: a black screen, nothing else. The clear
         * covers the whole allocation, not only the scan window: in
         * super-resolution the two differ. */
        idle_render_black(dst, bi->fb_pitch, (int)bi->fb_h);
    } else {
        char clock_text[8]  = "--:--";
        char addr_text[40];
        time_t t = time(NULL);
        struct tm tmv;
        if (localtime_r(&t, &tmv))
            (void)strftime(clock_text, sizeof clock_text, "%H:%M", &tmv);
        (void)snprintf(addr_text, sizeof addr_text, "%s", bi->idle_ip);

        const char *text_lines[4] = { "CRT-BRIDGE", "WAITING", addr_text, clock_text };

        int block_w = 0, block_h = 0;
        idle_text_extent(text_lines, 4, x_scale, y_scale, &block_w, &block_h);

        if (now >= bi->idle_next_move_ns) {
            idle_place(bi->idle_move_count++, fb_w, fb_h, block_w, block_h,
                       &bi->idle_x, &bi->idle_y);
            bi->idle_next_move_ns = now + IDLE_MOVE_NS;
        }

        /* Clear the whole allocation first -- idle_render only clears the
         * height it is given, and in super-resolution fb_h > vdisplay. */
        idle_render_black(dst, bi->fb_pitch, (int)bi->fb_h);
        (void)idle_render(dst, bi->fb_pitch, fb_w, fb_h, x_scale, y_scale,
                          bi->idle_x, bi->idle_y, text_lines, 4, IDLE_TEXT_LEVEL);
    }

    if (drmModePageFlip(bi->drm_fd, bi->crtc_id, bi->fb_id[bi->next_fb],
                        DRM_MODE_PAGE_FLIP_EVENT, bi) == 0) {
        bi->flip_pending  = 1;
        bi->idle_flip     = 1;
        bi->next_fb       = (bi->next_fb + 1) % NUM_FBS;
        bi->flip_fail_run = 0u;             /* a success resets the run */
    } else if (errno != EBUSY) {
        flip_failed(bi, errno, "idle");      /* threshold 60, then exit code 1 */
    }
}

static int apply_realtime(int cpu, int prio)
{
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET((size_t)cpu, &set);
        (void)pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    }
    if (prio > 0) {
        struct sched_param sp;
        memset(&sp, 0, sizeof sp);
        sp.sched_priority = prio;
        return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }
    return 0;
}

/* -----------------------------------------------------------------------------
 * blit_init_legacy -- the original single-modeline init path, the
 * --legacy-modeset bisection escape hatch, kept in its own function. Two
 * things changed in it since: switchres_setup replaces the three
 * configuration calls, and modeset_guard replaces the two raw checks. The
 * events that the legacy-modeset regression script filters keep their
 * names.
 *
 * Why switchres only knows the low band: sr_switch_to_mode, below, has the
 * card programmed BY switchres (its drmkms backend calls drmModeSetCrtc)
 * BEFORE modeset_guard judges the result. So this mode must come out at
 * 15 kHz whatever is asked: switchres_setup only gives it the bands of the
 * low-band mask, while the file may describe two. A launch request of 480
 * lines or more comes out as 15 kHz interlaced, never at 31 kHz; the guard
 * then judges the real mode. This path NEVER gives the high band to
 * switchres, 480p enabled or not -- only the default path
 * (register_dual_modelines) does.
 *
 * Error code key (logged on stderr at startup):
 *   -1 sr_init or sr_init_disp failed
 *   -2 mode refused by modeset_guard (hardware-loss-grade, site
 *      legacy_boot); replaces the old integer envelope AND the raw
 *      15000-16200 range check, which was code -3 (no longer exists)
 *   -4 drm open failed
 *   -5 drm set master failed
 *   -6 no connected crtc
 *   -7 dumb buffer alloc failed
 *   -8 drmModeSetCrtc failed
 * ---------------------------------------------------------------------------*/
static int blit_init_legacy(struct blit_internal *bi)
{
    /* Step 1: switchres -- modeline computation + initial modeset on its own fd.
     * switchres_setup reads the same monitor file as the guard (no more
     * hard-coded arcade_15 preset). */
    switchres_setup(bi->cfg.monitor, monitor_low_bands_mask(bi->cfg.monitor));
    /* sr_init_disp returns -1 on error, otherwise the display index (0-based).
     * The first display init thus returns 0, not 1. */
    int disp_idx = sr_init_disp(NULL, NULL);
    if (disp_idx < 0) {
        fprintf(stderr, "{\"event\":\"sr_init_disp_failed\",\"rc\":%d}\n", disp_idx);
        sr_deinit();
        return -1;
    }
    sr_mode result;
    memset(&result, 0, sizeof result);
    int rc = sr_switch_to_mode(bi->cfg.width, bi->cfg.height,
                               bi->cfg.refresh_hz, 0, &result);
    if (rc != 1) {
        fprintf(stderr,
                "{\"event\":\"sr_switch_to_mode_failed\",\"rc\":%d}\n", rc);
        sr_deinit();
        /* switchres cannot make this mode, the configuration is rejected --
         * exit code 3, no restart. */
        atomic_store(&s_config_rejected, 1);
        return -1;
    }
    fprintf(stderr,
            "{\"event\":\"sr_switch_succeeded\",\"hfreq_hz\":%.1f,"
            "\"vfreq_hz\":%.2f,\"pclock_hz\":%llu,\"w\":%d,\"h\":%d}\n",
            result.hfreq, result.vfreq,
            (unsigned long long)result.pclock,
            result.width, result.height);

    /* Step 2: NO silent fallback. The mode is converted first, then judged
     * AS IT WILL BE HANDED to the card (clock in kHz, in real numbers, as an
     * (H, V) pair inside one low band of the file). This single call
     * replaces the integer envelope validation AND the raw 15000-16200 check
     * (defense in depth: a silent amdgpu fallback to an EDID mode is judged
     * the same way, by what really goes out). The conversion happens here,
     * before opening DRM. */
    sr_mode_to_drm(&result, &bi->mode);
    if (modeset_guard(bi, "legacy_boot", &bi->mode, monitor_low_bands_mask(bi->cfg.monitor),
                      NULL, NULL, NULL) != MODELINE_ACCEPT) {
        sr_deinit();
        atomic_store(&s_config_rejected, 1);   /* exit code 3 */
        return -2; /* hardware-loss-grade abort */
    }
    fprintf(stderr, "{\"event\":\"envelope_validated\"}\n");

    /* Step 3: release switchres DRM master, then daemon takes master on its
     * own fd. Resolves the DRM master conflict between the two. */
    sr_deinit();

    const char *dev = bi->cfg.drm_device ? bi->cfg.drm_device : "/dev/dri/card0";
    bi->drm_fd = open(dev, O_RDWR | O_CLOEXEC);
    if (bi->drm_fd < 0) {
        fprintf(stderr,
                "{\"event\":\"drm_open_failed\",\"errno\":%d,\"dev\":\"%s\"}\n",
                errno, dev);
        return -4;
    }
    if (drmSetMaster(bi->drm_fd) < 0) {
        fprintf(stderr,
                "{\"event\":\"drm_set_master_failed\",\"errno\":%d}\n", errno);
        close(bi->drm_fd);
        bi->drm_fd = -1;
        return -5;
    }

    if (pick_crtc_connector(bi->drm_fd, &bi->crtc_id, &bi->conn_id,
                            &bi->crtc_index) < 0) {
        fprintf(stderr, "{\"event\":\"no_connected_crtc\"}\n");
        drmDropMaster(bi->drm_fd);
        close(bi->drm_fd);
        bi->drm_fd = -1;
        return -6;
    }

    /* Step 4: allocate 2 dumb buffers at the MODELINE dimensions (not the
     * source dimensions from CLI). switchres can promote the source res to a
     * super-resolution width (e.g. 320 -> 640 horizontal-double on radeon to
     * stay above the pclock floor). The CRTC scans out at result.width x
     * result.height, so the FB pitch and dumb buffer must match -- mismatch
     * causes drmModeSetCrtc to fail with ENOSPC. Source-to-FB scaling is the
     * blit thread's responsibility on each frame. */
    bi->fb_w = (uint32_t)result.width;
    bi->fb_h = (uint32_t)result.height;
    for (int i = 0; i < NUM_FBS; i++) {
        int afr = alloc_fb(bi->drm_fd, (int)bi->fb_w, (int)bi->fb_h,
                           &bi->fb_id[i], &bi->handle[i],
                           &bi->fb_map[i], &bi->fb_size, &bi->fb_pitch);
        if (afr != 0) {
            fprintf(stderr,
                    "{\"event\":\"alloc_fb_failed\",\"i\":%d,"
                    "\"step\":%d,\"errno\":%d}\n",
                    i, afr, errno);
            return -7;
        }
    }
    fprintf(stderr,
            "{\"event\":\"dumb_buffer_allocated\","
            "\"size\":%zu,\"pitch\":%u,\"count\":%d}\n",
            bi->fb_size, bi->fb_pitch, NUM_FBS);

    /* Step 5: drmModeSetCrtc to repoint at daemon fb[0]. */
    if (drmModeSetCrtc(bi->drm_fd, bi->crtc_id, bi->fb_id[0], 0, 0,
                       &bi->conn_id, 1, &bi->mode) < 0) {
        fprintf(stderr,
                "{\"event\":\"drmModeSetCrtc_failed\",\"errno\":%d}\n", errno);
        return -8;
    }
    fprintf(stderr, "{\"event\":\"crtc_pointed_at_daemon_fb\"}\n");

    bi->next_fb      = 1; /* fb[0] is on screen; next flip targets fb[1] */
    bi->flip_pending = 0;
    bi->last_flip_ns = 0;

    /* Startup state: first ACK after blit_init success carries
     * FLAG_VRAM_SYNCED. Owned by blit thread; net.c's build_ack reads this
     * atomic on every ACK assembly. */
    atomic_store(&bi->cfg.shared->synced, 1);
    fprintf(stderr, "{\"event\":\"blit_init_synced_set\"}\n");
    return 0;
}

/* -----------------------------------------------------------------------------
 * blit_init_default -- dual-modeline boot path (the default, used whenever
 * --legacy-modeset is NOT set).
 *
 * Sequence: open DRM device, take master, pick connector/CRTC (duplicated
 * from blit_init_legacy -- intentional, so the legacy path stays untouched
 * inside its own function), register both modelines (register_dual_modelines),
 * allocate 2 FBs sized for the larger of the two regimes (2560x480 --
 * covers 480i full-frame; 240p scans out only the first 240 rows of the
 * same buffer, a single-buffer design), then drmModeSetCrtc onto the 240p
 * super-res modeline as the first scan-out.
 *
 * Error code key (logged on stderr at startup):
 *   -4  drm open failed              (shared meaning with blit_init_legacy)
 *   -5  drm set master failed        (shared meaning with blit_init_legacy)
 *   -6  no connected crtc            (shared meaning with blit_init_legacy)
 *   -10 dual-modeline registration failed (register_dual_modelines)
 *   -11 dumb buffer alloc failed (default-path sizing)
 *   -12 drmModeSetCrtc failed (default-path, 240p super-res as first scan-out)
 *   -13 first scan-out mode refused by modeset_guard (site boot_default;
 *       s_config_rejected -> exit code 3)
 * ---------------------------------------------------------------------------*/
static int blit_init_default(struct blit_internal *bi)
{
    const char *dev = bi->cfg.drm_device ? bi->cfg.drm_device : "/dev/dri/card0";
    bi->drm_fd = open(dev, O_RDWR | O_CLOEXEC);
    if (bi->drm_fd < 0) {
        fprintf(stderr,
                "{\"event\":\"drm_open_failed\",\"errno\":%d,\"dev\":\"%s\"}\n",
                errno, dev);
        return -4;
    }
    if (drmSetMaster(bi->drm_fd) < 0) {
        fprintf(stderr,
                "{\"event\":\"drm_set_master_failed\",\"errno\":%d}\n", errno);
        close(bi->drm_fd);
        bi->drm_fd = -1;
        return -5;
    }
    if (pick_crtc_connector(bi->drm_fd, &bi->crtc_id, &bi->conn_id,
                            &bi->crtc_index) < 0) {
        fprintf(stderr, "{\"event\":\"no_connected_crtc\"}\n");
        drmDropMaster(bi->drm_fd);
        close(bi->drm_fd);
        bi->drm_fd = -1;
        return -6;
    }

    if (register_dual_modelines(bi) < 0) {
        fprintf(stderr,
                "{\"event\":\"blit_init_failed\","
                "\"reason\":\"dual_modeline_registration\"}\n");
        return -10;
    }

    /* Allocate 2 FBs at the max-of-both-regimes size. Paired geometry
     * (measured on the hardware, then widened 2x): both regimes
     * share width=2560, so only height varies -- 480 covers the 480i
     * full-frame case; 240p scans out only the top 240 rows of the same
     * buffer. */
    bi->fb_w = (uint32_t)MODELINE_240P_SR_NTSC.width;
    bi->fb_h = 480U;
    for (int i = 0; i < NUM_FBS; i++) {
        int afr = alloc_fb(bi->drm_fd, (int)bi->fb_w, (int)bi->fb_h,
                           &bi->fb_id[i], &bi->handle[i],
                           &bi->fb_map[i], &bi->fb_size, &bi->fb_pitch);
        if (afr != 0) {
            fprintf(stderr,
                    "{\"event\":\"alloc_fb_failed\",\"i\":%d,"
                    "\"step\":%d,\"errno\":%d}\n",
                    i, afr, errno);
            return -11;
        }
    }
    fprintf(stderr,
            "{\"event\":\"dumb_buffer_allocated\","
            "\"size\":%zu,\"pitch\":%u,\"count\":%d}\n",
            bi->fb_size, bi->fb_pitch, NUM_FBS);

    /* First scan-out: 240p super-res. Judged by the single point of
     * passage, in this function, just before it is handed to the card
     * (boot_guard_pinned already judged the constants; this call judges the
     * mode really passed to drmModeSetCrtc). */
    bi->mode = bi->mode_240p_super_res;
    if (modeset_guard(bi, "boot_default", &bi->mode_240p_super_res,
                      monitor_regime_mask(bi->cfg.monitor, (unsigned)REGIME_240P_SUPER_RES),
                      NULL, NULL, NULL) != MODELINE_ACCEPT) {
        atomic_store(&s_config_rejected, 1);   /* exit code 3, no restart */
        return -13;
    }
    if (drmModeSetCrtc(bi->drm_fd, bi->crtc_id, bi->fb_id[0], 0, 0,
                       &bi->conn_id, 1, &bi->mode_240p_super_res) < 0) {
        fprintf(stderr,
                "{\"event\":\"runtime_setcrtc_failed\","
                "\"path\":\"blit_init_default\","
                "\"errno\":%d}\n", errno);
        return -12;
    }
    fprintf(stderr, "{\"event\":\"crtc_pointed_at_daemon_fb\"}\n");

    /* Best-effort MMIO init for the seamless 240p<->480i toggle fast path
     * (mmap of PCI resource2 -- independent of the DRM master; called after
     * the initial SetCrtc so the active-CRTC scan sees the CRTC the daemon
     * itself just lit). On failure mmio_init() logs
     * {"event":"mmio_unavailable",...} itself and blit_regime_toggle falls
     * back to drmModeSetCrtc; the daemon stays fully functional (degraded =
     * the pre-seamless flash behavior). MUST NOT fail blit_init_default. */
    (void)mmio_init(dev);

    bi->next_fb      = 1;
    bi->flip_pending = 0;
    bi->last_flip_ns = 0;

    atomic_store(&bi->cfg.shared->current_regime, (uint8_t)REGIME_240P_SUPER_RES);
    bi->current_regime = (uint8_t)REGIME_240P_SUPER_RES;
    atomic_store(&bi->cfg.shared->synced, 1u);
    fprintf(stderr, "{\"event\":\"blit_init_synced_set\"}\n");
    return 0;
}

/* -----------------------------------------------------------------------------
 * blit_init -- dispatcher. The CRT_BRIDGE_NO_DRM=1 short-circuit stays
 * common to both regimes (test fixtures have no /dev/dri/card0; we skip
 * every DRM-touching call but keep enough sentinel state for the
 * runtime-modeset state machine to drive `synced`, `last_modeset_ns`,
 * switchres_seq observation, and modeset_guard through the fixtures).
 * Otherwise branches on shared->legacy_modeset: set -> blit_init_legacy
 * (the original single-modeline path, unchanged); clear (default) ->
 * blit_init_default (dual-modeline boot).
 * ---------------------------------------------------------------------------*/
static int blit_init(struct blit_internal *bi)
{
    /* The two pinned modes are judged BEFORE the CRT_BRIDGE_NO_DRM branch,
     * for both paths (default and legacy): the synthetic modes of test mode
     * have no timing, the guard judges the real constants. Before any
     * drmModeSetCrtc, any image, any blit_init_synced_set. */
    if (boot_guard_pinned(bi) != 0)
        return -2;

    if (bi->test_mode_no_drm) {
        /* Test hook, honoured only here, under CRT_BRIDGE_NO_DRM, and for
         * the exact value "1": simulates a configuration rejection, which
         * this mode cannot produce otherwise. Same step as the rejections of
         * blit_init_legacy (flag, then failure), and same outcome: the
         * blit_thread_init_failed path. */
        const char *reject = getenv("CRT_BRIDGE_TEST_REJECT_CONFIG");
        if (reject && strcmp(reject, "1") == 0) {
            fprintf(stderr, "{\"event\":\"test_reject_config\"}\n");
            atomic_store(&s_config_rejected, 1);
            return -2;
        }

        bi->fb_w     = (uint32_t)bi->cfg.width;
        bi->fb_h     = (uint32_t)bi->cfg.height;
        bi->fb_pitch = (uint32_t)(bi->cfg.width * 4);
        bi->fb_size  = (size_t)bi->fb_w * (size_t)bi->fb_h * 4U;
        for (int i = 0; i < NUM_FBS; i++) {
            bi->fb_map[i] = calloc(1, bi->fb_size);
            if (!bi->fb_map[i]) {
                fprintf(stderr,
                        "{\"event\":\"alloc_fb_failed\",\"i\":%d,"
                        "\"step\":-1,\"errno\":%d,\"test_mode\":true}\n",
                        i, errno);
                return -7;
            }
            bi->fb_id[i]  = 0;
            bi->handle[i] = 0;
        }
        /* Synthesize an initial sr_mode-equivalent for blit_same_geometry. */
        memset(&bi->mode, 0, sizeof bi->mode);
        bi->mode.hdisplay = (uint16_t)bi->cfg.width;
        bi->mode.vdisplay = (uint16_t)bi->cfg.height;
        bi->mode.vrefresh = (uint32_t)(bi->cfg.refresh_hz + 0.5);
        bi->mode.clock    = 13010U;  /* sentinel non-zero kHz so pclock cmp works */
        bi->next_fb       = 1;
        bi->flip_pending  = 0;
        bi->last_flip_ns  = 0;

        /* Synthesize cached regime modes too, so blit_regime_toggle's
         * test_mode_no_drm branch (invoked by test fixtures driving the
         * default/non-legacy path) has something plausible to switch bi->mode
         * to. */
        bi->mode_240p_super_res            = bi->mode;
        bi->mode_240p_super_res.vdisplay   = (uint16_t)MODELINE_240P_SR_NTSC.height;
        bi->mode_480i                      = bi->mode;
        bi->mode_480i.vdisplay             = (uint16_t)MODELINE_480I_NTSC.height;
        bi->mode_480i.flags               |= (uint32_t)DRM_MODE_FLAG_INTERLACE;
        /* The synthetic 480p -- the launch width, 480 lines, progressive (no
         * interlace flag), like the other two: no timing, the guard judges
         * the REAL constant (blit_regime_toggle). */
        bi->mode_480p                      = bi->mode;
        bi->mode_480p.vdisplay             = (uint16_t)MODELINE_480P.height;
        bi->current_regime                 = (uint8_t)REGIME_240P_SUPER_RES;
        atomic_store(&bi->cfg.shared->current_regime, (uint8_t)REGIME_240P_SUPER_RES);

        /* Mirror blit_init_default's registration event so NO_DRM fixtures
         * can assert modeline_registration_succeeded count=2 without real
         * DRM -- the real emission lives in register_dual_modelines, which
         * the test-mode short-circuit never reaches. */
        if (!atomic_load(&bi->cfg.shared->legacy_modeset)) {
            fprintf(stderr,
                    "{\"event\":\"modeline_registration_succeeded\",\"count\":%d,"
                    "\"default_regime\":\"240p_super_res\",\"test_mode\":true}\n",
                    2 + (blit_regime_480p_state() == R480P_ENABLED ? 1 : 0));
        }

        fprintf(stderr,
                "{\"event\":\"blit_init_test_mode_done\","
                "\"w\":%u,\"h\":%u}\n",
                (unsigned)bi->fb_w, (unsigned)bi->fb_h);

        /* Startup state: first ACK after blit_init carries
         * FLAG_VRAM_SYNCED. */
        atomic_store(&bi->cfg.shared->synced, 1);
        fprintf(stderr, "{\"event\":\"blit_init_synced_set\"}\n");
        return 0;
    }

    if (atomic_load(&bi->cfg.shared->legacy_modeset)) {
        return blit_init_legacy(bi);
    }
    return blit_init_default(bi);
}

static void blit_cleanup(struct blit_internal *bi)
{
    if (bi->test_mode_no_drm) {
        /* test mode: fb_map[] are heap calloc, not mmap. */
        for (int i = 0; i < NUM_FBS; i++) {
            if (bi->fb_map[i]) {
                free(bi->fb_map[i]);
                bi->fb_map[i] = NULL;
            }
        }
        return;
    }
    for (int i = 0; i < NUM_FBS; i++) {
        if (bi->fb_map[i]) {
            munmap(bi->fb_map[i], bi->fb_size);
            bi->fb_map[i] = NULL;
        }
        if (bi->fb_id[i]) {
            drmModeRmFB(bi->drm_fd, bi->fb_id[i]);
            bi->fb_id[i] = 0;
        }
        if (bi->handle[i]) {
            struct drm_mode_destroy_dumb dr;
            memset(&dr, 0, sizeof dr);
            dr.handle = bi->handle[i];
            (void)drmIoctl(bi->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dr);
            bi->handle[i] = 0;
        }
    }
    if (bi->drm_fd >= 0) {
        (void)drmDropMaster(bi->drm_fd);
        close(bi->drm_fd);
        bi->drm_fd = -1;
    }
}

/* -----------------------------------------------------------------------------
 * Runtime CMD_SWITCHRES handling.
 *
 * blit_same_geometry: tolerance check (exact dims, +/-0.5% pclock, +/-0.5 Hz).
 * blit_runtime_modeset: full sequence -- drain -> detach -> free -> alloc ->
 *   SetCrtc -- with the wire modeline forged into a drmModeModeInfo + the
 *   modeset_guard gate (envelope and raw-range check in one) + same-geometry
 *   skip + synced state machine + last_modeset_ns bump.
 * blit_check_pending_switchres: poll switchres_seq, apply the rate limit,
 *   coalesce bursts (last-write-wins), invoke runtime_modeset.
 * ---------------------------------------------------------------------------*/

/* Same-geometry tolerance check. Returns 1 if `req` matches the currently
 * live modeline within tolerance (exact match for active dims; refresh
 * +/-0.5 Hz; pclock +/-0.5%), 0 otherwise. */
static int blit_same_geometry(const struct blit_internal *bi,
                              const struct sr_mode_request *req)
{
    if ((unsigned)req->h_active != bi->fb_w) return 0;
    if ((unsigned)req->v_active != bi->fb_h) return 0;
    /* pclock match within +/-0.5%. bi->mode.clock is in kHz, req->pclock in Hz. */
    double current_pclock = (double)bi->mode.clock * 1000.0;
    if (current_pclock <= 0.0) return 0;
    double dp = (current_pclock - req->pclock) / current_pclock;
    if (dp < 0) dp = -dp;
    if (dp > 0.005) return 0;
    /* refresh +/-0.5 Hz. bi->mode.vrefresh is rounded uint, req->refresh_hz is double. */
    double dr = (double)bi->mode.vrefresh - req->refresh_hz;
    if (dr < 0) dr = -dr;
    if (dr > 0.5) return 0;
    return 1;
}

/* Regime-toggle fast path. This is the SINGLE call site that changes the
 * CRTC between the 240p super-res and 480i modelines in default
 * (non-legacy) mode -- deliberately isolated behind one well-identified
 * function so the seamless MMIO fast path (13 register writes under
 * CRTC_UPDATE_LOCK, INTERLACE_ENABLE double-buffered, no drmModeSetCrtc /
 * PLL relock -- proven on the hardware) could replace this function's
 * body without touching any caller (blit_runtime_modeset,
 * blit_check_pending_switchres, net.c's regime resolver are all unaware
 * of what happens inside).
 *
 * Uses the cached drmModeModeInfo from register_dual_modelines() -- no
 * sr_init/sr_add_mode re-entry, no per-frame modeset gymnastics. Only
 * called when shared->legacy_modeset == 0 and net.c's CMD_SWITCHRES
 * regime-resolver has already determined a genuine regime change occurred
 * (switchres_seq only bumps on actual toggle in default mode -- see
 * net.c's CMD_SWITCHRES handler).
 *
 * Seamless MMIO fast path: when mmio_available(), the toggle is a
 * 13-register CRTC poke under CRTC_UPDATE_LOCK (latched atomically at
 * V_UPDATE -- no drmModeSetCrtc, no PLL relock, no ~50 ms flash / audio
 * micro-cut). drmModeSetCrtc remains the reachable fallback (MMIO
 * unavailable at boot, or a poke returning <0).
 *
 * CAVEAT -- stale kernel state after the MMIO poke: the kernel's
 * crtc->mode is NOT updated by the poke, so vblank timestamps and pageflip
 * dimension validation keep reasoning on the previous mode until the next
 * drmModeSetCrtc. This is accepted because (a) both regimes share
 * pclock/htotal so the horizontal cadence is unchanged, (b) the daemon
 * paces by ACK, not by kernel timestamps, and resynchronizes its time
 * model after the toggle (last_flip_ns reset below), and (c) the FBs are
 * allocated at the max-of-both-regimes size (2560x480) so dimension
 * validation stays satisfied in both directions. Patching crtc->mode
 * kernel-side (radeon patch, ~250-400 LOC) is a possible refinement, not
 * done. Point to watch on the hardware: pacing/pageflip artifacts after
 * the first toggle.
 *
 * Three regimes. The wanted regime is the one of the request
 * (req->regime, decided once by the network thread); 480p has its frozen
 * mode, mode_480p, scanned out of the same 2560x480 buffer (1280 columns).
 * MMIO only serves the 240p <-> 480i switch; any band crossing (to or from
 * 480p) reprograms the PLL with a full mode change, and writes
 * band_crossing_begin here, band_crossing_first_image at the first page
 * flip that follows. */
/* Test hook -- dead on the hardware, like the CRT_BRIDGE_FB_DUMP_PATH dump
 * whose step it copies: it requires that same path AND CRT_BRIDGE_NO_DRM.
 * It writes `<path>.crossing`, the content of buffer 0 at the instant
 * when, on the real path, drmModeSetCrtc would point the card at it. The
 * ordinary dump cannot show this: converting an image clears the whole
 * scan window before drawing (blit_rgb888_to_xrgb8888), so the buffer
 * written by the first image of the new regime is always clean, zeroed or
 * not. Only the buffer the call points at BEFORE that image carries the
 * last image of the old regime. */
static void fb_dump_crossing(const struct blit_internal *bi)
{
    char dst_path[PATH_MAX];
    char tmp_path[PATH_MAX + 8];   /* dst_path + ".tmp" */

    if (!bi->fb_dump_path || !bi->test_mode_no_drm || !bi->fb_map[0]) {
        return;
    }
    int n = snprintf(dst_path, sizeof dst_path, "%s.crossing", bi->fb_dump_path);
    if (n <= 0 || n >= (int)sizeof dst_path - 8) {
        return;
    }
    (void)snprintf(tmp_path, sizeof tmp_path, "%s.tmp", dst_path);
    int dfd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (dfd < 0) {
        return;
    }
    ssize_t w = write(dfd, bi->fb_map[0], bi->fb_size);
    close(dfd);
    if (w == (ssize_t)bi->fb_size) {
        (void)rename(tmp_path, dst_path);
    }
}

static int blit_regime_toggle(struct blit_internal *bi,
                              const struct sr_mode_request *req)
{
    uint8_t have = atomic_load(&bi->cfg.shared->current_regime);
    /* The wanted regime is the DECISION of the network thread, carried by
     * the request -- not made again here from the interlace flag. A value
     * outside the three served regimes is not a mode: log, stay
     * synchronised, -1. */
    uint8_t want = req->regime;
    if (want != (uint8_t)REGIME_240P_SUPER_RES &&
        want != (uint8_t)REGIME_480I &&
        want != (uint8_t)REGIME_480P) {
        fprintf(stderr,
                "{\"event\":\"regime_toggle_unknown_regime\",\"regime\":%u}\n",
                (unsigned)want);
        atomic_store(&bi->cfg.shared->synced, 1u);
        return -1;
    }
    /* Defense in depth: the request is the decision of the network thread,
     * which already serves as 480i (fallback) a 480p that the state does not
     * allow. This re-read CONFIRMS, it decides nothing else: a 480p asked
     * without an enabled state (a request from elsewhere, a state that
     * changed meanwhile) is served as 480i, and the 480p mode is neither
     * judged nor set -- neither the guard, nor drmModeSetCrtc of 480p
     * without the state's consent. It comes before any choice of mode. */
    const int state_480p = blit_regime_480p_state();
    if (want == (uint8_t)REGIME_480P && state_480p != R480P_ENABLED) {
        fprintf(stderr,
                "{\"event\":\"regime_toggle_fallback\",\"reason\":\"%s\"}\n",
                regime480p_reason(state_480p));
        want = (uint8_t)REGIME_480I;
    }
    if (want == have) {
        /* The CANCELLATION path. net.c publishes a "stay in A" request when
         * the emitter comes back to the current regime while a switch to B is
         * waiting (regime_publish_needed); the last one wins, B is replaced,
         * and there is nothing to do but lift the mute: no-op, synced goes
         * back up (band_wait_begin had lowered it). */
        atomic_store(&bi->cfg.shared->synced, 1u);
        return 0;
    }

    drmModeModeInfo *target = (want == (uint8_t)REGIME_480I)
                              ? &bi->mode_480i
                              : (want == (uint8_t)REGIME_480P)
                                  ? &bi->mode_480p
                                  : &bi->mode_240p_super_res;
    /* Crossing the band (15 kHz <-> 31 kHz) reprograms the PLL: never the
     * MMIO path (13 registers, same clock and total width in both regimes),
     * only ever a full mode change here. regime_mmio_toggle_ok says yes only
     * for 240p <-> 480i. */
    const int crosses = regime_crosses_band(have, want);

    /* A guard, BEFORE lowering synced and BEFORE anything is sent to the
     * card -- it covers MMIO (mmio_seamless_toggle) AND its drmModeSetCrtc
     * fallback. On the real path, the judged mode is `target`. Under
     * test_mode_no_drm, the cached modes are synthetic (no timing) and
     * nothing goes to the card: the guard judges the REAL pinned mode of
     * the wanted regime, built by pinned_modeline_to_drm. Under the band
     * mask of the WANTED regime -- 480p only sees the high band, the other
     * two only the low bands. Refusal: synced stays at 1, the regime does
     * not change, -1 (the daemon stays alive). */
    drmModeModeInfo pinned;
    const drmModeModeInfo *judged = target;
    {
        if (bi->test_mode_no_drm) {
            pinned_modeline_to_drm(want == (uint8_t)REGIME_480I
                                        ? &MODELINE_480I_NTSC
                                        : (want == (uint8_t)REGIME_480P
                                               ? &MODELINE_480P
                                               : &MODELINE_240P_SR_NTSC),
                                    &pinned);
            judged = &pinned;
        }
        if (modeset_guard(bi, "regime_toggle", judged,
                          monitor_regime_mask(bi->cfg.monitor, want),
                          NULL, NULL, NULL) != MODELINE_ACCEPT) {
            atomic_store(&bi->cfg.shared->synced, 1u);
            if (crosses) {
                /* A refused crossing leaves the CRT in the old band while the
                 * requested regime is announced: black, and acknowledge without
                 * showing. The refusal comes before anything is sent to the card:
                 * it counts neither for the rule nor for the floor. */
                crossing_hold_black(bi);
            }
            return -1;
        }
    }

    /* Clear synced BEFORE the toggle -- the next ACK carries FLAG_VRAM_SYNCED=0
     * (mirrors the legacy runtime-modeset convention). The emitter fork reads
     * `frame_echo`, not this bit; whether GroovyMAME pauses its blit traffic
     * on it is NOT verified. */
    atomic_store(&bi->cfg.shared->synced, 0u);

    /* MMIO is only tried for 240p <-> 480i; any pair with 480p goes through
     * drmModeSetCrtc. The label follows the same decision, test mode
     * included (regime_toggle: 240p <-> 480i; regime_toggle_modeset: the
     * rest). */
    const int mmio_ok = regime_mmio_toggle_ok(have, want);
    const char *toggle_path = mmio_ok ? "regime_toggle" : "regime_toggle_modeset";

    /* A band crossing reprograms the PLL under buffers that still carry the
     * last image of the old regime: the CRT would show it stretched, under
     * another scan, until the first image of the new one. Two steps, BEFORE
     * the call (the buffer drmModeSetCrtc points at must already be black),
     * on the real path as in test mode:
     *   1. a flip in flight is drained, bounded to 100 ms (copy of the
     *      drain of blit_runtime_modeset, same timeout event): its event must
     *      not arrive after the call and close band_crossing_first_image on
     *      the image of the old regime;
     *   2. both buffers are zeroed.
     * The 240p <-> 480i switch crosses nothing: it goes neither through
     * here nor through the stale-image cut (its original path is
     * unchanged). */
    int buffers_cleared = 0;
    if (crosses) {
        drmEventContext ev;
        memset(&ev, 0, sizeof ev);
        ev.version           = 2;
        ev.page_flip_handler = page_flip_handler;
        uint64_t drain_deadline = now_ns() + 100000000ULL; /* 100 ms */
        while (bi->flip_pending && now_ns() < drain_deadline) {
            struct pollfd pfd = { .fd = bi->drm_fd, .events = POLLIN, .revents = 0 };
            int pr = poll(&pfd, 1, 10);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                (void)drmHandleEvent(bi->drm_fd, &ev);
            }
        }
        if (bi->flip_pending) {
            fprintf(stderr,
                    "{\"event\":\"runtime_modeset_drain_timeout\"}\n");
            bi->flip_pending = 0;
        }
        for (int i = 0; i < NUM_FBS; i++) {
            if (bi->fb_map[i]) {
                memset(bi->fb_map[i], 0, bi->fb_size);
            }
        }
        buffers_cleared = 1;
    }

    uint64_t t0 = now_ns();
    if (crosses) {
        /* The start of a crossing measurement, just before the call that sets
         * the mode again. The clock fields are those of the JUDGED mode.
         * waited_ms: the wait endured by the LAST request, from its reception
         * to this start -- 100 ms floor and band rule together. */
        const uint64_t waited_ms =
            (t0 > req->monotonic_ns_received && req->monotonic_ns_received != 0u)
                ? (t0 - req->monotonic_ns_received) / 1000000ULL : 0ULL;
        fprintf(stderr,
                "{\"event\":\"band_crossing_begin\",\"from\":\"%s\",\"to\":\"%s\","
                "\"t0_ns\":%llu,\"waited_ms\":%llu,\"clock_khz\":%u,"
                "\"htotal\":%u,\"vtotal\":%u,\"buffers_cleared\":%d,"
                "\"reason\":\"%s\"}\n",
                regime_name(have), regime_name(want),
                (unsigned long long)t0, (unsigned long long)waited_ms,
                (unsigned)judged->clock, (unsigned)judged->htotal,
                (unsigned)judged->vtotal, buffers_cleared,
                bi->crossing_reason);
    }
    if (bi->test_mode_no_drm) {
        /* Simulate the modeset window (see blit_runtime_modeset's legacy
         * test_mode_no_drm branch for the identical rationale/duration).
         * NEVER touches MMIO in test mode -- CRT_BRIDGE_NO_DRM=1 fixtures
         * must stay hardware-free (mmio_init is never called on this path,
         * this branch is what keeps the test fixtures hardware-free). */
        if (crosses) {
            fb_dump_crossing(bi);   /* what the call would point the card at */
        }
        struct timespec sim = { .tv_sec = 0, .tv_nsec = 25000000L };
        nanosleep(&sim, NULL);
        if (crosses && bi->crossing_test_fail) {
            /* The simulated call fails, like drmModeSetCrtc below -- same two
             * events, same exit state. */
            fprintf(stderr,
                    "{\"event\":\"runtime_setcrtc_failed\","
                    "\"path\":\"regime_toggle\","
                    "\"want\":%u,\"errno\":%d}\n", want, EIO);
            return crossing_failed(bi, have, want, t0, EIO);
        }
    } else {
        int mmio_done = 0;
        if (mmio_ok && mmio_available()) {
            if (mmio_seamless_toggle(want) == 0) {
                mmio_done = 1;
                toggle_path = "regime_toggle_mmio";
            } else {
                fprintf(stderr,
                        "{\"event\":\"mmio_toggle_failed\","
                        "\"fallback\":\"modeset\",\"want\":%u}\n", want);
            }
        }
        if (!mmio_done) {
            /* Fallback (MMIO unavailable or poke failed): full modeset --
             * the pre-seamless behavior, flash present but functional. */
            toggle_path = "regime_toggle_modeset";
            if (drmModeSetCrtc(bi->drm_fd, bi->crtc_id, bi->fb_id[0], 0, 0,
                               &bi->conn_id, 1, target) < 0) {
                const int err = errno;
                fprintf(stderr,
                        "{\"event\":\"runtime_setcrtc_failed\","
                        "\"path\":\"regime_toggle\","
                        "\"want\":%u,\"errno\":%d}\n", want, err);
                if (crosses) {
                    /* The attempt counts (rule, floor), the screen stays black. */
                    return crossing_failed(bi, have, want, t0, err);
                }
                atomic_store(&bi->cfg.shared->synced, 1u);
                return -1;
            }
        }
    }
    uint64_t t1 = now_ns();

    bi->mode          = *target;
    bi->fb_w          = (uint32_t)target->hdisplay;
    bi->fb_h          = (uint32_t)target->vdisplay;
    bi->next_fb       = 1;
    bi->flip_pending  = 0;
    bi->last_flip_ns  = 0;

    atomic_store(&bi->cfg.shared->current_regime, want);
    bi->current_regime = want;
    bi->phase_locked = 0u;   /* new regime entry, new phase to set */
    shed_reset(&bi->shed);   /* new regime, new window */
    bi->shed_armed   = 0;
    atomic_store(&bi->cfg.shared->synced, 1u);
    atomic_store(&bi->cfg.shared->last_modeset_ns, now_ns());

    /* A successful crossing arms the measurement of its first image: the
     * first page flip that follows (page_flip_handler, or the simulated
     * completion) writes band_crossing_first_image and disarms the flag. */
    if (crosses) {
        /* The crossing is recorded for the band rule at its start instant
         * (t0), not at the request's. A SUCCESSFUL crossing arrives here; a
         * failure of the call is counted above (crossing_failed); only a
         * guard refusal, which comes before anything is sent to the card,
         * counts neither in the minimum time nor in the ceiling. */
        bandrule_note_crossing(&bi->rule, t0);
        bi->crossing_t0_ns         = t0;
        bi->crossing_setcrtc_ns    = t1 - t0;
        bi->crossing_from          = have;
        bi->crossing_to            = want;
        bi->crossing_await_first   = 1;
        /* Any image that arrived before the call ended was composed for the
         * old regime; it will be acknowledged without being shown. */
        bi->drop_before_ns         = t1;
        /* crossing_dropped is not reset here: blit_check_pending_switchres
         * already carried into it the images acknowledged during the wait
         * (wait_dropped), or zero if there were none. */
    } else {
        bi->crossing_await_first   = 0;   /* a 240p <-> 480i switch measures nothing */
    }

    fprintf(stderr,
            "{\"event\":\"runtime_modeset_succeeded\","
            "\"path\":\"%s\","
            "\"regime\":\"%s\","
            "\"duration_ns\":%llu}\n",
            toggle_path,
            regime_name(want),
            (unsigned long long)(t1 - t0));
    return 0;
}

/* drmModeSetCrtc of the new mode refused AFTER the screen was detached
 * (dims_changed: CRTC detached, old buffers released). Nothing scans out
 * any more: a living daemon would leave the CRT black without systemd
 * restarting it. Same step as runtime_realloc_fb_failed: s_blit_fatal then
 * running = 0, exit code 1 through main(). No return to the old mode (an
 * idea set aside). Only this site is fatal; the other
 * runtime_setcrtc_failed do not change. */
static int modeset_refused_after_detach(struct blit_internal *bi, int err)
{
    fprintf(stderr,
            "{\"event\":\"runtime_setcrtc_failed\",\"errno\":%d,"
            "\"detached\":1}\n", err);
    atomic_store(&s_blit_fatal, 1);
    atomic_store(&bi->cfg.shared->running, 0);
    return -2;
}

/* Returns 0 success, -1 envelope rejection (daemon stays alive), -2
 * catastrophic. Caller must consume the request regardless. */
static int blit_runtime_modeset(struct blit_internal *bi,
                                const struct sr_mode_request *req)
{
    uint64_t now_mono = now_ns();
    fprintf(stderr,
            "{\"event\":\"runtime_modeset_attempt\","
            "\"req_w\":%u,\"req_h\":%u,\"req_refresh\":%.3f,"
            "\"mono_ns\":%llu}\n",
            (unsigned)req->h_active, (unsigned)req->v_active,
            req->refresh_hz, (unsigned long long)now_mono);

    /* Default (non-legacy) mode delegates entirely to the regime-toggle
     * fast path -- no switchres re-entry, no per-frame modeline forging
     * (that machinery is legacy-mode-only below). */
    if (!atomic_load(&bi->cfg.shared->legacy_modeset)) {
        return blit_regime_toggle(bi, req);
    }

    /* === Forge the modeline directly from MAME wire data. ===
     * MAME ships the exact source modeline (h_active, h_begin, h_end,
     * h_total, v_active, v_begin, v_end, v_total, pclock, interlace) in
     * CMD_SWITCHRES -- these are the timings the original PCB produces.
     * Mapping wire -> drm_mode_modeinfo is a straight copy via sr_mode_to_drm
     * downstream. switchres is no longer consulted at runtime; its choice
     * to canonicalise everything to a 640-wide super-resolution modeline
     * was the cause of the "game in the left half" bug (gridlee 341 -> fb 640
     * left ~47% of horizontal screen black under any blit-side fix).
     *
     * Pixel-perfect AND aspect-faithful by construction -- same modeline the
     * arcade PCB would drive. modeset_guard below is the sole
     * hardware-safety gate (the one choke point). The old drmSetMaster
     * gymnastics + ~100-500 ms switchres re-init disappear with this path:
     * nothing else opens /dev/dri/card0 at runtime.
     *
     * Validated on the hardware: gridlee 341x240 @ pclock 6.906 MHz, hfreq
     * 15.625 kHz at the very bottom of the JVC envelope -- accepted cleanly
     * by the radeon kernel module.
     *
     * KNOWN HW LIMIT: the amdgpu kernel module Oops-es with divide-by-zero
     * in amdgpu_pll_compute when pclock < ~10 MHz. radeon is fine. radeon
     * is the supported driver for this path until the amdgpu pclock floor
     * is mitigated daemon-side.
     *
     * Sync polarities hardcoded to arcade_15 NTSC convention (-hsync -vsync).
     * MAME's CMD_SWITCHRES does not carry sync polarity; if a non-NTSC
     * source ever needs different polarity, the wire protocol must be
     * extended -- out of scope here.
     *
     * Doublescan = 0 because 240p source means single-scan lines (no PPU
     * interpolation). Interlace flag preserved from wire. */
    sr_mode result;
    memset(&result, 0, sizeof result);
    result.width    = req->h_active;
    result.height   = req->v_active;
    result.refresh  = (int)(req->refresh_hz + 0.5);
    result.vfreq    = req->refresh_hz;
    /* hfreq = pclock / h_total. Defense against zero h_total (parser
     * should reject; defense-in-depth). */
    result.hfreq    = req->h_total
                      ? (req->pclock / (double)req->h_total)
                      : 0.0;
    result.pclock   = (uint64_t)req->pclock;
    result.hbegin   = (int)req->h_begin;
    result.hend     = (int)req->h_end;
    result.htotal   = (int)req->h_total;
    result.vbegin   = (int)req->v_begin;
    result.vend     = (int)req->v_end;
    result.vtotal   = (int)req->v_total;
    result.interlace = (int)req->interlace;
    result.hsync     = 0;       /* arcade_15 NTSC = -hsync */
    result.vsync     = 0;       /* arcade_15 NTSC = -vsync */
    result.doublescan = 0;      /* 240p source = single scan */

    /* === Hardware-loss-grade gate, the start-up checks re-applied at
     * runtime. ===
     * The mode forged from the wire is converted first, then judged AS IT
     * WOULD BE HANDED to the card, by the single point of passage: clock in
     * kHz, real numbers, (H, V) pair inside the low band of the file. This
     * call replaces the integer envelope validation AND the original raw
     * check. The mask leaves only the low band: a forged 31 408.6 Hz stays
     * refused here, even with the JVC file that does describe the high
     * band. A refusal leaves the daemon alive. */
    drmModeModeInfo new_mode;
    sr_mode_to_drm(&result, &new_mode);
    if (modeset_guard(bi, "legacy_runtime", &new_mode, monitor_low_bands_mask(bi->cfg.monitor),
                      NULL, NULL, NULL) != MODELINE_ACCEPT) {
        atomic_store(&bi->cfg.shared->synced, 1);
        return -1;
    }

    /* === Same-geometry skip. ===
     * No drmModeSetCrtc, no last_modeset_ns bump (preserves the
     * rate-limit budget for a real geometry change that follows). */
    if (blit_same_geometry(bi, req)) {
        fprintf(stderr,
                "{\"event\":\"runtime_modeset_succeeded\","
                "\"hfreq_hz\":%.1f,\"vfreq_hz\":%.2f,\"pclock_hz\":%llu,"
                "\"w\":%d,\"h\":%d,\"same_geom_skip\":true,"
                "\"mono_ns\":%llu}\n",
                result.hfreq, result.vfreq,
                (unsigned long long)result.pclock,
                result.width, result.height,
                (unsigned long long)now_ns());
        atomic_store(&bi->cfg.shared->synced, 1);
        return 0;
    }

    /* === Real modeset path. Clear synced BEFORE entering -- emitter sees
     * the next ACK with FLAG_VRAM_SYNCED=0 and pauses blit traffic. === */
    atomic_store(&bi->cfg.shared->synced, 0);

    int dims_changed = ((unsigned)result.width != bi->fb_w) ||
                       ((unsigned)result.height != bi->fb_h);

    if (bi->test_mode_no_drm) {
        /* Test hook: a mode refused after detaching, which this mode cannot
         * produce otherwise. Only the exact value "1" arms it, and only if
         * the dimensions change (the detached case); same path as the real
         * refusal. */
        const char *fail_setcrtc = dims_changed
            ? getenv("CRT_BRIDGE_TEST_FAIL_SETCRTC") : NULL;
        if (fail_setcrtc && strcmp(fail_setcrtc, "1") == 0) {
            errno = EINVAL;
            return modeset_refused_after_detach(bi, errno);
        }
        /* Skip DRM ops, but update mode/fb_w/fb_h so subsequent
         * same-geometry checks see the new state. */
        bi->mode = new_mode;
        bi->fb_w = (uint32_t)result.width;
        bi->fb_h = (uint32_t)result.height;
        /* Simulate the modeset window: on the hardware drmModeSetCrtc +
         * dumb-buffer realloc + flip-drain takes 10-50 ms during which
         * synced=0 is observable. In test_mode we sleep briefly so test
         * fixtures polling ACKs at ~8 ms cadence can catch the cleared bit.
         * 25 ms is the midpoint of the realistic hardware window and well
         * above the test poll interval. */
        struct timespec sim = { .tv_sec = 0, .tv_nsec = 25000000L };
        nanosleep(&sim, NULL);
        goto modeset_done;
    }

    /* === Step 1: drain pending page flip. ===
     * Never RmFB while a flip is pending (use-after-free risk on the kernel
     * side). Bounded 100 ms drain. */
    {
        drmEventContext ev;
        memset(&ev, 0, sizeof ev);
        ev.version           = 2;
        ev.page_flip_handler = page_flip_handler;
        uint64_t drain_deadline = now_ns() + 100000000ULL; /* 100 ms */
        while (bi->flip_pending && now_ns() < drain_deadline) {
            struct pollfd pfd = { .fd = bi->drm_fd, .events = POLLIN, .revents = 0 };
            int pr = poll(&pfd, 1, 10);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                (void)drmHandleEvent(bi->drm_fd, &ev);
            }
        }
        if (bi->flip_pending) {
            fprintf(stderr,
                    "{\"event\":\"runtime_modeset_drain_timeout\"}\n");
            bi->flip_pending = 0;
        }
    }

    if (dims_changed) {
        /* === Step 2: detach FBs from CRTC. ===
         * Continue on failure -- the next SetCrtc supersedes regardless. */
        if (drmModeSetCrtc(bi->drm_fd, bi->crtc_id, 0, 0, 0,
                           NULL, 0, NULL) < 0) {
            fprintf(stderr,
                    "{\"event\":\"runtime_crtc_detach_failed\","
                    "\"errno\":%d}\n", errno);
        }

        /* === Step 3: free old FBs. === */
        for (int i = 0; i < NUM_FBS; i++) {
            if (bi->fb_map[i]) {
                munmap(bi->fb_map[i], bi->fb_size);
                bi->fb_map[i] = NULL;
            }
            if (bi->fb_id[i]) {
                drmModeRmFB(bi->drm_fd, bi->fb_id[i]);
                bi->fb_id[i] = 0;
            }
            if (bi->handle[i]) {
                struct drm_mode_destroy_dumb dr;
                memset(&dr, 0, sizeof dr);
                dr.handle = bi->handle[i];
                (void)drmIoctl(bi->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dr);
                bi->handle[i] = 0;
            }
        }

        /* === Step 4: alloc new FBs at new dims. ===
         * If alloc fails post-RmFB, CRTC has no FB and is in an
         * inconsistent state. Abort daemon -- same catastrophic error path
         * as at start-up. */
        bi->fb_w = (uint32_t)result.width;
        bi->fb_h = (uint32_t)result.height;
        for (int i = 0; i < NUM_FBS; i++) {
            int afr = alloc_fb(bi->drm_fd, (int)bi->fb_w, (int)bi->fb_h,
                               &bi->fb_id[i], &bi->handle[i],
                               &bi->fb_map[i], &bi->fb_size, &bi->fb_pitch);
            if (afr != 0) {
                fprintf(stderr,
                        "{\"event\":\"runtime_realloc_fb_failed\","
                        "\"i\":%d,\"step\":%d,\"errno\":%d}\n",
                        i, afr, errno);
                atomic_store(&s_blit_fatal, 1);
                atomic_store(&bi->cfg.shared->running, 0);
                return -2;
            }
        }
    }

    /* === Step 5: drmModeSetCrtc with new mode + new FB. === */
    if (drmModeSetCrtc(bi->drm_fd, bi->crtc_id, bi->fb_id[0], 0, 0,
                       &bi->conn_id, 1, &new_mode) < 0) {
        if (dims_changed)   /* screen detached, nothing scans out any more */
            return modeset_refused_after_detach(bi, errno);
        fprintf(stderr,
                "{\"event\":\"runtime_setcrtc_failed\",\"errno\":%d}\n",
                errno);
        /* Daemon may be in a bad CRTC state; sticky-set synced so the
         * protocol layer doesn't deadlock. */
        atomic_store(&bi->cfg.shared->synced, 1);
        return -2;
    }

    bi->mode         = new_mode;
    bi->next_fb      = 1;
    bi->flip_pending = 0;
    bi->last_flip_ns = 0;  /* reset jitter delta -- first new flip is t0 */

modeset_done:
    shed_reset(&bi->shed);   /* other mode, other period */
    bi->shed_armed = 0;
    atomic_store(&bi->cfg.shared->synced, 1);
    atomic_store(&bi->cfg.shared->last_modeset_ns, now_ns());

    fprintf(stderr,
            "{\"event\":\"runtime_modeset_succeeded\","
            "\"hfreq_hz\":%.1f,\"vfreq_hz\":%.2f,\"pclock_hz\":%llu,"
            "\"w\":%d,\"h\":%d,\"same_geom_skip\":false,"
            "\"mono_ns\":%llu}\n",
            result.hfreq, result.vfreq,
            (unsigned long long)result.pclock,
            result.width, result.height,
            (unsigned long long)now_ns());
    return 0;
}

/* Polls switchres_seq (acquire-load), applies the modeset rate limit,
 * coalesces bursts (last-write-wins), invokes blit_runtime_modeset when the
 * window opens. *observed_seq is the blit-thread's private seq counter,
 * updated after a modeset attempt completes (success, rejection, or
 * same-geometry skip -- all consume the request). Returns the modeset
 * function's rc, or 0 if no work was done. */
static int blit_check_pending_switchres(struct blit_internal *bi,
                                        uint64_t *observed_seq)
{
    uint64_t cur_seq = atomic_load_explicit(
        &bi->cfg.shared->switchres_seq, memory_order_acquire);

    /* Latch the latest request if seq advanced. The acquire pairs with
     * net.c's release on switchres_seq -- the struct copy below is safe.
     * Last-write-wins: always keep the newest. */
    if (cur_seq != *observed_seq && cur_seq != bi->pending_seq) {
        /* `cur_seq != bi->pending_seq` -- a request already latched is not
         * copied again on every round while the rule delays it (thousands
         * of copies in a few seconds of waiting, and a coalesced_count that
         * no longer meant anything). A NEWER request has another number: it
         * is latched, the last one wins. */
        bi->pending_request = bi->cfg.shared->request;  /* struct copy */
        if (bi->pending_seq != 0) {
            /* Overwriting an already-pending request that hasn't fired
             * yet -- bump telemetry. */
            bi->coalesced_count++;
        }
        bi->pending_seq = cur_seq;
        /* Don't update *observed_seq yet -- that happens after the modeset
         * attempt completes below. */
    }

    if (bi->pending_seq == 0) return 0;

    /* Rate-limit window check. last_modeset_ns == 0 means no real
     * modeset has happened yet -- fire immediately. */
    uint64_t now_mono = now_ns();
    uint64_t last_ms  = atomic_load(&bi->cfg.shared->last_modeset_ns);
    if (last_ms != 0 && (now_mono - last_ms) < MIN_MODESET_INTERVAL_NS) {
        /* A request that crosses the band and that the floor holds back also
         * raises the wait (black screen, images acknowledged without being
         * shown): otherwise, up to six images of the other band would be
         * shown in the current mode before the rule takes over. No
         * band_crossing_deferred here: the rule will write it if it delays
         * further. */
        if (!atomic_load(&bi->cfg.shared->legacy_modeset) &&
            regime_crosses_band(atomic_load(&bi->cfg.shared->current_regime),
                                bi->pending_request.regime)) {
            if (!bi->band_wait) {
                band_wait_begin(bi);
            }
            /* The last target wins here too, wait already open or not (from
             * 480p, two targets cross the band: 240p and 480i). */
            bi->wait_target = bi->pending_request.regime;   /* what the wait aims at */
        }
        /* Too soon. Stay deferred. Log once per pending_seq value to
         * avoid log-flood when the page-flip loop sees the same pending
         * seq on every iteration. */
        if (bi->pending_seq != bi->last_rate_limited_seq) {
            fprintf(stderr,
                    "{\"event\":\"runtime_modeset_rate_limited\","
                    "\"pending_seq\":%llu,\"last_modeset_ns\":%llu,"
                    "\"mono_ns\":%llu,\"deferred_count\":%llu}\n",
                    (unsigned long long)bi->pending_seq,
                    (unsigned long long)last_ms,
                    (unsigned long long)now_mono,
                    (unsigned long long)bi->coalesced_count);
            bi->last_rate_limited_seq = bi->pending_seq;
        }
        return 0;
    }

    /* The band rule, AFTER the 100 ms floor and on the default path only
     * (the legacy path crosses no band). A crossing (15 kHz <-> 31 kHz)
     * asked before the instant the rule names is DELAYED, never refused:
     * the request stays pending, not consumed, and the last one always wins
     * (the latch above copies the newest). The 240p <-> 480i switch is not
     * a crossing and does not go through here. The floor above and
     * last_modeset_ns do not change: two clocks. */
    if (!atomic_load(&bi->cfg.shared->legacy_modeset)) {
        const uint8_t have = atomic_load(&bi->cfg.shared->current_regime);
        if (regime_crosses_band(have, bi->pending_request.regime) &&
            !bandrule_allows(&bi->rule, now_mono)) {
            /* The first time only, the screen turns black and the session
             * goes into an acknowledged wait; the following deferrals (a
             * newer request that still crosses) find it already raised. */
            if (!bi->band_wait) {
                band_wait_begin(bi);
            }
            bi->wait_target = bi->pending_request.regime;   /* the last target wins */
            if (bi->pending_seq != bi->last_deferred_seq) {
                int by_cap = 0;
                const uint64_t not_before = bandrule_not_before_ns(&bi->rule, &by_cap);
                fprintf(stderr,
                        "{\"event\":\"band_crossing_deferred\",\"from\":\"%s\","
                        "\"to\":\"%s\",\"wait_ms\":%llu,\"why\":\"%s\","
                        "\"pending_seq\":%llu}\n",
                        regime_name(have), regime_name(bi->pending_request.regime),
                        (unsigned long long)((not_before - now_mono) / 1000000ULL),
                        by_cap ? "cap" : "dwell",
                        (unsigned long long)bi->pending_seq);
                bi->last_deferred_seq = bi->pending_seq;
            }
            return 0;
        }
    }

    /* Window open OR first modeset (last_ms == 0). Fire. */
    uint64_t firing_seq = bi->pending_seq;
    uint64_t coal_count = bi->coalesced_count;
    bi->pending_seq           = 0;
    bi->coalesced_count       = 0;
    bi->last_rate_limited_seq = 0;
    bi->last_deferred_seq     = 0;

    /* The pending request is consumed, the wait ends. The images
     * acknowledged during the wait go into crossing_dropped
     * (band_crossing_first_image.dropped counts them with the cut after).
     * The starting regime and the wanted regime are read BEFORE the call:
     * after it, current_regime has moved. */
    const int     was_waiting = bi->band_wait;
    const uint8_t have_before = atomic_load(&bi->cfg.shared->current_regime);
    const uint8_t want_before = bi->pending_request.regime;
    /* band_crossing_cancelled is WRITTEN HERE, by the blit thread, and only
     * when a band wait ends without a crossing: the emitter came back to
     * the current regime (or went 240p <-> 480i) before the deadline the
     * rule imposed. The network thread does not know whether a wait took
     * place: it writes regime_request_reverted for every "stay in A"
     * request. Four cases used to be confused, only one is real: a
     * 240p <-> 480i switch under the floor, a request already served, a
     * crossing already under way opened no wait (was_waiting is false, or
     * the crossing happens) and write nothing. `from`: the current regime;
     * `to`: the abandoned target, kept when the wait opened; `now`: the
     * request that replaced it. A wait without a target (the black of a
     * failed crossing, crossing_hold_black) cancels nothing. */
    const int wait_ends_uncrossed = was_waiting && !regime_crosses_band(have_before, want_before);
    if (wait_ends_uncrossed && bi->wait_target != WAIT_TARGET_NONE) {
        fprintf(stderr,
                "{\"event\":\"band_crossing_cancelled\",\"from\":\"%s\",\"to\":\"%s\","
                "\"now\":\"%s\"}\n",
                regime_name(have_before), regime_name(bi->wait_target),
                regime_name(want_before));
    }
    bi->wait_target      = WAIT_TARGET_NONE;
    bi->band_wait        = 0;
    bi->crossing_dropped = bi->wait_dropped;
    bi->wait_dropped     = 0u;

    int rc = blit_runtime_modeset(bi, &bi->pending_request);

    /* Consume the request regardless of rc -- rejection is not a retry. */
    *observed_seq = firing_seq;

    /* A wait that ends WITHOUT crossing (cancellation, or 240p <-> 480i
     * asked meanwhile): the images of the other band that arrived during
     * the wait are still in the queue, composed for a regime the emitter
     * has left. They are acknowledged without being shown: the cut is the
     * reception instant of the request that ended the wait. (A crossing
     * sets its own cut, at the end of the call, in blit_regime_toggle.) */
    if (was_waiting && !regime_crosses_band(have_before, want_before) &&
        bi->pending_request.monotonic_ns_received > bi->drop_before_ns) {
        bi->drop_before_ns = bi->pending_request.monotonic_ns_received;
    }

    if (coal_count > 0 && rc == 0) {
        fprintf(stderr,
                "{\"event\":\"runtime_modeset_coalesced\","
                "\"coalesced_count\":%llu,\"final_seq\":%llu}\n",
                (unsigned long long)coal_count,
                (unsigned long long)firing_seq);
    }
    return rc;
}

/* blit_wake_drain -- empties the wake-up eventfd: one 8-byte read resets
 * the counter to zero, whatever the number of images signalled since.
 * Non-blocking (EFD_NONBLOCK, main.c); the result is ignored -- an EAGAIN
 * means there was nothing, and the SPSC queue, not this counter, says what
 * is left to consume. */
static void blit_wake_drain(const struct blit_internal *bi)
{
    uint64_t n = 0u;
    ssize_t r = read(bi->cfg.wake_fd, &n, sizeof n);
    (void)r;
}

/* -----------------------------------------------------------------------------
 * Main loop
 * ---------------------------------------------------------------------------*/
static void *blit_thread_main(void *arg)
{
    struct blit_internal *bi = (struct blit_internal *)arg;

    (void)apply_realtime(bi->cfg.cpu_affinity, bi->cfg.sched_fifo_priority);

    /* Env-var contract for test fixtures. Detect ONCE at thread entry;
     * blit_init + blit_runtime_modeset both branch on this. */
    {
        const char *no_drm = getenv("CRT_BRIDGE_NO_DRM");
        bi->test_mode_no_drm =
            (no_drm && no_drm[0] && no_drm[0] != '0') ? 1 : 0;
        if (bi->test_mode_no_drm) {
            fprintf(stderr, "{\"event\":\"blit_test_mode_no_drm\"}\n");
        }

        /* Test hook: capture FB dump path ONCE at thread entry (NULL in prod
         * -- getenv is never called again after this point, so the hot path
         * pays zero cost). Enables bit-exact test assertions by dumping the
         * post-blit XRGB framebuffer to a file. */
        bi->fb_dump_path = getenv("CRT_BRIDGE_FB_DUMP_PATH");
        if (bi->fb_dump_path) {
            fprintf(stderr,
                    "{\"event\":\"fb_dump_hook_enabled\",\"path\":\"%s\"}\n",
                    bi->fb_dump_path);
        }

        /* Latency instrumentation: env-var read ONCE at thread entry (same
         * pattern as CRT_BRIDGE_FB_DUMP_PATH above), ring calloc'd ONCE here
         * -- never on the hot path. Failure is non-fatal: lat_ring stays
         * NULL, lat_ring_record() no-ops, the daemon keeps running. */
        bi->lat_csv_path = getenv("CRT_BRIDGE_LATENCY_CSV");
        bi->lat_ring = (struct lat_sample *)calloc(LAT_RING_CAP, sizeof(struct lat_sample));
        if (!bi->lat_ring) {
            fprintf(stderr, "{\"event\":\"latency_ring_alloc_failed\"}\n");
        }

        /* Queue policy, read ONCE at thread entry (same pattern as
         * CRT_BRIDGE_NO_DRM / CRT_BRIDGE_FB_DUMP_PATH above). Logged
         * unconditionally: identifies which policy produced which
         * measurement set when two runs are compared. */
        const char *qpol = getenv("CRT_BRIDGE_QUEUE_POLICY");
        bi->queue_fifo = (qpol && strcmp(qpol, "fifo") == 0) ? 1 : 0;
        fprintf(stderr, "{\"event\":\"queue_policy\",\"policy\":\"%s\"}\n",
                bi->queue_fifo ? "fifo" : "latest");

        /* The wake-up policy, declared once per lifetime, next to
         * queue_policy, read by the bench tools. The value reported is the
         * one in use: an eventfd that main() could not create arrives here
         * as -1 and is reported as 0 (main wrote wake_fd_failed). */
        /* The shed window really in use. main() already refuses
         * --shed-window without --wake-on-frame; if the eventfd could not be
         * created, shedding turns off with the wake-up and is reported as 0,
         * like it. */
        int shed_window = (bi->cfg.wake_fd >= 0 && bi->cfg.shed_window > 0)
            ? bi->cfg.shed_window : 0;
        shed_init(&bi->shed, (uint32_t)shed_window);
        bi->shed_armed    = 0;
        bi->carry_skipped = 0u;
        bi->pending_shed  = 0u;
        fprintf(stderr, "{\"event\":\"flip_policy\",\"wake_on_frame\":%d,"
                "\"shed_window\":%d}\n",
                bi->cfg.wake_fd >= 0 ? 1 : 0, shed_window);

        /* 480i field phase: read once from cfg, like the rest of this
         * block. phase_locked starts at 0: the first 480i pair will decide. */
        bi->field_phase = bi->cfg.field_phase;
        bi->phase_locked = 0u;
        fprintf(stderr, "{\"event\":\"field_phase_mode\",\"mode\":%u}\n",
                (unsigned)bi->field_phase);
    }

    /* Test hook, honoured only under CRT_BRIDGE_NO_DRM: simulates a display
     * initialisation failure, which this mode cannot produce otherwise.
     * Only the exact value "1" arms it. */
    const char *fail_init = bi->test_mode_no_drm
        ? getenv("CRT_BRIDGE_TEST_FAIL_BLIT_INIT") : NULL;
    int init_rc = (fail_init && strcmp(fail_init, "1") == 0)
        ? -99 : blit_init(bi);
    if (init_rc != 0) {
        fprintf(stderr, "{\"event\":\"blit_thread_init_failed\",\"rc\":%d}\n", init_rc);
        atomic_store(&s_blit_fatal, 1);
        atomic_store(&bi->cfg.shared->running, 0);
        blit_cleanup(bi);
        free(bi->lat_ring);   /* do not leak the latency ring on init failure */
        free(bi);
        return NULL;
    }

    /* Page-flip test hooks, honoured only under CRT_BRIDGE_NO_DRM and read
     * once here. FAIL_FLIP: only the exact value "1" arms it, every
     * simulated flip fails. FAIL_FLIP_N=N (positive integer): N simulated
     * flips fail, then one succeeds, and so on -- which proves the reset on
     * success. */
    if (bi->test_mode_no_drm) {
        const char *ff = getenv("CRT_BRIDGE_TEST_FAIL_FLIP");
        bi->flip_test_fail_all = (ff && strcmp(ff, "1") == 0) ? 1 : 0;
        const char *fn = getenv("CRT_BRIDGE_TEST_FAIL_FLIP_N");
        if (fn && fn[0] >= '1' && fn[0] <= '9') {
            char *endp = NULL;
            unsigned long v = strtoul(fn, &endp, 10);
            if (endp && *endp == '\0' && v <= 1000000UL)
                bi->flip_test_fail_n = (unsigned)v;
        }
        if (bi->flip_test_fail_all || bi->flip_test_fail_n) {
            fprintf(stderr,
                    "{\"event\":\"test_hook_flip\",\"fail_all\":%d,\"fail_n\":%u}\n",
                    bi->flip_test_fail_all, bi->flip_test_fail_n);
        }

        /* CRT_BRIDGE_TEST_FAIL_CROSSING: only the exact value "1" arms it
         * (pattern of CRT_BRIDGE_TEST_FAIL_FLIP); every call that would cross
         * the band is then simulated as failing. Read here, once, under
         * test_mode_no_drm only: on real DRM, no getenv. */
        const char *fc = getenv("CRT_BRIDGE_TEST_FAIL_CROSSING");
        bi->crossing_test_fail = (fc && strcmp(fc, "1") == 0) ? 1 : 0;
        if (bi->crossing_test_fail) {
            fprintf(stderr, "{\"event\":\"test_hook_crossing\",\"fail_all\":1}\n");
        }
    }

    /* The band rule. The starting values are the constants of safety.h. Two
     * test hooks shorten them, read HERE, once, and ONLY under
     * test_mode_no_drm: on real DRM no getenv is called, the rule is never
     * shortened. CRT_BRIDGE_TEST_BAND_DWELL_MS: decimal integer >= 0 (ms);
     * CRT_BRIDGE_TEST_BAND_CAP: decimal integer from 1 to BANDRULE_MAX_CAP.
     * An invalid value is ignored and reported (the variable name, never
     * its value). The band_rule line states the rule in force and its
     * source. Band entry is the initialisation instant (the CRT is at
     * 15 kHz at start-up). */
    {
        uint64_t rule_dwell_ns = BAND_DWELL_MIN_NS;
        unsigned rule_cap      = BAND_CROSSINGS_PER_MIN_MAX;
        int      rule_hooked   = 0;
        if (bi->test_mode_no_drm) {
            const char *bd = getenv("CRT_BRIDGE_TEST_BAND_DWELL_MS");
            if (bd) {
                char *endp = NULL;
                unsigned long v = strtoul(bd, &endp, 10);
                if (bd[0] >= '0' && bd[0] <= '9' && endp && *endp == '\0' &&
                    v <= 86400000UL) {
                    rule_dwell_ns = (uint64_t)v * 1000000ULL;
                    rule_hooked   = 1;
                } else {
                    fprintf(stderr,
                            "{\"event\":\"test_hook_band_ignored\","
                            "\"var\":\"CRT_BRIDGE_TEST_BAND_DWELL_MS\"}\n");
                }
            }
            const char *bc = getenv("CRT_BRIDGE_TEST_BAND_CAP");
            if (bc) {
                char *endp = NULL;
                unsigned long v = strtoul(bc, &endp, 10);
                if (bc[0] >= '0' && bc[0] <= '9' && endp && *endp == '\0' &&
                    v >= 1UL && v <= (unsigned long)BANDRULE_MAX_CAP) {
                    rule_cap    = (unsigned)v;
                    rule_hooked = 1;
                } else {
                    fprintf(stderr,
                            "{\"event\":\"test_hook_band_ignored\","
                            "\"var\":\"CRT_BRIDGE_TEST_BAND_CAP\"}\n");
                }
            }
        }
        bandrule_init(&bi->rule, rule_dwell_ns, rule_cap, now_ns());
        bi->last_deferred_seq = 0;
        fprintf(stderr,
                "{\"event\":\"band_rule\",\"dwell_ms\":%llu,\"cap_per_min\":%u,"
                "\"source\":\"%s\"}\n",
                (unsigned long long)(bi->rule.dwell_ns / 1000000ULL), bi->rule.cap,
                rule_hooked ? "test_hook" : "constants");
    }

    fprintf(stderr,
            "{\"event\":\"page_flip_loop_starting\",\"start_ns\":%" PRIu64 "}\n",
            now_ns());

    drmEventContext ev;
    memset(&ev, 0, sizeof ev);
    ev.version           = 2;
    ev.page_flip_handler = page_flip_handler;

    /* Blit-thread's private switchres_seq counter. Loaded once at thread
     * entry to absorb any spurious bump that landed before
     * blit_thread_main started. blit_check_pending_switchres updates this
     * after every modeset attempt (success/rejection/skip). */
    uint64_t observed_seq = atomic_load(&bi->cfg.shared->switchres_seq);

    while (atomic_load(&bi->cfg.shared->running)) {

        /* Check for a pending CMD_SWITCHRES BEFORE the page-flip dequeue.
         * Sub-microsecond when no seq delta. */
        /* -2 with running dropped to 0 = the image buffers could not be
         * reallocated. Leave at once: finishing the round would write a
         * frame into a released buffer (SIGSEGV instead of exit code 1). A
         * -2 without a stop (refused mode change) carries on. */
        if (blit_check_pending_switchres(bi, &observed_seq) == -2
            && !atomic_load(&bi->cfg.shared->running))
            break;

        /* Schedule a page flip if one isn't already in flight. */
        if (!bi->flip_pending) {
            void *raw_slot = NULL;
            uint32_t skipped = 0U;
            struct spsc_ring *ring =
                (struct spsc_ring *)bi->cfg.shared->spsc_ring;
            int got = 0;
            if (ring) {
                got = bi->queue_fifo
                    ? spsc_try_consume(ring, &raw_slot)
                    : spsc_consume_latest(ring, &raw_slot, &skipped);
            }
            /* The arming set by page_flip_handler only holds for THIS pass,
             * the first one without a flip in flight after the event: image
             * taken or not, it drops here. An image that arrives later is
             * therefore on time and will never be shed; the series, for its
             * part, stays above the window and re-arms at the next excess.
             * Off (window 0), shed_armed is always 0 and this block does
             * nothing. */
            int shed_now = bi->shed_armed;
            bi->shed_armed = 0;
            if (got == 1 && raw_slot &&
                (bi->band_wait ||
                 ((const struct frame_slot *)raw_slot)->ready_ns < bi->drop_before_ns)) {
                /* An image that arrived before the band crossing ended is
                 * acknowledged and dropped -- neither converted, nor shown,
                 * no flip. drop_before_ns is 0 outside a crossing: this
                 * branch is then never taken. During the wait imposed by
                 * the rule (band_wait), EVERY image is: the screen stays
                 * black and the emitter keeps receiving its
                 * acknowledgements. */
                blit_ack_without_display(bi, (const struct frame_slot *)raw_slot);
            } else if (got == 1 && raw_slot && shed_now &&
                !((const struct frame_slot *)raw_slot)->is_field) {
                /* The image is neither converted nor flipped: the slot goes
                 * back to the producer through the tail advance already
                 * done (spsc.h), flip_pending stays 0, and the next image,
                 * woken up by the eventfd, leaves at the first possible
                 * vblank. It will carry in `skipped` those the queue skipped
                 * here plus this one, and `shed` = 1. No wait, no call, no
                 * allocation. */
                bi->carry_skipped += skipped + 1u;
                bi->pending_shed   = 1u;
                shed_done(&bi->shed);
            } else if (got == 1 && raw_slot) {
                struct frame_slot *slot = (struct frame_slot *)raw_slot;

                /* Capture BEFORE pixel conversion, common to both
                 * completion paths (real DRM via page_flip_handler,
                 * simulated via test_mode_no_drm below).
                 * Plain loads/copies only -- cur_spsc_depth reflects the
                 * queue depth immediately after THIS slot was retired. */
                bi->cur_first_byte_ns = slot->first_byte_ns;
                bi->cur_ready_ns      = slot->ready_ns;
                bi->cur_consume_ns    = now_ns();
                /* A real frame: leave idle if needed. */
                bi->last_content_ns   = bi->cur_consume_ns;
                if (bi->idle_active) {
                    bi->idle_active  = 0;
                    bi->last_flip_ns = 0;  /* no giant gap in the first jitter sample */
                    fprintf(stderr, "{\"event\":\"idle_exit\"}\n");
                }
                bi->cur_spsc_depth    = spsc_count(ring);
                /* The images shed since the last image shown count in this
                 * one's `skipped` (hence in latency_1s), and separately in
                 * `shed`. 0 and 0 without shedding. */
                bi->cur_skipped       = skipped + bi->carry_skipped;
                bi->cur_decomp_us     = slot->decomp_us;
                bi->cur_flipreq_ns    = 0u;   /* set at the flip, below */
                bi->cur_shed          = bi->pending_shed;
                bi->carry_skipped     = 0u;
                bi->pending_shed      = 0u;
                bi->lat_skipped_total += (uint64_t)bi->cur_skipped;

                /* Bound blit by min(slot bytes, fb capacity).
                 * An is_field slot holds BOTH fields interleaved across 2x
                 * the per-field row count (received_bytes is field-local
                 * after the between-fields reset) -- the conversion must
                 * span the full interleaved row range. */
                int slot_pixels = slot->is_field
                    ? (int)(slot->received_bytes * 2U / 3U)
                    : (int)(slot->received_bytes / 3U);
                /* Prefer dims announced via CMD_SWITCHRES; fall back to CLI cfg.* when
                 * we haven't seen one yet (cold start). Either dim being zero triggers the
                 * fallback for that axis independently -- see net.h for the atomicity note. */
                uint32_t h_ann = atomic_load(&bi->cfg.shared->h_active_announced);
                uint32_t v_ann = atomic_load(&bi->cfg.shared->v_active_announced);
                int blit_w = h_ann ? (int)h_ann : bi->cfg.width;
                int blit_h = v_ann ? (int)v_ann : bi->cfg.height;
                if (slot_pixels > 0) {
                    /* Measured on the hardware: center against the ACTIVE
                     * regime's scanout height (mode.vdisplay: 240 in super-res,
                     * 480 in 480i), not the 480-row allocation height -- centering in the full
                     * buffer pushed the image half out of the 240-line scanout
                     * window (top half of image on bottom half of screen). */
                    int scan_h = bi->mode.vdisplay ? (int)bi->mode.vdisplay
                                                   : (int)bi->fb_h;
                    blit_rgb888_to_xrgb8888(slot->buf,
                                            bi->fb_map[bi->next_fb],
                                            blit_w,
                                            blit_h,
                                            slot_pixels,
                                            bi->fb_pitch,
                                            (int)bi->fb_w,
                                            scan_h);

                    /* Test hook: dump the post-blit XRGB framebuffer for a
                     * bit-exact test assertion. Gated by BOTH fb_dump_path !=
                     * NULL AND test_mode_no_drm -- production daemon sets
                     * neither, so this is fully dead code on real hardware.
                     * Atomic rename (.tmp + rename()) avoids torn reads in the
                     * test fixture. */
                    if (bi->fb_dump_path && bi->test_mode_no_drm) {
                        char tmp_path[PATH_MAX];
                        int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp",
                                        bi->fb_dump_path);
                        if (n > 0 && n < (int)sizeof(tmp_path)) {
                            int dfd = open(tmp_path,
                                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                                          0644);
                            if (dfd >= 0) {
                                size_t fb_bytes = (size_t)bi->fb_w *
                                                  (size_t)bi->fb_h * 4u;
                                ssize_t w = write(dfd, bi->fb_map[bi->next_fb],
                                                  fb_bytes);
                                close(dfd);
                                if (w == (ssize_t)fb_bytes) {
                                    (void)rename(tmp_path, bi->fb_dump_path);
                                }
                            }
                        }
                    }
                }
                bi->current_frame_id = slot->frame_id;
                bi->current_vcount   = slot->last_vsync;
                /* Set HERE, before any branch (test_mode_no_drm or real
                 * drmModePageFlip), the SAME discipline as is_field/field on
                 * the net.c side (set before any break/branch, never after):
                 * the frame REALLY consumed carries its own parity, immune to
                 * the delay of bi->current_regime (hardware mirror, see
                 * field_flag_for). page_flip_handler will read it later,
                 * asynchronously, on THIS SAME slot (one flip in flight at a
                 * time, bi->flip_pending guarantees it). */
                bi->current_is_field = slot->is_field;

                /* ONE-TIME re-phasing of the flip. The slot carries the
                 * number of the half-frame that COMPLETES the pair; the
                 * opening is therefore (frame_id + 1) & 1. If the field the
                 * CRT will scan right after the flip does not carry this
                 * parity, let ONE more vblank go by. Cost: at most one field
                 * of delay, ONCE per entry into 480i. No sharing between
                 * threads, no field added to the duplicated structs.
                 *
                 * drmWaitVBlank rather than DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE:
                 * the wait is a one-off (once per session), and this path
                 * depends on no optional driver capability.
                 *
                 * THIS LEVER IS AN INSTRUMENT, NOT A FIX (measured on the
                 * bench): on this kernel, the DRM vblank counter counts
                 * IMAGES in 480i, not fields. The parity of next_seq
                 * therefore says nothing about the scanned field, and the
                 * wait below delays by a whole IMAGE (33 ms), never by a
                 * field. It is BLOCKING on this SCHED_FIFO 80 thread, bounded
                 * by the kernel at 3 s (drm_wait_vblank_ioctl), not by "one
                 * field period". Off by default; to be rewritten as a
                 * per-frame re-phasing or removed, never to be retried as a
                 * fix for the field order. */
                if (!bi->test_mode_no_drm &&
                    bi->field_phase != (uint8_t)FIELD_PHASE_OFF &&
                    bi->current_is_field && !bi->phase_locked) {
                    drmVBlank vbl;
                    uint32_t cur_seq = 0u;
                    int vbl_read, delay_flip = 0;
                    /* Target the CRTC of the flip, not pipe 0: without the
                     * DRM_VBLANK_HIGH_CRTC_MASK bits, drmWaitVBlank queries
                     * pipe 0, which bi->crtc_id (pick_crtc_connector) does
                     * not necessarily name -- the CRTC0 guard of mmio_init
                     * does not cover this path. Unknown index (-1) or outside
                     * the five bits of the mask: nothing is set, the original
                     * behaviour (pipe 0) stays, and field_phase_lock says so. */
                    drmVBlankSeqType vbl_target = DRM_VBLANK_RELATIVE;
                    if (bi->crtc_index > 0 &&
                        bi->crtc_index <= (int)(DRM_VBLANK_HIGH_CRTC_MASK
                                                >> DRM_VBLANK_HIGH_CRTC_SHIFT)) {
                        vbl_target = (drmVBlankSeqType)((unsigned)vbl_target |
                                (((unsigned)bi->crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT)
                                 & (unsigned)DRM_VBLANK_HIGH_CRTC_MASK));
                    }
                    memset(&vbl, 0, sizeof vbl);
                    vbl.request.type     = vbl_target;
                    vbl.request.sequence = 0;     /* query, does not block */
                    vbl_read = (drmWaitVBlank(bi->drm_fd, &vbl) == 0);
                    if (vbl_read) {
                        cur_seq  = vbl.reply.sequence;
                        delay_flip = field_phase_delay_needed(
                                       bi->field_phase,
                                       field_phase_pair_open_parity(bi->current_frame_id),
                                       cur_seq + 1u);
                    }
                    fprintf(stderr,
                            "{\"event\":\"field_phase_lock\",\"mode\":%u,"
                            "\"frame\":%u,\"p_open\":%u,\"cur_seq\":%u,"
                            "\"seq_read\":%d,\"delay\":%d,\"crtc_index\":%d}\n",
                            (unsigned)bi->field_phase,
                            bi->current_frame_id,
                            (unsigned)field_phase_pair_open_parity(bi->current_frame_id),
                            cur_seq, vbl_read, delay_flip, bi->crtc_index);
                    bi->phase_locked = 1u;
                    if (delay_flip) {
                        drmVBlank wait_vbl;
                        memset(&wait_vbl, 0, sizeof wait_vbl);
                        wait_vbl.request.type     = vbl_target;
                        wait_vbl.request.sequence = 1;  /* blocks until the next vblank: a whole
                                                    * IMAGE in 480i on this kernel,
                                                    * bounded at 3 s (drm_wait_vblank_ioctl) */
                        if (drmWaitVBlank(bi->drm_fd, &wait_vbl) != 0) {
                            fprintf(stderr,
                                    "{\"event\":\"field_phase_wait_failed\",\"errno\":%d}\n",
                                    errno);
                        }
                    }
                }

                /* A simulated flip to make fail (hooks read at thread entry,
                 * test_mode_no_drm only). */
                int sim_flip_refused = 0;
                if (bi->test_mode_no_drm) {
                    if (bi->flip_test_fail_all) {
                        sim_flip_refused = 1;
                    } else if (bi->flip_test_fail_n) {
                        sim_flip_refused = (bi->flip_test_k
                                              % (bi->flip_test_fail_n + 1u))
                                             < bi->flip_test_fail_n;
                        bi->flip_test_k++;
                    }
                }

                if (sim_flip_refused) {
                    /* Same path as the real refusal below: no completion,
                     * no acknowledgement, the buffer does not advance. */
                    errno = EINVAL;
                    flip_failed(bi, errno, "blit");
                } else if (bi->test_mode_no_drm) {
                    /* Test mode: simulate immediate flip completion so the
                     * ACK echo path runs end-to-end for test fixtures. No
                     * real drmModePageFlip; no drmHandleEvent. Mirrors
                     * page_flip_handler's atomic stores so net.c's build_ack
                     * sees the same state. */
                    /* The conversion is done, just before the simulated
                     * completion -- the mirror of the return of
                     * drmModePageFlip on the real path. */
                    bi->cur_flipreq_ns = sat_delta_ns(now_ns(), bi->cur_consume_ns);
                    bi->flip_fail_run = 0u;   /* a success resets the run */
                    bi->next_fb = (bi->next_fb + 1) % NUM_FBS;
                    /* Same expression as page_flip_handler -- the two MUST
                     * stay identical (field_flag_for).
                     * bi->current_is_field, not bi->current_regime. */
                    uint8_t field_bit = field_flag_for(bi->current_is_field,
                                                    bi->current_frame_id);
                    atomic_store(&bi->cfg.shared->ack_frame,
                                 bi->current_frame_id);
                    atomic_store(&bi->cfg.shared->ack_vcount,
                                 bi->current_vcount);
                    atomic_store(&bi->cfg.shared->ack_flags,
                        (uint8_t)(FLAG_VRAM_READY | FLAG_VRAM_END_FRAME | field_bit));
                    atomic_store(&bi->cfg.shared->ack_ready, (uint8_t)1);

                    /* Simulated completion path (CRT_BRIDGE_NO_DRM=1). No
                     * DRM timestamp available here, so now_ns() stands in as
                     * flip_ts_ns -- required so the smoke-test fixture
                     * produces real samples without hardware. */
                    lat_ring_record(bi, now_ns(), 0u, 0u);  /* no vblank sequence here */
                    /* Same end of measurement as page_flip_handler. */
                    crossing_note_first_image(bi, now_ns());
                } else if (drmModePageFlip(bi->drm_fd,
                                    bi->crtc_id,
                                    bi->fb_id[bi->next_fb],
                                    DRM_MODE_PAGE_FLIP_EVENT,
                                    bi) == 0) {
                    /* Conversion and flip call, measured on return;
                     * page_flip_handler files it with the rest. */
                    bi->cur_flipreq_ns = sat_delta_ns(now_ns(), bi->cur_consume_ns);
                    /* Never page-flip while flip_pending. */
                    bi->flip_pending = 1;
                    bi->next_fb = (bi->next_fb + 1) % NUM_FBS;
                    bi->flip_fail_run = 0u;   /* a success resets the run */
                } else if (errno != EBUSY) {
                    flip_failed(bi, errno, "blit");  /* threshold 60, then exit code 1 */
                }
            } else {
                /* Empty queue: the only place idle starts from. */
                blit_idle_step(bi, now_ns());
            }
        }

        if (bi->test_mode_no_drm && bi->cfg.wake_fd >= 0) {
            /* Wake-on-frame in test mode: no drm_fd, the eventfd alone
             * replaces the 2 ms nanosleep. Same 2 ms ceiling (running=0,
             * switchres_seq), same drain as the real path. */
            struct pollfd wfd;
            wfd.fd      = bi->cfg.wake_fd;
            wfd.events  = POLLIN;
            wfd.revents = 0;
            int wr = poll(&wfd, 1, 2);
            if (wr > 0 && (wfd.revents & POLLIN)) {
                blit_wake_drain(bi);
            }
            continue;
        }

        if (bi->test_mode_no_drm) {
            /* No drm_fd to poll; sleep briefly so the loop is responsive to
             * running=0 + SPSC + switchres_seq without busy-spinning. */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000L };
            nanosleep(&ts, NULL);
            continue;
        }

        if (bi->cfg.wake_fd >= 0) {
            /* Wait in the SAME poll for the end of the flip (DRM) and the
             * arrival of an image (eventfd of the network thread): an image
             * that arrives with no flip in flight is taken on arrival, no
             * longer on the 2 ms step. The 2 ms ceiling is kept. The eventfd
             * is drained at every wake-up, so a flip in flight that prevents
             * consuming does not make the loop spin: the next poll sleeps
             * until the flip, the next image or the 2 ms. */
            struct pollfd pfd2[2];
            pfd2[0].fd      = bi->drm_fd;
            pfd2[0].events  = POLLIN;
            pfd2[0].revents = 0;
            pfd2[1].fd      = bi->cfg.wake_fd;
            pfd2[1].events  = POLLIN;
            pfd2[1].revents = 0;
            int pr2 = poll(pfd2, 2, 2);
            if (pr2 > 0 && (pfd2[1].revents & POLLIN)) {
                blit_wake_drain(bi);
            }
            if (pr2 > 0 && (pfd2[0].revents & POLLIN)) {
                (void)drmHandleEvent(bi->drm_fd, &ev);
            }
            continue;
        }

        /* Wait for a page-flip completion event, with a 2 ms ceiling so we
         * stay responsive to running=0 and to new SPSC work even when no
         * flip is currently pending. */
        struct pollfd pfd;
        pfd.fd      = bi->drm_fd;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 2);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            (void)drmHandleEvent(bi->drm_fd, &ev);
        }
    }

    fprintf(stderr, "{\"event\":\"page_flip_loop_stopped\"}\n");

    /* Latency CSV dump -- cold path only, after the loop exits and BEFORE
     * blit_cleanup (mirrors jitter's cold-path-only I/O discipline). Wrap
     * handling identical to jitter_flush_csv: start at head&mask once the
     * ring has wrapped, else linear from index 0. total_ns is derived here,
     * not stored in the ring. */
    if (bi->lat_ring) {
        char default_path[64];
        const char *path = bi->lat_csv_path;
        if (!path) {
            snprintf(default_path, sizeof default_path,
                     "/var/log/crt-bridge/latency-%d.csv", (int)getpid());
            path = default_path;
        }
        FILE *fp = fopen(path, "w");
        if (fp) {
            /* Twelve-column contract: the eight original columns, in their
             * order, then the four added later. Compared word for word with
             * the header the bench's run-report tool expects. */
            fprintf(fp, "flip_ts_ns,frame_id,reasm_ns,queue_ns,scanout_ns,total_ns,spsc_depth,skipped,vbl_seq,flipreq_ns,decomp_us,shed\n");
            size_t n = (bi->lat_ring_written < (size_t)LAT_RING_CAP)
                       ? bi->lat_ring_written : (size_t)LAT_RING_CAP;
            size_t start = (bi->lat_ring_written < (size_t)LAT_RING_CAP)
                           ? 0u : (bi->lat_ring_head & (size_t)(LAT_RING_CAP - 1u));
            for (size_t i = 0u; i < n; i++) {
                size_t idx = (start + i) & (size_t)(LAT_RING_CAP - 1u);
                const struct lat_sample *s = &bi->lat_ring[idx];
                uint64_t total_ns = (uint64_t)s->reasm_ns + (uint64_t)s->queue_ns
                                   + (uint64_t)s->scanout_ns;
                fprintf(fp, "%" PRIu64 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
                        ",%" PRIu64 ",%" PRIu32 ",%" PRIu32
                        ",%" PRIu32 ",%" PRIu32 ",%u,%u\n",
                        s->flip_ts_ns, s->frame_id, s->reasm_ns, s->queue_ns,
                        s->scanout_ns, total_ns, s->spsc_depth, s->skipped,
                        s->vbl_seq, s->flipreq_ns,
                        (unsigned)s->decomp_us, (unsigned)s->shed);
            }
            fclose(fp);
            fprintf(stderr,
                    "{\"event\":\"latency_csv_written\",\"path\":\"%s\",\"rows\":%zu}\n",
                    path, n);
        } else {
            fprintf(stderr,
                    "{\"event\":\"latency_csv_open_failed\",\"path\":\"%s\",\"errno\":%d}\n",
                    path, errno);
        }
        free(bi->lat_ring);
        bi->lat_ring = NULL;
    }

    blit_cleanup(bi);
    free(bi);
    return NULL;
}

/* -----------------------------------------------------------------------------
 * Public lifecycle
 * ---------------------------------------------------------------------------*/

int blit_thread_start(struct blit_thread_config *cfg, pthread_t *out_tid)
{
    if (!cfg || !out_tid || !cfg->shared) {
        return -1;
    }
    /* The guard reads the CRT description at every mode handed over;
     * without a table, it would have nothing to judge. main() always sets
     * it (built-in band by default). */
    if (!cfg->monitor) {
        return -1;
    }
    struct blit_internal *bi = calloc(1, sizeof *bi);
    if (!bi) {
        return -1;
    }
    bi->cfg    = *cfg;
    bi->idle_after_ns = (cfg->idle_after_s > 0)
        ? (uint64_t)cfg->idle_after_s * UINT64_C(1000000000) : UINT64_C(0);
    idle_local_ipv4(bi->idle_ip, sizeof bi->idle_ip);
    bi->drm_fd = -1;
    bi->crossing_reason = CROSSING_REASON_REQUEST;
    bi->wait_target     = WAIT_TARGET_NONE;

    if (pthread_create(out_tid, NULL, blit_thread_main, bi) != 0) {
        free(bi);
        return -1;
    }
    return 0;
}

void blit_thread_join(pthread_t tid)
{
    (void)pthread_join(tid, NULL);
}
