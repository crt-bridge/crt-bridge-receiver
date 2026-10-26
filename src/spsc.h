/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECEIVER_SPSC_H
#define RECEIVER_SPSC_H

/*
 * Header-only single-producer / single-consumer lock-free ring.
 * Slot type is `void *` (caller owns payload memory).
 *
 * Capacity = SPSC_CAPACITY (must be a power of two; default 16,
 * override with -DSPSC_CAPACITY=N at compile time).
 *
 * Memory ordering: producer stores head with release; consumer reads
 * head with acquire (and vice versa for tail). No mutexes -- incompatible
 * with the sub-millisecond latency budget of the receiver.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

#ifndef SPSC_CAPACITY
#define SPSC_CAPACITY 16U
#endif

_Static_assert((SPSC_CAPACITY & (SPSC_CAPACITY - 1U)) == 0U,
               "SPSC_CAPACITY must be a power of two");

#define SPSC_MASK (SPSC_CAPACITY - 1U)

struct spsc_ring {
    _Atomic uint32_t head;   /* producer writes; consumer reads */
    _Atomic uint32_t tail;   /* consumer writes; producer reads */
    void *slots[SPSC_CAPACITY];
};

static inline void spsc_init(struct spsc_ring *r) {
    atomic_store_explicit(&r->head, 0U, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0U, memory_order_relaxed);
    for (uint32_t i = 0U; i < SPSC_CAPACITY; ++i) {
        r->slots[i] = NULL;
    }
}

/* Returns 1 on enqueue, 0 if ring full. */
static inline int spsc_try_produce(struct spsc_ring *r, void *slot) {
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    if ((uint32_t)(h - t) >= SPSC_CAPACITY) {
        return 0;
    }
    r->slots[h & SPSC_MASK] = slot;
    atomic_store_explicit(&r->head, h + 1U, memory_order_release);
    return 1;
}

/* Returns 1 on dequeue (writes *out), 0 if ring empty. */
static inline int spsc_try_consume(struct spsc_ring *r, void **out) {
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    if (h == t) {
        return 0;
    }
    *out = r->slots[t & SPSC_MASK];
    atomic_store_explicit(&r->tail, t + 1U, memory_order_release);
    return 1;
}

/* Consumes the MOST RECENT element and frees the stale ones in a single step.
 * Returns 1 (writes *out = most recent element, *skipped = number of older
 * elements dropped), 0 if the ring is empty (*skipped = 0).
 *
 * SPSC contract unchanged: a single consumer writes tail. Moving tail by n at
 * once is equivalent to n calls of spsc_try_consume -- handing the slots back
 * to the producer is carried by that same move of tail (no free list in this
 * project, see the round-robin current_slot_idx in net.c).
 *
 * Latency bounded by construction: the queue can no longer accumulate the
 * clock drift between emitter and receiver. */
static inline int spsc_consume_latest(struct spsc_ring *r, void **out,
                                      uint32_t *skipped) {
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    if (h == t) {
        *skipped = 0U;
        return 0;
    }
    uint32_t n = (uint32_t)(h - t);          /* 1 .. SPSC_CAPACITY */
    *out = r->slots[(uint32_t)(h - 1U) & SPSC_MASK];
    atomic_store_explicit(&r->tail, h, memory_order_release);
    *skipped = n - 1U;
    return 1;
}

static inline uint32_t spsc_count(const struct spsc_ring *r) {
    uint32_t h = atomic_load_explicit(&((struct spsc_ring *)r)->head,
                                      memory_order_acquire);
    uint32_t t = atomic_load_explicit(&((struct spsc_ring *)r)->tail,
                                      memory_order_acquire);
    return (uint32_t)(h - t);
}

#endif /* RECEIVER_SPSC_H */
