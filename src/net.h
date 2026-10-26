/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECEIVER_NET_H
#define RECEIVER_NET_H

/*
 * net.h -- UDP receiver thread public surface.
 *
 * The net thread owns the UDP/32100 socket, runs SCHED_FIFO 80 with optional
 * CPU pinning and SO_BUSY_POLL, accumulates BLIT_FIELD_VSYNC payloads into
 * frame_slot buffers via proto_reassemble_push, and posts completed slots
 * to the blit thread through an SPSC ring (header-only, see spsc.h).
 *
 * The companion blit thread owns DRM page-flip + ACK pacing; it signals
 * the net thread to send a 13-byte status_pkt_t back to the last-seen emitter
 * via the _Atomic ack_* fields in net_blit_shared.
 *
 * Heartbeat watchdog: if no CMD_BLIT_FIELD_VSYNC is observed within
 * heartbeat_timeout_ms (default 2000), FLAG_VRAM_READY is masked out of any
 * pending ACK and a single-line structured log is emitted; recovery is
 * automatic when packets resume.
 *
 * Trust boundary: every byte from recvmmsg() is treated as untrusted input.
 * Length checks happen in the proto.c parsers before any memcpy. ACKs go
 * ONLY to last_emitter_addr, set on the most recent BLIT/INIT/SWITCHRES --
 * no broadcast amplification.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>
#ifdef _WIN32
#  include <winsock2.h>   /* sockaddr_in for the Windows-hosted test builds */
#else
#  include <netinet/in.h>
#endif

#include "spsc.h"

/* Audio output. Opaque here: net.c only needs to hand it data and
 * configure it, never to know its insides. */
struct audio_out;

/* ---------------------------------------------------------------------------
 * MAX_FRAME_BYTES -- maximum slot buffer capacity, in bytes.
 *
 * Sizing: covers the larger of the two registered super-res regimes at
 * XRGB8888 with height = 480 rows (matches the 480i FB; 240p super-res
 * scans only the first 240 rows of the same slot). The width, 1280, was
 * chosen by a comparative measurement of super-res widths on the bench.
 *
 * Heap footprint: 8 slots x MAX_FRAME_BYTES ~ 18.75 MiB on the receiver
 * (8+ GiB RAM -- negligible). A static upper bound was preferred over
 * re-allocating the slots on every regime toggle.
 *
 * Exposed in the header so every entry point that produces an
 * `expected_frame_bytes` value (CLI parser in main.c, CMD_SWITCHRES handler
 * in net.c, BLIT reassembly path) can clamp against the same upper bound.
 * No path may ever store a value in `expected_frame_bytes` exceeding this
 * constant: reassembly writes into a buffer of exactly this size.
 * ------------------------------------------------------------------------- */
#define MAX_FRAME_BYTES (1280U * 480U * 4U)

/* ---------------------------------------------------------------------------
 * Frame slot (owned by net thread, consumed by blit thread).
 *
 * Layout matches CMD_BLIT_FIELD_VSYNC reassembly state:
 *   - frame_id / field / last_vsync echoed back in the ACK after page flip;
 *   - expected_bytes derived from latest CMD_SWITCHRES (h_active*v_active*3)
 *     or a 320*240*3 default at cold start;
 *   - received_bytes incremented by proto_reassemble_push; complete when
 *     received_bytes >= expected_bytes;
 *   - buf is heap-allocated by the daemon (main.c) at size MAX_FRAME_BYTES
 *     and shared between net (writer) and blit (reader) -- SPSC ring of
 *     pointers serializes ownership transfer.
 * ------------------------------------------------------------------------- */
