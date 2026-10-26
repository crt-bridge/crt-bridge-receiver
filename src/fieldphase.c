/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fieldphase.c -- pure implementation of fieldphase.h. See that file for the
 * why. No include beyond <string.h>: no DRM, no state.
 */
#include "fieldphase.h"

#include <string.h>

int field_phase_parse(const char *s, uint8_t *out)
{
    if (s == NULL || out == NULL) return -1;
    if (strcmp(s, "off") == 0) { *out = (uint8_t)FIELD_PHASE_OFF; return 0; }
    if (strcmp(s, "a")   == 0) { *out = (uint8_t)FIELD_PHASE_A;   return 0; }
    if (strcmp(s, "b")   == 0) { *out = (uint8_t)FIELD_PHASE_B;   return 0; }
    return -1;
}

uint8_t field_phase_pair_open_parity(uint32_t completing_frame_id)
{
    return (uint8_t)((completing_frame_id + 1u) & 1u);
}

int field_phase_delay_needed(uint8_t mode, uint8_t pair_open_parity,
                             uint32_t next_seq)
{
    uint8_t scanned_parity;
    if (mode != (uint8_t)FIELD_PHASE_A && mode != (uint8_t)FIELD_PHASE_B)
        return 0;                       /* OFF and any unknown value */
    scanned_parity = (uint8_t)(next_seq & 1u);  /* convention K = 0 */
    if (mode == (uint8_t)FIELD_PHASE_B)
        scanned_parity = (uint8_t)(scanned_parity ^ 1u);
    return (scanned_parity != (pair_open_parity & 1u)) ? 1 : 0;
}
