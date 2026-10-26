/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * net.c -- UDP receiver thread implementation.
 *
 * Datapath: recvmmsg(32 packets) -> dispatch by opcode -> proto.c parsers ->
 * proto_reassemble_push into the current frame_slot -> spsc_try_produce
 * when the frame completes. The companion blit thread consumes the
 * SPSC ring, page-flips, then writes ack_* atomics + ack_ready=1; this thread
 * sendmmsg's the 13-byte status_pkt_t back to last_emitter_addr after each
 * recvmmsg batch.
 *
 * Realtime tuning:
 *   - SCHED_FIFO priority 80
 *   - CPU affinity pinned via pthread_setaffinity_np (caller-supplied)
 *   - SO_BUSY_POLL 50 us
 *   - SO_RCVBUF >= 4MB to absorb burst arrivals during page-flip pauses
 *
 * Heartbeat: if (now_ns - last_packet_ns) > heartbeat_timeout_ms,
 * log once, mask FLAG_VRAM_READY out of subsequent ACKs. Auto-recovers when
 * the next packet arrives. recvmmsg is given a 100ms timeout via struct
 * timespec so the watchdog can fire even when the emitter goes silent --
 * MSG_WAITFORONE alone with NULL timeout would block indefinitely.
 *
 * Trust boundary: every byte from the wire passes through proto.c parsers,
 * which bound-check len before any memcpy. ACKs are sent ONLY to
 * last_emitter_addr, set on the most recent packet -- no broadcast
 * amplification. A CMD_INIT whose fields are out of bounds
 * (compression > 1, rate code > 3) is not a command any more:
 * it is an orphan, counted and dropped, and locks nothing -- the former
 * compression_blocked lockout is gone. Out of a frame, only the exact
 * shape of a command is read; anything else is an orphan, counted in a
 * once-per-second rx_garbage_1s line, never one line per datagram.
 */

#define _GNU_SOURCE
#include "net.h"
#include "lz4dec.h"
#include "proto.h"
#include "pcm.h"       /* audio output */
#include "modelines.h"  /* enum daemon_regime (REGIME_240P_SUPER_RES/REGIME_480I) */
#include "regime.h"     /* regime_choose, regime480p_reason, regime_name */
#include "blit.h"       /* blit_regime_480p_state, read by regime_choose to fold a 480p that is not enabled */

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* True when the net thread stopped by itself on a failure. Kept out of
 * struct net_blit_shared on purpose, like the blit thread's flag: that
 * struct is defined twice. */
static atomic_int s_net_fatal;

int net_fatal(void)
{
    return atomic_load(&s_net_fatal);
}

/* Stop on failure: the flag first, then running = 0, so the blit thread
 * exits and main() does not wait for it forever. */
static void net_stop_fatal(struct net_blit_shared *shared)
{
    atomic_store(&s_net_fatal, 1);
    atomic_store(&shared->running, 0);
}

#define RECVMMSG_BATCH        32
#define MAX_DGRAM             65535
#define RECVMMSG_TIMEOUT_NS   100000000ULL  /* 100 ms -- bounds heartbeat detection latency */

/* ---------------------------------------------------------------------------
 * Internal per-thread state. Heap-allocated by net_thread_start, freed by
 * the thread itself on exit (pthread_self owns it).
 * ------------------------------------------------------------------------- */
struct net_internal {
    struct net_thread_config cfg;
    int                      sock;
    struct sockaddr_in       last_emitter;
    socklen_t                last_emitter_len;
    uint64_t                 last_packet_ns;
    /* --- LZ4 -----------------------------------------------------------
     * compression : 0 or 1, as announced by CMD_INIT. The wire only carries
     *               these two values (groovymister.cpp); a CMD_INIT that
     *               carries another one is not a command any more: it is
     *               counted as an orphan and changes nothing.
     * lz4_in      : the staging buffer where the compressed block is
     *               reassembled.
     * lz4_out     : the linear output, used only in 480i, where the lines
     *               of the field must then be interleaved into the slot.
     * delta_warned : the refusal of delta mode is logged only once. */
    uint8_t                  compression;
    /* --- Padded session mode --------------------------------------------
     * padded : the session mode, set by the LENGTH of CMD_INIT
     *          (8 = padded, 4 or 5 = classic) via proto_session_on_init
     *          -- the rule lives in proto.c, never recomputed here.
     *          calloc() leaves 0, so a daemon that starts is in a classic
     *          session.
     *          Lives HERE, next to `compression`, and NEVER in
     *          struct net_blit_shared nor struct frame_slot: both are
     *          duplicated WITHOUT A GUARD between net.h and blit.c
     *          -- a new field there would be a mine, exactly the defect
     *          that once killed the daemon (see the comment near the
     *          _Static_assert further down in this file). */
    uint8_t                  padded;
    /* --- Guard against orphans and absurd commands ----------------------
     * garbage_orphans_win  : datagrams outside a frame without the exact
     *                        shape of a command (proto_out_of_frame_accepts),
     *                        dropped; of which
     * garbage_unknown_win  : those whose first byte is outside 0x01..0x08
     *                        (the former `unknown_opcode`);
     * garbage_episodes_win : episodes. An episode starts at the first
     *                        orphan that follows an accepted datagram and
     *                        ends at the next accepted datagram: a lost
     *                        header gives a whole frame of orphans, and ONE
     *                        episode;
     * garbage_absurd_win   : commands of exact shape but absurd
     *                        (proto_switchres_plausible, proto_audio_size_ok);
     * in_episode           : 1 between an orphan and the next accepted one;
     * implausible_logged   : one switchres_implausible line per window at
     *                        most -- never one line per datagram;
     * garbage_log_ns       : last rx_garbage_1s log.
     * Counters reset at each log (garbage_report). calloc() leaves 0
     * everywhere: a daemon that starts has counted nothing and is in no
     * episode.
     *          They live HERE, next to `padded`, and NEVER in
     *          struct net_blit_shared nor struct frame_slot: both are
     *          duplicated WITHOUT A GUARD between net.h and blit.c
     *          -- a new field there would be a mine, exactly the defect
     *          that once killed the daemon (see the comment near the
     *          _Static_assert further down in this file). */
    uint32_t                 garbage_orphans_win;
    uint32_t                 garbage_unknown_win;
    uint32_t                 garbage_episodes_win;
    uint32_t                 garbage_absurd_win;
    uint8_t                  in_episode;
    uint8_t                  implausible_logged;
    uint64_t                 garbage_log_ns;
    /* --- Blindness -------------------------------------------------------
     * sight, garbage_guard : the two pure decisions of proto.c (proto_sight,
     *                    proto_guard); thresholds and rationale in proto.h;
     * is_blind         : 1 = the daemon has lost the picture. Its statuses
     *                    carry frame_echo = 0 (send_pending_ack,
     *                    send_status_now), at least one every
     *                    PROTO_BLIND_STATUS_MS; frames no longer go to the
     *                    flip thread, sound is muted
     *                    (audio_out_configure(audio, 0, 0));
     * has_picture      : 1 as soon as a frame of the session has gone to the
     *                    flip thread. 0 at startup, on CMD_CLOSE and on
     *                    entering blindness: without a picture, no status
     *                    carries a non-zero echo, frame_dup included;
     * switchres_since_blind : an accepted CMD_SWITCHRES arrived during
     *                    blindness; the next complete frame lifts it.
     *                    Never CMD_INIT alone: its status reply would make
     *                    the emitter cancel its deferred CMD_SWITCHRES;
     * raw_to_confirm   : a RAW frame has just gone out complete; it is only
     *                    good at the next accepted datagram with no orphan
     *                    in between (an expected size that is too small
     *                    ends early and spills the rest as orphans);
     * blind_entry_*    : the entry decided during a datagram; it happens
     *                    AFTER it (net_thread_main), never in the middle of
     *                    a slot manipulation;
     * startup_size     : expected_frame_bytes read when the thread starts,
     *                    the size that blindness restores (the cold-start
     *                    path of net.h).
     * calloc() leaves 0 everywhere: a daemon that starts can see, has no
     * picture yet, and has decided nothing.
     *          They live HERE, next to `padded`, and NEVER in
     *          struct net_blit_shared nor struct frame_slot: both are
     *          duplicated WITHOUT A GUARD between net.h and blit.c
     *          -- a new field there would be a mine, exactly the defect
     *          that once killed the daemon (see the comment near the
     *          _Static_assert further down in this file). */
    struct proto_sight       sight;
    struct proto_guard       garbage_guard;
    int                      is_blind;
    int                      has_picture;
    int                      switchres_since_blind;
    int                      raw_to_confirm;
    const char              *blind_cause;       /* cause of the current blindness */
    const char              *blind_entry_cause;      /* non-NULL: entry decided */
    const char              *blind_entry_what;       /* "frames", "absurd", "episodes" */
    uint32_t                 blind_entry_n;
    uint64_t                 blind_entry_span_ms;
    uint32_t                 startup_size;
    uint64_t                 blind_since_ns;
    uint64_t                 blind_status_ns;   /* last blind status sent */
    uint64_t                 blind_blocks_muted;
    uint64_t                 blind_frames_dropped;
    uint8_t                 *lz4_in;
    uint8_t                 *lz4_out;
    int                      delta_warned;
    int                      heartbeat_lost;        /* 1 = currently in lost state */
    struct frame_slot       *current_slot;
    size_t                   current_slot_idx;
    uint8_t                  slot_fields_mask;  /* completion mask of the
                              * DISTINCT fields of an interlaced frame. */
    uint8_t                  gm_interlaced_0x06_logged; /* a single log line
                              * per entry into the interlaced 0x06 path,
                              * reset on every CMD_SWITCHRES. */
    uint8_t                  regime_fallback_logged; /* a 480p requested from
                              * a receiver whose 480p is not enabled is SERVED
                              * in 480i; a single regime_fallback line per entry
                              * into the fallback, reset by the first
                              * CMD_SWITCHRES that is not a fallback. Net thread
                              * only. */
    uint8_t                  last_published_regime; /* the regime of the LAST
                              * request published to blit.c (at startup: the
                              * startup regime, 240p). This is not
                              * current_regime, which is the hardware state: a
                              * pending A -> B switch (delayed by the band rule)
                              * has already published B; if the emitter comes
                              * back to A before the deadline, want == have but
                              * the last published is B, and "stay in A" must be
                              * published to cancel it (regime_publish_needed).
                              * Net thread only. */
    uint8_t                  announced_serve;   /* the regime SERVED by the
                              * last accepted announcement (CMD_SWITCHRES,
                              * default path): 240p at startup. This is what
                              * the emitter believes it sees; when the blit
                              * thread has returned the tube to 240p while idle
                              * (blit_idle_return_take) and images come back
                              * without a new announcement, the net thread
                              * republishes this regime (regime_resume).
                              * Distinct from announced_regime, which is the
                              * WIRE SHAPE. Net thread only. */
    uint8_t                  announced_regime;  /* regime per the LAST
                              * accepted CMD_SWITCHRES (wire semantics). Distinct
                              * from shared->current_regime, which is HARDWARE
                              * state owned by blit.c and updated only after the
                              * async toggle completes -- 0x07 dispatch must follow
                              * the announcement, not the modeset (a race
                              * caught by test_field_split_reassembly). Net-thread
                              * local, no atomic needed. calloc zero-init ==
                              * REGIME_240P_SUPER_RES (boot default). */

    /* --- Audio ---
     *
     * The audio reassembly state is DELIBERATELY separate from the video
     * one. Both cannot be active at the same time, and this is guaranteed
     * by construction on both sides:
     *
     *   - on the emitter side, gm_send_blit sends the header AND all its
     *     chunks before returning, and push_audio as well as push_video run
     *     on the runloop thread. A video frame therefore leaves in one
     *     piece, and an audio block can only slip in BETWEEN two frames;
     *   - on the receiver side, an audio header abandons a stuck video frame
     *     and a video header abandons a stuck audio block. Each abandon is
     *     counted.
     *
     * What makes this necessary is not normal interleaving, it is the video
     * frame left incomplete after a lost datagram: without the escape, the
     * continuation guard would swallow the audio header as pixels. Same
     * defect, same remedy as the earlier regression on frame headers.
     */
    struct frame_reassembly audio_rx;      /* buf points to audio_buf */
    uint8_t  audio_buf[AUDIO_MAX_BLOCK_BYTES];
    int      audio_active;                 /* a block is being reassembled */
    uint8_t  audio_rate_code;              /* announced by CMD_INIT; 0 = no audio */
    uint8_t  audio_channels;               /* announced by CMD_INIT */
    uint64_t audio_log_ns;                 /* last audio_1s log */

    /* Counters. Accumulated since startup for the totals, and reset at each
     * log for the per-second values. */
    uint64_t audio_blocks_total,   audio_bytes_total;
    uint64_t audio_blocks_window,  audio_bytes_window;
    uint64_t audio_dropped_total,  audio_dropped_bytes_total;
    uint64_t audio_dropped_window;
    uint64_t audio_blocks_reported;        /* total at the last log emitted */

    /* --- Opening parity of the 480i pairs of the 0x06 path.
     *
     * A pair opens when a 480i slot is acquired AFRESH by this path
     * (acquire_slot resets slot_fields_mask to zero); its opening parity is
     * that of the frame number of this FIRST half-frame. It varies from one
     * 480i entry to the next for the RetroArch emitter (the current number
     * at the moment of the regime switch sets it), and is always 0 for
     * GroovyMAME, whose counter starts at zero (seven entries read from the
     * log).
     *
     * Counters LOCAL to the net thread: nothing shared, no field added to
     * the shared net<->blit struct (see net.h) nor to struct frame_slot.
     * That is why this count goes out on its own log line and not in
     * latency_1s, which belongs to the flip thread. */
    uint64_t field_open_p0_total, field_open_p1_total;
    uint32_t field_open_p0_win,   field_open_p1_win;
    uint64_t field_pairs_total;
    uint32_t field_pairs_win;
    uint64_t field_log_ns;
};

