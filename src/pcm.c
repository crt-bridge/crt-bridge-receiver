/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pcm.c -- the receiver's sound output, by direct ioctl on the kernel's PCM
 * device. See pcm.h for why, and for the start-up guarantee.
 *
 * The opening sequence is that of any ALSA client, without the library:
 * open, HW_PARAMS, SW_PARAMS, PREPARE, then write() calls.
 *
 * The detail that is not obvious when reading `struct snd_pcm_hw_params` for
 * the first time: it is not a structure to fill in field by field but a set
 * of CONSTRAINTS to refine. Start from "everything is allowed" -- every bit
 * of the masks set, every interval from 0 to infinity -- then narrow what
 * matters; the kernel chooses the rest and returns its choice in the same
 * structure. This is what libasound does, and tinyalsa before us.
 */

#define _GNU_SOURCE

#include "pcm.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <sound/asound.h>

#define PCM_DEFAULT_DEVICE   "/dev/snd/pcmC0D0p"

/* The HARDWARE buffer has one role only: to cover the scheduling jitter of
 * the audio thread. On a PREEMPT_RT kernel with this thread at SCHED_FIFO, a
 * few milliseconds are enough. 128 frames at 44100 Hz are 2.9 ms; four
 * periods are 11.6 ms. Anything more would be pure delay. */
#define PCM_PERIOD_FRAMES    128u
#define PCM_PERIODS          4u

/* --- The priming cushion, and why it exists -------------------------------
 *
 * The emitter does not deliver sound continuously: the core returns ALL the
 * sound of a frame in a single block, once per video frame. At 60 Hz that is
 * a burst of 2940 bytes every 16.7 ms, while the sound card consumes
 * steadily, period by period.
 *
 * Without a cushion, the fill level therefore swings between 16.7 ms and
 * ZERO, and the audio thread completes with silence at every trough.
 * Measured on the bench with no cushion: 33% of the periods completed with
 * silence, 14% dropped at the high threshold, fill min 0.0 ms max 16.7 ms --
 * the sawtooth shape of the burst, read as is in the log.
 *
 * So the cushion must be worth at least one burst, with a margin for network
 * jitter. 26 ms is a burst and a half: the trough settles near 9 ms instead
 * of zero. It is the smallest defensible value, and it is pure delay added
 * to the sound -- hence the refusal to be generous here.
 *
 * The high threshold serves only the clock drift, not the burst: it is far
 * enough never to fire in normal operation. */
#define AUDIO_PRIME_MS       26u
#define AUDIO_HIGH_MS        60u

/* Idle. Without it, the audio thread keeps writing silence to the card
 * forever after the emitter stops: the device stays open, the thread wakes
 * the core 344 times a second for nothing, and the log gains one line per
 * second in a journal that lives in a tmpfs on a diskless machine. Seen on
 * the bench: the emitter stopped for several minutes, the daemon still
 * writing.
 *
 * Two seconds of complete silence are enough to conclude the game is over.
 * Resuming is automatic at the next block, at the cost of a new priming --
 * 26 ms, imperceptible at the start of a session. */
#define AUDIO_IDLE_NS        2000000000ULL

/* --- Fades at the edges of silence -----------------------------------------
 *
 * A click is not noise, it is a DISCONTINUITY. When the buffer runs short
 * and the period is completed with silence, the waveform jumps from its
 * current value to zero in one sample; when data comes back, it jumps again
 * from zero to its value. Two jumps, two clicks, for every trough -- and
 * there is one at the very first sound too, when the priming silence gives
 * way to the waveform.
 *
 * They were heard on the bench as crackles at the start of a game. The
 * counters showed six underruns in the first seconds, then none -- the
 * emitter does not deliver steadily yet while RetroArch starts the CD.
 *
 * Ramping down to zero over 64 frames instead of a single sample is enough
 * to make them disappear. 64 frames at 44100 Hz are 1.45 ms: too short to be
 * heard as a change of level, long enough that there is no discontinuity
 * any more.
 *
 * The fade is armed ONLY where silence was already being written. In normal
 * operation -- zero underruns over whole minutes -- it never runs and costs
 * nothing. */
#define FADE_FRAMES          64u

/* --- Splicing after a drift correction ---------------------------------------
 *
 * Correcting the drift means dropping sound, hence splicing the waveform
 * wherever it stands. That is a discontinuity, exactly like the abrupt
 * silence was, and only the other edge had been handled.
 *
 * Measured on the bench, Chrono Cross, 138 seconds: the emitter delivers
 * perfectly steadily -- 61 to 64 blocks per second, no second with a gap --
 * and there was only ONE second with underruns. What was heard was
 * therefore neither a hiccup of the emitter nor an empty buffer, but these
 * splices: 1.8 per second on average, MAXIMUM 14, relative spread 0.69. In
 * bursts, not at a fixed interval -- as it sounded.
 *
 * 128 frames are 2.9 ms, the exact length of the dropped period. */
