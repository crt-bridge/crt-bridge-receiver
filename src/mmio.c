/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mmio.c -- Seamless 240p<->480i toggle via direct CRTC register MMIO.
 *
 * See mmio.h for the full contract. Three things matter here:
 *
 * 1. The 13-register tables below are EXACT copies of the write sequence
 *    proven on the real hardware (20 transitions on the JVC tube, zero
 *    sync loss). No value is derived AT RUNTIME. test_mmio.c locks them
 *    bit-for-bit. Do NOT edit without a new test on the tube.
 *
 *    Exception: three fields ARE derived AT COMPILE TIME from
 *    modelines.h -- 0x6d74/0x6d88 (VIEWPORT_SIZE, upper 16 bits =
 *    H_ACTIVE) and the upper 16 bits of 0x6e4c (= H_TOTAL - 1). This is
 *    still a compile-time constant expression, not a runtime computation,
 *    so the sentence above ("no value is derived at runtime") remains
 *    true. The reason: all-literal tables once let the 1280 -> 2560
 *    super-res widening silently desynchronize the seamless-toggle
 *    viewport from the CRTC's actual scanout width. A future width change
 *    can no longer do that unnoticed, because the compiler links these
 *    three fields to modelines.h and test_mmio.c's independent literal
 *    transcription (case g) would go RED on any drift.
 *
 * 2. CRTC0-only guard: the values come from CRTC0 dumps (base +0x0).
 *    The non-0x6exx offsets (0x6178, 0x6838, 0x6a84, 0x6b00, 0x6b04,
 *    0x6d74, 0x6d88, 0x7684) have no characterized per-CRTC-base
 *    arithmetic -- if the active CRTC is not CRTC0, mmio_init() fails
 *    and the daemon falls back to drmModeSetCrtc (no untested offsets
 *    are ever written). The receiver drives CRTC0.
 *
 * 3. PCI allowlist: the card checked is the one
 *    behind the DRM node the daemon actually opened, never a default card.
 *    BAR discovery: stat(<drm device>) -> major:minor ->
 *    /sys/dev/char/<maj>:<min>/device/{vendor,device} checked against
 *    PCI_ALLOWLIST (1002:683f only) -> only then resource2 of the same
 *    directory -> mmap RW (O_SYNC). Any refusal logs mmio_unavailable and
 *    leaves the drmModeSetCrtc fallback. (resource2 = register BAR for
 *    Cape Verde < Bonaire, cf. radeon_device.c: rdev->rmmio_base =
 *    pci_resource_start(pdev, 2); the mapping itself was ported from the
 *    register-access tooling first proven on the bench.)
 *
 * IMPORTANT: the toggle NEVER writes GRPH surface registers
 * (address/pitch) -- the normal pageflip keeps scanning the current FB.
 * It also never writes volatile counters/status (34 of the 47 dwords that
 * differed between the two modes were volatile; they were excluded by
 * dumping the registers twice).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "mmio.h"
#include "modelines.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

/* Key CRTC0 control offsets (byte offsets; EVERGREEN family, checked on
 * the DCE6 hardware -- dce_6_0_d.h dword*4 == evergreen_reg.h). */
#define MMIO_CRTC_CONTROL               0x6e70U /* bit0 = CRTC_MASTER_EN */
#define MMIO_CRTC_UPDATE_LOCK           0x6ed4U
#define MMIO_CRTC_DOUBLE_BUFFER_CONTROL 0x6ed8U /* bit0 = UPDATE_PENDING */

/* The 6 CRTC bases on DCE6 (evergreen_reg.h). */
static const uint32_t CRTC_BASES[6] = {
    0x0U, 0xc00U, 0x9800U, 0xa400U, 0xb000U, 0xbc00U
};

/* -----------------------------------------------------------------------
 * Static toggle tables -- the 13-register delta between the paired
 * modelines (values bit-for-bit from the write sequence proven on the tube).
 * H timing registers (e.g. CRTC_H_TOTAL 0x6e00) are deliberately ABSENT:
 * pclock and htotal are shared by construction (modelines.h CRITICAL
 * INVARIANT) so only vertical/interlace state differs.
 * ----------------------------------------------------------------------- */