/* ---------------------------------------------------------------------------
 * now_ns -- CLOCK_MONOTONIC nanoseconds. Used for both heartbeat watchdog and
 * structured log timestamps. Same clock as jitter.c.
 * ------------------------------------------------------------------------- */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---------------------------------------------------------------------------
 * setup_socket -- bind UDP/INADDR_ANY:port, set SO_RCVBUF >= 4MB and (when
 * busy_us > 0) SO_BUSY_POLL. SO_BUSY_POLL failure is logged but non-fatal:
 * the kernel CAP_NET_ADMIN check is relaxed on 4.7+, and a defensive
 * fallback to plain blocking recvmmsg is acceptable.
 * Returns the bound socket fd, or -1 on socket()/bind() failure.
 * ------------------------------------------------------------------------- */
static int setup_socket(uint16_t port, int busy_us)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        fprintf(stderr, "{\"event\":\"socket_failed\",\"errno\":%d}\n", errno);
        return -1;
    }
    int rcvbuf = 4 * 1024 * 1024;
    if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf) < 0) {
        fprintf(stderr, "{\"event\":\"so_rcvbuf_failed\",\"errno\":%d}\n", errno);
        /* non-fatal: kernel default is usually still adequate */
    }
    /* SO_RCVTIMEO backstop (found while measuring the field-pair counters --
     * a daemon left genuinely idle for >1s after a burst never advanced the
     * loop). recvmmsg(2)
     * BUGS: "the timeout is checked only after the receipt of each datagram, so
     * that if up to vlen-1 datagrams are received before the timeout expires,
     * but then no further datagrams are received, the call will block forever
     * (if the socket is blocking), since the last recvmsg() call ... blocks
     * indefinitely." Confirmed via /proc/<pid>/task/<tid>/wchan showing the net
     * thread parked in __skb_wait_for_more_packets long past
     * RECVMMSG_TIMEOUT_NS. Without this, field_report/audio_report/the
     * heartbeat watchdog never run again once the emitter goes silent
     * right after a burst -- the exact condition the heartbeat exists to
     * detect. Socket-level timeout is the documented workaround: it bounds the
     * kernel's own blocking recvmsg() call regardless of recvmmsg()'s timeout
     * argument. */
    struct timeval rcvtimeo = {
        .tv_sec  = (time_t)(RECVMMSG_TIMEOUT_NS / 1000000000ULL),
        .tv_usec = (suseconds_t)((RECVMMSG_TIMEOUT_NS % 1000000000ULL) / 1000ULL)
    };
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof rcvtimeo) < 0) {
        fprintf(stderr, "{\"event\":\"so_rcvtimeo_failed\",\"errno\":%d}\n", errno);
        /* non-fatal: reverts to the recvmmsg(2) BUGS behavior above --
         * everything else keeps working, only the idle-loop cadence guarantee
         * is lost */
    }
    int reuse = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) < 0) {
        fprintf(stderr, "{\"event\":\"so_reuseaddr_failed\",\"errno\":%d}\n", errno);
        /* non-fatal: only matters on quick daemon restarts during dev */
    }
    if (busy_us > 0) {
        if (setsockopt(s, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof busy_us) < 0) {
            fprintf(stderr, "{\"event\":\"so_busy_poll_failed\",\"errno\":%d}\n", errno);
            /* non-fatal: thread still functions, just with higher wakeup latency */
        }
    }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "{\"event\":\"bind_failed\",\"port\":%u,\"errno\":%d}\n",
                (unsigned)port, errno);
        close(s);
        return -1;
    }
    return s;
}

/* ---------------------------------------------------------------------------
 * apply_realtime -- set CPU affinity (if cpu >= 0) then SCHED_FIFO priority
 * (if prio > 0). Affinity FIRST so the kernel does not migrate the thread
 * after the priority promotion. Failures are logged but non-fatal -- daemon
 * still runs, just without RT guarantees.
 * ------------------------------------------------------------------------- */
