/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * blit.h -- DRM/KMS blit thread public API.
 *
 * The blit thread:
 *   1. Initialises switchres (switchres_setup: sr_init -> the ten crt_rangeN
 *      reset to "auto" -> one crt_rangeN per band of the monitor file
 *      -> sr_set_option "api"="drmkms" -> sr_set_monitor "custom" last
 *      -> sr_init_disp -> sr_switch_to_mode). switchres reads the same
 *      monitor file as the mode guard; there is no hard-coded arcade_15
 *      preset any more.
 *   2. Every mode handed to the card goes through modeset_guard
 *      (monitor_check_mode), on the REAL drmModeModeInfo, in real numbers,
 *      as an (H, V) pair inside one band of the file; only the low band is
 *      usable for now.
 *      Hardware-loss-grade abort on rejection -- NO silent fallback.
 *   3. Releases switchres DRM master (sr_deinit), opens /dev/dri/card0 fresh,
 *      drmSetMaster, picks the first connected connector + its CRTC.
 *   4. Allocates 2 dumb buffers (XRGB8888) and drmModeAddFB on each.
 *   5. drmModeSetCrtc to repoint the CRTC at fb[0].
 *   6. Page-flip loop: pull a slot from the SPSC ring, blit RGB888 -> XRGB8888,
 *      drmModePageFlip(DRM_MODE_PAGE_FLIP_EVENT), poll/drmHandleEvent,
 *      record jitter, signal net thread to ACK.
 *
 * Threading: SCHED_FIFO sched_fifo_priority, pinned to cpu_affinity.
 *
 * Public types from net.h are referenced by pointer only -- this header does
 * not need to know their layout. blit.c's full definition is guarded so it
 * coexists with net.h without redefinition.
 */
#ifndef RECEIVER_BLIT_H
#define RECEIVER_BLIT_H

#include <pthread.h>
#include <stdint.h>

/* Forward decls -- full layout owned by net.h and jitter.h. */
struct net_blit_shared;
struct jitter_ring;
struct monitor;     /* monitor.h: the CRT, described by a file */

struct blit_thread_config {
    const char              *drm_device;        /* default "/dev/dri/card0" */
    int                      width;             /* 320 */
    int                      height;            /* 240 */
    double                   refresh_hz;        /* 60.0 */
    int                      cpu_affinity;      /* -1 to skip */
    int                      sched_fifo_priority; /* 0 to skip */
    /* Idle screen. Turns the CRT black after a silence, so the last image
     * does not burn into the phosphor.
     *   idle_after_s : seconds of silence before idling; 0 = disabled
     *   idle_black   : black only, without the text block */
    int                      idle_after_s;
    int                      idle_black;
    /* Field phase of a 480i switch: FIELD_PHASE_OFF (default, the original
     * behaviour), FIELD_PHASE_A or FIELD_PHASE_B.
     * Values in receiver/fieldphase.h. */
    uint8_t                  field_phase;
    /* eventfd that the network thread increments for each image it
     * publishes in the queue; the blit thread waits on it in the same poll
     * as the DRM descriptor. -1 = off (default, the original behaviour:
     * poll on a single descriptor every 2 ms). Created and closed by
     * main(), passed through the configuration and never through struct
     * net_blit_shared, which is defined twice (net.h and blit.c). Always set
     * explicitly: 0 would be standard input. */
    int                      wake_fd;
    /* Window of the adaptive shed (receiver/shed.h): the number of images
     * shown at least one vblank late that arms shedding of the next one.
     * 0 = off (default, the original behaviour). main() refuses N > 0
     * without --wake-on-frame; the blit thread also turns it off if wake_fd
     * is -1 (no eventfd) and reports the value in use (flip_policy). Passed
     * through the configuration, never through struct net_blit_shared
     * (defined twice). */
    int                      shed_window;
    /* The CRT, read by main() BEFORE any thread (monitor.h). `monitor` is
     * never NULL: the built-in 15 kHz band stands in when --monitor is
     * absent; blit_thread_start refuses NULL. The table is written before
     * the threads are created and only read afterwards. `monitor_from_file`
     * is 1 under --monitor: a mode that switchres refuses to register is
     * then a rejected configuration (exit code 3) and not a failure (exit
     * code 1, restarted). Passed through the configuration, never through
     * struct net_blit_shared (defined twice). */
    const struct monitor    *monitor;
    int                      monitor_from_file;
    struct net_blit_shared  *shared;
    struct jitter_ring      *jitter;
};

int  blit_thread_start(struct blit_thread_config *cfg, pthread_t *out_tid);
void blit_thread_join(pthread_t tid);

/* Non-zero if the blit thread stopped the daemon on a display failure.
 * main() turns it into its exit code, so that systemd sees a failure
 * (Restart=on-failure) and not a clean stop. */
int  blit_fatal(void);

/* Non-zero if the blit thread rejected the configuration (impossible
 * mode, modeline outside the envelope). main() exits with code 3, which
 * RestartPreventExitStatus=3 excludes from restarts. Checked before
 * blit_fatal(). */
int  blit_config_rejected(void);

/* The 480p state, a value of `enum regime480p_state` (regime.h):
 * R480P_ENABLED when the guard judged and accepted the constant frozen at
 * start-up (site boot_480p); R480P_NOT_JUDGED (zero) as long as the blit
 * thread has judged nothing; otherwise the reason 480p is disabled. Read by
 * the network thread (regime_choose) to serve as 480i a 480p that a
 * non-enabled state does not allow; written by the blit thread at start-up.
 * Accessor outside struct net_blit_shared, which is defined twice. */
int  blit_regime_480p_state(void);

/* The return-to-idle flag: the blit thread raises it when it has just put
 * the CRT back to 240p because the receiver is idle (--idle-after seconds
 * of silence in 480p); the network thread reads it AND clears it in one
 * atomic step, and then knows that images coming back without a new
 * SWITCHRES arrive in a scan that is no longer the one the emitter
 * believes. Returns 1 if it was raised, 0 otherwise. Accessor outside
 * struct net_blit_shared, which is defined twice. */
int  blit_idle_return_take(void);

#endif /* RECEIVER_BLIT_H */