#define SPLICE_FRAMES        128u

/* Byte ring buffer between the network thread and the audio thread.
 * 64 KiB are 370 ms at 44100 Hz stereo: far more than needed, but memory is
 * free on this machine and a wide buffer avoids losing sound during the
 * scheduler's rare pauses. A power of two: the mask replaces the modulo. */
#define RING_BYTES           (64u * 1024u)
#define RING_MASK            (RING_BYTES - 1u)

/* ------------------------------------------------------------------------- */

struct audio_out {
    /* --- Ring buffer, one producer and one consumer --- */
    uint8_t          ring[RING_BYTES];
    _Atomic uint32_t head;          /* written by the network thread */
    _Atomic uint32_t tail;          /* written by the audio thread */

    /* --- Configuration announced by CMD_INIT, written by the network thread --- */
    _Atomic uint32_t want_rate;     /* 0 = muted */
    _Atomic uint32_t want_channels;

    /* --- Device state, private to the audio thread --- */
    char             device[128];
    int              fd;
    unsigned         open_rate;
    unsigned         open_channels;
    unsigned         frame_bytes;
    unsigned         period_bytes;
    unsigned         prime_bytes;    /* cushion to rebuild before playing */
    unsigned         high_bytes;     /* high threshold of strategy A */
    int              primed;         /* 0 = filling the cushion, in silence */
    int              in_silence;     /* 1 = the output is already at zero */
    int              splice_pending; /* 1 = a period has just been dropped */
    char             dump_path[256]; /* empty = no recording */
    int              dump_fd;
    uint8_t          last_frame[4];  /* last real frame, for the fade */
    uint64_t         next_open_ns;  /* no reopening before this time */
    int              open_failed_logged;

    /* --- Counters. Written by the audio thread, read by the network thread
     * for the log. A counter one byte late has no consequence for a
     * measurement: they are atomic for ownership, not for consistency with
     * each other. --- */
    _Atomic uint64_t underruns;     /* periods completed with silence */
    _Atomic uint64_t overruns;      /* periods dropped, high threshold reached */
    _Atomic uint64_t dropped_push;  /* bytes lost, buffer full on write */
    _Atomic uint64_t written;       /* periods written to the device */
    _Atomic uint64_t bytes_in;      /* bytes deposited by the network thread */

    /* Peak level over the second, as an absolute sample value, from 0 to
     * 32767. Without it, silence at the receiver's output stays ambiguous: is
     * nothing arriving, or did the codec volume stay at zero as it was before
     * hda-volume ran? This number settles it without leaving the log. */
    _Atomic uint32_t peak;
    /* Reference points set at priming time: the drift is measured over the
     * whole playback, not over one second. One second is far too short a
     * window -- the two counters are read at slightly different instants and
     * the number jumps from -4000 to +21000 ppm from one line to the next,
     * which means nothing. Measured on the bench before switching to the
     * cumulative figure. */
    _Atomic uint64_t base_bytes_in;
    _Atomic uint64_t base_written;
    _Atomic uint64_t base_ns;
    _Atomic uint64_t xruns;         /* EPIPE returned by the kernel */
    _Atomic uint32_t fill_min;      /* fill level in bytes, over the second */
    _Atomic uint32_t fill_max;
    _Atomic uint64_t fill_sum;
    _Atomic uint32_t fill_n;

    /* --- Life cycle --- */
    _Atomic int      running;
    pthread_t        tid;
    int              thread_started;
    int              cpu_affinity;
    int              prio;

    /* --- Log, private to the network thread --- */
    uint64_t         log_ns;
    uint64_t         last_written;
    uint64_t         last_bytes_in;
    uint64_t         last_log_ns;
};

/* ------------------------------------------------------------------------- */

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint32_t ring_used(const struct audio_out *a)
{
    uint32_t h = atomic_load_explicit(&a->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&a->tail, memory_order_relaxed);
    return h - t;                    /* unsigned arithmetic: the wrap-around is correct */
}

/* ---------------------------------------------------------------------------
 * Setting the hw_params constraints. See the file header: start from
 * "everything is allowed", then narrow.
 * ------------------------------------------------------------------------- */
