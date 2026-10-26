/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECEIVER_PCM_H
#define RECEIVER_PCM_H

/*
 * pcm.h -- the receiver's sound output, written directly to the kernel's PCM
 * device.
 *
 * WHY NOT libasound
 * -----------------
 * The receiver image carries neither libasound nor aplay; adding them would
 * mean rebuilding the image, a heavy and risky operation on a diskless
 * machine. On the other hand `/usr/include/sound/asound.h` is there: the
 * kernel interface is available without any extra package, and it is
 * exactly what libasound uses under the hood. Three hundred lines of ioctl
 * replace a dependency and an image rebuild.
 *
 * What libasound would bring is precisely what we do not need: the
 * configuration file, the plugins, software mixing, format conversion. One
 * format, one rate, no conversion.
 *
 * START-UP GUARANTEE
 * ------------------
 * The device is NEVER opened when the daemon starts. It is opened when sound
 * first arrives, in the audio thread, and a failure to open is logged then
 * ignored. A daemon that opens the sound device only when it is sent sound
 * cannot fail to start because of it.
 *
 * THREADS
 * -------
 * One producer (the network thread, which calls audio_out_push) and one
 * consumer (the audio thread, which drains the buffer into the device). The
 * ring buffer is cast in the same mould as spsc.h: two atomic indices, no
 * allocation on the hot path, no lock.
 *
 * The audio thread runs at SCHED_FIFO 70, below the 80 of the blit thread. A
 * missed frame is seen; a micro-gap in the sound is heard but does not break
 * the CRT's sync; in a conflict, the CRT wins.
 *
 * DRIFT STRATEGY ("A")
 * --------------------
 * The crystal of the receiver's sound card has nothing to do with the core's
 * rate nor with the Radeon's pixel clock. Left alone, the buffer slowly
 * empties or fills up.
 *
 *   - too empty: the period is completed with silence;
 *   - too full: a period is dropped.
 *
 * Both are counted, and the fill level is logged every second: it can be
 * measured without listening. The receiver does no finer correction: the
 * drift is measured here, and the emitter's clock servo is what keeps it
 * small.
 */

#include <stdint.h>
#include <stddef.h>

struct audio_out;

/* Creates the audio output and starts its thread. Does NOT touch the device.
 * `device` defaults to "/dev/snd/pcmC0D0p" when NULL.
 * `cpu_affinity` < 0 for no pinning. Returns NULL if the allocation or the
 * thread creation fails -- the caller then goes on without sound. */
/* `dump_path` != NULL: the audio thread writes into this file an exact copy
 * of what it hands to the card -- after fades and splices, so the waveform
 * as it is played. Raw format, S16 little-endian interleaved, at the
 * announced rate. For diagnosis: if the file crackles, the fault is in the
 * bytes; if it is clean, it is in their playback.
 * NULL in normal use -- writing is not free on a real-time thread, even to
 * a tmpfs. */
struct audio_out *audio_out_create(const char *device, int cpu_affinity,
                                   int sched_fifo_priority,
                                   const char *dump_path);

/* Stops the thread, closes the device if it was open, frees the memory. */
void audio_out_destroy(struct audio_out *a);

/* Announces the rate and the channel count, from CMD_INIT.
 * rate_hz = 0 mutes the output. A change midway closes and reopens the
 * device at the next block. Called by the network thread. */
void audio_out_configure(struct audio_out *a, unsigned rate_hz, unsigned channels);

/* Deposits a whole PCM block. Called by the network thread, never blocks.
 * Returns the number of bytes accepted; less than `len` means the buffer
 * was full and the surplus is lost (counted as an overflow). */
size_t audio_out_push(struct audio_out *a, const uint8_t *pcm, size_t len);

/* Logs `audio_fill_1s` if a second has passed and something happened.
 * Called by the network thread, at the same place as audio_report. */
void audio_out_report(struct audio_out *a);

#endif /* RECEIVER_PCM_H */
