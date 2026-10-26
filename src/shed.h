/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECEIVER_SHED_H
#define RECEIVER_SHED_H

/*
 * shed.h -- R2, the adaptive drop of a frame.
 *
 * Pure module, header only, on the pattern of spsc.h: `static inline`
 * functions, no allocation, no system call, no global state. The blit thread
 * (blit.c) holds the state in `struct blit_internal` and calls these
 * functions; nothing here goes into the structures defined twice (net.h and
 * blit.c).
 *
 * The rule. The daemon keeps only one flip in flight: a frame requested
 * during the vblank only takes effect at the next one, and the loop can stay
 * in that "one frame late" state without getting out by itself. R2 gets out
 * of it by not displaying one frame:
 *   - for each frame displayed, its excess: how many vblanks too many it
 *     waited, between the moment it was ready and the moment of its flip;
 *   - when the last W frames displayed all have an excess of at least 1, the
 *     rule is armed; the frame taken at the next consume is dropped, and the
 *     window starts again from zero.
 *
 * The same rule is written a second time, in Python, by a flip simulator kept
 * outside this tree, to give the simulated cost of R2. Both writings are
 * proven equal on a single table of cases, `receiver/shed_cas.h`, played by
 * `test_shed.c` (make test) and by the simulator's tests. A gap between them
 * is fixed in the faulty code, never in the table.
 *
 * No field received from the network enters the decision: the excess depends
 * only on local timestamps (CLOCK_MONOTONIC) and on the active mode.
 */

#include <stdint.h>

/* The cost of converting the frame and calling the flip. The simulations run
 * with the same margin, 0.6 ms; a test checks that the two agree. */
#define SHED_MARGIN_NS 600000ULL

struct shed_state {
    uint32_t window;   /* W: number of late frames displayed that arms; 0 = off */
    uint32_t run;      /* run of frames displayed with an excess of at least 1 */
};

/* Duration, in ns, of `line_count` lines of a mode with `htotal` pixels per line at
 * `clock_khz` kHz: htotal * line_count * 10^6 / clock_khz, truncated. With vtotal,
 * the frame period; with vtotal - vdisplay, the vblank duration. A zero clock
 * gives 0, so a zero excess (shed_excess). */
static inline uint64_t shed_mode_ns(uint32_t htotal, uint32_t line_count,
                                    uint32_t clock_khz)
{
    if (clock_khz == 0u) {
        return 0u;
    }
    return (uint64_t)htotal * (uint64_t)line_count * 1000000ULL / (uint64_t)clock_khz;
}

/* Vblanks too many that a frame waited: 0 if `period_ns` is 0, or if
 * `flip_ts_ns - ready_ns <= vblank_ns + margin_ns` (a flip before the frame
 * was ready included); otherwise floor((flip_ts_ns - ready_ns - vblank_ns -
 * margin_ns) / period_ns). Unsigned 64-bit integers: the negative gap is
 * handled before any subtraction. */
static inline uint64_t shed_excess(uint64_t flip_ts_ns, uint64_t ready_ns,
                                   uint64_t period_ns, uint64_t vblank_ns,
                                   uint64_t margin_ns)
{
    if (period_ns == 0u || flip_ts_ns <= ready_ns) {
        return 0u;
    }
    uint64_t late_ns = flip_ts_ns - ready_ns;
    uint64_t allowed_ns = vblank_ns + margin_ns;
    if (late_ns <= allowed_ns) {
        return 0u;
    }
    return (late_ns - allowed_ns) / period_ns;
}

static inline void shed_init(struct shed_state *st, uint32_t window)
{
    st->window = window;
    st->run    = 0u;
}

/* Records the excess of the frame just displayed. Returns 1 if the rule is
 * armed (W > 0 and run >= W), 0 otherwise. The run saturates at UINT32_MAX
 * instead of wrapping (the Python has no ceiling; the gap only shows after
 * 2^32 frames, more than two years at 60 Hz). */
static inline int shed_note(struct shed_state *st, uint64_t excess)
{
    if (excess >= 1u) {
        if (st->run < UINT32_MAX) {
            st->run++;
        }
    } else {
        st->run = 0u;
    }
    return (st->window > 0u && st->run >= st->window) ? 1 : 0;
}

/* A frame was just dropped: the window starts again from zero. */
static inline void shed_done(struct shed_state *st)
{
    st->run = 0u;
}

/* Regime change, entering idle: the run so far says nothing about the new
 * state. Same effect as shed_done, named for the call site. */
static inline void shed_reset(struct shed_state *st)
{
    st->run = 0u;
}

#endif /* RECEIVER_SHED_H */