static struct snd_mask *param_mask(struct snd_pcm_hw_params *p, int n)
{
    return &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

static struct snd_interval *param_interval(struct snd_pcm_hw_params *p, int n)
{
    return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static void params_any(struct snd_pcm_hw_params *p)
{
    memset(p, 0, sizeof(*p));
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK;
         n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++) {
        struct snd_mask *m = param_mask(p, n);
        memset(m->bits, 0xFF, sizeof(m->bits));
    }
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
         n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
        struct snd_interval *i = param_interval(p, n);
        i->min = 0u;
        i->max = ~0u;
    }
    p->rmask = ~0u;
    p->cmask = 0u;
    p->info  = ~0u;
}

static void params_set_mask(struct snd_pcm_hw_params *p, int n, unsigned bit)
{
    struct snd_mask *m = param_mask(p, n);
    memset(m->bits, 0, sizeof(m->bits));
    m->bits[bit >> 5] = 1u << (bit & 31u);
}

static void params_set_int(struct snd_pcm_hw_params *p, int n, unsigned val)
{
    struct snd_interval *i = param_interval(p, n);
    i->min     = val;
    i->max     = val;
    i->integer = 1;
    i->openmin = 0;
    i->openmax = 0;
}

static unsigned params_get_int(struct snd_pcm_hw_params *p, int n)
{
    return param_interval(p, n)->min;
}

/* ---------------------------------------------------------------------------
 * fade_in -- a rising ramp from 0 to 1 over the first `fade` frames.
 *
 * Applied to the real data, in place, when coming out of silence. Frame 0 is
 * set to zero and the ramp reaches its full value at index `fade` -- which
 * is not touched. Integer multiplication: no floating point on a real-time
 * thread.
 * ------------------------------------------------------------------------- */
static void fade_in(uint8_t *buf, unsigned frame_bytes, unsigned fade)
{
    if (!buf || fade == 0u || frame_bytes == 0u) return;
    unsigned nch = frame_bytes / 2u;
    for (unsigned i = 0u; i < fade; i++) {
        for (unsigned c = 0u; c < nch; c++) {
            size_t off = (size_t)i * frame_bytes + (size_t)c * 2u;
            int16_t v;
            memcpy(&v, buf + off, sizeof v);
            v = (int16_t)(((int32_t)v * (int32_t)i) / (int32_t)fade);
            memcpy(buf + off, &v, sizeof v);
        }
    }
}

/* ---------------------------------------------------------------------------
 * fade_out -- writes `fade` frames of a falling ramp from `from` down to zero.
 *
 * Applied where silence is about to be written: `from` is the last real
 * frame played, and the ramp starts from it and ends at zero. The frame
 * right after the ramp is therefore zero, and the silence can go on without
 * a discontinuity.
 * ------------------------------------------------------------------------- */
static void fade_out(uint8_t *buf, const uint8_t *from,
                     unsigned frame_bytes, unsigned fade)
{
    if (!buf || !from || fade == 0u || frame_bytes == 0u) return;
    unsigned nch = frame_bytes / 2u;
    for (unsigned i = 0u; i < fade; i++) {
        for (unsigned c = 0u; c < nch; c++) {
            int16_t v;
            memcpy(&v, from + (size_t)c * 2u, sizeof v);
            v = (int16_t)(((int32_t)v * (int32_t)(fade - i)) / (int32_t)fade);
            memcpy(buf + (size_t)i * frame_bytes + (size_t)c * 2u, &v, sizeof v);
        }
    }
}

/* ---------------------------------------------------------------------------
 * fade_splice -- a crossfade from `from` into the signal, over `fade` frames.
 *
 * Applied where sound was dropped to correct the drift: `from` is the last
 * frame actually played, and the incoming signal comes from elsewhere in
 * the stream. Without a fade, the jump between the two is a click; with it,
 * the transition spreads over 2.9 ms and is no longer heard.
 *
 * 32-bit integer arithmetic: the largest product is 32768 x 128, very far
 * from overflow. No floating point on a real-time thread.
 * ------------------------------------------------------------------------- */
static void fade_splice(uint8_t *buf, const uint8_t *from,
                        unsigned frame_bytes, unsigned fade)
{
    if (!buf || !from || fade == 0u || frame_bytes == 0u) return;
    unsigned nch = frame_bytes / 2u;
    for (unsigned i = 0u; i < fade; i++) {
        for (unsigned c = 0u; c < nch; c++) {
            size_t off = (size_t)i * frame_bytes + (size_t)c * 2u;
            int16_t new_sample, old_sample;
            memcpy(&new_sample, buf + off, sizeof new_sample);
            memcpy(&old_sample,  from + (size_t)c * 2u, sizeof old_sample);
            int32_t v = ((int32_t)old_sample  * (int32_t)(fade - i)
                       + (int32_t)new_sample * (int32_t)i) / (int32_t)fade;
            int16_t r = (int16_t)v;
            memcpy(buf + off, &r, sizeof r);
        }
    }
}