static int apply_realtime(int cpu, int prio)
{
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET((size_t)cpu, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0) {
            fprintf(stderr, "{\"event\":\"set_affinity_failed\",\"cpu\":%d,\"errno\":%d}\n",
                    cpu, errno);
        }
    }
    if (prio > 0) {
        struct sched_param sp = { .sched_priority = prio };
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
            fprintf(stderr, "{\"event\":\"sched_fifo_failed\",\"prio\":%d,\"errno\":%d}\n",
                    prio, errno);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * build_ack -- atomic snapshot of shared->ack_* into a packed status_pkt_t.
 * Layout asserted by proto.h _Static_assert(sizeof(status_pkt_t) == 13).
 * Each field load is independently atomic; consistency across fields is the
 * blit thread's responsibility (it stores all five then sets ack_ready last).
 * ------------------------------------------------------------------------- */
static void build_ack(struct net_blit_shared *sh, status_pkt_t *out)
{
    out->frame_echo  = atomic_load(&sh->ack_frame_echo);
    out->vcount_echo = atomic_load(&sh->ack_vcount_echo);
    out->frame       = atomic_load(&sh->ack_frame);
    out->vcount      = atomic_load(&sh->ack_vcount);

    /* ack_flags carries FLAG_VRAM_READY/END_FRAME set by the blit
     * thread's page_flip_handler. OR in FLAG_VRAM_SYNCED from the
     * blit-owned `synced` atomic. The heartbeat-loss path of
     * send_pending_ack may still strip FLAG_VRAM_READY (bit
     * 0x01) downstream; FLAG_VRAM_SYNCED (bit 0x04) is independent of
     * heartbeat -- it reflects modeset state, not connection state, and the
     * mask `~FLAG_VRAM_READY` = 0xFE preserves it. */
    uint8_t flags = atomic_load(&sh->ack_flags);
    if (atomic_load(&sh->synced)) {
        flags = (uint8_t)(flags | FLAG_VRAM_SYNCED);
    }
    out->flags = flags;
}

/* ---------------------------------------------------------------------------
 * audio_flag -- FLAG_AUDIO reflects a SESSION STATE, not a reception.
 *
 * The former version set the bit after a byte arrived, within a 500 ms
 * window. Against GroovyMAME this is a complete circular deadlock:
 * drawnogpu.cpp only sends sound if this bit is already set, and we only
 * set it after receiving some. Nobody starts.
 *
 * Do NOT derive it from pcm.c: the ALSA device only opens after the first
 * byte is pushed into the ring, so any definition based on "device ready"
 * recreates the same deadlock one level down.
 * ------------------------------------------------------------------------- */
static uint8_t audio_flag(const struct net_internal *ni)
{
    return (ni->audio_rate_code != 0u && ni->audio_channels != 0u)
         ? (uint8_t)FLAG_AUDIO : 0u;
}

/* ---------------------------------------------------------------------------
 * mask_echo -- blind, or without any picture displayed in the session,
 * the daemon does not pretend to have one: frame_echo = 0 and
 * vcount_echo = 0, the shape of the software client's status without
 * geometry (send_status(c, 0, 0), client/gmclient.c). Called by BOTH send
 * functions, hence on all three paths: frame acknowledgement
 * (send_pending_ack), reply to CMD_INIT / GET_STATUS / GET_VERSION and
 * frame_dup (send_status_now). The reply to the CMD_INIT of a
 * re-announcement depends on it: with the old echo, the emitter would go
 * back to synchronised and cancel its deferred CMD_SWITCHRES
 * (groovy_reannounce_flush_deferred).
 *
 * `frame` (ack_frame, the last frame displayed) is not touched: it must
 * never go backwards. Nor is FLAG_AUDIO (audio_flag, session state).
 * ------------------------------------------------------------------------- */
static void mask_echo(const struct net_internal *ni, status_pkt_t *ack)
{
    if (ni->is_blind || !ni->has_picture) {
        ack->frame_echo  = 0u;
        ack->vcount_echo = 0u;
    }
}

/* ---------------------------------------------------------------------------
 * audio_abandon -- an audio block in progress is lost.
 *
 * Two possible causes: a lost payload datagram (the count never reaches
 * the announced size), or a command header arriving in the middle. An
 * audio gap is a gap -- unlike video, there is no next frame that repairs
 * it. We count, and silence comes by itself from the playback buffer
 * running dry.
 * ------------------------------------------------------------------------- */
static void audio_abandon(struct net_internal *ni, const char *cause)
{
    if (!ni->audio_active) return;
    fprintf(stderr,
            "{\"event\":\"audio_block_dropped\",\"cause\":\"%s\","
            "\"received\":%u,\"expected\":%u}\n",
            cause, ni->audio_rx.received_bytes, ni->audio_rx.expected_bytes);
    ni->audio_dropped_total++;
    ni->audio_dropped_window++;
    ni->audio_dropped_bytes_total += ni->audio_rx.received_bytes;
    ni->audio_rx.received_bytes = 0;
    ni->audio_rx.expected_bytes = 0;
    ni->audio_active            = 0;
}

/* ---------------------------------------------------------------------------
 * audio_block_complete -- a whole block has been reassembled.
 *
 * Counts the block and hands it to the PCM output.
 * ------------------------------------------------------------------------- */
static void audio_block_complete(struct net_internal *ni)
{
    /* The block goes to the audio thread. audio_out_push never blocks and
     * only copies bytes; the net thread keeps its hot path. A full buffer
     * takes less than the requested size, and pcm.c counts the lost excess.
     * Blind, the daemon stays silent. The block is counted, not pushed; the
     * output is muted anyway (enter_blind). */
    if (ni->is_blind)
        ni->blind_blocks_muted++;
    else if (ni->cfg.shared->audio)
        audio_out_push(ni->cfg.shared->audio,
                       ni->audio_buf, ni->audio_rx.received_bytes);

    ni->audio_blocks_total++;
    ni->audio_blocks_window++;
    ni->audio_bytes_total  += ni->audio_rx.received_bytes;
    ni->audio_bytes_window += ni->audio_rx.received_bytes;
    ni->audio_rx.received_bytes = 0;
    ni->audio_rx.expected_bytes = 0;
    ni->audio_active            = 0;
}

/* ---------------------------------------------------------------------------
 * audio_report -- logs once per second, if and only if sound has flowed.
 * A bench without audio must not see one more line.
 * ------------------------------------------------------------------------- */
static void audio_report(struct net_internal *ni)
{
    uint64_t t = now_ns();
    if (ni->audio_log_ns == 0) { ni->audio_log_ns = t; return; }
    if (t - ni->audio_log_ns < 1000000000ULL) return;
    ni->audio_log_ns = t;
    if (ni->audio_blocks_window == 0 && ni->audio_dropped_window == 0) {
        /* Nothing during this second. One last log is still due if blocks
         * arrived since the previous one: without it, the partial second
         * that follows the emitter stopping is never counted, and the
         * receiver's total looks lower than the emitter's although nothing
         * was lost. Found by comparing both totals: ten blocks apart, that
         * is one hundred and sixty milliseconds of sound never logged. */
        if (ni->audio_blocks_total == ni->audio_blocks_reported) return;
    }
    ni->audio_blocks_reported = ni->audio_blocks_total;

    fprintf(stderr,
            "{\"event\":\"audio_1s\",\"blocks\":%llu,\"bytes\":%llu,"
            "\"dropped\":%llu,\"blocks_total\":%llu,\"bytes_total\":%llu,"
            "\"dropped_total\":%llu,\"rate_code\":%u,\"channels\":%u}\n",
            (unsigned long long)ni->audio_blocks_window,
            (unsigned long long)ni->audio_bytes_window,
            (unsigned long long)ni->audio_dropped_window,
            (unsigned long long)ni->audio_blocks_total,
            (unsigned long long)ni->audio_bytes_total,
            (unsigned long long)ni->audio_dropped_total,
            (unsigned)ni->audio_rate_code,
            (unsigned)ni->audio_channels);
    ni->audio_blocks_window  = 0;
    ni->audio_bytes_window   = 0;
    ni->audio_dropped_window = 0;
}

/* ---------------------------------------------------------------------------
 * field_report -- logs once per second, and ONLY if something interlaced
 * flowed during that second. A 240p bench must not see one more line:
 * per-frame telemetry disturbs the bench, and this line exists to be read
 * LIVE while someone watches the tube.
 * ------------------------------------------------------------------------- */
static void field_report(struct net_internal *ni)
{
    uint64_t t = now_ns();
    if (ni->field_log_ns == 0) { ni->field_log_ns = t; return; }
    if (t - ni->field_log_ns < 1000000000ULL) return;
    ni->field_log_ns = t;
    if (ni->field_open_p0_win == 0u && ni->field_open_p1_win == 0u &&
        ni->field_pairs_win == 0u) {
        return;
    }
    fprintf(stderr,
            "{\"event\":\"field_open_1s\",\"p0\":%u,\"p1\":%u,\"pairs\":%u,"
            "\"p0_total\":%llu,\"p1_total\":%llu,\"pairs_total\":%llu}\n",
            ni->field_open_p0_win,
            ni->field_open_p1_win,
            ni->field_pairs_win,
            (unsigned long long)ni->field_open_p0_total,
            (unsigned long long)ni->field_open_p1_total,
            (unsigned long long)ni->field_pairs_total);
    ni->field_open_p0_win = 0u;
    ni->field_open_p1_win = 0u;
    ni->field_pairs_win   = 0u;
}

/* ---------------------------------------------------------------------------
 * garbage_report -- one line per second at most, and ONLY if a counter is
 * non-zero: a healthy stream writes nothing. Replaces the per-datagram
 * `unknown_opcode` line, which wrote hundreds of lines per second from a
 * SCHED_FIFO 80 thread as soon as the stream was lost.
 * ------------------------------------------------------------------------- */
static void garbage_report(struct net_internal *ni)
{
    uint64_t t = now_ns();
    if (ni->garbage_log_ns == 0) { ni->garbage_log_ns = t; return; }
    if (t - ni->garbage_log_ns < 1000000000ULL) return;
    ni->garbage_log_ns = t;
    ni->implausible_logged = 0u;
    if (ni->garbage_orphans_win == 0u && ni->garbage_episodes_win == 0u &&
        ni->garbage_absurd_win == 0u) {
        return;
    }
    fprintf(stderr,
            "{\"event\":\"rx_garbage_1s\",\"unknown_opcode\":%u,\"orphans\":%u,"
            "\"episodes\":%u,\"absurd\":%u}\n",
            ni->garbage_unknown_win, ni->garbage_orphans_win,
            ni->garbage_episodes_win, ni->garbage_absurd_win);
    ni->garbage_unknown_win  = 0u;
    ni->garbage_orphans_win  = 0u;
    ni->garbage_episodes_win = 0u;
    ni->garbage_absurd_win   = 0u;
}

/* ---------------------------------------------------------------------------
 * send_pending_ack -- if blit thread set ack_ready, send a 13-byte status_pkt
 * back to last_emitter_addr (never broadcast). If the
 * heartbeat is currently lost, mask FLAG_VRAM_READY out of the flags so the
 * emitter knows the receiver is alive but no frame was rendered. Cleared
 * (ack_ready = 0) after sendto returns.
 * ------------------------------------------------------------------------- */
static void send_pending_ack(struct net_internal *ni)
{
    if (ni->last_emitter_len == 0) return;            /* no emitter yet -- cold start */
    if (!atomic_load(&ni->cfg.shared->ack_ready)) return;

    status_pkt_t ack;
    build_ack(ni->cfg.shared, &ack);
    if (ni->heartbeat_lost) {
        ack.flags = (uint8_t)(ack.flags & (uint8_t)~FLAG_VRAM_READY);
    }
    mask_echo(ni, &ack);
    ack.flags = (uint8_t)(ack.flags | audio_flag(ni));
    ssize_t sent = sendto(ni->sock, &ack, sizeof ack, MSG_DONTWAIT,
                          (struct sockaddr *)&ni->last_emitter,
                          ni->last_emitter_len);
    if (sent < 0) {
        fprintf(stderr, "{\"event\":\"ack_send_failed\",\"errno\":%d}\n", errno);
    }
    atomic_store(&ni->cfg.shared->ack_ready, 0);
}

/* ---------------------------------------------------------------------------
 * send_status_now -- force-send a 13-byte status_pkt_t to last_emitter,
 * bypassing the ack_ready gate. Used to ACK control commands (CMD_INIT,
 * CMD_GET_STATUS, CMD_GET_VERSION) that the emitter blocks on with a 1s
 * timeout -- psakhis Groovy_MiSTer's GroovyMAME nogpu driver requires a
 * status reply on these or it aborts the session ("nogpu: Sending CMD_INIT
 * ...failed."). Found on the first hardware-in-the-loop session.
 * ------------------------------------------------------------------------- */
static void send_status_now(struct net_internal *ni)
{
    if (ni->last_emitter_len == 0) return;
    status_pkt_t ack;
    build_ack(ni->cfg.shared, &ack);
    if (ni->heartbeat_lost) {
        ack.flags = (uint8_t)(ack.flags & (uint8_t)~FLAG_VRAM_READY);
    }
    mask_echo(ni, &ack);
    ack.flags = (uint8_t)(ack.flags | audio_flag(ni));
    ssize_t sent = sendto(ni->sock, &ack, sizeof ack, MSG_DONTWAIT,
                          (struct sockaddr *)&ni->last_emitter,
                          ni->last_emitter_len);
    if (sent < 0) {
        fprintf(stderr, "{\"event\":\"status_ack_failed\",\"errno\":%d}\n", errno);
    }
}

/* ---------------------------------------------------------------------------
 * Blindness. The decisions are pure (proto.c, proto_sight and proto_guard);
 * only their wiring lives here.
 *
 * note_reject, note_absurd and note_episode DECIDE the entry and record it
 * (note_entry, blind_entry_*);
 * they touch no slot, and may be called in the middle of a slot
 * manipulation. The entry itself (enter_blind) happens between two
 * datagrams, in net_thread_main.
 * ------------------------------------------------------------------------- */
static uint64_t now_ms(void)
{
    return now_ns() / 1000000ULL;
}

static void note_entry(struct net_internal *ni, const char *cause,
                         const char *entry_what, uint32_t n, uint64_t span_ms)
{
    ni->blind_entry_cause   = cause;
    ni->blind_entry_what    = entry_what;
    ni->blind_entry_n       = n;
    ni->blind_entry_span_ms = span_ms;
}

/* A frame lost on the expected geometry (list in proto.h). */
static void note_reject(struct net_internal *ni)
{
    if (ni->is_blind || ni->blind_entry_cause) return;
    const uint64_t t = now_ms();
    if (proto_sight_reject(&ni->sight, t))
        note_entry(ni, "stuck_size", "frames", ni->sight.reject_count,
                     t - ni->sight.first_ms);
}

/* An absurd command: the garbage safeguard. */
static void note_absurd(struct net_internal *ni)
{
    if (ni->is_blind || ni->blind_entry_cause) return;
    const uint64_t t = now_ms();
    if (proto_guard_absurd(&ni->garbage_guard, t))
        note_entry(ni, "garbage", "absurd", ni->garbage_guard.absurd_count,
                     t - ni->garbage_guard.start_ms);
}

/* The start of an orphan episode: the garbage safeguard. */
static void note_episode(struct net_internal *ni)
{
    if (ni->is_blind || ni->blind_entry_cause) return;
    const uint64_t t = now_ms();
    if (proto_guard_episode(&ni->garbage_guard, t))
        note_entry(ni, "garbage", "episodes", ni->garbage_guard.episodes,
                     t - ni->garbage_guard.start_ms);
}

/* The periodic blind status: frame_echo = 0 (mask_echo), the `frame`
 * counter as is, FLAG_AUDIO as is. */
static void send_blind_status(struct net_internal *ni)
{
    send_status_now(ni);
    ni->blind_status_ns = now_ns();
}

/* The entry: go silent, forget the wrong size, say so. */
static void enter_blind(struct net_internal *ni)
{
    struct net_blit_shared *sh = ni->cfg.shared;
    const char *cause = ni->blind_entry_cause;
    ni->blind_entry_cause = NULL;

    /* The forgotten geometry, read BEFORE the reset: a change of dimensions
     * within the same regime writes no log line, so this log must itself
     * say which geometry is abandoned. */
    const uint32_t expected = atomic_load(&sh->expected_frame_bytes);
    const uint32_t h        = atomic_load(&sh->h_active_announced);
    const uint32_t v        = atomic_load(&sh->v_active_announced);

    ni->is_blind                  = 1;
    ni->blind_cause            = cause;
    ni->has_picture                = 0;
    ni->switchres_since_blind = 0;
    ni->raw_to_confirm        = 0;
    ni->blind_since_ns        = now_ns();
    ni->blind_blocks_muted        = 0u;
    ni->blind_frames_dropped      = 0u;
    ni->sight   = (struct proto_sight){0};
    ni->garbage_guard = (struct proto_guard){0};

    /* Nothing in flight when the CMD_SWITCHRES of the re-announcement
     * arrives: a slot with the wrong size could swallow it if it measured
     * exactly the remainder. */
    if (ni->current_slot) {
        ni->current_slot->received_bytes = 0;
        ni->current_slot                 = NULL;
    }
    ni->slot_fields_mask = 0u;
    audio_abandon(ni, "blind");

    /* Mute: the audio thread closes the card and empties the ring (pcm.c,
     * audio_thread), which also realigns left and right. audio_rate_code
     * and audio_channels are NOT touched: FLAG_AUDIO stays set. */
    int muted = 0;
    if (sh->audio) {
        audio_out_configure(sh->audio, 0u, 0u);
        muted = 1;
    }

    /* Forget the size: the cold-start path (net.h: 0 for h/v = the flip
     * thread falls back to --width/--height). */
    atomic_store(&sh->expected_frame_bytes, ni->startup_size);
    atomic_store(&sh->h_active_announced, 0u);
    atomic_store(&sh->v_active_announced, 0u);

    fprintf(stderr,
            "{\"event\":\"receiver_blind\",\"cause\":\"%s\",\"trigger\":\"%s\","
            "\"expected\":%u,\"h\":%u,\"v\":%u,\"rejects\":%u,\"span_ms\":%llu,"
            "\"reset_to\":%u,\"muted\":%d}\n",
            cause, ni->blind_entry_what ? ni->blind_entry_what : "",
            expected, h, v, ni->blind_entry_n,
            (unsigned long long)ni->blind_entry_span_ms,
            ni->startup_size, muted);

    /* The first blind status goes out at once. */
    send_blind_status(ni);
}

/* The exit: an accepted CMD_SWITCHRES, then a complete frame. Called at the
 * completion point, just before the frame goes to the flip thread. */
static void leave_blind(struct net_internal *ni)
{
    struct net_blit_shared *sh = ni->cfg.shared;
    const uint64_t blind_ms = (now_ns() - ni->blind_since_ns) / 1000000ULL;

    ni->is_blind                  = 0;
    ni->switchres_since_blind = 0;
    ni->sight   = (struct proto_sight){0};
    ni->garbage_guard = (struct proto_guard){0};
    if (sh->audio)
        audio_out_configure(sh->audio, proto_audio_rate_hz(ni->audio_rate_code),
                            ni->audio_channels);

    fprintf(stderr,
            "{\"event\":\"receiver_sighted\",\"cause\":\"%s\",\"blind_ms\":%llu,"
            "\"audio_blocks_muted\":%llu,\"frames_dropped\":%llu,"
            "\"expected\":%u,\"h\":%u,\"v\":%u}\n",
            ni->blind_cause ? ni->blind_cause : "",
            (unsigned long long)blind_ms,
            (unsigned long long)ni->blind_blocks_muted,
            (unsigned long long)ni->blind_frames_dropped,
            atomic_load(&sh->expected_frame_bytes),
            atomic_load(&sh->h_active_announced),
            atomic_load(&sh->v_active_announced));
    ni->blind_cause = NULL;
}

/* At the completion point of a frame: returns 1 if it goes to the flip
 * thread, 0 if blindness holds it back (the caller drops the slot). */
static int blind_lets_through(struct net_internal *ni)
{
    if (!ni->is_blind) return 1;
    if (ni->switchres_since_blind) {
        leave_blind(ni);
        return 1;
    }
    ni->blind_frames_dropped++;
    return 0;
}

/* ---------------------------------------------------------------------------
 * acquire_slot -- initialize the next round-robin slot from the pool with
 * the BLIT header's frame_id/field/vsync and the current expected_frame_bytes.
 * Falls back to 320*240*3 (RGB888) if SWITCHRES has not landed yet.
 * ------------------------------------------------------------------------- */
static void acquire_slot(struct net_internal *ni,
                         const cmd_blit_field_vsync_base_t *hdr)
{
    ni->current_slot = ni->cfg.shared->slots[ni->current_slot_idx];
    ni->current_slot->frame_id       = hdr->frame_id;
    ni->current_slot->expected_bytes = atomic_load(&ni->cfg.shared->expected_frame_bytes);
    if (ni->current_slot->expected_bytes == 0) {
        ni->current_slot->expected_bytes = 320U * 240U * 3U;  /* cold-start default */
    }
    ni->current_slot->received_bytes = 0;
    ni->slot_fields_mask = 0u;  /* fresh on every slot acquisition */
    /* The uncompressed size is the one the blit thread will consume. In a
     * compressed session, expected_bytes is then overwritten by the block
     * size. */
    ni->current_slot->clear_bytes    = ni->current_slot->expected_bytes;
    /* The key of the blindness detector -- expected uncompressed size and
     * compression state. A new key resets its run to zero. */
    proto_sight_key(&ni->sight, ni->current_slot->clear_bytes, ni->compression);
    ni->current_slot->compressed     = 0;
    ni->current_slot->decomp_us      = 0;   /* sum of the fields to come */
    ni->current_slot->field          = hdr->field;
    ni->current_slot->last_vsync     = hdr->vsync;
    /* Latency point B t0: frame-start timestamp. 480i's field 1
     * reuses the slot from field 0 without re-acquisition, so this stays the
     * arrival of the FRAME's first byte -- intended. */
    ni->current_slot->first_byte_ns  = now_ns();
}


/* --- The guard that was missing ---------------------------------------------
 *
 * struct frame_slot is defined TWICE: here and in the other file
 * (net.h canonical; the one in blit.c active, since blit.c does not
 * include net.h). Adding a field to one without the other produces no
 * warning: the two units read the same memory with two different layouts.
 *
 * What happened when `compressed` and `clear_bytes` were added to net.h
 * alone: the flip thread read `buf` at the wrong offset, got a bogus
 * pointer, and the kernel killed the daemon -- "segfault at 34800", that is
 * NULL + 320*224*3, the exact size of a Sonic 2 frame.
 *
 * Both definitions are now checked against the SAME numbers. A divergence
 * breaks the build instead of breaking the bench. Changing one requires
 * changing both, which is exactly the reminder wanted. */
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
               "in the padding after `compressed`, at the same position as in blit.c");

/* ---------------------------------------------------------------------------
 * wake_blit -- called right after each of the two successful
 * spsc_try_produce: one 8-byte write per published IMAGE, never per
 * datagram. The eventfd is non-blocking (main.c); the result is ignored --
 * a lost wake-up is caught up by the 2 ms ceiling of the blit thread's
 * poll, as before the option. wake_fd < 0: option off, no system call,
 * the former behaviour.
 * ------------------------------------------------------------------------- */
static void wake_blit(const struct net_internal *ni)
{
    if (ni->cfg.wake_fd >= 0) {
        const uint64_t wake_value = 1u;
        ssize_t w = write(ni->cfg.wake_fd, &wake_value, sizeof wake_value);
        (void)w;
    }
}

/* ---------------------------------------------------------------------------
 * resume_after_idle -- resume after idle. Called when a complete image is
 * about to be queued, AFTER the blindness check and BEFORE spsc_try_produce,
 * at both completion points (the continuation reassembly and
 * CMD_BLIT_FIELD_VSYNC, whose text stays the same).
 *
 * The blit thread may have returned the tube to 240p because the emitter
 * went quiet (blit_idle_step): a core pause, an open menu or a load longer
 * than --idle-after seconds. When images come back, the emitter announces
 * nothing again (its cache still says 480p): without this function, a
 * 921 600-byte image would be shown in a 240p scan, cropped by half, until
 * the next announcement. The return indicator (blit_idle_return_take, kept
 * out of struct net_blit_shared, which is defined twice) says there was a
 * return; if the regime served by the last announcement then differs from
 * the current regime, the request for that regime is republished through
 * the ordinary path (same publication as the CMD_SWITCHRES case: fields,
 * receive time, switchres_seq with a release store, last published
 * request). The crossing follows the band rule, the guard and the floor;
 * the screen stays black and images are acknowledged without being shown
 * until the first image of the new mode. Published BEFORE the image that
 * triggers it is queued: the blit thread latches the request before it
 * sees the image.
 *
 * A pass without a return costs only one atomic exchange.
 * ------------------------------------------------------------------------- */