/* VIEWPORT_SIZE (0x6d74, 0x6d88): upper 16 bits = H_ACTIVE, lower 16 bits
 * = V_ACTIVE. Derived at COMPILE TIME from modelines.h (the exception
 * described at the top of this file) so a future width change cannot silently
 * desynchronize this table from the CRTC's actual scanout width. */
#define MMIO_VIEWPORT_SIZE_240P \
    (((uint32_t)MODELINE_240P_SR_NTSC_H_ACTIVE << 16) | \
     (uint32_t)MODELINE_240P_SR_NTSC_V_ACTIVE)
#define MMIO_VIEWPORT_SIZE_480I \
    (((uint32_t)MODELINE_480I_NTSC_H_ACTIVE << 16) | \
     (uint32_t)MODELINE_480I_NTSC_V_ACTIVE)

/* 0x6e4c: upper 16 bits = H_TOTAL - 1, lower 16 bits = vertical blank-start
 * packing (0x0102 for 240p, 0x0205 for 480i -- unchanged by this widening,
 * these bits are vertical and do not move with h_total). Same compile-time
 * derivation rationale as VIEWPORT_SIZE above. */
#define MMIO_0X6E4C_240P \
    ((((uint32_t)MODELINE_240P_SR_NTSC_H_TOTAL - 1U) << 16) | 0x0102U)
#define MMIO_0X6E4C_480I \
    ((((uint32_t)MODELINE_480I_NTSC_H_TOTAL - 1U) << 16) | 0x0205U)

static const struct mmio_reg TOGGLE_240P[MMIO_TOGGLE_REG_COUNT] = {
    { 0x6178U, 0x03fffffeU }, /* (range/cap)                              */
    { 0x6838U, 0x000000f0U }, /* GRPH_Y_END 240                           */
    { 0x6a84U, 0x00000001U }, /* (field mode)                             */
    { 0x6b00U, 0x00001000U }, /* DATA_FORMAT progressive                  */
    { 0x6b04U, 0x000000f0U }, /* DESKTOP_HEIGHT 240                       */
    { 0x6d74U, MMIO_VIEWPORT_SIZE_240P }, /* VIEWPORT_SIZE 2560x240       */
    { 0x6d88U, MMIO_VIEWPORT_SIZE_240P }, /* (viewport bis)               */
    { 0x6e1cU, 0x00000105U }, /* CRTC_V_TOTAL 261 (vtotal-1)              */
    { 0x6e34U, 0x00120102U }, /* CRTC_V_BLANK_START_END end=18 start=258  */
    { 0x6e38U, 0x00030000U }, /* CRTC_V_SYNC_A width 3                    */
    { 0x6e4cU, MMIO_0X6E4C_240P }, /* (h/v packed)                        */
    { 0x6e78U, 0x00000000U }, /* CRTC_INTERLACE_CONTROL: INTERLACE off    */
    { 0x7684U, 0x00000001U }, /* (encoder field mode)                     */
};

static const struct mmio_reg TOGGLE_480I[MMIO_TOGGLE_REG_COUNT] = {
    { 0x6178U, 0x05fffffeU }, /* (range/cap)                              */
    { 0x6838U, 0x000001e0U }, /* GRPH_Y_END 480                           */
    { 0x6a84U, 0x00000002U }, /* (field mode)                             */
    { 0x6b00U, 0x00001001U }, /* DATA_FORMAT bit0 INTERLEAVE_EN           */
    { 0x6b04U, 0x000001e0U }, /* DESKTOP_HEIGHT 480                       */
    { 0x6d74U, MMIO_VIEWPORT_SIZE_480I }, /* VIEWPORT_SIZE 2560x480       */
    { 0x6d88U, MMIO_VIEWPORT_SIZE_480I }, /* (viewport bis)               */
    { 0x6e1cU, 0x0000020cU }, /* CRTC_V_TOTAL 524 (frame lines - 1)       */
    { 0x6e34U, 0x00250205U }, /* CRTC_V_BLANK_START_END end=37 start=517  */
    { 0x6e38U, 0x00060000U }, /* CRTC_V_SYNC_A width 6                    */
    { 0x6e4cU, MMIO_0X6E4C_480I }, /* (h/v packed)                        */
    { 0x6e78U, 0x00000001U }, /* CRTC_INTERLACE_CONTROL: INTERLACE on     */
    { 0x7684U, 0x00000002U }, /* (encoder field mode)                     */
};