/* ---------------------------------------------------------------------------
 * pcm_open -- opens and configures the device. Returns 0 or -1.
 * Any error is logged only once and leaves the daemon alive.
 * ------------------------------------------------------------------------- */
static int pcm_open(struct audio_out *a, unsigned rate, unsigned channels)
{
    int fd = open(a->device, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        if (!a->open_failed_logged) {
            fprintf(stderr,
                    "{\"event\":\"audio_open_failed\",\"device\":\"%s\",\"errno\":%d}\n",
                    a->device, errno);
            a->open_failed_logged = 1;
        }
        return -1;
    }

    struct snd_pcm_hw_params hw;
    params_any(&hw);
    params_set_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS,
                    SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    params_set_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT,
                    SNDRV_PCM_FORMAT_S16_LE);
    params_set_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT,
                    SNDRV_PCM_SUBFORMAT_STD);
    params_set_int(&hw, SNDRV_PCM_HW_PARAM_SAMPLE_BITS,  16u);
    params_set_int(&hw, SNDRV_PCM_HW_PARAM_FRAME_BITS,   16u * channels);
    params_set_int(&hw, SNDRV_PCM_HW_PARAM_CHANNELS,     channels);
    params_set_int(&hw, SNDRV_PCM_HW_PARAM_RATE,         rate);
    params_set_int(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,  PCM_PERIOD_FRAMES);
    params_set_int(&hw, SNDRV_PCM_HW_PARAM_PERIODS,      PCM_PERIODS);

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
        if (!a->open_failed_logged) {
            fprintf(stderr,
                    "{\"event\":\"audio_hw_params_failed\",\"errno\":%d,"
                    "\"rate\":%u,\"channels\":%u}\n",
                    errno, rate, channels);
            a->open_failed_logged = 1;
        }
        close(fd);
        return -1;
    }

    unsigned period_frames = params_get_int(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
    unsigned buffer_frames = params_get_int(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
    if (period_frames == 0u || buffer_frames == 0u) {
        close(fd);
        return -1;
    }

    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode       = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step       = 1u;
    sw.avail_min         = period_frames;
    /* Start as soon as two periods are in the buffer: enough not to start
     * dry, little enough that the sound does not take on a fixed delay. */
    sw.start_threshold   = (buffer_frames >= 2u * period_frames)
                         ? 2u * period_frames : period_frames;
    /* The kernel must NOT stop the stream on an underrun: we handle it
     * ourselves by completing with silence. A huge threshold disarms the
     * automatic stop. */
    sw.stop_threshold    = buffer_frames * 16u;
    sw.silence_threshold = 0u;
    sw.silence_size      = 0u;
    sw.boundary          = buffer_frames;
    while (sw.boundary * 2u < 0x7fffffffU)
        sw.boundary *= 2u;
    /* DIAGNOSTIC: make the kernel talk. With stop_threshold at boundary it
     * NEVER reports an underrun -- it replays whatever lingers in its
     * buffer, which crackles without anything counting it. At buffer_size, a
     * real shortage returns EPIPE, which pcm_write_period counts and repairs.
     * The xruns counter becomes a measurement again. */
    sw.stop_threshold    = buffer_frames;

    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        if (!a->open_failed_logged) {
            fprintf(stderr,
                    "{\"event\":\"audio_sw_params_failed\",\"errno\":%d}\n", errno);
            a->open_failed_logged = 1;
        }
        close(fd);
        return -1;
    }

    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        if (!a->open_failed_logged) {
            fprintf(stderr,
                    "{\"event\":\"audio_prepare_failed\",\"errno\":%d}\n", errno);
            a->open_failed_logged = 1;
        }
        close(fd);
        return -1;
    }

    a->fd            = fd;
    a->open_rate     = rate;
    a->open_channels = channels;
    a->frame_bytes   = 2u * channels;
    a->period_bytes  = period_frames * a->frame_bytes;
    a->prime_bytes   = (rate * a->frame_bytes / 1000u) * AUDIO_PRIME_MS;
    a->high_bytes    = (rate * a->frame_bytes / 1000u) * AUDIO_HIGH_MS;
    if (a->high_bytes > RING_BYTES / 2u) a->high_bytes = RING_BYTES / 2u;
    a->primed        = 0;
    /* The device starts from silence: the first real period will therefore
     * be faded in, and the very first sound does not pop. */
    a->in_silence     = 1;
    a->splice_pending = 0;

    if (a->dump_path[0] && a->dump_fd < 0) {
        a->dump_fd = open(a->dump_path,
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        fprintf(stderr,
                "{\"event\":\"audio_dump\",\"path\":\"%s\",\"fd\":%d,"
                "\"rate\":%u,\"channels\":%u}\n",
                a->dump_path, a->dump_fd, rate, channels);
    }
    memset(a->last_frame, 0, sizeof a->last_frame);
    a->open_failed_logged = 0;

    fprintf(stderr,
            "{\"event\":\"audio_device_opened\",\"device\":\"%s\",\"rate\":%u,"
            "\"channels\":%u,\"period_frames\":%u,\"buffer_frames\":%u,"
            "\"buffer_ms\":%.1f,\"prime_ms\":%u,\"high_ms\":%u}\n",
            a->device, rate, channels, period_frames, buffer_frames,
            1000.0 * (double)buffer_frames / (double)rate,
            AUDIO_PRIME_MS, AUDIO_HIGH_MS);
    return 0;
}

