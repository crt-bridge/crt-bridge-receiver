/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECEIVER_REGIME_H
#define RECEIVER_REGIME_H

/*
 * regime.h -- the regime resolver and the decisions that go with it.
 *
 * Pure module, header only, on the pattern of shed.h: `static inline`
 * functions, no allocation, no system call, no global state, neither DRM nor
 * switchres.
 *
 * The rule. One authority alone decides the regime of a SWITCHRES: the network
 * thread calls `regime_from_switchres`, and the blit thread reads the decision
 * in the request, it does not decide it again (two authorities is the defect
 * that was removed on the emitter side). The thread picks a regime, never a
 * timing: the mode served is the fixed constant of modelines.h, judged next by
 * the receiver's guard against the monitor file.
 *
 * Where the rules come from.
 *   - A progressive mode whose line rate is in the high band of the monitor
 *     (threshold H_BAND_HIGH_MIN_HZ, read from safety.h, never copied) is a
 *     480p.
 *   - An INTERLACED SWITCHRES is a 480i, whatever its rate. The emitter
 *     announces its 480i with v_total = 525 and interlace = 1, at 31,468.5 Hz
 *     nominal; read literally, the rate rule would send it to 31 kHz with
 *     badly cut images. So the interlace flag comes before the rate.
 *   - A hostile field (zero total width, NaN, infinity, negative clock) never
 *     divides by zero and gives the old behaviour: 240p. It is the guard, not
 *     the resolver, that refuses a timing.
 *
 *   - The fallback separates the wire shape from the regime served. A 480p
 *     asked of a receiver whose 480p is not enabled is no longer refused: it
 *     is SERVED as 480i (low band, MMIO toggle already proven, no band
 *     crossing). The wire shape stays that of 480p (whole frame `0x06`):
 *     `wire` says how to reassemble, `serve` says which scan to set. The 480p
 *     state has six values, and "not judged yet" is not a refusal.
 *
 * No field received from the network comes in here except through these three
 * arguments (and the 480p state, which comes from the receiver itself).
 */

#include <math.h>
#include "modelines.h"

/* The regime of a SWITCHRES: `interlace` (wire flag), `pclock_mhz` (clock, in
 * MHz, as the wire carries it) and `h_total` (total width, in pixels).
 *   - interlaced (== 1)                                      -> 480i, always;
 *   - otherwise, line >= H_BAND_HIGH_MIN_HZ (non-zero width,
 *     finite and positive clock)                             -> 480p;
 *   - otherwise                                              -> 240p (dead
 *     band included: the old behaviour). */
static inline enum daemon_regime
regime_from_switchres(unsigned interlace, double pclock_mhz, unsigned h_total)
{
    if (interlace == 1u) {
        return REGIME_480I;
    }
    if (h_total != 0u && isfinite(pclock_mhz) && pclock_mhz > 0.0) {
        double h_hz = pclock_mhz * 1.0e6 / (double)h_total;
        if (h_hz >= (double)H_BAND_HIGH_MIN_HZ) {
            return REGIME_480P;
        }
    }
    return REGIME_240P_SUPER_RES;
}

/* True for the only high-band regime (480p). */
static inline int regime_is_high_band(unsigned r)
{
    return r == (unsigned)REGIME_480P;
}

/* True when going from `have` to `want` changes band (15 kHz <-> 31 kHz).
 * Going 240p <-> 480i does not: both are in the low band. */
static inline int regime_crosses_band(unsigned have, unsigned want)
{
    return regime_is_high_band(have) != regime_is_high_band(want);
}

/* True only if the toggle can be done through MMIO (13 registers under
 * CRTC_UPDATE_LOCK, without reprogramming the PLL): both ends in
 * {240p, 480i} and different. Any pair with 480p, or a regime outside these
 * two, goes through drmModeSetCrtc. */