/* Module state. Single-threaded use by design: mmio_init() is called once
 * from blit_init_default (blit thread), mmio_seamless_toggle() only from
 * blit_regime_toggle (same thread). */
static volatile uint32_t *g_mmio;
static size_t             g_size;
static int                g_available; /* 1 after successful mmio_init */

static uint32_t rd(uint32_t off) { return g_mmio[off / 4U]; }
static void     wr(uint32_t off, uint32_t val) { g_mmio[off / 4U] = val; }

/* Reason of the last refusal (NULL after a success). Read by test_mmio.c,
 * which includes this file; the daemon only sees the JSON line. */
static const char        *g_last_reason;

/* PCI allowlist: the only cards that ever receive a raw
 * register write. The 13-register tables above were proven on ONE card,
 * the bench's Cape Verde HD 7750 (1002:683f). Another AMD card may decode
 * the same offsets differently and lock up or upset the tube, so it gets
 * the drmModeSetCrtc fallback instead. Add a card here only after a bench
 * session on that card has proven the toggle on it. */
static const struct { uint32_t vendor, device; } PCI_ALLOWLIST[] = {
    { 0x1002U, 0x683fU }, /* AMD Cape Verde (Radeon HD 7750), the bench card */
};

static void log_unavailable(const char *reason)
{
    g_last_reason = reason;
    fprintf(stderr, "{\"event\":\"mmio_unavailable\",\"reason\":\"%s\"}\n",
            reason);
}

/* Read a sysfs PCI id file ("0x1002\n"): hex, either case, optional 0x,
 * trailing whitespace only, at most 16 bits. 0 on success, -1 otherwise. */