struct frame_slot {
    uint32_t frame_id;
    uint32_t expected_bytes;
    uint32_t received_bytes;
    /* --- LZ4 -----------------------------------------------------------
     * compressed : the bytes being reassembled are an LZ4 block, not pixels.
     *              They go into the net thread's staging buffer, NOT into
     *              buf -- in 480i, buf already holds the even lines of field
     *              0 when field 1 arrives, and overwriting them would destroy
     *              them.
     * clear_bytes: the expected UNCOMPRESSED size. While a compressed frame
     *              is reassembled, expected_bytes holds the block size;
     *              this one keeps the true size of the image. */
    uint8_t  compressed;
    /* Time slot_decompress took for this image, in microseconds rounded up,
     * sum of both fields in 480i, saturated at 65 535; 0 for raw frames.
     * Reset to 0 by acquire_slot (net.c). Fits in the padding that followed
     * `compressed` (bytes 13 to 15): the size (48) and the offsets of buf
     * (24) and ready_ns (40) do not move. SAME POSITION in the copy of this
     * struct in blit.c, guarded by _Static_assert (offset 14) in blit.c and
     * in net.c. */
    uint16_t decomp_us;
    uint32_t clear_bytes;
    uint16_t last_vsync;
    uint8_t  field;
    uint8_t  is_field;       /* slot primed by 0x07 in the 480i regime --
                              * continuation guard must route pure-payload
                              * datagrams through proto_reassemble_push_field */
    uint8_t *buf;            /* heap-allocated, size = max_frame_bytes */

    /* Latency instrumentation. Written by the net thread, read by the blit
     * thread after spsc_try_consume -- same ownership window as buf, no new
     * risk. */
    uint64_t first_byte_ns;   /* CLOCK_MONOTONIC when the slot is acquired */
    uint64_t ready_ns;        /* CLOCK_MONOTONIC just before spsc_try_produce */
};

/* ---------------------------------------------------------------------------
 * sr_mode_request (mode switching).
 *
 * Non-atomic struct populated by net.c on every accepted CMD_SWITCHRES,
 * read by blit.c after observing a `switchres_seq` bump. Fields round-trip
 * the 26-byte cmd_switchres_t wire payload (proto.h) plus a derived
 * refresh_hz and a CLOCK_MONOTONIC capture timestamp.
 *
 * Cross-thread safety: net writes this struct in full BEFORE atomic_store
 * (release) on shared->switchres_seq; blit reads atomic_load (acquire) on
 * the seq THEN reads this struct. The release-acquire pair makes the
 * struct contents visible to blit without a separate atomic per field.
 * ------------------------------------------------------------------------- */
struct sr_mode_request {
    /* Round-tripped from cmd_switchres_t (proto.h, 26 bytes on the wire). */
    uint16_t h_active;
    uint16_t h_begin;
    uint16_t h_end;
    uint16_t h_total;
    uint16_t v_active;
    uint16_t v_begin;
    uint16_t v_end;
    uint16_t v_total;
    double   pclock;        /* IEEE 754 -- same as cmd_switchres_t.pclock */
    uint8_t  interlace;
    /* The regime decided by the net thread, ONCE, by regime_choose
     * (regime.h): the regime SERVED, which differs from the wire shape
     * when a 480p request is folded into 480i. A value of enum
     * daemon_regime. The blit thread reads it here and never decides it
     * again. Takes the first byte of the padding: the size and the offsets
     * after it do not move. This struct is defined TWICE (here and in
     * blit.c): both copies change together. */
    uint8_t  regime;
    uint8_t  _pad[6];       /* explicit padding so layout is stable across compilers */

    /* Net-thread-derived: refresh = pclock / (h_total * v_total).
     * Pre-computed so blit doesn't repeat the divide on the hot path. */
    double   refresh_hz;

    /* CLOCK_MONOTONIC nanoseconds at parse time. Used by blit's rate-limit
     * to decide if this request arrived inside the 100 ms floor. Read via
     * plain field access -- stable as long as net writes the struct BEFORE
     * bumping switchres_seq. */
    uint64_t monotonic_ns_received;
};

_Static_assert(sizeof(struct sr_mode_request) >= 40,
               "sr_mode_request must hold all CMD_SWITCHRES fields + derived");

/* struct sr_mode_request is defined TWICE: here (canonical) and in blit.c,
 * whose copy is always active (blit.c does not include net.h). Both
 * definitions are checked against the SAME numbers: a divergence breaks the
 * build instead of breaking the bench. Changing one requires changing the
 * other, which is exactly the reminder wanted (same rule as frame_slot). */
