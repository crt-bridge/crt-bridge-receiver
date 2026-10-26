/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * input.h -- the wire contract of the Groovy input channel, port 32101/UDP
 * (the video port + 1), direction receiver -> emitter.
 *
 * The four layouts below come from a DIRECT reading of two independent
 * upstream sources, which agree to the bit:
 *   - `input_mister.cpp` (GroovyMAME 0.289, its input_mister module; not
 *     drawnogpu.cpp, which knows nothing of input)
 *   - `groovymister_wrapper.h` (the antonioginer/RetroArch fork, branch
 *     mister, groovymister library)
 * Unlike the video channel (blit opcode, size of CMD_INIT, delta mode: all
 * different between the two sources, see proto.h), there is NO upstream
 * divergence to settle here: a single layout serves all three references
 * (GroovyMAME, the fork, and our own encoder) at once.
 *
 * The twin of this file on the emitter side is libgm's `gm.h`; the two
 * must stay mirrors of each other, exactly like `proto.h` and `gm.h` for
 * the video channel.
 */

#ifndef GROOVY_INPUT_H
#define GROOVY_INPUT_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/* ---------------------------------------------------------------------------
 * Button masks: input_mister.cpp == groovymister_wrapper.h, the same values
 * bit for bit in both upstream sources.
 *
 * Bits 14 and 15 are defined by NEITHER source: leave them at zero, do not
 * invent them.
 * ------------------------------------------------------------------------- */
#define GROOVY_JOY_RIGHT (1u<<0)
#define GROOVY_JOY_LEFT  (1u<<1)
#define GROOVY_JOY_DOWN  (1u<<2)
#define GROOVY_JOY_UP    (1u<<3)
#define GROOVY_JOY_B1    (1u<<4)
#define GROOVY_JOY_B2    (1u<<5)
#define GROOVY_JOY_B3    (1u<<6)
#define GROOVY_JOY_B4    (1u<<7)
#define GROOVY_JOY_B5    (1u<<8)
#define GROOVY_JOY_B6    (1u<<9)
#define GROOVY_JOY_B7    (1u<<10)
#define GROOVY_JOY_B8    (1u<<11)
#define GROOVY_JOY_B9    (1u<<12)
#define GROOVY_JOY_B10   (1u<<13)

/* ---------------------------------------------------------------------------
 * Wire structures, packed on 1 byte (see the directive below), with field
 * names and order taken EXACTLY from the two upstream sources
 * (input_mister.cpp, groovymister_wrapper.h).
 *
 * Design note: the 9-byte message (groovy_joy_dig_inputs) is a legitimate
 * case DISTINCT from the 17-byte message (groovy_joy_inputs), not a
 * truncation: GroovyMAME and the fork accept both lengths on the joystick
 * channel. The encoder may therefore legitimately send 9 bytes when no
 * active analogue axis needs the 8 axis bytes.
 * ------------------------------------------------------------------------- */
#pragma pack(push, 1)

struct groovy_joy_dig_inputs {
    uint32_t frame;
    uint8_t  order;
    uint16_t joy1;
    uint16_t joy2;
};

struct groovy_joy_inputs {
    uint32_t frame;
    uint8_t  order;
    uint16_t joy1;
    uint16_t joy2;
    int8_t   joy1_lx, joy1_ly, joy1_rx, joy1_ry;
    int8_t   joy2_lx, joy2_ly, joy2_rx, joy2_ry;
};

struct groovy_keyboard_inputs {
    uint32_t frame;
    uint8_t  order;
    uint8_t  keys[32];
};

struct groovy_ps2_inputs {
    uint32_t frame;
    uint8_t  order;
    uint8_t  keys[32];
    uint8_t  mouse_bits, mouse_x, mouse_y, mouse_z;
};

#pragma pack(pop)

/* Size guards, placed IMMEDIATELY after the pack(pop) directive, together
 * with the structures, not as an afterthought. Without them, a compiler's
 * padding would produce lengths that GroovyMAME rejects silently (none of
 * the sizes 9/17/37/41 would match its switch); a structure duplicated
 * without a guard is exactly how such a mismatch goes unnoticed. */
_Static_assert(sizeof(struct groovy_joy_dig_inputs) == 9,
    "upstream layout (input_mister.cpp / groovymister_wrapper.h): do not change without re-checking");
_Static_assert(sizeof(struct groovy_joy_inputs) == 17,
    "upstream layout (input_mister.cpp / groovymister_wrapper.h): do not change without re-checking");