static void resume_after_idle(struct net_internal *ni)
{
    if (!blit_idle_return_take())
        return;
    const uint8_t have = atomic_load(&ni->cfg.shared->current_regime);
    /* The last published request (480p) is no longer what the tube has: align
     * it on the current regime, as when a CMD_SWITCHRES is handled. */
    ni->last_published_regime = have;
    if (atomic_load(&ni->cfg.shared->legacy_modeset) || ni->announced_serve == have)
        return;

    const uint8_t want = ni->announced_serve;
    struct sr_mode_request *r = &ni->cfg.shared->request;
    r->h_active   = (uint16_t)atomic_load(&ni->cfg.shared->h_active_announced);
    r->v_active   = (uint16_t)atomic_load(&ni->cfg.shared->v_active_announced);
    r->interlace  = (want == (uint8_t)REGIME_480I) ? 1u : 0u;
    r->regime = want;
    r->monotonic_ns_received = now_ns();

    uint64_t prev = atomic_load_explicit(&ni->cfg.shared->switchres_seq,
                                         memory_order_relaxed);
    atomic_store_explicit(&ni->cfg.shared->switchres_seq, prev + 1ULL,
                          memory_order_release);
    ni->last_published_regime = ni->announced_serve;

    fprintf(stderr,
            "{\"event\":\"regime_resume\",\"from\":\"%s\",\"to\":\"%s\","
            "\"h_active\":%u,\"v_active\":%u}\n",
            regime_name(have), regime_name(want),
            (unsigned)r->h_active, (unsigned)r->v_active);
}

/* ---------------------------------------------------------------------------
 * slot_add_decomp -- adds the duration of one decompression to the slot,
 * in microseconds rounded up (a compressed image never reports 0, 0 stays
 * the mark of a raw one), saturated at 65 535. In 480i, both fields of an
 * image add up: acquire_slot resets to 0 once per image, not per field.
 * ------------------------------------------------------------------------- */
static void slot_add_decomp(struct frame_slot *s, uint64_t ns)
{
    const uint64_t sum_us = (uint64_t)s->decomp_us + (ns + 999u) / 1000u;
    s->decomp_us = (uint16_t)(sum_us > 65535u ? 65535u : sum_us);
}

/* ---------------------------------------------------------------------------
 * slot_decompress -- the LZ4 block reassembled in lz4_in becomes pixels in
 * the slot. Called when the count of compressed bytes is reached.
 *
 * Returns 1 if the slot now carries a usable image, 0 if the frame must be
 * dropped.
 * ------------------------------------------------------------------------- */