_Static_assert(sizeof(struct sr_mode_request) == 48,
               "sr_mode_request changed size -- net.h and blit.c must be "
               "changed TOGETHER");
_Static_assert(offsetof(struct sr_mode_request, regime) == 25,
               "the offset of sr_mode_request.regime moved -- net.h and blit.c "
               "must be changed TOGETHER");

/* ---------------------------------------------------------------------------
 * net_blit_shared -- the only contention surface between the two threads.
 *
 * spsc_ring         -- pointer enqueue/dequeue, header-only inline.
 * slots             -- net-side allocation pool (capacity slots of frame_slot*).
 *                      Net rotates current_slot_idx round-robin; blit reads via
 *                      spsc_try_consume(). Once blit page-flips, it writes the
 *                      ack_* atomics and sets ack_ready = 1.
 * expected_frame_bytes -- updated by net on every CMD_SWITCHRES (atomic store);
 *                      read by net on every BLIT to size the next slot.
 * ack_*             -- written by blit thread after page-flip event; net reads
 *                      after every recvmmsg batch and sendmmsg's a 13-byte
 *                      status_pkt_t to last_emitter_addr. Cleared (ack_ready=0)
 *                      after each send.
 * running           -- process-wide stop flag; both threads spin on it.
 * ------------------------------------------------------------------------- */
struct net_blit_shared {
    /* SPSC pointer ring (header-only, see spsc.h). Net is producer; blit is consumer. */
    struct spsc_ring   *spsc_ring;
    struct frame_slot **slots;              /* pool of slot pointers, length = slot_capacity */
    size_t              slot_capacity;      /* power of 2 (matches SPSC_CAPACITY) */

    /* Geometry from latest CMD_SWITCHRES (or the daemon's cold-start 320x240x3) */
    _Atomic uint32_t    expected_frame_bytes;

    /* Active dimensions from latest CMD_SWITCHRES, in pixels. Both 0 until
     * the first valid SWITCHRES arrives -- blit thread treats 0 as "fall back
     * to cfg.width/cfg.height" (cold-start path). Updated atomically alongside
     * expected_frame_bytes; readers may observe one updated and the other not
     * for a brief window -- that's acceptable because the blit hot path reads
     * both and either dim being zero triggers the fallback for that read. */
    _Atomic uint32_t    h_active_announced;
    _Atomic uint32_t    v_active_announced;

    /* ACK signal from blit thread.
     * Layout matches status_pkt_t (proto.h, 13 bytes, packed). */
    _Atomic uint8_t     ack_ready;
    _Atomic uint32_t    ack_frame_echo;
    _Atomic uint16_t    ack_vcount_echo;
    _Atomic uint32_t    ack_frame;
    _Atomic uint16_t    ack_vcount;
    _Atomic uint8_t     ack_flags;

    /* Lifecycle -- both threads spin on this. */
    _Atomic int         running;

    /* --- Mode switching ---
     *
     * Net->blit signaling: net populates `request` then bumps
     * `switchres_seq` (release). Blit's page-flip loop checks the seq
     * each iteration (acquire); on delta, blit reads `request` and
     * enters the runtime modeset path.
     *
     * synced: FLAG_VRAM_SYNCED state, owned by blit.
     *  - 0 at construct (calloc-zeroed by main.c)
     *  - 1 after blit_init startup modeset succeeds
     *  - 0 entering runtime modeset path (clears FLAG_VRAM_SYNCED in next ACK)
     *  - 0 also during the wait imposed by the band rule: several seconds,
     *    up to 60 s under the cap, until the pending request is consumed
     *  - 1 after successful drmModeSetCrtc OR on envelope rejection (sticky)
     *  - 1 after a FAILED band crossing too: the screen stays black and
     *    frames are acked without display until the next request
     *
     * last_modeset_ns: CLOCK_MONOTONIC ns of the most recent successful
     * drmModeSetCrtc. Input of the modeset rate-limit.
     *
     * request: stable as long as net writes BEFORE seq-bump. Reading is
     * safe in blit only after observing a seq delta via atomic_load.
     */
    _Atomic uint64_t          switchres_seq;
    struct sr_mode_request    request;
    _Atomic uint8_t           synced;
    _Atomic uint64_t          last_modeset_ns;

