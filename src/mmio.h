/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECEIVER_MMIO_H
#define RECEIVER_MMIO_H

#include <stdint.h>
#include <stddef.h>

/*
 * mmio.h -- Seamless 240p<->480i toggle via direct CRTC register MMIO.
 *
 * Ported from register-access tooling and a register write sequence that
 * were first proven on the real hardware: 20 transitions on the JVC tube,
 * zero sync loss.
 *
 * Mechanism: mmap of resource2 of the PCI function behind the opened DRM
 * node, /sys/dev/char/<maj>:<min>/device/resource2, once its PCI id has
 * passed the allowlist (BAR2 =
 * register aperture on Cape Verde, families < BONAIRE under kmod radeon),
 * then a grouped write of the 13 static delta registers between the
 * paired 240p/480i modelines under CRTC_UPDATE_LOCK. The group latches
 * atomically at V_UPDATE -- the PLL is never touched (pclock/htotal shared
 * by construction, see modelines.h CRITICAL INVARIANT), so the CRT never
 * has a physical reason to re-lock.
 *
 * CRTC0-only guard (v1 hardware honesty): the 13 register values come
 * from CRTC0 dumps (base +0x0). The non-0x6exx offsets (0x6178, 0x6838,
 * 0x6a84, 0x6b00, 0x6b04, 0x6d74, 0x6d88, 0x7684) have NO characterized
 * per-CRTC-base arithmetic. mmio_init() therefore REFUSES to initialize
 * when the active CRTC is not CRTC0 -- mmio_available() stays 0 and the
 * daemon falls back to drmModeSetCrtc. The receiver drives CRTC0, so the
 * guarded path is the nominal one.
 *
 * No DRM dependency: this module only needs libc (stat/open/read/mmap).
 * Root is required (the daemon already runs as root DRM master).
 */

/* Regime selector values -- kept numerically identical to modelines.h's
 * enum daemon_regime (REGIME_240P_SUPER_RES = 0, REGIME_480I = 1) without
 * including that header (this module stays dependency-free). */
#define MMIO_REGIME_240P_SUPER_RES 0U
#define MMIO_REGIME_480I           1U

/* Number of static delta registers between the paired modelines
 * (volatile registers were excluded by dumping the registers twice). */
#define MMIO_TOGGLE_REG_COUNT 13U

/* One register write of the toggle sequence: byte offset from the BAR2
 * base (CRTC0 base = +0x0) and the exact 32-bit value to program. */
struct mmio_reg {
    uint32_t off;
    uint32_t val;
};

/* Check the card behind drm_device, map its BAR2 and detect the active
 * CRTC. drm_device is the DRM node the caller actually opened (e.g.
 * "/dev/dri/card0"); its PCI id is read from
 * /sys/dev/char/<maj>:<min>/device/{vendor,device} and must be in the
 * allowlist (1002:683f only) BEFORE resource2 is opened. Returns 0
 * on success (allowlisted, mapped AND active CRTC is CRTC0). Returns <0 on
 * any failure:
 *   -1 drm_device NULL, not stat-able, or not a character device
 *   -2..-4 resource2 open/fstat/mmap failed (e.g. not root)
 *   -5 no active CRTC, or active CRTC != 0 (CRTC0-only guard above)
 *   -6 PCI id unreadable (or sysfs path too long)
 *   -7 PCI id not in the allowlist; resource2 never opened
 * Logs a JSON event on stderr either way:
 *   {"event":"mmio_mapped","size":N,"crtc_base":0}          on success
 *   {"event":"mmio_unavailable","reason":"..."}             on failure
 * (the -7 line also carries "pci_id":"vvvv:dddd"). On failure the caller
 * keeps the drmModeSetCrtc fallback. Idempotent: calling again after
 * success is a no-op returning 0. */
int mmio_init(const char *drm_device);

/* 1 if mmio_init() succeeded, 0 otherwise. */
int mmio_available(void);

/* Perform the seamless toggle: write CRTC_UPDATE_LOCK=1, program the 13
 * delta registers of the target regime, write CRTC_UPDATE_LOCK=0 (the
 * group latches at V_UPDATE). Reads CRTC_DOUBLE_BUFFER_CONTROL
 * (UPDATE_PENDING) afterwards and logs it as telemetry:
 *   {"event":"mmio_seamless_toggle","regime":"...","update_pending":"0x..."}
 * regime: MMIO_REGIME_240P_SUPER_RES (0) or MMIO_REGIME_480I (1).
 * Returns 0 on success, <0 if not initialized or regime invalid. */
int mmio_seamless_toggle(uint8_t regime);

/* DCE6 offset of the interlace status, CRTC0 base (+0x0). The offset table
 * gives 0x6e78 for CRTC_INTERLACE_CONTROL (already written by the seamless
 * toggle, TOGGLE_240P/TOGGLE_480I in mmio.c); the status follows it
 * immediately at 0x6e7c (evergreen_reg.h).
 *
 * EXPECTED MEANING, NOT CONFIRMED ON THIS CARD: bit 0 = current field,
 * bit 1 = next field. The register dumps show it at zero, but they were
 * taken outside an interlaced scan. The daemon reads it for telemetry only.
 * Draw no conclusion from this register until a bench log has shown BOTH
 * of its values. */
#define MMIO_CRTC_INTERLACE_STATUS 0x6e7cU

/* Read-only access to MMIO_CRTC_INTERLACE_STATUS. No side effect (a read
 * in the display block does not change its state). Returns the raw register
 * value, or 0xFFFFFFFFu when MMIO is not available -- that value cannot be
 * mistaken for a real read (only the first two bits of this register are
 * defined). */
uint32_t mmio_interlace_status(void);

/* Test access: returns the static toggle table for `regime` and stores
 * its entry count in *count. NULL if regime invalid. Used by test_mmio.c
 * to lock the tables bit-for-bit against the proven write sequence --
 * carries no runtime cost and no hardware access. */
const struct mmio_reg *mmio_toggle_table(uint8_t regime, size_t *count);

#endif /* RECEIVER_MMIO_H */