static inline int regime_mmio_toggle_ok(unsigned have, unsigned want)
{
    int have_ok = (have == (unsigned)REGIME_240P_SUPER_RES || have == (unsigned)REGIME_480I);
    int want_ok = (want == (unsigned)REGIME_240P_SUPER_RES || want == (unsigned)REGIME_480I);
    return have_ok && want_ok && have != want;
}

/* Must the request be published? Yes when the wanted regime differs from the
 * current one, OR when it differs from the last published request: the second
 * term cancels a pending toggle (A -> B published, then the emitter goes back
 * to A before the deadline: want == have, but the last published is B; "stay
 * in A" must be published). */
static inline int regime_publish_needed(unsigned want, unsigned have, unsigned last_published)
{
    return (want != have) || (want != last_published);
}

/* Log name of the regime. */
static inline const char *regime_name(unsigned r)
{
    switch (r) {
    case REGIME_240P_SUPER_RES: return "240p_super_res";
    case REGIME_480I:           return "480i";
    case REGIME_480P:           return "480p";
    default:                    return "unknown";
    }
}

/* The receiver's 480p state. Six values, of which only one serves 480p.
 * "Not judged yet" (zero, the value of a static atomic before the blit thread
 * has judged) is NOT a refusal: the guard has said nothing. It is served as a
 * fallback like a refusal, but its name in the log does not blame it.
 * `R480P_LEGACY`: under --legacy-modeset 480p is never judged (no regime
 * resolver runs there). */
enum regime480p_state {
    R480P_NOT_JUDGED        = 0,
    R480P_ENABLED           = 1,
    R480P_NO_HIGH_BAND      = 2,
    R480P_GUARD_REFUSED     = 3,
    R480P_SWITCHRES_REFUSED = 4,
    R480P_LEGACY            = 5
};

/* The resolver's choice: `wire` is the wire SHAPE (what the emitter announces,
 * how to reassemble), `serve` the regime SERVED (the scan set on the tube),
 * `fallback` is 1 when the two differ because 480p is not enabled. */
struct regime_choice {
    enum daemon_regime wire;
    enum daemon_regime serve;
    int                fallback;
};

/* `wire` = regime_from_switchres, unchanged. `serve` = `wire`, except a 480p
 * when the state is not R480P_ENABLED: it is served as 480i. Only 480p falls
 * back: an interlaced mode at 31.5 kHz stays a 480i, a 240p stays a 240p. No
 * band is crossed: the fallback stays in the low band. */
static inline struct regime_choice
regime_choose(unsigned interlace, double pclock_mhz, unsigned h_total, int state480p)
{
    struct regime_choice c;
    c.wire     = regime_from_switchres(interlace, pclock_mhz, h_total);
    c.serve    = c.wire;
    c.fallback = 0;
    if (c.wire == REGIME_480P && state480p != R480P_ENABLED) {
        c.serve    = REGIME_480I;
        c.fallback = 1;
    }
    return c;
}

/* Must the tube be given back to 15 kHz? Only when the current regime is 480p
 * (the high band), the receiver is idle (silence of --idle-after seconds) and
 * the legacy path is not running (it crosses no band). Idle in 480i or 240p
 * crosses nothing: the tube is already in the low band. Pure function: the
 * guard, the floor and the band rule judge the crossing afterwards, they are
 * not part of this decision. */
static inline int regime_idle_return_wanted(unsigned current, int idle_active, int legacy)
{
    return current == (unsigned)REGIME_480P && idle_active != 0 && legacy == 0;
}

/* Log name of the 480p state: the "reason" of the fallback and of
 * regime_disabled. */
static inline const char *regime480p_reason(int state)
{
    switch (state) {
    case R480P_NOT_JUDGED:        return "not_ready";
    case R480P_ENABLED:           return "enabled";
    case R480P_NO_HIGH_BAND:      return "no_high_band";
    case R480P_GUARD_REFUSED:     return "guard_refused";
    case R480P_SWITCHRES_REFUSED: return "switchres_refused";
    case R480P_LEGACY:            return "legacy_modeset";
    default:                      return "unknown";
    }
}

#endif /* RECEIVER_REGIME_H */