static int read_pci_id(const char *dir, const char *name, uint32_t *out)
{
    char p[PATH_MAX + 32], text[32];
    int n = snprintf(p, sizeof p, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof p)
        return -1;
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t got = read(fd, text, sizeof text - 1);
    close(fd);
    if (got <= 0)
        return -1;
    text[got] = 0;

    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(text, &end, 16);
    if (errno != 0 || end == text)
        return -1;
    while (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')
        end++;
    if (*end != 0 || v > 0xffffUL)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static int pci_id_allowlisted(uint32_t vendor, uint32_t device)
{
    for (size_t i = 0; i < sizeof PCI_ALLOWLIST / sizeof PCI_ALLOWLIST[0]; i++)
        if (PCI_ALLOWLIST[i].vendor == vendor &&
            PCI_ALLOWLIST[i].device == device)
            return 1;
    return 0;
}

/* mmio_init with the sysfs root as a parameter ("/sys" in service, a fake
 * tree in test_mmio.c). Strict order: the opened DRM path must be a
 * character device; its major:minor names <root>/dev/char/<maj>:<min>/
 * device, the PCI function behind THAT node (not card0); vendor and device
 * are read there and checked against PCI_ALLOWLIST; only then is resource2
 * of the same directory opened. */
static int mmio_init_at(const char *sysfs_root, const char *drm_device)
{
    char dir[PATH_MAX], path[PATH_MAX + 32];

    if (g_available)
        return 0; /* idempotent */
    g_last_reason = NULL;

    struct stat dst;
    if (!drm_device) {
        log_unavailable("drm_device_not_char");
        return -1;
    }
    if (stat(drm_device, &dst) != 0) {
        log_unavailable("drm_device_stat_failed");
        return -1;
    }
    if (!S_ISCHR(dst.st_mode)) {
        log_unavailable("drm_device_not_char");
        return -1;
    }

    int n = snprintf(dir, sizeof dir, "%s/dev/char/%u:%u/device", sysfs_root,
                     major(dst.st_rdev), minor(dst.st_rdev));
    if (n < 0 || (size_t)n >= sizeof dir) {
        log_unavailable("sysfs_path_too_long");
        return -6;
    }

    uint32_t vendor = 0, device = 0;
    if (read_pci_id(dir, "vendor", &vendor) != 0 ||
        read_pci_id(dir, "device", &device) != 0) {
        log_unavailable("pci_id_unreadable");
        return -6;
    }
    if (!pci_id_allowlisted(vendor, device)) {
        g_last_reason = "pci_id_not_allowlisted";
        fprintf(stderr,
                "{\"event\":\"mmio_unavailable\","
                "\"reason\":\"pci_id_not_allowlisted\","
                "\"pci_id\":\"%04x:%04x\"}\n",
                vendor, device);
        return -7;
    }

    snprintf(path, sizeof path, "%s/resource2", dir);

    int fd = open(path, O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        log_unavailable("open_resource2_failed");
        return -2;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        log_unavailable("fstat_resource2_failed");
        close(fd);
        return -3;
    }
    size_t size = (size_t)st.st_size;

    void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        log_unavailable("mmap_resource2_failed");
        return -4;
    }
    g_mmio = (volatile uint32_t *)m;
    g_size = size;

    /* Detect the active CRTC: CRTC_CONTROL bit0 (MASTER_EN) across the 6
     * DCE6 bases. v1 guard: only CRTC0 (base +0x0) is characterized. */
    int active = -1;
    for (int i = 0; i < 6; i++) {
        uint32_t b = CRTC_BASES[i];
        if ((size_t)b + MMIO_CRTC_CONTROL + 4U > g_size)
            break;
        if (rd(MMIO_CRTC_CONTROL + b) & 1U) {
            active = i;
            break;
        }
    }
    if (active != 0) {
        log_unavailable(active < 0 ? "no_active_crtc"
                                   : "active_crtc_not_crtc0");
        munmap((void *)g_mmio, g_size);
        g_mmio = NULL;
        g_size = 0;
        return -5;
    }

    g_available = 1;
    fprintf(stderr,
            "{\"event\":\"mmio_mapped\",\"size\":%zu,\"crtc_base\":0}\n",
            g_size);
    return 0;
}

int mmio_init(const char *drm_device)
{
    return mmio_init_at("/sys", drm_device);
}

int mmio_available(void)
{
    return g_available;
}

uint32_t mmio_interlace_status(void)
{
    if (!g_available)
        return 0xFFFFFFFFu;
    return rd(MMIO_CRTC_INTERLACE_STATUS);
}

const struct mmio_reg *mmio_toggle_table(uint8_t regime, size_t *count)
{
    if (count)
        *count = MMIO_TOGGLE_REG_COUNT;
    if (regime == (uint8_t)MMIO_REGIME_240P_SUPER_RES)
        return TOGGLE_240P;
    if (regime == (uint8_t)MMIO_REGIME_480I)
        return TOGGLE_480I;
    if (count)
        *count = 0;
    return NULL;
}

int mmio_seamless_toggle(uint8_t regime)
{
    if (!g_available)
        return -1;

    size_t count = 0;
    const struct mmio_reg *tbl = mmio_toggle_table(regime, &count);
    if (!tbl)
        return -2;

    /* Grouped write under CRTC_UPDATE_LOCK; the unlock latches the whole
     * group atomically at V_UPDATE (double-buffered, INTERLACE_ENABLE
     * included -- seen on the bench: UPDATE_PENDING set on every flip,
     * dropping back within 50 ms). */
    wr(MMIO_CRTC_UPDATE_LOCK, 1U);
    for (size_t i = 0; i < count; i++)
        wr(tbl[i].off, tbl[i].val);
    wr(MMIO_CRTC_UPDATE_LOCK, 0U);

    uint32_t pending = rd(MMIO_CRTC_DOUBLE_BUFFER_CONTROL);
    fprintf(stderr,
            "{\"event\":\"mmio_seamless_toggle\",\"regime\":\"%s\","
            "\"update_pending\":\"0x%08x\"}\n",
            regime == (uint8_t)MMIO_REGIME_480I ? "480i" : "240p_super_res",
            pending);
    return 0;
}