static void pcm_close(struct audio_out *a)
{
    if (a->fd >= 0) {
        ioctl(a->fd, SNDRV_PCM_IOCTL_DROP);
        close(a->fd);
        a->fd = -1;
        fprintf(stderr, "{\"event\":\"audio_device_closed\"}\n");
    }
}

/* ---------------------------------------------------------------------------
 * pcm_write_period -- writes exactly one period, handling EPIPE.
 *
 * `write()` on a PCM device blocks until there is room: it is what paces the
 * audio thread on the sound card's crystal, and that is intended. No busy
 * wait, no timer to tune.
 * ------------------------------------------------------------------------- */
static int pcm_write_period(struct audio_out *a, const uint8_t *buf,
                            unsigned frames)
{
    struct snd_xferi xfer;
    xfer.buf    = (void *)(uintptr_t)buf;
    xfer.frames = frames;
    xfer.result = 0;

    for (int write_try = 0; write_try < 2; write_try++) {
        if (ioctl(a->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer) == 0)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno == EPIPE || errno == ESTRPIPE) {
            /* An underrun on the kernel side despite our precautions, or a
             * resume after suspend. Prepare again and retry once. */
            atomic_fetch_add(&a->xruns, 1u);
            if (ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE) < 0)
                return -1;
            continue;
        }
        return -1;
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * Audio thread.
 * ------------------------------------------------------------------------- */
static void apply_rt(int cpu, int prio)
{
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET((size_t)cpu, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
            fprintf(stderr, "{\"event\":\"audio_affinity_failed\",\"cpu\":%d}\n", cpu);
    }
    if (prio > 0) {
        struct sched_param sp;
        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = prio;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
            fprintf(stderr,
                    "{\"event\":\"audio_sched_fifo_failed\",\"prio\":%d,\"errno\":%d}\n",
                    prio, errno);
    }
}