static int slot_decompress(struct net_internal *ni)
{
    struct frame_slot *s = ni->current_slot;
    if (!s || !ni->lz4_in) return 0;

    /* In progressive we decompress DIRECTLY into the slot: this is the hot
     * path, and a copy of two megabytes per frame is not worth it. In 480i a
     * linear output is needed, because the lines of the field are then
     * interleaved with those of the field already in the slot. */
    uint8_t *dst = s->is_field ? ni->lz4_out : s->buf;
    if (!dst) return 0;

    int n = lz4dec_block(ni->lz4_in, s->received_bytes, dst, s->clear_bytes);
    if (n < 0 || (uint32_t)n != s->clear_bytes) {
        fprintf(stderr,
                "{\"event\":\"lz4_decompress_failed\",\"frame\":%u,"
                "\"csize\":%u,\"clear\":%u,\"got\":%d}\n",
                s->frame_id, s->received_bytes, s->clear_bytes, n);
        return 0;
    }

    if (s->is_field) {
        uint32_t fb_w = atomic_load(&ni->cfg.shared->h_active_announced);
        struct frame_reassembly fr = {
            .frame_id       = s->frame_id,
            .expected_bytes = s->clear_bytes,
            .received_bytes = 0u,
            .buf            = s->buf
        };
        (void)proto_reassemble_push_field(&fr, s->field, fb_w,
                                          dst, s->clear_bytes);
        s->received_bytes = fr.received_bytes;
    } else {
        s->received_bytes = s->clear_bytes;
    }
    s->expected_bytes = s->clear_bytes;
    s->compressed     = 0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * arm_compression -- prepares the slot in flight to receive an LZ4 block.
 *
 * Returns 1 if the slot is armed, 0 if the frame must be dropped:
 * implausible announced size, or a delta mode we cannot undo.
 *
 * The size comes from the NETWORK. Two bounds, and both are needed: the
 * compression bound (beyond it, this is not compression) and the capacity
 * of the staging buffer (beyond it, this is an overflow).
 * ------------------------------------------------------------------------- */
static int arm_compression(struct net_internal *ni, uint32_t lz4_size,
                            uint8_t delta)
{
    struct frame_slot *s = ni->current_slot;
    if (!s) return 0;

    if (delta) {
        /* The XOR against the previous image needs one more image buffer
         * and an extra pass: that is a separate piece of work, and a plain
         * refusal is better than a corrupted image. A GroovyMAME set to
         * mister_lz4 = 2, 4 or 6 lands here: it must be set to 1 or 3. */
        if (!ni->delta_warned) {
            fprintf(stderr,
                    "{\"event\":\"lz4_delta_unsupported\",\"frame\":%u,"
                    "\"hint\":\"mister_lz4=1 or 3, not 2/4/6\"}\n",
                    s->frame_id);
            ni->delta_warned = 1;
        }
        return 0;
    }

    if (!lz4dec_size_ok(lz4_size, s->clear_bytes) ||
        (size_t)lz4_size > (size_t)MAX_FRAME_BYTES) {
        fprintf(stderr,
                "{\"event\":\"lz4_size_rejected\",\"frame\":%u,"
                "\"csize\":%u,\"clear\":%u}\n",
                s->frame_id, lz4_size, s->clear_bytes);
        note_reject(ni);   /* the expected uncompressed size does not fit */
        return 0;
    }

    s->compressed      = 1;
    s->expected_bytes  = lz4_size;   /* what must be reassembled */
    s->received_bytes  = 0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * handle_packet -- single-datagram dispatch. Updates last_packet_ns (heartbeat),
 * remembers last_emitter (for ACKs), then routes by opcode. parse_dispatch
 * is invoked first for length validation; per-opcode typed parsers re-run
 * because they expose populated structs (parse_dispatch is a yes/no gate).
 * ------------------------------------------------------------------------- */
static void handle_packet(struct net_internal *ni, const uint8_t *buf, size_t len,
                          const struct sockaddr_in *src, socklen_t src_len)
{
    if (len < 1) return;

    /* --- AUDIO continuation ------------------------------------------------
     *
     * Runs BEFORE the video continuation. Both states are never active at
     * the same time: opening an audio block abandons the video frame in
     * progress (see case CMD_AUDIO), and a command header abandons the audio
     * block in progress (right here). The order between the two guards
     * therefore has no functional effect; it is fixed so that reading is
     * unambiguous.
     *
     * An audio block takes two or three datagrams against two hundred and
     * twenty-five for a video frame: the window where this guard is armed is
     * narrow.
     */
    if (ni->audio_active) {
        /* proto_audio_escape, and not plain header recognition: an 8-byte
         * block tail starting with 0x07 IS sound, not a BLIT header. See
         * proto.h for the measurement. */
        if (proto_audio_escape(buf, len,
                               ni->audio_rx.expected_bytes,
                               ni->audio_rx.received_bytes,
                               ni->compression, ni->padded)) {
            audio_abandon(ni, "header");
            /* and fall back to the normal handling of the datagram */
        } else {
            ni->last_packet_ns = now_ns();
            if (ni->heartbeat_lost) {
                fprintf(stderr, "{\"event\":\"heartbeat_resumed\"}\n");
                ni->heartbeat_lost = 0;
            }
            if (src_len <= sizeof(ni->last_emitter)) {
                memcpy(&ni->last_emitter, src, src_len);
                ni->last_emitter_len = src_len;
            }

            /* proto_reassemble_push is reused as is: it bounds the write to
             * expected_bytes and never overflows the buffer. Three similar
             * lines are better than a premature abstraction -- here it is
             * even zero extra lines. */
            if (proto_reassemble_push(&ni->audio_rx, buf, len) == 1)
                audio_block_complete(ni);
            return;   /* do NOT fall back to parse_dispatch */
        }
    }

    /* blit_continuation -- continuation-mode dispatch.
     *
     * Only the FIRST packet of a BLIT_FIELD_VSYNC frame carries cmd byte 0x07 +
     * the 8-byte header. Every subsequent chunk for the same frame is pure RGB888
     * payload bytes with NO opcode prefix. If we let those bytes fall through to
     * parse_dispatch, buf[0] is just the next R/G/B byte (values 0x00..0xFF) which
     * always produces PARSE_BAD_OPCODE -- the frame never completes, nothing reaches
     * the blit thread. Found on the first hardware-in-the-loop session.
     *
     * Guard: when a partial reassembly slot is in flight, route the entire datagram
     * directly to proto_reassemble_push and return -- bypassing parse_dispatch and the
     * opcode switch entirely. The CMD_BLIT_FIELD_VSYNC case below primes current_slot
     * via acquire_slot; this guard handles all subsequent continuation packets until
     * received_bytes reaches expected_bytes and the slot is enqueued to the blit thread. */
    /* Escape hatch from continuation mode.
     *
     * If a wire-shaped control packet arrives during a partial reassembly
     * window (e.g. the emitter sends a fresh CMD_SWITCHRES while the
     * previous frame is still streaming, or the previous SWITCHRES set
     * expected_frame_bytes too high so the slot will never complete),
     * recognise it as a new command and break out of continuation mode.
     *
     * Detection: proto_video_escape. A datagram
     * escapes when it has the exact shape of a command (proto_is_command_header)
     * AND it does NOT measure exactly what the slot still expects -- or
     * when it is a real CMD_INIT, which always escapes.
     *
     * This comment used to call a 26-byte tail starting with 0x03
     * "astronomically unlikely". It happened: Chrono Cross,
     * `slot_received 11776`, `slot_expected 11802` -- the LZ4 tail of the
     * frame was taken for a CMD_SWITCHRES, the frame was lost and a
     * geometry was read out of the picture. In LZ4 the tail
     * length changes every frame, so every short length gets its turn.
     * The rule of the exact remainder closes that door: a tail always
     * measures exactly the remainder. In a padded session there are two
     * short chunks per block, never below 256 bytes (GM_TAIL_MIN),
     * so no command shape can ever be a tail there. The arithmetic of the
     * case the rule swallows (only the tail lost, and the next datagram
     * measures exactly that tail) lives in proto.h above
     * proto_video_escape -- not copied here.
     *
     * Reset the partial slot before falling through to the opcode switch.
     * The CMD_SWITCHRES handler below also has a partial-frame-drop log
     * path; that path stays a no-op when current_slot is already NULL.
     *
     * BLIT headers belong in this list too (an earlier regression).
     * ---------------------------------------------------------------
     * The escape originally covered only control opcodes, which left one
     * catastrophic hole: if a field arrives SHORT (a datagram lost anywhere
     * upstream), the slot never reaches expected_bytes, so the guard below
     * swallows the NEXT field's 8-byte 0x07 header as if it were pixels.
     * The slot then completes ~one datagram into that next field, is
     * enqueued, and every remaining datagram of the field lands in
     * parse_dispatch as a bogus opcode. Observed cost of a SINGLE lost
     * datagram in a 640x432 480i field: ~273 unknown_opcode lines plus a
     * visibly corrupted frame. With the header recognised, the same loss
     * costs exactly one dropped frame and one log line, and the stream
     * resynchronises on the very next header.
     *
     * A continuation chunk is GM_MTU_DEFAULT (1472) bytes except for the
     * tail of a field, so only that tail could measure one of the
     * recognised header lengths AND start with a matching opcode -- and
     * the tail is exactly what the remainder rule now keeps as payload.
     * A header that arrives after a real loss measures something other
     * than the remainder, and still escapes: the header fix holds. */
    int is_control_escape = 0;
    if (ni->current_slot != NULL &&
        ni->current_slot->received_bytes < ni->current_slot->expected_bytes) {
        if (proto_video_escape(buf, len,
                               ni->current_slot->expected_bytes,
                               ni->current_slot->received_bytes,
                               ni->compression, ni->padded)) {
            fprintf(stderr,
                    "{\"event\":\"continuation_escape\",\"opcode\":%u,"
                    "\"slot_received\":%u,\"slot_expected\":%u}\n",
                    (unsigned)buf[0],
                    ni->current_slot->received_bytes,
                    ni->current_slot->expected_bytes);
            /* A slot that had received bytes is lost. */
            if (ni->current_slot->received_bytes > 0)
                note_reject(ni);
            ni->current_slot->received_bytes = 0;
            ni->current_slot                 = NULL;
            is_control_escape = 1;
        }
    }

    if (!is_control_escape && ni->current_slot != NULL &&
        ni->current_slot->received_bytes < ni->current_slot->expected_bytes) {

        /* Heartbeat + emitter-addr bookkeeping (mirrors lines below for normal packets). */
        ni->last_packet_ns = now_ns();
        if (ni->heartbeat_lost) {
            fprintf(stderr, "{\"event\":\"heartbeat_resumed\"}\n");
            ni->heartbeat_lost = 0;
        }
        if (src_len <= sizeof(ni->last_emitter)) {
            memcpy(&ni->last_emitter, src, src_len);
            ni->last_emitter_len = src_len;
        }

        /* Wrap the in-flight slot and push the entire datagram as payload -- mirroring
         * the frame_reassembly wrapper in the CMD_BLIT_FIELD_VSYNC branch below.
         * A slot primed by 0x07 in the 480i regime carries interleaved
         * field rows -- continuation chunks must keep the even/odd placement, not
         * append linearly (a gap caught by the reassembly fixtures). */
        /* A compressed block is reassembled LINEARLY in the staging buffer:
         * it has no lines, and the slot may already carry field 0. */
        const int was_compressed = ni->current_slot->compressed;
        struct frame_reassembly fr = {
            .frame_id       = ni->current_slot->frame_id,
            .expected_bytes = ni->current_slot->expected_bytes,
            .received_bytes = ni->current_slot->received_bytes,
            .buf            = was_compressed ? ni->lz4_in
                                              : ni->current_slot->buf
        };
        int complete;
        if (was_compressed) {
            complete = proto_reassemble_push(&fr, buf, len);
        } else if (ni->current_slot->is_field) {
            uint32_t fb_w = atomic_load(&ni->cfg.shared->h_active_announced);
            complete = proto_reassemble_push_field(&fr, ni->current_slot->field,
                                                   fb_w, buf, len);
        } else {
            complete = proto_reassemble_push(&fr, buf, len);
        }
        ni->current_slot->received_bytes = fr.received_bytes;

        /* The block is complete: it becomes pixels here, and only here.
         * slot_decompress rewrites received_bytes as uncompressed. */
        if (was_compressed && complete) {
            /* The decompression measured alone. It falls between
             * first_byte_ns and ready_ns, hence INSIDE reasm_ns; the latency
             * CSV separates it out with decomp_us. */
            const uint64_t t_dec = now_ns();
            if (!slot_decompress(ni)) {
                note_reject(ni);   /* unreadable frame */
                ni->current_slot->received_bytes = 0;
                ni->current_slot                 = NULL;
                return;
            }
            slot_add_decomp(ni->current_slot, now_ns() - t_dec);
            /* Decompressed to the exact size: the frame is good. */
            proto_sight_good(&ni->sight);
        }

        if (complete && ni->current_slot->is_field) {
            /* Same rule as at the completion point of 0x07 -- see the
             * comment there for the detail. Both copies must stay
             * TEXTUALLY identical. */
            ni->slot_fields_mask |= (uint8_t)(1u << ni->current_slot->field);
            if (ni->slot_fields_mask != 0x3u) {
                complete = 0;
            } else {
                /* A pair of DISTINCT fields has just been completed. Counts
                 * every interlaced pair, 0x06 as well as 0x07 -- the
                 * denominator of the openings counted further down. */
                ni->field_pairs_win++;
                ni->field_pairs_total++;
            }
        }

        if (complete && !blind_lets_through(ni)) {
            /* Blind: the frame is not displayed. */
            ni->current_slot->received_bytes = 0;
            ni->current_slot                 = NULL;
            return;
        }
        if (complete) {
            /* Mirror the complete-path in CMD_BLIT_FIELD_VSYNC: enqueue to blit thread,
             * echo ACK fields, rotate slot index, clear current_slot. */
            resume_after_idle(ni);   /* BEFORE the image is queued */
            ni->current_slot->ready_ns = now_ns();  /* latency point B t1 */
            if (spsc_try_produce(ni->cfg.shared->spsc_ring, ni->current_slot) == 1) {
                wake_blit(ni);   /* no-op when the option is off */
                atomic_store(&ni->cfg.shared->ack_frame_echo,
                             ni->current_slot->frame_id);
                atomic_store(&ni->cfg.shared->ack_vcount_echo,
                             ni->current_slot->last_vsync);
                /* An image of the session is displayed; when raw, it is
                 * only good at the next accepted datagram with no orphan
                 * in between. */
                ni->has_picture = 1;
                if (!was_compressed) ni->raw_to_confirm = 1;
                ni->current_slot_idx = (ni->current_slot_idx + 1U) %
                                       ni->cfg.shared->slot_capacity;
                ni->current_slot = NULL;
            } else {
                /* SPSC ring full -- blit thread fell behind. Drop frame defensively. */
                fprintf(stderr,
                        "{\"event\":\"spsc_full_dropped\",\"frame\":%u}\n",
                        ni->current_slot->frame_id);
                ni->current_slot->received_bytes = 0;
                ni->current_slot = NULL;
            }
        }
        return;  /* Do NOT fall through to parse_dispatch or the opcode switch. */
    }

    /* --- Orphan guard --------------------------------------------------------
     *
     * Here, no frame and no audio block is in flight. A datagram that does
     * not have the EXACT shape of a command (proto_out_of_frame_accepts, one
     * single table shared with the software client) is a piece of image
     * whose slot is lost: a lost header, or the header swallowed by the
     * remainder rule (proto.h, proto_video_escape). Before this guard,
     * parse_dispatch and the handlers accepted loose lengths, and a 1472-byte
     * chunk starting with 0x01, 0x03 or 0x04 became a CMD_CLOSE, a geometry
     * or an audio block: a cascade of false close, freeze and white noise.
     * Same rule as the software client (`stray_payload`, client/gmclient.c).
     *
     * The orphan is emitter traffic: it keeps the heartbeat and the return
     * address, like a payload. It is counted, never logged one by one
     * (garbage_report). */
    if (!proto_out_of_frame_accepts(buf, len, ni->compression, ni->padded)) {
        ni->last_packet_ns = now_ns();
        if (ni->heartbeat_lost) {
            fprintf(stderr, "{\"event\":\"heartbeat_resumed\"}\n");
            ni->heartbeat_lost = 0;
        }
        if (src_len <= sizeof(ni->last_emitter)) {
            memcpy(&ni->last_emitter, src, src_len);
            ni->last_emitter_len = src_len;
        }
        ni->garbage_orphans_win++;
        if (buf[0] < CMD_CLOSE || buf[0] > CMD_GET_VERSION)
            ni->garbage_unknown_win++;
        if (!ni->in_episode) {
            ni->in_episode = 1u;
            ni->garbage_episodes_win++;
            /* Right after a "complete" raw frame, the episode says the
             * expected size was smaller than the real one: it is a reject
             * of that frame (proto_sight), not garbage. Otherwise it is a
             * lost stream, which the safeguard counts (proto_guard). */
            if (ni->raw_to_confirm) {
                ni->raw_to_confirm = 0;
                note_reject(ni);
            } else {
                note_episode(ni);
            }
        }
        return;
    }
    ni->in_episode = 0u;
    /* An accepted datagram with no orphan since the last complete raw
     * frame confirms that frame as good. */
    if (ni->raw_to_confirm) {
        ni->raw_to_confirm = 0;
        proto_sight_good(&ni->sight);
    }

    /* Length/opcode pre-validation -- drops malformed packets without crashing.
     * parse_dispatch returns PARSE_BAD_OPCODE / PARSE_TOO_SHORT / PARSE_BAD_COMPRESSION
     * for malformed input. Behind the orphan guard above, only exact command
     * shapes reach it. */
    parse_result_t pre = parse_dispatch(buf, len);
    if (pre == PARSE_TOO_SHORT) {
        fprintf(stderr, "{\"event\":\"parse_too_short\",\"len\":%zu}\n", len);
        return;
    }

    ni->last_packet_ns = now_ns();
    if (ni->heartbeat_lost) {
        fprintf(stderr, "{\"event\":\"heartbeat_resumed\"}\n");
        ni->heartbeat_lost = 0;
    }
    if (src_len <= sizeof(ni->last_emitter)) {
        memcpy(&ni->last_emitter, src, src_len);
        ni->last_emitter_len = src_len;
    }

    switch (buf[0]) {
    case CMD_GET_STATUS:
    case CMD_GET_VERSION:
        /* Emitter polls -- reply immediately with the current status. */
        send_status_now(ni);
        break;
    case CMD_INIT: {
        cmd_init_t init;
        parse_result_t r = parse_cmd_init(buf, len, &init);
        /* The PURE transition, called, never rewritten -- see proto.h for
         * the full contract. It guarantees by itself that a refused
         * CMD_INIT (PARSE_BAD_COMPRESSION, or any value other than
         * PARSE_OK) changes neither the compression nor the mode; do NOT
         * repeat that test here, it would be exactly the second copy of a
         * rule that the pure transition avoids (the same shape as the
         * struct defined twice).
         *
         * Rule for a mode change between two INITs (proto.h): the LAST
         * CMD_INIT wins, nothing is refused -- strictly symmetric with
         * `compression`. A real limit, neither worsened nor fixed here:
         * `ni->padded`, like `ni->compression` today, is a state PER NET
         * THREAD, not PER PEER -- with two emitters of different modes
         * aiming at this daemon, the last CMD_INIT governs both. */
        const int mode_change = proto_session_on_init(&ni->padded, r, len);
        if (mode_change) {
            fprintf(stderr,
                    "{\"event\":\"padding_announced\",\"mode\":\"%s\",\"len\":%zu}\n",
                    ni->padded ? "pad8" : "classic", len);
        }
        /* A CMD_INIT with compression > 1 no longer gets here --
         * proto_is_command_header refuses it, the orphan guard counts it.
         * The former compression_blocked lock, which a single false INIT
         * of exact remainder set for the whole session, is gone with it. */
        if (r == PARSE_OK) {
            /* Mode 1 (LZ4) is handled. */
            if (ni->compression != init.compression) {
                fprintf(stderr,
                        "{\"event\":\"compression_announced\",\"mode\":%u,"
                        "\"name\":\"%s\"}\n",
                        (unsigned)init.compression,
                        init.compression ? "lz4" : "raw");
            }
            ni->compression  = init.compression;
            ni->delta_warned  = 0;
            /* The emitter announces its sample rate and channels.
             * 0 = no audio, 1 = 22050, 2 = 44100, 3 = 48000. They are kept
             * for opening the PCM device and for the log. */
            if (ni->audio_rate_code != init.sample_rate
             || ni->audio_channels  != init.channels) {
                /* The audio_flag field here is computed on the NEW values
                 * (init.*), not on ni->*, which is only updated on the next
                 * two lines -- this is exactly what the next
                 * acknowledgement will carry. */
                fprintf(stderr,
                        "{\"event\":\"audio_announced\",\"rate_code\":%u,"
                        "\"channels\":%u,\"audio_flag\":%u}\n",
                        (unsigned)init.sample_rate, (unsigned)init.channels,
                        (unsigned)(init.sample_rate != 0u && init.channels != 0u));
            }
            ni->audio_rate_code = init.sample_rate;
            ni->audio_channels  = init.channels;
            /* The output learns what to open. It opens nothing now --
             * only when the first block arrives.
             * Blind, the mute holds until the exit (leave_blind), which
             * sets these same values. The CMD_INIT of the re-announcement
             * does NOT lift blindness. */
            if (ni->cfg.shared->audio && !ni->is_blind)
                audio_out_configure(ni->cfg.shared->audio,
                                    proto_audio_rate_hz(init.sample_rate),
                                    init.channels);
        }
        /* Emitter blocks on a 13-byte status reply for CMD_INIT (1s timeout
         * before it aborts the session). Reply now regardless of whether
         * the parse succeeded -- same shape as a normal status_pkt. */
        send_status_now(ni);
        break;
    }
    case CMD_SWITCHRES: {
        cmd_switchres_t sw;
        if (parse_cmd_switchres(buf, len, &sw) == PARSE_OK) {
            /* A geometry of exact shape but absurd (positions out of order,
             * interlace outside {0,1}, pclock outside [1, 200] MHz -- see
             * proto.h) is refused BEFORE any effect, and counted. One line
             * per one-second window at most: never one line per datagram. */
            if (!proto_switchres_plausible(&sw)) {
                ni->garbage_absurd_win++;
                note_absurd(ni);   /* the garbage safeguard */
                if (!ni->implausible_logged) {
                    const double pclock_mhz = sw.pclock;
                    fprintf(stderr,
                            "{\"event\":\"switchres_implausible\","
                            "\"h_active\":%u,\"h_begin\":%u,\"h_end\":%u,\"h_total\":%u,"
                            "\"v_active\":%u,\"v_begin\":%u,\"v_end\":%u,\"v_total\":%u,"
                            /* pclock as a string: NaN and infinity
                             * would be written nan/inf, invalid JSON. */
                            "\"interlace\":%u,\"pclock_mhz\":\"%g\"}\n",
                            (unsigned)sw.h_active, (unsigned)sw.h_begin,
                            (unsigned)sw.h_end, (unsigned)sw.h_total,
                            (unsigned)sw.v_active, (unsigned)sw.v_begin,
                            (unsigned)sw.v_end, (unsigned)sw.v_total,
                            (unsigned)sw.interlace, pclock_mhz);
                    ni->implausible_logged = 1u;
                }
                break;
            }
            /* Clamp the byte count against MAX_FRAME_BYTES (the slot
             * buffer capacity). cmd_switchres_t.h_active/v_active are u16, so
             * the worst-case product (65535 * 65535 * 3 ~ 12.8 GB) overflows
             * uint32_t. Compute in u64, reject if zero or larger than the
             * preallocated slot.
             *
             * The blit thread (with safety.c) additionally validates
             * hfreq/vfreq against the monitor envelope via validate_modeline()
             * BEFORE accepting the modeline. */
            uint64_t exp64 = (uint64_t)sw.h_active *
                             (uint64_t)sw.v_active * 3ULL;
            if (exp64 == 0 || exp64 > (uint64_t)MAX_FRAME_BYTES) {
                fprintf(stderr,
                        "{\"event\":\"switchres_geometry_rejected\","
                        "\"h\":%u,\"v\":%u,\"need\":%llu,\"max\":%u}\n",
                        (unsigned)sw.h_active, (unsigned)sw.v_active,
                        (unsigned long long)exp64,
                        (unsigned)MAX_FRAME_BYTES);
                /* Keep prior expected_frame_bytes -- emitter sees no ACK
                 * change but we never let the slot overflow. */
                break;
            }
            /* The regime is decided HERE, once, by the resolver of regime.h,
             * before ANY state write. A 480p requested from a receiver whose
             * 480p is not enabled (no high band, guard or switchres refusing,
             * not judged yet) is no longer refused: it is SERVED in 480i
             * (`serve`), without crossing a band, and the session goes on.
             * The WIRE SHAPE stays that of the 480p (`wire`: whole 0x06 image,
             * h*v*3 bytes) -- that is what reassembly follows, never the
             * served regime (a whole image read as a field would leave
             * orphans). The --legacy-modeset path does not go through the
             * resolver (no regime is chosen there): it is never folded here.
             * One line per entry into the fallback (a flag private to the net
             * thread), never one line per datagram. */
            const int sw_legacy = atomic_load(&ni->cfg.shared->legacy_modeset);
            const int sw_state480p = blit_regime_480p_state();
            struct regime_choice sw_choice = {
                REGIME_240P_SUPER_RES, REGIME_240P_SUPER_RES, 0
            };
            if (!sw_legacy) {
                sw_choice = regime_choose((unsigned)sw.interlace, sw.pclock,
                                          (unsigned)sw.h_total, sw_state480p);
            }
            const uint8_t wire_regime = (uint8_t)sw_choice.wire;   /* wire shape */
            const uint8_t want_regime = (uint8_t)sw_choice.serve;  /* served regime */
            if (sw_choice.fallback) {
                if (!ni->regime_fallback_logged) {
                    fprintf(stderr,
                            "{\"event\":\"regime_fallback\",\"from\":\"480p\","
                            "\"to\":\"480i\",\"reason\":\"%s\","
                            "\"h_active\":%u,\"v_active\":%u}\n",
                            regime480p_reason(sw_state480p),
                            (unsigned)sw.h_active, (unsigned)sw.v_active);
                    ni->regime_fallback_logged = 1u;
                }
            } else {
                ni->regime_fallback_logged = 0u;
            }

            /* Plausible and under MAX_FRAME_BYTES, hence accepted. Blind,
             * it arms the exit: the next complete frame lifts blindness
             * (blind_lets_through). */
            if (ni->is_blind)
                ni->switchres_since_blind = 1;
            if (sw_legacy) {
                /* --- LEGACY PATH (per-request modeset) -- UNCHANGED --- */
                atomic_store(&ni->cfg.shared->expected_frame_bytes,
                             (uint32_t)exp64);
                atomic_store(&ni->cfg.shared->h_active_announced, (uint32_t)sw.h_active);
                atomic_store(&ni->cfg.shared->v_active_announced, (uint32_t)sw.v_active);

                /* --- Publish the request to blit. --- */

                /* Drop any partial reassembly in flight
                 * when a fresh CMD_SWITCHRES arrives. Mirrors the existing
                 * frame_id_mismatch reset path in CMD_BLIT_FIELD_VSYNC.
                 * Best-effort -- no interleave preservation across modeset
                 * boundaries; the emitter has just told us the geometry has
                 * changed, so any partial pixels in flight belong to the OLD
                 * geometry and would fail the slot bounds anyway. */
                if (ni->current_slot && ni->current_slot->received_bytes > 0) {
                    fprintf(stderr,
                            "{\"event\":\"partial_frame_dropped_on_switchres\","
                            "\"frame_id\":%u,\"received\":%u,\"expected\":%u}\n",
                            ni->current_slot->frame_id,
                            ni->current_slot->received_bytes,
                            ni->current_slot->expected_bytes);
                    ni->current_slot->received_bytes = 0;
                    ni->current_slot                 = NULL;
                }

                /* Populate the request struct field-by-field (no memcpy from
                 * network bytes -- proto.c::parse_cmd_switchres already validated
                 * the layout via _Static_assert(sizeof(cmd_switchres_t) == 26)). */
                struct sr_mode_request *r = &ni->cfg.shared->request;
                r->h_active   = sw.h_active;
                r->h_begin    = sw.h_begin;
                r->h_end      = sw.h_end;
                r->h_total    = sw.h_total;
                r->v_active   = sw.v_active;
                r->v_begin    = sw.v_begin;
                r->v_end      = sw.v_end;
                r->v_total    = sw.v_total;
                /* GroovyMAME 0.287 wire format sends pclock as
                 * a double in MHz, NOT Hz (verified by sniffer: gridlee -> 6.90625
                 * which is 6.90625 MHz = 6_906_250 Hz, hfreq=15_624 Hz, vfreq=59.18 Hz --
                 * matches arcade_15 envelope). The first daemon never used pclock
                 * for compute (only for h_active*v_active*3 byte budget), so this
                 * units mismatch first surfaced when blit_runtime_modeset
                 * called sr_add_mode(..., refresh_hz=0, ...) and segfaulted.
                 * switchres internally uses Hz. We normalize at publish time. */
                r->pclock     = sw.pclock * 1.0e6;  /* MHz wire -> Hz internal */
                r->interlace  = sw.interlace;

                /* Derive refresh = pclock_hz / (h_total * v_total). Guard against
                 * /0 even though parse_cmd_switchres should reject zero totals --
                 * defense in depth, the divide is on the cold path anyway. */
                uint64_t htotal = (uint64_t)sw.h_total;
                uint64_t vtotal = (uint64_t)sw.v_total;
                r->refresh_hz   = (htotal && vtotal)
                                  ? r->pclock / (double)(htotal * vtotal)
                                  : 0.0;

                r->monotonic_ns_received = now_ns();

                /* Release-store on switchres_seq makes the struct fields visible
                 * to blit via its acquire-load on the same atomic.
                 * Relaxed load on prev is safe -- net is the sole writer. */
                uint64_t prev = atomic_load_explicit(&ni->cfg.shared->switchres_seq,
                                                     memory_order_relaxed);
                atomic_store_explicit(&ni->cfg.shared->switchres_seq, prev + 1ULL,
                                      memory_order_release);

                fprintf(stderr,
                        "{\"event\":\"switchres_request_published\",\"seq\":%llu,"
                        "\"h\":%u,\"v\":%u,\"refresh\":%.3f,\"pclock\":%.0f,"
                        "\"interlace\":%u,\"mono_ns\":%llu}\n",
                        (unsigned long long)(prev + 1ULL),
                        (unsigned)sw.h_active, (unsigned)sw.v_active,
                        r->refresh_hz, r->pclock,
                        (unsigned)sw.interlace,
                        (unsigned long long)r->monotonic_ns_received);
                break;
            }

            /* --- DEFAULT PATH (super-res regime resolver) ---
             * Resolves CMD_SWITCHRES to one of the
             * pre-registered regimes. No drmModeSetCrtc unless the regime
             * actually changed.
             *
             * Three regimes, one single authority. The resolver of regime.h
             * decides HERE, once; the decision travels in the request
             * (sr_mode_request.regime) and the blit thread never makes it
             * again. An interlaced mode is a 480i whatever its frequency; a
             * progressive mode whose line rate reaches the high band is a
             * 480p. The net thread chooses a regime, never a timing. */
            {
                const uint8_t want = want_regime;  /* SERVED regime, decided above (fallback included) */
                uint8_t have = atomic_load(&ni->cfg.shared->current_regime);
                ni->gm_interlaced_0x06_logged = 0;  /* a fresh log line per regime entry */
                /* The wire shape, NOT the served regime. Under the fallback
                 * (wire 480p, serve 480i) the emitter still sends whole 0x06
                 * images: reassembly reads them as a 480p. */
                ni->announced_regime = wire_regime;  /* wire truth for 0x07 dispatch */
                ni->announced_serve  = want;         /* what the emitter believes it sees */
                /* If the blit thread returned the tube to 240p while idle, the
                 * "last published request" (480p) is no longer what the tube
                 * has: align it on the current regime BEFORE judging the
                 * publication. Otherwise a new session in 240p would be read
                 * as cancelling a switch that waits for nothing (a phantom
                 * regime_request_reverted). */
                if (blit_idle_return_take()) {
                    /* The current regime is read again AFTER the indicator:
                     * the blit thread wrote it before raising it. */
                    have = atomic_load(&ni->cfg.shared->current_regime);
                    ni->last_published_regime = have;
                }

                /* expected_frame_bytes is field-local in 480i
                 * (each CMD_BLIT_FIELD_VSYNC carries ONE field --
                 * v_active/2 rows). 240p super-res keeps whole-frame semantics
                 * (h_active * v_active * 3, already computed as exp64).
                 * Computed from the wire shape, never the served regime -- a
                 * 480p folded into 480i keeps the whole image (921 600 bytes). */
                uint32_t bytes_per_unit = (wire_regime == (uint8_t)REGIME_480I)
                    ? (uint32_t)((uint64_t)sw.h_active *
                                (uint64_t)(sw.v_active / 2u) * 3ULL)
                    : (uint32_t)exp64;

                atomic_store(&ni->cfg.shared->expected_frame_bytes, bytes_per_unit);
                atomic_store(&ni->cfg.shared->h_active_announced, (uint32_t)sw.h_active);
                atomic_store(&ni->cfg.shared->v_active_announced, (uint32_t)sw.v_active);

                /* Same drop-partial-reassembly-in-flight defense as the
                 * legacy path (the geometry/regime-local byte budget
                 * just changed under any in-flight partial slot). */
                if (ni->current_slot && ni->current_slot->received_bytes > 0) {
                    fprintf(stderr,
                            "{\"event\":\"partial_frame_dropped_on_switchres\","
                            "\"frame_id\":%u,\"received\":%u,\"expected\":%u}\n",
                            ni->current_slot->frame_id,
                            ni->current_slot->received_bytes,
                            ni->current_slot->expected_bytes);
                    ni->current_slot->received_bytes = 0;
                    ni->current_slot                 = NULL;
                }

                if (!regime_publish_needed(want, have, ni->last_published_regime)) {
                    /* Within-regime dim change (new game, same regime) --
                     * no modeset, no switchres_seq bump. blit.c keeps
                     * scanning out the current modeline; it reads
                     * h_active_announced/v_active_announced for the
                     * crop/center/pad math. Only when no switch to ANOTHER
                     * regime is pending (want == have == last published). */
                    break;
                }

                /* Regime CHANGE -- populate sr_mode_request (only `interlace`
                 * matters for the toggle since blit.c uses cached
                 * drmModeModeInfo, not the wire modeline). Bump
                 * switchres_seq release-store so blit's page-flip loop
                 * observes the delta.
                 *
                 * want == have here means "stay in A": a switch to B is
                 * pending (the band rule delays it by seconds) and the
                 * emitter came back to A before the deadline. The request is
                 * published anyway, and IT is what cancels: blit.c latches it
                 * (the last one wins), it crosses nothing, and its
                 * "want == have" branch does nothing. */
                const int cancels = (want == have);
                const uint8_t was_published = ni->last_published_regime;
                struct sr_mode_request *r = &ni->cfg.shared->request;
                r->h_active   = sw.h_active;
                r->v_active   = sw.v_active;
                r->interlace  = sw.interlace;
                r->regime     = want;   /* the decision, read by blit.c */
                r->monotonic_ns_received = now_ns();
                /* pclock/refresh_hz left zero -- blit.c's regime-toggle fast
                 * path doesn't use them (cached drmModeModeInfo already
                 * carries the locked timings from modelines.h). */

                uint64_t prev = atomic_load_explicit(&ni->cfg.shared->switchres_seq,
                                                     memory_order_relaxed);
                atomic_store_explicit(&ni->cfg.shared->switchres_seq, prev + 1ULL,
                                      memory_order_release);

                ni->last_published_regime = want;

                if (cancels) {
                    /* "Request reverted", NOT "crossing cancelled". The net
                     * thread does not know whether a band wait took place (a
                     * 240p <-> 480i switch under the floor, or a request already
                     * served, produce the same want == have):
                     * band_crossing_cancelled is written by the blit thread
                     * alone, when a wait ends without a crossing. */
                    fprintf(stderr,
                            "{\"event\":\"regime_request_reverted\","
                            "\"from\":\"%s\",\"to\":\"%s\","
                            "\"h_active\":%u,\"v_active\":%u}\n",
                            regime_name(was_published), regime_name(want),
                            (unsigned)sw.h_active, (unsigned)sw.v_active);
                } else {
                    fprintf(stderr,
                            "{\"event\":\"regime_change\","
                            "\"from\":\"%s\",\"to\":\"%s\","
                            "\"h_active\":%u,\"v_active\":%u}\n",
                            regime_name(have), regime_name(want),
                            (unsigned)sw.h_active, (unsigned)sw.v_active);
                }
            }
        }
        break;
    }
    case CMD_BLIT_FIELD_VSYNC: {
        cmd_blit_field_vsync_base_t hdr;
        blit_field_extras_t          xtra;
        const uint8_t *payload;
        size_t         payload_len;
        if (parse_cmd_blit_field_vsync_ex(buf, len, ni->compression,
                                          &hdr, &xtra,
                                          &payload, &payload_len) != PARSE_OK) {
            break;
        }

        /* frame_dup: the frame repeats the previous one, and there is
         * nothing to reassemble. Doing NOTHING is the right answer -- the
         * flip thread only flips on a new slot, so the tube keeps showing
         * exactly the intended image. Acknowledge, and move on. */
        if (xtra.dup) {
            if (ni->current_slot) {
                /* The slot in flight is dropped unconditionally -- and if it
                 * is a COMPLETE 480i field waiting for its twin (0x06 path,
                 * slot_fields_mask at 0x1 or 0x2), a valid half-image
                 * disappears. Rule 5 of libgm (gm_dup_should_skip) protects
                 * OUR emitter; it protects neither from a header lost on the
                 * wire (the receiver believes the pair open, the emitter
                 * believes it closed), nor from an upstream emitter. Say so
                 * and count it, on the model of
                 * partial_frame_dropped_on_switchres: a defect that no
                 * counter catches once cost half a day. */
                if (ni->current_slot->received_bytes > 0 ||
                    ni->slot_fields_mask != 0u) {
                    fprintf(stderr,
                            "{\"event\":\"partial_frame_dropped_on_dup\","
                            "\"frame_id\":%u,\"dup_frame_id\":%u,"
                            "\"received\":%u,\"expected\":%u,"
                            "\"fields_mask\":%u}\n",
                            ni->current_slot->frame_id, hdr.frame_id,
                            ni->current_slot->received_bytes,
                            ni->current_slot->expected_bytes,
                            (unsigned)ni->slot_fields_mask);
                }
                ni->current_slot->received_bytes = 0;
                ni->current_slot                 = NULL;
            }
            /* A dup only repeats what the daemon has already displayed.
             * Without a picture in the session, or blind, it does not
             * advance the echo; mask_echo clears it anyway in the status
             * below. Before: a fresh daemon acknowledged dups of an image
             * never received, and the emitter believed it synchronised. */
            if (ni->has_picture && !ni->is_blind) {
                atomic_store(&ni->cfg.shared->ack_frame_echo,  hdr.frame_id);
                atomic_store(&ni->cfg.shared->ack_vcount_echo, hdr.vsync);
            }
            /* A dup does not flip: the flip thread will therefore never set
             * ack_ready for it, and without this line the echo above was
             * overwritten by the next one without ever travelling -- on a
             * still scene, the emitter received NO acknowledgement for up to
             * a second (net = sent - echo climbed towards 60 with no real
             * delay, and the 2 s ack_lost window became reachable). Reply as
             * to a CMD_GET_STATUS, from this thread, which already owns the
             * sending. ack_ready belongs to the flip thread and is not
             * touched: ack_frame/ack_vcount stay those of the last real
             * flip, only the echo advances. */
            send_status_now(ni);
            break;
        }

        /* 480i field-local reassembly. expected_bytes
         * is already field-local (set per-field in the CMD_SWITCHRES
         * regime-resolver above). Reset received_bytes between field 0 and
         * field 1 of the SAME frame -- frame_id match is the gate, distinct
         * from the frame_id_mismatch guard below (which handles a DIFFERENT
         * frame_id arriving mid-reassembly). Must run before that guard. */
        if (ni->announced_regime == (uint8_t)REGIME_480I) {
            if (ni->current_slot && ni->current_slot->frame_id == hdr.frame_id
                && ni->current_slot->field != hdr.field) {
                ni->current_slot->received_bytes = 0;
            }
        }

        /* Validate frame_id against the slot in flight. UDP can reorder
         * or duplicate; if a chunk for frame N+1 arrives before frame N is
         * complete, accumulating its bytes into the partial slot would mix
         * pixels from two frames. Drop the stale partial and start fresh. */
        if (ni->current_slot && ni->current_slot->frame_id != hdr.frame_id) {
            fprintf(stderr,
                    "{\"event\":\"frame_id_mismatch\",\"old\":%u,\"new\":%u,"
                    "\"received\":%u,\"expected\":%u}\n",
                    ni->current_slot->frame_id, hdr.frame_id,
                    ni->current_slot->received_bytes,
                    ni->current_slot->expected_bytes);
            if (ni->current_slot->received_bytes > 0)
                note_reject(ni);   /* a frame lost with bytes received */
            ni->current_slot->received_bytes = 0;
            ni->current_slot                 = NULL;
        }
        if (!ni->current_slot) {
            acquire_slot(ni, &hdr);
        }
        /* is_field/field must be set HERE, before any early return of the
         * compressed path below: slot_decompress() (when the LZ4 block is
         * reassembled) reads is_field to choose its destination. The old
         * code only set it in the NON-compressed branch; in a compressed
         * session this block left through 'break' before reaching it, so
         * slot_decompress read a STALE value left by the previous
         * round-robin turn of the slot. A whole field frame was then written
         * LINEARLY instead of interleaved -- the whole image squeezed into
         * the top half of the screen. These two fields only depend on the
         * regime and on the header received, never on compression: the
         * non-compressed branch below no longer needs to set them again. */
        ni->current_slot->is_field = (ni->announced_regime == (uint8_t)REGIME_480I) ? 1 : 0;
        ni->current_slot->field    = hdr.field;
        if (ni->compression) {
            /* Each field carries its own compressed size: re-arm on every
             * header, including for field 1 of a 480i frame. */
            if (!arm_compression(ni, xtra.lz4_size, xtra.delta)) {
                ni->current_slot->received_bytes = 0;
                ni->current_slot                 = NULL;
                break;
            }
            /* The compressed header travels ALONE: nothing to reassemble
             * here, the bytes of the block arrive in the next datagrams and
             * the continuation guard takes care of them. */
            break;
        }
        struct frame_reassembly fr = {
            .frame_id       = ni->current_slot->frame_id,
            .expected_bytes = ni->current_slot->expected_bytes,
            .received_bytes = ni->current_slot->received_bytes,
            .buf            = ni->current_slot->buf
        };

        /* Regime-aware reassembly. 480i writes fields into the
         * paired even/odd rows of a single 480-row buffer
         * (proto_reassemble_push_field); 240p super-res (and legacy
         * progressive) use the primitive verbatim -- CMD_BLIT_VSYNC (0x06)
         * below is untouched. is_field/field are set above now, before the
         * compressed early-return -- not re-set here. */
        int complete;
        if (ni->announced_regime == (uint8_t)REGIME_480I) {
            uint32_t fb_w = atomic_load(&ni->cfg.shared->h_active_announced);
            complete = proto_reassemble_push_field(&fr, hdr.field, fb_w,
                                                    payload, payload_len);
        } else {
            complete = proto_reassemble_push(&fr, payload, payload_len);
        }
        ni->current_slot->received_bytes = fr.received_bytes;
        if (complete && ni->current_slot->is_field) {
            /* An interlaced frame is complete when BOTH DISTINCT fields
             * have arrived -- not when field 1 arrives. GroovyMAME sends its
             * two fields under TWO consecutive frame numbers, and nothing
             * guarantees that the pair starts with field 0. A mask, not a
             * counter: a duplicated field must not let a half-populated
             * frame through. */
            ni->slot_fields_mask |= (uint8_t)(1u << ni->current_slot->field);
            if (ni->slot_fields_mask != 0x3u) {
                complete = 0;
            } else {
                /* A pair of DISTINCT fields has just been completed. Counts
                 * every interlaced pair, 0x06 as well as 0x07 -- the
                 * denominator of the openings counted further down. */
                ni->field_pairs_win++;
                ni->field_pairs_total++;
            }
        }
        if (complete && !blind_lets_through(ni)) {
            /* Blind: the frame is not displayed. */
            ni->current_slot->received_bytes = 0;
            ni->current_slot                 = NULL;
            break;
        }
        if (complete) {
            /* Hand slot to blit thread; spsc_try_produce returns 1 on enqueue,
             * 0 if the ring is full (blit thread fell behind). */
            resume_after_idle(ni);   /* BEFORE the image is queued */
            ni->current_slot->ready_ns = now_ns();  /* latency point B t1 */
            if (spsc_try_produce(ni->cfg.shared->spsc_ring, ni->current_slot) == 1) {
                wake_blit(ni);   /* no-op when the option is off */
                /* Update ACK echo fields so the next ACK back to the emitter
                 * carries this frame's identifiers. */
                atomic_store(&ni->cfg.shared->ack_frame_echo,  hdr.frame_id);
                atomic_store(&ni->cfg.shared->ack_vcount_echo, hdr.vsync);
                /* Image displayed; this path is raw (the compressed one
                 * leaves further up), to be confirmed as in the
                 * continuation guard. */
                ni->has_picture         = 1;
                ni->raw_to_confirm = 1;
                ni->current_slot_idx = (ni->current_slot_idx + 1U) % ni->cfg.shared->slot_capacity;
                ni->current_slot     = NULL;
            } else {
                /* SPSC full -- drop frame defensively, reset slot for reuse. */
                fprintf(stderr,
                        "{\"event\":\"spsc_full_dropped\",\"frame\":%u}\n",
                        hdr.frame_id);
                ni->current_slot->received_bytes = 0;
                ni->current_slot                 = NULL;
            }
        }
        break;
    }
    case CMD_CLOSE:
        /* Emitter is gracefully shutting down. Clear emitter state so the
         * watchdog doesn't fire heartbeat_lost on the next idle window, but
         * KEEP the daemon running -- it should remain ready for the next
         * emitter session without a full daemon restart cycle. The original
         * implementation set running=0, which made `Stop-Process
         * mame` simultaneously tear down the daemon and the modeset, falling
         * the CRT back to the kernel console. Found on the first
         * hardware-in-the-loop session. */
        /* This case used to log nothing. Yet it resets audio
         * (audio_rate_code/audio_channels) with no log line saying so -- a
         * CMD_CLOSE received in the middle of a real GroovyMAME session,
         * followed by no re-announcing CMD_INIT, would explain a permanent
         * silence while video goes on (the video flags do not depend on
         * audio_rate_code). Without this line, that hypothesis could not be
         * checked from the receiver's log. */
        fprintf(stderr,
                "{\"event\":\"cmd_close_received\",\"had_audio\":%d,"
                "\"had_padding\":%d}\n",
                (ni->audio_rate_code != 0u && ni->audio_channels != 0u) ? 1 : 0,
                ni->padded ? 1 : 0);
        ni->last_emitter_len = 0;
        ni->audio_rate_code = 0u;   /* a new session does not inherit */
        ni->audio_channels  = 0u;   /* the audio permission of the previous one. */
        /* Nor does a new session inherit the mode of the previous one --
         * same reasoning as above, pure transition called, never
         * rewritten. */
        (void)proto_session_on_close(&ni->padded);
        /* The session is over, the next one starts cold -- without a
         * picture, and without inherited blindness. The mute set on entry
         * holds: audio_rate_code has just dropped to 0, and the next
         * CMD_INIT configures the output. */
        ni->has_picture         = 0;
        ni->raw_to_confirm = 0;
        ni->blind_entry_cause    = NULL;
        ni->sight   = (struct proto_sight){0};
        ni->garbage_guard = (struct proto_guard){0};
        /* A band switch held back by the rule does not outlive the session
         * that requested it. Without an emitter, it would go at its deadline
         * (up to 60 s later) and put the tube in 31 kHz for nothing. The last
         * published request differs from the current regime: publish "stay
         * in the current regime" -- the same cancellation as
         * regime_publish_needed (see CMD_SWITCHRES): the blit thread latches
         * it (the last one wins), its "want == have" branch does nothing and
         * lifts the wait. Returning to 15 kHz at the end of a session is the
         * idle return's job, not this one. */
        {
            const uint8_t have_now = atomic_load(&ni->cfg.shared->current_regime);
            if (ni->last_published_regime != have_now) {
                const uint8_t was_published = ni->last_published_regime;
                struct sr_mode_request *r = &ni->cfg.shared->request;
                r->regime = have_now;
                r->monotonic_ns_received = now_ns();
                uint64_t prev = atomic_load_explicit(&ni->cfg.shared->switchres_seq,
                                                     memory_order_relaxed);
                atomic_store_explicit(&ni->cfg.shared->switchres_seq, prev + 1ULL,
                                      memory_order_release);
                ni->last_published_regime = have_now;
                fprintf(stderr,
                        "{\"event\":\"cmd_close_regime_cancelled\","
                        "\"from\":\"%s\",\"to\":\"%s\"}\n",
                        regime_name(was_published), regime_name(have_now));
            }
        }
        if (ni->is_blind) {
            fprintf(stderr,
                    "{\"event\":\"receiver_blind_closed\",\"cause\":\"%s\","
                    "\"blind_ms\":%llu,\"audio_blocks_muted\":%llu,"
                    "\"frames_dropped\":%llu}\n",
                    ni->blind_cause ? ni->blind_cause : "",
                    (unsigned long long)((now_ns() - ni->blind_since_ns) / 1000000ULL),
                    (unsigned long long)ni->blind_blocks_muted,
                    (unsigned long long)ni->blind_frames_dropped);
            ni->is_blind                  = 0;
            ni->switchres_since_blind = 0;
            ni->blind_cause            = NULL;
        }
        break;
    case CMD_AUDIO: {
        /* Opening of an audio block.
         *
         * A video frame still in flight at this point is necessarily stuck --
         * the emitter cannot slip sound into the middle of a frame. The
         * escape guard above has already abandoned it; what remains here is
         * belt and braces. */
        cmd_audio_t ah;
        if (parse_cmd_audio(buf, len, &ah) != PARSE_OK)
            break;

        /* A size impossible for the session (no sound announced, empty
         * block, samples cut in half) is refused and counted: NO block
         * opened. A false block of odd size shifted left and right until
         * restart (pcm.c). */
        if (!proto_audio_size_ok(ah.size, ni->audio_rate_code, ni->audio_channels)) {
            ni->garbage_absurd_win++;
            /* The garbage safeguard, EXCEPT for the empty block: GroovyMAME
             * 0.289 sends samples_this_frame * channels * 2 bytes without
             * ever testing for zero (drawnogpu.cpp,
             * renderer_nogpu::add_audio_to_recording), and MAME may render
             * zero samples on a frame. Refused and counted, it does not
             * blind. */
            if (ah.size != 0u)
                note_absurd(ni);
            break;
        }

        audio_abandon(ni, "new_block");   /* no-op if nothing was in progress */

        ni->audio_rx.frame_id       = 0u;
        ni->audio_rx.expected_bytes = ah.size;
        ni->audio_rx.received_bytes = 0u;
        ni->audio_rx.buf            = ni->audio_buf;
        ni->audio_active            = 1;
        break;
    }
    case CMD_BLIT_VSYNC: {
        /* Progressive BLIT path for 240p/288p/480p games. GroovyMAME 0.287
         * emits 0x06 here, not 0x07 (the first assumption was wrong --
         * corrected by a hardware-in-the-loop capture showing
         * `06 01 00 00 00 f1 00 00 00 00 00` len=11).
         *
         * Wire layout per psakhis/Groovy_MiSTer hps_linux/src/support/groovy/
         * groovy.cpp: 7 or 11 bytes total, NO payload. The header
         * packet stands alone -- payload chunks follow as pure-payload UDP
         * datagrams with no opcode prefix, routed to proto_reassemble_push by
         * the continuation guard at the top of handle_packet.
         *
         * Three similar lines are better than a premature abstraction: we
         * build a synthetic cmd_blit_field_vsync_base_t (field=0) and reuse
         * acquire_slot rather than introducing acquire_slot_progressive. */
        cmd_blit_vsync_base_t hdr_v;
        if (parse_cmd_blit_vsync(buf, len, &hdr_v) != PARSE_OK) {
            break;
        }
        const int i480 = (ni->announced_regime == (uint8_t)REGIME_480I);
        const uint8_t field_parity = (uint8_t)(i480 ? (hdr_v.frame_id & 1u) : 0u);

        cmd_blit_field_vsync_base_t hdr_synth = {
            .cmd      = CMD_BLIT_FIELD_VSYNC,
            .frame_id = hdr_v.frame_id,
            .field    = field_parity,
            .vsync    = hdr_v.vsync,
        };

        /* In 480i, GroovyMAME sends TWO half-height frames per image,
         * under two consecutive numbers, with no field byte -- the field is
         * the parity of the number. The frame_id_mismatch guard below would
         * drop the first field when the second arrives: it is
         * short-circuited when the slot in flight is waiting for exactly
         * that field.
         *
         * The missing parity alone is not enough -- it never checked that
         * hdr_v.frame_id is the IMMEDIATE SUCCESSOR of the field already
         * held. When a whole frame is lost (header AND payload -- a failure
         * mode already measured on this receiver, the RX FIFO overflow of
         * the network card), two NON-consecutive fields shared the same
         * missing parity and silently merged into a composite image. The
         * explicit adjacency (+1u, unsigned arithmetic well defined even when
         * frame_id wraps) restores the same guarantee as the 0x07 path, where
         * pairing is guaranteed by an IDENTICAL frame_id carried explicitly
         * on the wire. A non-consecutive frame then falls back to the
         * frame_id_mismatch path below, which logs and restarts cleanly --
         * at the cost of a lost pair rather than a silently corrupted image
         * (the same trade-off as the frame_id checks everywhere else in this
         * file). */
        int pair_continues = 0;
        if (i480 && ni->current_slot && ni->slot_fields_mask != 0u &&
            ((ni->slot_fields_mask >> field_parity) & 1u) == 0u &&
            hdr_v.frame_id == ni->current_slot->frame_id + 1u) {
            ni->current_slot->frame_id       = hdr_v.frame_id;
            ni->current_slot->last_vsync     = hdr_v.vsync;
            ni->current_slot->received_bytes = 0;
            pair_continues = 1;
        }

        /* Reset partial slot if frame_id moved on us mid-reassembly --
         * unless it is the expected continuation of an interlaced pair (above). */
        if (!pair_continues && ni->current_slot &&
            ni->current_slot->frame_id != hdr_synth.frame_id) {
            fprintf(stderr,
                    "{\"event\":\"frame_id_mismatch\",\"old\":%u,\"new\":%u,"
                    "\"received\":%u,\"expected\":%u}\n",
                    ni->current_slot->frame_id, hdr_synth.frame_id,
                    ni->current_slot->received_bytes,
                    ni->current_slot->expected_bytes);
            if (ni->current_slot->received_bytes > 0)
                note_reject(ni);   /* a frame lost with bytes received */
            ni->current_slot->received_bytes = 0;
            ni->current_slot                 = NULL;
        }
        if (!ni->current_slot) {
            acquire_slot(ni, &hdr_synth);
            /* This new slot OPENS a pair. `field_parity` is hdr_v.frame_id & 1 here
             * (set at the top of this case), hence exactly the opening parity.
             * Counted after acquire_slot so that only acquisitions that
             * really succeeded are counted. */
            if (i480 && ni->current_slot) {
                if (field_parity) {
                    ni->field_open_p1_win++;
                    ni->field_open_p1_total++;
                } else {
                    ni->field_open_p0_win++;
                    ni->field_open_p0_total++;
                }
            }
        }
        /* is_field/field reflect the ANNOUNCED regime, not a frozen zero --
         * same lesson as the defect of the image squeezed into the top half:
         * these two fields must be set BEFORE the 'break' of the compressed
         * branch below, since slot_decompress() reads them. */
        ni->current_slot->is_field = (uint8_t)(i480 ? 1 : 0);
        ni->current_slot->field    = field_parity;
        if (ni->compression) {
            /* len == 11 carries the compressed size in [7..10]; len == 7 in
             * a compressed session makes no sense and lands here as zero, so
             * arm_compression refuses it. Re-armed on EVERY header, including
             * for the second field of a pair -- same rule as 0x07. */
            if (!arm_compression(ni, hdr_v.lz4_size, 0u)) {
                ni->current_slot->received_bytes = 0;
                ni->current_slot                 = NULL;
                break;
            }
        }
        if (i480 && !ni->gm_interlaced_0x06_logged) {
            /* A single log line per entry into this path -- the thread
             * handles 60 headers per second, one line per frame would be
             * unusable. */
            fprintf(stderr,
                    "{\"event\":\"gm_interlaced_0x06\",\"frame\":%u,"
                    "\"field\":%u,\"expected\":%u}\n",
                    hdr_v.frame_id, (unsigned)field_parity,
                    ni->current_slot->expected_bytes);
            ni->gm_interlaced_0x06_logged = 1;
        }
        /* No payload in the header datagram -- payload chunks arrive as
         * pure-payload UDP packets and are routed to proto_reassemble_push
         * by the continuation guard above. Just prime the slot and
         * fall through to ACK emission (see post-switch / per-tick path). */
        break;
    }
    default:
        /* An unknown opcode no longer gets here -- the orphan guard,
         * further up, counted it (`unknown_opcode` of rx_garbage_1s) and
         * dropped it. The per-datagram `unknown_opcode` line that lived here
         * is gone with it. */
        break;
    }
}

/* ---------------------------------------------------------------------------
 * net_thread_main -- main loop. recvmmsg with 100 ms timeout (so the heartbeat
 * watchdog and shared->running poll progress even in silence), dispatch each
 * received datagram, drain any pending ACK, then check the watchdog.
 * Exits when shared->running == 0; closes socket and frees state.
 * ------------------------------------------------------------------------- */
static void *net_thread_main(void *arg)
{
    struct net_internal *ni = (struct net_internal *)arg;
    apply_realtime(ni->cfg.cpu_affinity, ni->cfg.sched_fifo_priority);
    ni->last_packet_ns = now_ns();
    /* The size that blindness restores. main() set it
     * (--width x --height x 3) before starting this thread. */
    ni->startup_size = atomic_load(&ni->cfg.shared->expected_frame_bytes);

    /* Heap-allocate the recvmmsg scratch (~2 MB total) instead of
     * sticking it on the thread stack. PREEMPT_RT setups sometimes shrink
     * the per-pthread stack (8 MB default but tunable down to 64 KB), and
     * any further scratch buffer would push us over.
     * One-shot calloc per thread; freed below before return. */
    struct mmsghdr     *msgs = calloc(RECVMMSG_BATCH, sizeof(*msgs));
    struct iovec       *iovs = calloc(RECVMMSG_BATCH, sizeof(*iovs));
    unsigned char     (*bufs)[MAX_DGRAM] = calloc(RECVMMSG_BATCH, sizeof(*bufs));
    struct sockaddr_in *srcs = calloc(RECVMMSG_BATCH, sizeof(*srcs));
    if (!msgs || !iovs || !bufs || !srcs) {
        fprintf(stderr, "{\"event\":\"net_scratch_alloc_failed\"}\n");
        free(msgs); free(iovs); free(bufs); free(srcs);
        /* Unpublish before close, like the normal exit. */
        atomic_store(&ni->cfg.shared->net_sock_fd, -1);
        close(ni->sock);
        net_stop_fatal(ni->cfg.shared);
        free(ni->lz4_in);
        free(ni->lz4_out);
        free(ni);
        return NULL;
    }

    /* Test hook, read once, honoured only under CRT_BRIDGE_NO_DRM and for
     * the exact value "1": simulates the death of the net thread on a
     * failure, through the same path as the real ones. */
    {
        const char *no_drm = getenv("CRT_BRIDGE_NO_DRM");
        const char *fail   = getenv("CRT_BRIDGE_TEST_FAIL_NET");
        if (no_drm && no_drm[0] && no_drm[0] != '0' && fail && strcmp(fail, "1") == 0) {
            fprintf(stderr, "{\"event\":\"net_test_fail\"}\n");
            free(msgs); free(iovs); free(bufs); free(srcs);
            /* Same exit as net_scratch_alloc_failed. */
            atomic_store(&ni->cfg.shared->net_sock_fd, -1);
            close(ni->sock);
            net_stop_fatal(ni->cfg.shared);
            free(ni->lz4_in);
            free(ni->lz4_out);
            free(ni);
            return NULL;
        }
    }

    /* Hot-path tidy: iov pointers/lengths never change, so set them
     * once before the loop. recvmmsg only rewrites msg_namelen and msg_flags
     * per call; we restore those each batch. */
    for (int i = 0; i < RECVMMSG_BATCH; i++) {
        iovs[i].iov_base           = bufs[i];
        iovs[i].iov_len            = MAX_DGRAM;
        msgs[i].msg_hdr.msg_iov    = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name   = &srcs[i];
    }

    while (atomic_load(&ni->cfg.shared->running)) {
        for (int i = 0; i < RECVMMSG_BATCH; i++) {
            msgs[i].msg_hdr.msg_namelen = sizeof(srcs[i]);
            msgs[i].msg_hdr.msg_flags   = 0;
            msgs[i].msg_hdr.msg_control = NULL;
            msgs[i].msg_hdr.msg_controllen = 0;
        }
        struct timespec to = {
            .tv_sec  = 0,
            .tv_nsec = (long)RECVMMSG_TIMEOUT_NS
        };
        int n = recvmmsg(ni->sock, msgs, RECVMMSG_BATCH, MSG_WAITFORONE, &to);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Timeout or signal -- fall through to ACK + heartbeat checks. */
            } else {
                fprintf(stderr, "{\"event\":\"recvmmsg_err\",\"errno\":%d}\n", errno);
                /* An error while nobody asked for a stop is fatal. After a
                 * SIGTERM, on_signal has already set running = 0 before
                 * closing the socket: no false fatal. */
                if (atomic_load(&ni->cfg.shared->running))
                    net_stop_fatal(ni->cfg.shared);
                break;
            }
        } else {
            for (int i = 0; i < n; i++) {
                handle_packet(ni,
                              bufs[i],
                              msgs[i].msg_len,
                              &srcs[i],
                              msgs[i].msg_hdr.msg_namelen);
                /* The entry decided during this datagram happens here,
                 * between two datagrams. */
                if (ni->blind_entry_cause)
                    enter_blind(ni);
            }
        }

        send_pending_ack(ni);
        /* Blind: a status with a null echo at least every
         * PROTO_BLIND_STATUS_MS, even without a complete frame (modelled on
         * the software client's maybe_status_nogeom). When nothing arrives
         * any more, the thread sleeps up to RECVMMSG_TIMEOUT_NS (100 ms,
         * SO_RCVTIMEO) in recvmmsg: waiting for the whole period before
         * sending left up to 350 ms between two statuses. The status
         * therefore goes out as soon as the next wake-up could exceed the
         * period: 150 to 250 ms apart, plus processing time. */
        _Static_assert((uint64_t)PROTO_BLIND_STATUS_MS * 1000000ULL > RECVMMSG_TIMEOUT_NS,
                       "the blind status period must exceed the thread wake-up");
        if (ni->is_blind && ni->last_emitter_len != 0 &&
            now_ns() - ni->blind_status_ns + RECVMMSG_TIMEOUT_NS >=
                (uint64_t)PROTO_BLIND_STATUS_MS * 1000000ULL) {
            send_blind_status(ni);
        }
        audio_report(ni);
        field_report(ni);
        garbage_report(ni);
        audio_out_report(ni->cfg.shared->audio);

        /* Heartbeat watchdog -- fires once on transition healthy->lost.
         * Auto-recovers in handle_packet on the first packet after silence. */
        uint64_t age = now_ns() - ni->last_packet_ns;
        uint64_t threshold_ns = (uint64_t)(unsigned)ni->cfg.heartbeat_timeout_ms * 1000000ULL;
        if (!ni->heartbeat_lost && age > threshold_ns) {
            fprintf(stderr,
                    "{\"event\":\"heartbeat_lost\",\"age_ms\":%llu,\"timeout_ms\":%d}\n",
                    (unsigned long long)(age / 1000000ULL),
                    ni->cfg.heartbeat_timeout_ms);
            ni->heartbeat_lost = 1;
        }
    }

    /* Clear the published fd BEFORE close() so a late signal can't
     * shutdown() a freshly-closed (or recycled) descriptor. */
    atomic_store(&ni->cfg.shared->net_sock_fd, -1);
    close(ni->sock);
    free(msgs);
    free(iovs);
    free(bufs);
    free(srcs);
    /* The LZ4 buffers live with the thread, they die with it. */
    free(ni->lz4_in);
    free(ni->lz4_out);
    free(ni);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * net_thread_start -- public entry point. Validates config, opens socket,
 * spawns the pthread. Returns 0 on success / -1 on any failure (caller can
 * inspect stderr for the structured failure reason).
 * ------------------------------------------------------------------------- */
int net_thread_start(struct net_thread_config *cfg, pthread_t *out_tid)
{
    if (!cfg || !cfg->shared || !out_tid) return -1;

    struct net_internal *ni = calloc(1, sizeof *ni);
    /* The two buffers of the LZ4 path. Allocated once for the life of the
     * thread: the hot path never allocates. lz4_in receives the compressed
     * block, lz4_out the linear output of a 480i field before interleaving.
     * Their absence is not fatal -- it disarms compression, and
     * arm_compression will refuse frames rather than write nowhere. */
    if (ni) {
        ni->lz4_in  = malloc(MAX_FRAME_BYTES);
        ni->lz4_out = malloc(MAX_FRAME_BYTES);
        if (!ni->lz4_in || !ni->lz4_out) {
            fprintf(stderr, "{\"event\":\"lz4_buffers_alloc_failed\"}\n");
            free(ni->lz4_in);  ni->lz4_in  = NULL;
            free(ni->lz4_out); ni->lz4_out = NULL;
        }
    }
    if (!ni) return -1;
    ni->cfg = *cfg;
    /* The tube starts in 240p (main.c sets current_regime); nothing has been
     * published yet, the last "published" request is the startup. */
    ni->last_published_regime = (uint8_t)REGIME_240P_SUPER_RES;
    ni->announced_serve       = (uint8_t)REGIME_240P_SUPER_RES;   /* idem */

    ni->sock = setup_socket(cfg->listen_port, cfg->busy_poll_us);
    if (ni->sock < 0) {
        free(ni->lz4_in);    /* leave nothing behind */
        free(ni->lz4_out);
        free(ni);
        return -1;
    }

    /* Publish fd so on_signal() can shutdown() the socket
     * to unblock recvmmsg on SIGTERM. Net thread retains sole ownership of
     * close(); handler only calls shutdown() (idempotent, AS-Safe). */
    atomic_store(&cfg->shared->net_sock_fd, ni->sock);

    /* Do NOT set running = 1 again here: main() sets it before starting the
     * threads. Setting it again erased a stop already requested -- by the
     * blit thread on a display failure, or by a SIGTERM received during
     * startup -- and the daemon stayed alive without a display. */

    if (pthread_create(out_tid, NULL, net_thread_main, ni) != 0) {
        fprintf(stderr, "{\"event\":\"pthread_create_failed\",\"errno\":%d}\n", errno);
        atomic_store(&cfg->shared->net_sock_fd, -1);
        close(ni->sock);
        free(ni->lz4_in);
        free(ni->lz4_out);
        free(ni);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * net_thread_request_stop -- signal exit; thread observes within one
 * recvmmsg timeout (100 ms) plus current-batch processing time.
 * ------------------------------------------------------------------------- */
void net_thread_request_stop(struct net_blit_shared *shared)
{
    if (shared) atomic_store(&shared->running, 0);
}