_Static_assert(sizeof(struct groovy_keyboard_inputs) == 37,
    "upstream layout (input_mister.cpp / groovymister_wrapper.h): do not change without re-checking");
_Static_assert(sizeof(struct groovy_ps2_inputs) == 41,
    "upstream layout (input_mister.cpp / groovymister_wrapper.h): do not change without re-checking");

/* Calibration of an analogue axis (its range as the device reports it). Not
 * a wire type (never serialised as is): no special packing nor size assert
 * needed here. */
struct groovy_axis_cal {
    int32_t min, max;
    int     have;
};

/* ---------------------------------------------------------------------------
 * The pure half of input.c: buttons, axis quantisation, packing of the four
 * layouts above.
 *
 * groovy_input_pack_joy: with_analog == 0 produces 9 bytes (digital only),
 * != 0 produces 17 (with the 8 axis bytes). groovy_input_pack_ps2:
 * mouse == NULL produces 37 bytes, non-NULL produces 41. Both return 0 if
 * `cap` is too small: never a partial write.
 * ------------------------------------------------------------------------- */
uint16_t groovy_input_button_bit(unsigned short evdev_code);
int8_t   groovy_input_scale_axis(const struct groovy_axis_cal *cal, int32_t value);
size_t   groovy_input_pack_joy(uint8_t *out, size_t cap, uint32_t frame, uint8_t order,
                               uint16_t joy1, uint16_t joy2,
                               const int8_t axes[8], int with_analog);
size_t   groovy_input_pack_ps2(uint8_t *out, size_t cap, uint32_t frame, uint8_t order,
                               const uint8_t keys[32], const uint8_t mouse[4]);

/* ---------------------------------------------------------------------------
 * The pure half of the keyboard/mouse channel.
 *
 * groovy_input_evdev_to_sdl: table evdev -> SDL scancode (see its origin in
 * input.c); returns -1 if the evdev code has no known SDL scancode OR if
 * that scancode is beyond the 256 positions of the `keys[32]` field.
 * groovy_input_set_key_bit: sets or clears the matching bit in `keys[32]`;
 * returns 1 if the evdev code was within the handled range, 0 otherwise
 * (the caller must log that case, not file it away silently).
 * groovy_input_encode_mouse_delta: encodes ONE accumulated relative axis (dx
 * or dy) into the matching PS/2 byte, the exact inverse of the upstream
 * decoder in input_mister.cpp (`rel = byte - ((bits << n) & 0x100)`);
 * bounds the input to [-256, 255] and reports saturation through
 * *saturated.
 * groovy_input_encode_mouse: builds the four bytes `mouse[4]` (mouse_bits,
 * mouse_x, mouse_y, mouse_z) consumed by groovy_input_pack_ps2, buttons and
 * sign bits included.
 * ------------------------------------------------------------------------- */
int      groovy_input_evdev_to_sdl(unsigned short evdev_code);
int      groovy_input_set_key_bit(uint8_t keys[32], unsigned short evdev_code, int pressed);
uint8_t  groovy_input_encode_mouse_delta(int32_t delta, int *sign_bit, int *saturated);
void     groovy_input_encode_mouse(uint8_t mouse[4], int32_t dx, int32_t dy, int32_t dz,
                                   uint8_t buttons, int *saturated_x, int *saturated_y);

/* ---------------------------------------------------------------------------
 * The Linux half of input.c: the dedicated thread that listens for a
 * one-byte hook on `listen_port`, reads the pad through evdev with
 * hot-plugging, and sends to the learned emitter the packets that the pure
 * encoder above builds.
 *
 * `struct net_blit_shared` stays opaque here: only input.c includes it
 * (`net.h`), read-only on `ack_frame`. The input channel adds NO field to it
 * and copies none: that structure is defined twice (net.h and blit.c), and
 * every new field there is a chance for the two copies to drift.
 * ------------------------------------------------------------------------- */
struct net_blit_shared;

struct input_thread_config {
    uint16_t                 listen_port;         /* input channel port; 0 = disabled */
    int                      cpu_affinity;        /* -1 not to pin */
    int                      sched_fifo_priority; /* 70 by default, NEVER above net/blit: the picture comes first */
    unsigned                 rescan_ms;           /* period of the /dev/input rescan, for hot-plugging */
    struct net_blit_shared  *shared;              /* read-only: ack_frame */
};

int  input_thread_start(struct input_thread_config *cfg, pthread_t *out_tid);
void input_thread_request_stop(void);

#endif /* GROOVY_INPUT_H */