static void *audio_thread(void *arg)
{
    struct audio_out *a = (struct audio_out *)arg;
    apply_rt(a->cpu_affinity, a->prio);

    /* Work buffer: one period in the largest possible format. */
    static const unsigned MAX_PERIOD_BYTES = PCM_PERIOD_FRAMES * 2u * 2u;
    uint8_t period_buf[PCM_PERIOD_FRAMES * 2u * 2u];

    /* The last time the buffer held something. Private to the thread. */
    uint64_t last_data_ns = mono_ns();

    while (atomic_load(&a->running)) {
        unsigned rate = atomic_load(&a->want_rate);
        unsigned ch   = atomic_load(&a->want_channels);

        /* Muted, or not announced yet: sleep briefly and come back. The
         * device stays closed as long as nothing arrives -- this is the
         * start-up guarantee of pcm.h. */
        if (rate == 0u || ch == 0u || ch > 2u) {
            if (a->fd >= 0) pcm_close(a);
            /* Drain: otherwise the buffer fills with sound nobody will play,
             * and the next opening would start hundreds of milliseconds late
             * right away. Measured on the bench: three megabytes lost on
             * write between a restart of the daemon and that of the emitter,
             * for lack of this line. */
            atomic_store(&a->tail, atomic_load(&a->head));
            struct timespec t = { .tv_sec = 0, .tv_nsec = 20000000L };
            nanosleep(&t, NULL);
            continue;
        }

        /* The configuration changed: close, to reopen in the right format. */
        if (a->fd >= 0 && (rate != a->open_rate || ch != a->open_channels))
            pcm_close(a);

        if (a->fd < 0) {
            /* Nothing to play: no point opening. */
            if (ring_used(a) == 0u) {
                struct timespec t = { .tv_sec = 0, .tv_nsec = 20000000L };
                nanosleep(&t, NULL);
                continue;
            }
            /* A failed opening must not be retried in a tight loop. */
            uint64_t now = mono_ns();
            if (now < a->next_open_ns) {
                struct timespec t = { .tv_sec = 0, .tv_nsec = 50000000L };
                nanosleep(&t, NULL);
                continue;
            }
            if (pcm_open(a, rate, ch) != 0) {
                a->next_open_ns = now + 5000000000ULL;   /* five seconds */
                /* Sound keeps arriving: drain, so as not to keep a buffer
                 * full of stale sound. */
                atomic_store(&a->tail, atomic_load(&a->head));
                continue;
            }
        }

        unsigned pb = a->period_bytes;
        if (pb == 0u || pb > MAX_PERIOD_BYTES) { pcm_close(a); continue; }

        uint32_t used = ring_used(a);
        if (used > 0u)
            last_data_ns = mono_ns();

        /* --- Idle: nothing for two seconds, release the card. --- */
        if (used == 0u && mono_ns() - last_data_ns > AUDIO_IDLE_NS) {
            fprintf(stderr, "{\"event\":\"audio_idle\"}\n");
            pcm_close(a);
            a->primed = 0;
            continue;
        }

        /* --- Priming. Until the cushion is built, feed the card with silence
         * WITHOUT consuming: the buffer fills at the pace of the emitter's
         * bursts. This is what keeps the fill level from touching zero at
         * every trough between bursts. --- */
        if (!a->primed) {
            if (used < a->prime_bytes) {
                memset(period_buf, 0, pb);
                if (pcm_write_period(a, period_buf, pb / a->frame_bytes) != 0) {
                    pcm_close(a);
                    a->next_open_ns = mono_ns() + 1000000000ULL;
                }
                continue;
            }
            a->primed = 1;
            atomic_store(&a->base_bytes_in, atomic_load(&a->bytes_in));
            atomic_store(&a->base_written,  atomic_load(&a->written));
            atomic_store(&a->base_ns,       mono_ns());
            fprintf(stderr,
                    "{\"event\":\"audio_primed\",\"fill_ms\":%.1f}\n",
                    1000.0 * (double)used
                        / ((double)a->open_rate * (double)a->frame_bytes));
        }

        /* --- Strategy A, high threshold: the sound has run too far ahead of
         * the card. Drop a whole period rather than let the delay grow without
         * end. This threshold answers the clock drift only: it sits well
         * beyond the amplitude of a burst. --- */
        /* ONE period per pass, on purpose. An earlier `while` loop here
         * removed as many periods as needed to get back under the threshold,
         * all at once: fourteen periods seen on the bench, 40 ms of sound
         * erased at a single splice point. That is no longer a micro-defect,
         * it is a hole, and it comes in bursts.
         *
         * At 344 passes per second, one period per pass leaves ample room to
         * follow a drift that needs two corrections per second. The same
         * amount of sound is dropped; it is simply spread out. */
        if (used > a->high_bytes) {
            atomic_store_explicit(&a->tail,
                atomic_load_explicit(&a->tail, memory_order_relaxed) + pb,
                memory_order_release);
            atomic_fetch_add(&a->overruns, 1u);
            used -= pb;
            a->splice_pending = 1;
        }

        /* --- Record the fill level, before consuming. --- */
        {
            uint32_t mn = atomic_load(&a->fill_min);
            uint32_t mx = atomic_load(&a->fill_max);
            if (used < mn || atomic_load(&a->fill_n) == 0u)
                atomic_store(&a->fill_min, used);
            if (used > mx)
                atomic_store(&a->fill_max, used);
            atomic_fetch_add(&a->fill_sum, used);
            atomic_fetch_add(&a->fill_n, 1u);
        }

        /* --- Take one period out. What is missing becomes silence: this is
         * the low threshold of strategy A. --- */
        uint32_t t = atomic_load_explicit(&a->tail, memory_order_relaxed);
        unsigned taken_bytes = (used < pb) ? used : pb;
        /* Align on a whole frame: playing half a frame would misalign the
         * left and right channels for the rest of the session. */
        taken_bytes -= taken_bytes % a->frame_bytes;
        for (unsigned k = 0u; k < taken_bytes; k++)
            period_buf[k] = a->ring[(t + k) & RING_MASK];
        /* --- The two possible joins at the start of this period. --- */
        if (taken_bytes > 0u) {
            unsigned nf = taken_bytes / a->frame_bytes;
            if (a->in_silence) {
                /* Coming out of silence: the fade-in covers the join,
                 * including that of a pending splice. */
                fade_in(period_buf, a->frame_bytes,
                        (nf < FADE_FRAMES) ? nf : FADE_FRAMES);
                a->in_silence     = 0;
                a->splice_pending = 0;
            } else if (a->splice_pending) {
                /* Sound was dropped just before: crossfade from the last
                 * frame played instead of jumping. */
                fade_splice(period_buf, a->last_frame, a->frame_bytes,
                            (nf < SPLICE_FRAMES) ? nf : SPLICE_FRAMES);
                a->splice_pending = 0;
            }
        }
        if (taken_bytes > 0u)
            memcpy(a->last_frame, period_buf + taken_bytes - a->frame_bytes,
                   a->frame_bytes);

        if (taken_bytes < pb) {
            /* --- Fade-out, then silence. The ramp starts from the last real
             * frame played, even if it belonged to the PREVIOUS period: that
             * is the case where the buffer is completely empty, and it is
             * precisely the one that popped the loudest. --- */
            unsigned nsil = (pb - taken_bytes) / a->frame_bytes;
            unsigned f    = 0u;
            if (!a->in_silence) {
                f = (nsil < FADE_FRAMES) ? nsil : FADE_FRAMES;
                fade_out(period_buf + taken_bytes, a->last_frame, a->frame_bytes, f);
                a->in_silence = 1;
            }
            memset(period_buf + taken_bytes + (size_t)f * a->frame_bytes, 0,
                   pb - taken_bytes - f * a->frame_bytes);
            atomic_fetch_add(&a->underruns, 1u);
            /* The cushion is lost. Rebuild it rather than sawing around zero:
             * one short gap, then normal operation. Otherwise a network
             * hiccup would leave the sound choppy until the end of the game. */
            if (used == 0u)
                a->primed = 0;
        }
        atomic_store_explicit(&a->tail, t + taken_bytes, memory_order_release);

        /* Peak of the period. One sample in eight is plenty to say "something
         * is moving", and costs eight times less on the hot path. */
        {
            uint32_t peak_now = atomic_load_explicit(&a->peak, memory_order_relaxed);
            for (unsigned k = 0u; k + 1u < taken_bytes; k += 16u) {
                int16_t v;
                memcpy(&v, period_buf + k, sizeof v);
                uint32_t abs_v = (v < 0) ? (uint32_t)(-(int32_t)v) : (uint32_t)v;
                if (abs_v > peak_now) peak_now = abs_v;
            }
            atomic_store_explicit(&a->peak, peak_now, memory_order_relaxed);
        }

        /* An exact copy of what goes to the card, fades included. Only the
         * normal path is recorded: the priming silence, which goes through
         * the other call above, is of no interest and would skew the length. */
        if (a->dump_fd >= 0) {
            ssize_t dump_written = write(a->dump_fd, period_buf, pb);
            (void)dump_written;
        }

        if (pcm_write_period(a, period_buf, pb / a->frame_bytes) != 0) {
            fprintf(stderr,
                    "{\"event\":\"audio_write_failed\",\"errno\":%d}\n", errno);
            pcm_close(a);
            a->next_open_ns = mono_ns() + 1000000000ULL;
            continue;
        }
        atomic_fetch_add(&a->written, 1u);
    }

    pcm_close(a);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Public interface.
 * ------------------------------------------------------------------------- */
struct audio_out *audio_out_create(const char *device, int cpu_affinity,
                                   int sched_fifo_priority,
                                   const char *dump_path)
{
    struct audio_out *a = calloc(1u, sizeof(*a));
    if (!a) return NULL;
    a->dump_fd = -1;
    if (dump_path && dump_path[0])
        snprintf(a->dump_path, sizeof(a->dump_path), "%s", dump_path);

    snprintf(a->device, sizeof(a->device), "%s",
             (device && device[0]) ? device : PCM_DEFAULT_DEVICE);
    a->fd           = -1;
    a->cpu_affinity = cpu_affinity;
    a->prio         = sched_fifo_priority;
    atomic_store(&a->running, 1);

    if (pthread_create(&a->tid, NULL, audio_thread, a) != 0) {
        fprintf(stderr, "{\"event\":\"audio_thread_create_failed\"}\n");
        free(a);
        return NULL;
    }
    a->thread_started = 1;
    fprintf(stderr,
            "{\"event\":\"audio_out_ready\",\"device\":\"%s\",\"cpu\":%d,"
            "\"prio\":%d}\n",
            a->device, cpu_affinity, sched_fifo_priority);
    return a;
}

void audio_out_destroy(struct audio_out *a)
{
    if (!a) return;
    atomic_store(&a->running, 0);
    if (a->thread_started)
        pthread_join(a->tid, NULL);
    if (a->dump_fd >= 0) close(a->dump_fd);
    free(a);
}

void audio_out_configure(struct audio_out *a, unsigned rate_hz, unsigned channels)
{
    if (!a) return;
    atomic_store(&a->want_channels, channels);
    atomic_store(&a->want_rate, rate_hz);
}

size_t audio_out_push(struct audio_out *a, const uint8_t *pcm, size_t len)
{
    if (!a || !pcm || len == 0u) return 0u;

    uint32_t h    = atomic_load_explicit(&a->head, memory_order_relaxed);
    uint32_t t    = atomic_load_explicit(&a->tail, memory_order_acquire);
    uint32_t free_bytes = RING_BYTES - (h - t);
    size_t   n     = (len < free_bytes) ? len : free_bytes;

    for (size_t k = 0u; k < n; k++)
        a->ring[(h + (uint32_t)k) & RING_MASK] = pcm[k];
    atomic_store_explicit(&a->head, h + (uint32_t)n, memory_order_release);
    atomic_fetch_add(&a->bytes_in, (uint64_t)n);

    if (n < len)
        atomic_fetch_add(&a->dropped_push, (uint64_t)(len - n));
    return n;
}

void audio_out_report(struct audio_out *a)
{
    if (!a) return;
    uint64_t now = mono_ns();
    if (a->log_ns == 0u) { a->log_ns = now; return; }
    if (now - a->log_ns < 1000000000ULL) return;
    a->log_ns = now;

    uint64_t w  = atomic_load(&a->written);
    uint64_t bi = atomic_load(&a->bytes_in);
    if (w == a->last_written) return;    /* nothing played: no line */

    uint32_t n  = atomic_load(&a->fill_n);
    uint32_t mn = atomic_load(&a->fill_min);
    uint32_t mx = atomic_load(&a->fill_max);
    uint64_t sm = atomic_load(&a->fill_sum);
    atomic_store(&a->fill_n, 0u);
    atomic_store(&a->fill_sum, 0u);
    atomic_store(&a->fill_min, 0u);
    atomic_store(&a->fill_max, 0u);

    unsigned rate = a->open_rate ? a->open_rate : 44100u;
    unsigned fb   = a->frame_bytes ? a->frame_bytes : 4u;
    double   parms = 1000.0 / ((double)rate * (double)fb);   /* bytes -> ms */
    double   avg  = n ? ((double)sm / (double)n) : 0.0;

    /* --- Drift between the two clocks, in parts per million ----------------
     *
     * The emitter delivers the sound at the core's rate, itself slaved to the
     * PC's sound card; the receiver plays it at the rate of ITS crystal. The
     * two cannot be equal. This number says by how much they differ, and in
     * which direction: positive, the emitter runs faster and the buffer
     * climbs to the high threshold; negative, it empties.
     *
     * It is the error signal a finer correction would need. It is measured
     * here, and nothing more is done with it on this side.
     *
     * During the first second both reference counters are zero, which would
     * give an absurd number; it is reported as zero. */
    double drift_ppm = 0.0;
    double in_fps = 0.0, out_fps = 0.0;
    uint64_t bns = atomic_load(&a->base_ns);
    if (bns != 0u && now > bns + 2000000000ULL) {
        double elapsed_sec = (double)(now - bns) / 1e9;
        double frames_in  = (double)(bi - atomic_load(&a->base_bytes_in))
                          / (double)fb;
        double frames_out = (double)(w - atomic_load(&a->base_written))
                          * (double)(a->period_bytes / fb);
        in_fps  = frames_in  / elapsed_sec;
        out_fps = frames_out / elapsed_sec;
        if (out_fps > 0.0)
            drift_ppm = (in_fps - out_fps) / out_fps * 1.0e6;
    }
    a->last_written  = w;
    a->last_bytes_in = bi;
    a->last_log_ns   = now;
    (void)a->last_bytes_in;

    fprintf(stderr,
            "{\"event\":\"audio_fill_1s\",\"fill_ms\":{\"min\":%.1f,\"avg\":%.1f,"
            "\"max\":%.1f},\"periods\":%llu,\"underruns\":%llu,\"overruns\":%llu,"
            "\"xruns\":%llu,\"push_dropped\":%llu,\"primed\":%d,"
            "\"in_fps\":%.0f,\"out_fps\":%.0f,\"drift_ppm\":%.0f,\"peak\":%u}\n",
            (double)mn * parms, avg * parms, (double)mx * parms,
            (unsigned long long)w,
            (unsigned long long)atomic_load(&a->underruns),
            (unsigned long long)atomic_load(&a->overruns),
            (unsigned long long)atomic_load(&a->xruns),
            (unsigned long long)atomic_load(&a->dropped_push),
            a->primed, in_fps, out_fps, drift_ppm,
            (unsigned)atomic_exchange(&a->peak, 0u));
}
