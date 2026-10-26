/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RECEIVER_FIELDPHASE_H
#define RECEIVER_FIELDPHASE_H

#include <stdint.h>

/*
 * fieldphase.h -- on which field to land the flip in 480i.
 *
 * The problem, in one sentence: the paired 0x06 stream delivers two half
 * frames under two consecutive numbers, n then n+1; the older one must be
 * scanned first, and nothing in the daemon guaranteed it. Four cases follow
 * from the parity of the pair's opening half (n) and the parity of the field
 * the flip would land on; the flip is delayed exactly when the two differ.
 *
 * This module is PURE: no state, no DRM, no allocation, no logging. It
 * answers a single question -- "must this flip be delayed by one field?" --
 * and is tested without hardware.
 *
 * Convention K = 0: mode A assumes that the field scanned at vblank sequence
 * s carries the lines of parity (s & 1). That relation is an UNKNOWN hardware
 * constant; mode B is its exact complement. Nobody knows in advance which of
 * the two is right.
 *
 * Caution: on the kernel in service, the vblank sequence of a 480i mode
 * counts whole images, not fields (see the caller in blit.c), so the delay
 * this module asks for is a whole image. The --field-phase lever is a
 * measuring instrument, not a fix for the field order.
 */

enum field_phase_mode {
    FIELD_PHASE_OFF = 0,   /* the behaviour without field phasing, byte for byte */
    FIELD_PHASE_A   = 1,
    FIELD_PHASE_B   = 2
};

/* Parses "off" | "a" | "b" (exactly, case-sensitive). Returns 0 and sets
 * *out on success; returns -1 without touching *out otherwise, including
 * when s or out is NULL. */
int field_phase_parse(const char *s, uint8_t *out);

/* Opening parity of the pair whose slot carries the number of the half frame
 * THAT COMPLETES it (n+1: the pair_continues branch of the CMD_BLIT_VSYNC
 * case of receiver/net.c copies hdr_v.frame_id into the slot). The opening
 * is n, hence (completing_frame_id + 1) & 1. Unsigned arithmetic: defined
 * even when frame_id wraps around. */
uint8_t field_phase_pair_open_parity(uint32_t completing_frame_id);

/* 1 if the flip must be delayed by one field, 0 otherwise.
 *   mode              : FIELD_PHASE_OFF/A/B; any other value returns 0.
 *   pair_open_parity  : output of field_phase_pair_open_parity (0 or 1).
 *   next_seq          : vblank sequence at which the flip would latch
 *                       WITHOUT delay, that is (current sequence + 1).
 * No side effect. */
int field_phase_delay_needed(uint8_t mode, uint8_t pair_open_parity,
                             uint32_t next_seq);

#endif /* RECEIVER_FIELDPHASE_H */
