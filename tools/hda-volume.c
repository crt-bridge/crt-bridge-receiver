/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * hda-volume.c -- sets the volume of the receiver's sound card, without amixer.
 *
 * DELIBERATELY OUTSIDE THE DAEMON. The daemon plays sound; it has no business
 * touching the mixer. This is a one-off operation, done once after a boot,
 * and a separate binary can be replaced without restarting the bridge.
 *
 * WHY IT EXISTS
 * -------------
 * The receiver's PXE image carries neither alsa-utils nor libasound: no
 * `amixer`, no `alsactl`, no `alsamixer`. So no mixer state is restored at
 * boot, and the kernel leaves the codec in its reset state.
 *
 * Read on the receiver on 2026-08-30, before a single line of sound output
 * had been written, in /proc/asound/card0/codec#0:
 *
 *     Node 0x0c [Audio Mixer] ... Control: name="Front Playback Volume"
 *       Amp-Out caps: ofs=0x1f, nsteps=0x1f, stepsize=0x05, mute=0
 *       Amp-Out vals:  [0x00 0x00]          <-- volume at ZERO
 *
 * In other words: even a perfect bridge would have given perfect silence.
 * This trap had been predicted as "the dumbest and the most likely", and it
 * was there indeed.
 *
 * WHAT IT DOES
 * ------------
 * Opens /dev/snd/controlC0, and for each named control: asks for its bounds
 * with SNDRV_CTL_IOCTL_ELEM_INFO, then writes the wanted value with
 * SNDRV_CTL_IOCTL_ELEM_WRITE, on all its channels. Integers get a percentage
 * of their range, booleans get "on".
 *
 * A missing control is not an error: codecs do not all have the same names,
 * and the program simply says what it found.
 *
 * USAGE
 * -----
 *     hda-volume            sets 75 %
 *     hda-volume 90         sets 90 %
 *     hda-volume 0          mutes (booleans go to zero)
 *     hda-volume -c 1 80    on card 1 (the HDMI output of the Radeon)
 *
 * BUILD -- no dependency, a gcc is enough:
 *     gcc -std=c11 -O2 -o hda-volume hda-volume.c
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sound/asound.h>

/* The controls to set, in order. "Front" is the green line output of the
 * ALC883; "Headphone" the headphone jack. Both are set because there is no
 * telling which one the user will plug in, and setting the one that is not
 * plugged in costs nothing. Missing controls are skipped. */
static const char *const CONTROLS[] = {
    "Front Playback Volume",
    "Front Playback Switch",
    "Headphone Playback Volume",
    "Headphone Playback Switch",
    "Master Playback Volume",     /* absent on the ALC883, present elsewhere */
    "Master Playback Switch",
    "PCM Playback Volume",
    "PCM Playback Switch",
    "Speaker Playback Volume",
    "Speaker Playback Switch",
};

static void fill_ctl_id(struct snd_ctl_elem_id *id, const char *ctl_name)
{
    memset(id, 0, sizeof(*id));
    id->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    id->index = 0u;
    snprintf((char *)id->name, sizeof(id->name), "%s", ctl_name);
}

/* Returns 0 if the control was written, 1 if it is missing, -1 on error. */
static int set_control(int fd, const char *ctl_name, int level_pct)
{
    struct snd_ctl_elem_info info;
    memset(&info, 0, sizeof(info));
    fill_ctl_id(&info.id, ctl_name);

    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) {
        if (errno == ENOENT) return 1;          /* not this control */
        fprintf(stderr, "  %-28s ERROR info: %s\n", ctl_name, strerror(errno));
        return -1;
    }

    struct snd_ctl_elem_value val;
    memset(&val, 0, sizeof(val));
    fill_ctl_id(&val.id, ctl_name);

    unsigned n = info.count;
    if (n == 0u) n = 1u;
    if (n > 128u) n = 128u;

    long set_val = 0;
    if (info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN) {
        set_val = (level_pct > 0) ? 1 : 0;
        for (unsigned k = 0u; k < n; k++) val.value.integer.value[k] = set_val;
    } else if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
        long mn = info.value.integer.min;
        long mx = info.value.integer.max;
        set_val = mn + ((mx - mn) * level_pct + 50) / 100;
        if (set_val < mn) set_val = mn;
        if (set_val > mx) set_val = mx;
        for (unsigned k = 0u; k < n; k++) val.value.integer.value[k] = set_val;
    } else {
        printf("  %-28s skipped (type %d not handled)\n", ctl_name, (int)info.type);
        return 1;
    }

    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0) {
        fprintf(stderr, "  %-28s ERROR write: %s\n", ctl_name, strerror(errno));
        return -1;
    }

    if (info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN)
        printf("  %-28s %s (%u channels)\n", ctl_name,
               set_val ? "on" : "off", n);
    else
        printf("  %-28s %ld in %ld..%ld (%u channels)\n", ctl_name, set_val,
               info.value.integer.min, info.value.integer.max, n);
    return 0;
}

int main(int argc, char **argv)
{
    int card_no = 0;
    int level_pct = 75;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            card_no = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: %s [-c card] [percent]\n"
                   "  sets the output volume of the sound card, without amixer.\n"
                   "  default: card 0, 75 %%\n", argv[0]);
            return 0;
        } else {
            level_pct = atoi(argv[i]);
        }
    }
    if (level_pct < 0)   level_pct = 0;
    if (level_pct > 100) level_pct = 100;

    char dev_path[64];
    snprintf(dev_path, sizeof(dev_path), "/dev/snd/controlC%d", card_no);

    int fd = open(dev_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", dev_path, strerror(errno));
        return 1;
    }

    printf("%s -- setting %d %%\n", dev_path, level_pct);

    int n_written = 0, n_missing = 0, n_errors = 0;
    for (size_t k = 0; k < sizeof(CONTROLS) / sizeof(CONTROLS[0]); k++) {
        int r = set_control(fd, CONTROLS[k], level_pct);
        if (r == 0) n_written++;
        else if (r == 1) n_missing++;
        else n_errors++;
    }
    close(fd);

    printf("%d control(s) set, %d missing, %d error(s)\n",
           n_written, n_missing, n_errors);

    if (n_written == 0) {
        fprintf(stderr,
                "NO control set: check the card number "
                "(cat /proc/asound/cards)\n");
        return 2;
    }
    return n_errors ? 1 : 0;
}
