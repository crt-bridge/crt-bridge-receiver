/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECEIVER_BANDRULE_H
#define RECEIVER_BANDRULE_H

/*
 * bandrule.h -- the band rule: how long the tube stays in a band, how many
 * times a minute it crosses it.
 *
 * Pure module, header only, on the pattern of shed.h: `static inline`
 * functions, no allocation, no system call, no global state. The state lives
 * in a `struct bandrule` held by the blit thread; the rule never reads the
 * clock, the caller gives it the instant.
 *
 * The rule. A band crossing (15 kHz <-> 31 kHz) is forbidden before
 *     max( last crossing + minimum dwell ,
 *          oldest of the last `cap` crossings + 60 s ).
 * The second term exists only once the ring of the last `cap` crossings is
 * full. The rule gives an INSTANT (`bandrule_not_before_ns`), never a refusal:
 * the caller delays the request until then, and the last one wins.
 *
 * What counts. Only band crossings are recorded here. The caller decides what
 * is one (`regime_crosses_band`, regime.h): 240p <-> 480i is not. Entering a
 * band at start-up counts as a crossing for the minimum dwell
 * (`last_cross_ns` = entry instant), not for the cap (the ring starts empty).
 *
 * The values (BAND_DWELL_MIN_NS, BAND_CROSSINGS_PER_MIN_MAX, in safety.h) were
 * set on 2026-10-18. This clock is separate from the 100 ms modeset floor.
 *
 * The rule is NEVER shortened outside the test mode without DRM: no function
 * here reads an option or the environment, and the starting duration comes
 * from safety.h.
 */

#include <stddef.h>
#include <stdint.h>
#include "safety.h"

#define BANDRULE_WINDOW_NS 60000000000ULL   /* window of the cap: 60 s */
#define BANDRULE_MAX_CAP   16u              /* ring size */

_Static_assert(BAND_CROSSINGS_PER_MIN_MAX <= BANDRULE_MAX_CAP,
               "band crossing cap must fit the bandrule ring");

struct bandrule {
    uint64_t dwell_ns;                 /* minimum time in a band */
    unsigned cap;                      /* crossings per 60 s window, 1..BANDRULE_MAX_CAP */
    uint64_t last_cross_ns;            /* last crossing, or band entry at start-up */
    uint64_t ring[BANDRULE_MAX_CAP];   /* timestamps of the last `cap` crossings */
    unsigned head;                     /* next slot to write; the oldest once the ring is full */
    unsigned n;                        /* slots in use, 0..cap */
};

/* Sets up the rule: `cap` clamped to [1, BANDRULE_MAX_CAP], empty ring,
 * `entered_ns` = instant the current band was entered. */
static inline void bandrule_init(struct bandrule *r, uint64_t dwell_ns,
                                 unsigned cap, uint64_t entered_ns)
{
    if (cap < 1u) {
        cap = 1u;
    }
    if (cap > BANDRULE_MAX_CAP) {
        cap = BANDRULE_MAX_CAP;
    }
    r->dwell_ns      = dwell_ns;
    r->cap           = cap;
    r->last_cross_ns = entered_ns;
    for (unsigned i = 0u; i < BANDRULE_MAX_CAP; i++) {
        r->ring[i] = 0u;
    }
    r->head = 0u;
    r->n    = 0u;
}

/* The instant before which a crossing is forbidden (allowed from that instant
 * on). `*by_cap` (if not NULL) is 1 when the cap term wins STRICTLY over the
 * minimum dwell, 0 otherwise. */
static inline uint64_t bandrule_not_before_ns(const struct bandrule *r, int *by_cap)
{
    uint64_t dwell_term = r->last_cross_ns + r->dwell_ns;
    uint64_t cap_term   = 0u;
    if (r->n >= r->cap) {
        cap_term = r->ring[r->head] + BANDRULE_WINDOW_NS;
    }
    if (by_cap != NULL) {
        *by_cap = (cap_term > dwell_term) ? 1 : 0;
    }
    return (cap_term > dwell_term) ? cap_term : dwell_term;
}

/* True if a crossing is allowed at `now_ns`. */
static inline int bandrule_allows(const struct bandrule *r, uint64_t now_ns)
{
    return now_ns >= bandrule_not_before_ns(r, NULL);
}

/* Records a band crossing made at `at_ns`. */
static inline void bandrule_note_crossing(struct bandrule *r, uint64_t at_ns)
{
    r->last_cross_ns = at_ns;
    r->ring[r->head] = at_ns;
    r->head = (r->head + 1u) % r->cap;
    if (r->n < r->cap) {
        r->n++;
    }
}

#endif /* RECEIVER_BANDRULE_H */
