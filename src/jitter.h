/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECEIVER_JITTER_H
#define RECEIVER_JITTER_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

struct jitter_ring;

struct jitter_ring *jitter_create(size_t capacity);
void               jitter_record(struct jitter_ring *r,
                                  uint64_t ts_ns,
                                  uint64_t delta_ns,
                                  uint32_t vblank_seq);
/* Writes header + rows; returns rows written (>= 0) or -1 on error. */
int                jitter_flush_csv(struct jitter_ring *r, FILE *fp);
void               jitter_summary(struct jitter_ring *r,
                                  double *p50,
                                  double *p95,
                                  double *p99,
                                  double *p999);
void               jitter_destroy(struct jitter_ring *r);

#endif /* RECEIVER_JITTER_H */
