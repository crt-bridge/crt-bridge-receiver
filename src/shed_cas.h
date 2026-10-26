/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECEIVER_SHED_CAS_H
#define RECEIVER_SHED_CAS_H

/*
 * shed_cas.h -- the reference cases of R2.
 *
 * What they are for. The drop rule is written twice: in C for the daemon
 * (receiver/shed.h) and in Python for the simulator that gives its cost (a
 * script kept outside this tree). These two tables are the rule as the
 * research wrote it; both codes must give exactly the expected value on them.
 *
 * Who reads them:
 *   - the C test of shed.h, which includes this file (make test);
 *   - the simulator's contract test, which reads it as text, line by line,
 *     with a simple regular expression.
 *
 * RULE: a case is added HERE, never in only one of the two tests. A case that
 * fails is fixed in the faulty code (shed.h or the simulator), never in the
 * table.
 *
 * Exact format, one line per case, nothing else on the line:
 *
 *   SHED_CAS_EXCESS:
 *     { flip_early, gap_ns, period_ns, vblank_ns, margin_ns, expected_out },
 *     decimal integers, `flip_early` with suffix `u`, the other five `ULL`.
 *     The frame is ready at SHED_CAS_BASE_NS; the flip falls at
 *     SHED_CAS_BASE_NS + gap_ns if flip_early is 0u, at SHED_CAS_BASE_NS -
 *     gap_ns if flip_early is 1u (flip before the frame is ready: a negative
 *     gap, written by this flag). `expected_out` is the excess, in vblanks.
 *
 *   SHED_CAS_WINDOW: { window, "step_seq", "expected_out" },
 *     `window` with suffix `u`. The `step_seq` string is played from left to
 *     right on a fresh state (shed_init(window)): a digit d is a recorded
 *     excess (shed_note(d)), `F` a drop done (shed_done). The `expected_out`
 *     string holds one `0` or `1` per DIGIT of `step_seq` (the `F` have
 *     none): the value shed_note returns at that step.
 *
 * Constants of the excess cases: T = 16,685,257 ns (240p period measured by
 * the research), VB = 1,375,000 ns, margin = 600,000 ns (SHED_MARGIN_NS). The
 * threshold of an excess of 1 is VB + margin + T = 18,660,257 ns.
 *
 * The start of a segment has no case here: the table only holds the rule (the
 * excess of a frame, the window), not the loop that plays it. The daemon
 * starts from a fresh window at start-up, at each regime change, at each mode
 * change (modeset_done) and at each entry into idle; the simulator starts from
 * a fresh window at each segment, whose vblank 0 it does not simulate. This
 * loop difference changes no case of this table.
 */

#include <stdint.h>

#define SHED_CAS_BASE_NS 1000000000000ULL

struct shed_cas_excess {
    unsigned flip_early;
    uint64_t gap_ns;
    uint64_t period_ns;
    uint64_t vblank_ns;
    uint64_t margin_ns;
    uint64_t expected_out;
};

struct shed_cas_window {
    unsigned    window;
    const char *step_seq;
    const char *expected_out;
};

static const struct shed_cas_excess SHED_CAS_EXCESS[] = {
    { 0u, 14690000ULL, 16685257ULL, 1375000ULL, 600000ULL, 0ULL },
    { 0u, 18650000ULL, 16685257ULL, 1375000ULL, 600000ULL, 0ULL },
    { 0u, 18660256ULL, 16685257ULL, 1375000ULL, 600000ULL, 0ULL },
    { 0u, 18660257ULL, 16685257ULL, 1375000ULL, 600000ULL, 1ULL },
    { 0u, 18670000ULL, 16685257ULL, 1375000ULL, 600000ULL, 1ULL },
    { 0u, 22820000ULL, 16685257ULL, 1375000ULL, 600000ULL, 1ULL },
    { 0u, 36000000ULL, 16685257ULL, 1375000ULL, 600000ULL, 2ULL },
    { 0u, 1000000ULL, 16685257ULL, 1375000ULL, 600000ULL, 0ULL },
    { 1u, 5000000ULL, 16685257ULL, 1375000ULL, 600000ULL, 0ULL },
    { 0u, 36000000ULL, 0ULL, 1375000ULL, 600000ULL, 0ULL },
};

static const struct shed_cas_window SHED_CAS_WINDOW[] = {
    { 3u, "111", "001" },
    { 3u, "110111", "000001" },
    { 3u, "111F111", "001001" },
    { 3u, "1111", "0011" },
    { 3u, "222", "001" },
    { 0u, "111", "000" },
};

#define SHED_CAS_N_EXCESS (sizeof SHED_CAS_EXCESS / sizeof SHED_CAS_EXCESS[0])
#define SHED_CAS_N_WINDOW (sizeof SHED_CAS_WINDOW / sizeof SHED_CAS_WINDOW[0])

#endif /* RECEIVER_SHED_CAS_H */