    /* UDP socket fd published by the net thread after setup_socket()
     * succeeds, so the SIGTERM signal handler can call
     * shutdown(fd, SHUT_RDWR) to unblock recvmmsg(). recvmmsg(MSG_WAITFORONE)
     * does not honour its userspace timeout when zero packets ever arrive,
     * and SA_RESTART re-enters the syscall after signal delivery -- without
     * shutdown(), the net thread parks forever in __skb_wait_for_more_packets.
     * Initialised to -1 by main(); handler skips shutdown when still -1. */
    _Atomic int         net_sock_fd;

    /* --- Super-resolution ---
     *
     * legacy_modeset: 0 (default) -> daemon resolves CMD_SWITCHRES to one
     * of two pre-registered modelines (super-res mode). 1 (--legacy-modeset
     * CLI flag set) -> daemon keeps the older per-frame modeset path
     * verbatim. Set ONCE at main() startup; readers in net.c use atomic_load
     * for the read-only safety.
     *
     * current_regime: 0 = REGIME_240P_SUPER_RES, 1 = REGIME_480I. Owned by
     * blit thread (sole writer); net.c reads via atomic_load on the
     * CMD_SWITCHRES regime-resolver fast path. PAL regimes (2, 3) reserved.
     * See receiver/modelines.h enum daemon_regime.
     */
    _Atomic int               legacy_modeset;
    _Atomic uint8_t           current_regime;

    /* --- Audio output ---
     *
     * Created by main() before the threads start, destroyed after they
     * stop. NULL when --no-audio is passed or creation failed: net.c tests
     * this pointer and stays silent, the daemon carries on. The sound
     * device is NOT opened at creation -- see pcm.h. */
    struct audio_out         *audio;
};

/* ---------------------------------------------------------------------------
 * Thread configuration. Constructed by main(); passed by pointer to
 * net_thread_start which owns a private copy. cfg->shared must outlive the
 * thread (typically a static or main-stack-rooted struct).
 * ------------------------------------------------------------------------- */
struct net_thread_config {
    uint16_t                 listen_port;          /* default 32100 */
    int                      busy_poll_us;         /* default 50; <=0 disables SO_BUSY_POLL */
    int                      cpu_affinity;         /* -1 to skip pthread_setaffinity_np */
    int                      sched_fifo_priority;  /* default 80; <=0 skips SCHED_FIFO */
    int                      heartbeat_timeout_ms; /* default 2000 */
    /* eventfd that wakes the blit thread: one 8-byte write after each image
     * published in the ring, never per datagram. -1 = off (default). The
     * same descriptor as blit_thread_config.wake_fd; kept out of struct
     * net_blit_shared, which is defined twice. */
    int                      wake_fd;
    struct net_blit_shared  *shared;
};

/*
 * net_thread_start -- spawn the net thread. Returns 0 on success, -1 on error
 * (cfg/shared NULL, socket bind failure, pthread_create failure). On success,
 * *out_tid is the pthread handle.
 *
 * The function does NOT set shared->running. The caller sets it to 1
 * before starting the threads; setting it again here would erase a stop
 * already requested.
 */
int  net_thread_start(struct net_thread_config *cfg, pthread_t *out_tid);

/* Non-zero if the net thread stopped by itself on a failure (allocation,
 * recvmmsg). main() turns it into its exit code. */
int  net_fatal(void);

/*
 * net_thread_request_stop -- signal the thread to exit. The thread observes
 * shared->running == 0 within one recvmmsg timeout (driven by MSG_WAITFORONE
 * + at least one packet, or by heartbeat watchdog timer). Caller pthread_join's
 * the returned tid afterward.
 */
void net_thread_request_stop(struct net_blit_shared *shared);

#endif /* RECEIVER_NET_H */
