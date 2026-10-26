/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * input.c -- the Groovy input channel (port 32101/UDP): button table, axis
 * quantisation, packing of the four layouts of `input.h`, then the thread
 * that reads evdev devices and sends their state to the emitter.
 *
 * Two halves behind a guard: the pure half first (this file up to the
 * guard), then everything that needs evdev, sockets and pthread. A unit test
 * defines the guard macro, then does `#include "input.c"`, and reaches only
 * the pure half, so the encoders run on any host.
 */

/* _GNU_SOURCE must come before ANY #include, even in the pure half: once
 * <features.h> is processed by the first glibc header (here <string.h>
 * below), its include guard freezes _USE_GNU for the whole file. Setting it
 * further down, just before the Linux half, would be too late to make
 * `pthread_setaffinity_np` visible (the same trap as net.c/pcm.c, which
 * both set it on their very first line). No effect on the pure half: none
 * of the <string.h> functions it uses depends on it. */
#define _GNU_SOURCE

#include "input.h"

#include <string.h>
#include <stdint.h>

/* The evdev codes the button table below needs, defined locally so that the
 * pure half compiles on a host without <linux/input.h> (Windows/MinGW in
 * particular). Guarded by #ifndef: when the Linux half includes the real
 * <linux/input.h>, the values match exactly and the #ifndef stay silent;
 * no conflict, no diverging redefinition. Numeric values:
 * linux/input-event-codes.h. */
#ifndef BTN_SOUTH
#define BTN_SOUTH   0x130
#endif
#ifndef BTN_EAST
#define BTN_EAST    0x131
#endif
#ifndef BTN_NORTH
#define BTN_NORTH   0x133
#endif
#ifndef BTN_WEST
#define BTN_WEST    0x134
#endif
#ifndef BTN_TL
#define BTN_TL      0x136
#endif
#ifndef BTN_TR
#define BTN_TR      0x137
#endif
#ifndef BTN_TL2
#define BTN_TL2     0x138
#endif
#ifndef BTN_TR2
#define BTN_TR2     0x139
#endif
#ifndef BTN_SELECT
#define BTN_SELECT  0x13a
#endif
#ifndef BTN_START
#define BTN_START   0x13b
#endif
#ifndef BTN_MODE
#define BTN_MODE    0x13c
#endif
#ifndef BTN_THUMBL
#define BTN_THUMBL  0x13d
#endif
#ifndef BTN_THUMBR
#define BTN_THUMBR  0x13e
#endif

/* ---------------------------------------------------------------------------
 * groovy_input_button_bit -- table evdev -> Groovy mask.
 *
 * The B1..B10 mapping is not free: it must produce, at the CONSUMER (the
 * `mister_joypad.c` driver), the button the player expects. The table comes
 * from composing two direct readings of upstream code, neither of them
 * guessed:
 *
 *   1. `mister_joypad.c` (function mister_pad_get_button,
 *      input/drivers_joypad/mister_joypad.c), GPL-3.0-or-later, fork
 *      antonioginer/RetroArch,
 *      https://raw.githubusercontent.com/antonioginer/RetroArch/mister/
 *      input/drivers_joypad/mister_joypad.c: the
 *      RETRO_DEVICE_ID_JOYPAD_* -> Groovy bit mapping the driver ACTUALLY
 *      consumes:
 *        _B -> GMW_JOY_B1   _A -> GMW_JOY_B2   _Y -> GMW_JOY_B3
 *        _X -> GMW_JOY_B4   _SELECT -> GMW_JOY_B5   _START -> GMW_JOY_B6
 *        _L -> GMW_JOY_B7   _R -> GMW_JOY_B8   _L2 -> GMW_JOY_B9
 *        _R2 -> GMW_JOY_B10; NO case for _L3/_R3: the upstream switch has
 *        only 14 entries (4 directions + 10 buttons) and falls back on
 *        `return 0` for everything else.
 *   2. the PHYSICAL evdev -> RetroPad id mapping already proven on the
 *      bench with the older Network Gamepad protocol: BTN_SOUTH->_B,
 *      BTN_EAST->_A, BTN_NORTH->_X, BTN_WEST->_Y, BTN_TL/TR->_L/_R,
 *      BTN_TL2/TR2->_L2/_R2, BTN_SELECT/START->_SELECT/_START,
 *      BTN_THUMBL/THUMBR->_L3/_R3.
 *
 * Composing the two: BTN_SOUTH->B1, BTN_EAST->B2, BTN_WEST->B3,
 * BTN_NORTH->B4, BTN_SELECT->B5, BTN_START->B6, BTN_TL->B7, BTN_TR->B8,
 * BTN_TL2->B9, BTN_TR2->B10. BTN_THUMBL/BTN_THUMBR have NO equivalent on
 * the wire: the MiSTer protocol has never carried L3/R3 (consistent with
 * `input.h`: bits 14/15 are undefined upstream, to be left at zero), so
 * they return 0, like any non-pad key (e.g. KEY_A). The directions (the
 * d-pad) do NOT go through this function: they arrive as
 * EV_ABS/ABS_HAT0X/ABS_HAT0Y and are translated in the Linux half to
 * GROOVY_JOY_LEFT/RIGHT/UP/DOWN.
 * ------------------------------------------------------------------------- */
uint16_t groovy_input_button_bit(unsigned short evdev_code)
{
    switch (evdev_code) {
        case BTN_SOUTH:  return GROOVY_JOY_B1;
        case BTN_EAST:   return GROOVY_JOY_B2;
        case BTN_WEST:   return GROOVY_JOY_B3;
        case BTN_NORTH:  return GROOVY_JOY_B4;
        case BTN_SELECT: return GROOVY_JOY_B5;
        case BTN_START:  return GROOVY_JOY_B6;
        case BTN_TL:     return GROOVY_JOY_B7;
        case BTN_TR:     return GROOVY_JOY_B8;
        case BTN_TL2:    return GROOVY_JOY_B9;
        case BTN_TR2:    return GROOVY_JOY_B10;
        default:         return 0;
    }
}

/* ---------------------------------------------------------------------------
 * groovy_input_scale_axis -- EVIOCGABS calibration and a 10% dead zone,
 * the same as the older bench pad client, with ONE thing changed: the final
 * quantisation. That client returned an int16_t (norm multiplied by the
 * bounds of that range); here it is a signed int8_t (`norm * 128.0` /
 * `norm * 127.0`). Trap: truncating an int16 to an int8 afterwards would
 * change the scale, not only the range; hence the quantisation redone from
 * `norm`, not from an intermediate int16.
 * ------------------------------------------------------------------------- */
int8_t groovy_input_scale_axis(const struct groovy_axis_cal *cal, int32_t value)
{
    double mid, half, norm;

    if (!cal->have || cal->max <= cal->min)
        return 0;

    mid  = ((double)cal->max + (double)cal->min) / 2.0;
    half = ((double)cal->max - (double)cal->min) / 2.0;
    norm = ((double)value - mid) / half;

    if (norm > 1.0) norm = 1.0;
    if (norm < -1.0) norm = -1.0;
    if (norm > -0.10 && norm < 0.10) norm = 0.0;

    if (norm < 0.0)
        return (int8_t)(norm * 128.0);
    return (int8_t)(norm * 127.0);
}

/* ---------------------------------------------------------------------------
 * groovy_input_pack_joy -- fills a `struct groovy_joy_dig_inputs` (9) or
 * `struct groovy_joy_inputs` (17) on the stack, then memcpy to `out`. The
 * pack(1) structures of `input.h` are the only writer, no bytes by hand:
 * the size asserts of `input.h` are the guard, and both machines are
 * little-endian (the receiver is Linux x86_64, the emitter Windows x86_64),
 * like the wire. Returns the length written, or 0 if `cap` is too small:
 * never a partial write.
 * ------------------------------------------------------------------------- */
size_t groovy_input_pack_joy(uint8_t *out, size_t cap, uint32_t frame, uint8_t order,
                              uint16_t joy1, uint16_t joy2,
                              const int8_t axes[8], int with_analog)
{
    if (!with_analog) {
        struct groovy_joy_dig_inputs pkt;

        if (cap < sizeof(pkt))
            return 0;

        pkt.frame = frame;
        pkt.order = order;
        pkt.joy1  = joy1;
        pkt.joy2  = joy2;
        memcpy(out, &pkt, sizeof(pkt));
        return sizeof(pkt);
    } else {
        struct groovy_joy_inputs pkt;

        if (cap < sizeof(pkt))
            return 0;

        pkt.frame = frame;
        pkt.order = order;
        pkt.joy1  = joy1;
        pkt.joy2  = joy2;
        pkt.joy1_lx = axes[0]; pkt.joy1_ly = axes[1];
        pkt.joy1_rx = axes[2]; pkt.joy1_ry = axes[3];
        pkt.joy2_lx = axes[4]; pkt.joy2_ly = axes[5];
        pkt.joy2_rx = axes[6]; pkt.joy2_ry = axes[7];
        memcpy(out, &pkt, sizeof(pkt));
        return sizeof(pkt);
    }
}

/* ---------------------------------------------------------------------------
 * groovy_input_pack_ps2 -- the same pattern as groovy_input_pack_joy for the
 * 37-byte (keyboard only) and 41-byte (keyboard + mouse) layouts.
 * ------------------------------------------------------------------------- */
size_t groovy_input_pack_ps2(uint8_t *out, size_t cap, uint32_t frame, uint8_t order,
                              const uint8_t keys[32], const uint8_t mouse[4])
{
    if (!mouse) {
        struct groovy_keyboard_inputs pkt;

        if (cap < sizeof(pkt))
            return 0;

        pkt.frame = frame;
        pkt.order = order;
        memcpy(pkt.keys, keys, sizeof(pkt.keys));
        memcpy(out, &pkt, sizeof(pkt));
        return sizeof(pkt);
    } else {
        struct groovy_ps2_inputs pkt;

        if (cap < sizeof(pkt))
            return 0;

        pkt.frame = frame;
        pkt.order = order;
        memcpy(pkt.keys, keys, sizeof(pkt.keys));
        pkt.mouse_bits = mouse[0];
        pkt.mouse_x    = mouse[1];
        pkt.mouse_y    = mouse[2];
        pkt.mouse_z    = mouse[3];
        memcpy(out, &pkt, sizeof(pkt));
        return sizeof(pkt);
    }
}

/* ---------------------------------------------------------------------------
 * Meaning of keys[32], CONFIRMED BY DIRECT READING of the two independent
 * upstream consumers, not assumed:
 *
 *   1. GroovyMAME (antonioginer/GroovyMAME, BSD-3-Clause),
 *      `input_mister.cpp` (mister_keyboard_device::update):
 *          for (i = 0; i < 256; i++)
 *              m_state[i] = (1 & (keys[i/8] >> (i%8))) ? 0x80 : 0x00;
 *      a field of 256 BITS, one bit per position, NOT a list of pressed
 *      codes. The indexing is filled by `configure()` (same file) through
 *      `table[keynum].sdl_scancode`: MAME's `keyboard_trans_table` maps
 *      each key to an SDL SCANCODE (SDL_Scancode, 0-255), which matches to
 *      the bit the comment in `groovymister_wrapper.h` (psakhis/Groovy_MiSTer,
 *      GPL-2.0-only, a documented protocol fact).
 *
 *   2. Fork antonioginer/RetroArch (GPL-3.0-or-later), `mister_input.c`
 *      (mister_input_poll):
 *          for (i=0; i<256; i++)
 *              bit_pos = 1 & (ps2Inputs.ps2Keys[i/8] >> (i%8));
 *      the SAME bit-per-position layout over 256 positions: no divergence
 *      of SIZE or PRINCIPLE with GroovyMAME. The index i is then translated
 *      by `input_keymaps_translate_keysym_to_rk(i)`, which looks up a table
 *      filled by the active windowing backend; under SDL2 (`input_keymaps.c`,
 *      `rarch_key_map_sdl[]`) that table maps `SDLK_*` (logical keycodes),
 *      not `SDL_SCANCODE_*` (physical scancodes) in the strict sense of the
 *      SDL2 naming. That is a NAMING nuance on the fork's side, not a
 *      PROTOCOL divergence: both upstream sources agree on the shape of the
 *      field (256 bits, direct indexing by position), and GroovyMAME, which
 *      explicitly names its index "sdl_scancode" in its own translation
 *      table, is the reference that settles the exact detail anyway.
 *
 * No DISAGREEMENT between the two sources on the size or principle of the
 * field, so a single encoder serves both.
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Table evdev -> SDL scancode. From a PUBLISHED source, not derived by hand
 * (a hand-made table over 150+ mappings carries a high and silent risk of
 * error):
 *
 *   https://raw.githubusercontent.com/libsdl-org/SDL/SDL2/src/events/scancodes_linux.h
 *   branch SDL2 (`linux_scancode_table[]`, indexed by evdev code /
 *   linux/input-event-codes.h).
 *
 * Each `SDL_SCANCODE_*` symbol of that table was resolved to its numeric
 * value from the same branch of the same repository
 * (`include/SDL_scancode.h`) by a script (not a hand transcription), then
 * checked on three known entries before integration: KEY_A(30)->4,
 * KEY_F2(60)->59, KEY_UP(103)->82, which a unit test hard-codes as is.
 *
 * Only the entries whose resulting SDL scancode is < 256 are kept below:
 * the `keys[32]` field has only 256 positions (32*8). 31 multimedia/consumer
 * entries of `linux_scancode_table[]` (KEY_WWW, KEY_MAIL, KEY_PLAYPAUSE,
 * etc.) resolve to an SDL scancode >= 256 (e.g. SDL_SCANCODE_CALCULATOR=266)
 * and are ABSENT by construction, not by oversight:
 * `groovy_input_evdev_to_sdl` returns them as unmapped (-1), and the caller
 * logs that case once rather than filing them silently in a wrong slot.
 *
 * Index = evdev code (KEY_*), value = SDL scancode. 0 == SDL_SCANCODE_
 * UNKNOWN, never a real value assigned below (checked at generation: every
 * source line pointing to SDL_SCANCODE_UNKNOWN was left out), so it serves
 * as the "unmapped" sentinel for every position this designated
 * initialiser omits (C's implicit zero-init).
 * ------------------------------------------------------------------------- */
static const uint8_t groovy_evdev_to_sdl[256] = {
    [  1] =  41, /* KEY_ESC              -> SDL_SCANCODE_ESCAPE */
    [  2] =  30, /* KEY_1                -> SDL_SCANCODE_1 */
    [  3] =  31, /* KEY_2                -> SDL_SCANCODE_2 */
    [  4] =  32, /* KEY_3                -> SDL_SCANCODE_3 */
    [  5] =  33, /* KEY_4                -> SDL_SCANCODE_4 */
    [  6] =  34, /* KEY_5                -> SDL_SCANCODE_5 */
    [  7] =  35, /* KEY_6                -> SDL_SCANCODE_6 */
    [  8] =  36, /* KEY_7                -> SDL_SCANCODE_7 */
    [  9] =  37, /* KEY_8                -> SDL_SCANCODE_8 */
    [ 10] =  38, /* KEY_9                -> SDL_SCANCODE_9 */
    [ 11] =  39, /* KEY_0                -> SDL_SCANCODE_0 */
    [ 12] =  45, /* KEY_MINUS            -> SDL_SCANCODE_MINUS */
    [ 13] =  46, /* KEY_EQUAL            -> SDL_SCANCODE_EQUALS */
    [ 14] =  42, /* KEY_BACKSPACE        -> SDL_SCANCODE_BACKSPACE */
    [ 15] =  43, /* KEY_TAB              -> SDL_SCANCODE_TAB */
    [ 16] =  20, /* KEY_Q                -> SDL_SCANCODE_Q */
    [ 17] =  26, /* KEY_W                -> SDL_SCANCODE_W */
    [ 18] =   8, /* KEY_E                -> SDL_SCANCODE_E */
    [ 19] =  21, /* KEY_R                -> SDL_SCANCODE_R */
    [ 20] =  23, /* KEY_T                -> SDL_SCANCODE_T */
    [ 21] =  28, /* KEY_Y                -> SDL_SCANCODE_Y */
    [ 22] =  24, /* KEY_U                -> SDL_SCANCODE_U */
    [ 23] =  12, /* KEY_I                -> SDL_SCANCODE_I */
    [ 24] =  18, /* KEY_O                -> SDL_SCANCODE_O */
    [ 25] =  19, /* KEY_P                -> SDL_SCANCODE_P */
    [ 26] =  47, /* KEY_LEFTBRACE        -> SDL_SCANCODE_LEFTBRACKET */
    [ 27] =  48, /* KEY_RIGHTBRACE       -> SDL_SCANCODE_RIGHTBRACKET */
    [ 28] =  40, /* KEY_ENTER            -> SDL_SCANCODE_RETURN */
    [ 29] = 224, /* KEY_LEFTCTRL         -> SDL_SCANCODE_LCTRL */
    [ 30] =   4, /* KEY_A                -> SDL_SCANCODE_A */
    [ 31] =  22, /* KEY_S                -> SDL_SCANCODE_S */
    [ 32] =   7, /* KEY_D                -> SDL_SCANCODE_D */
    [ 33] =   9, /* KEY_F                -> SDL_SCANCODE_F */
    [ 34] =  10, /* KEY_G                -> SDL_SCANCODE_G */
    [ 35] =  11, /* KEY_H                -> SDL_SCANCODE_H */
    [ 36] =  13, /* KEY_J                -> SDL_SCANCODE_J */
    [ 37] =  14, /* KEY_K                -> SDL_SCANCODE_K */
    [ 38] =  15, /* KEY_L                -> SDL_SCANCODE_L */
    [ 39] =  51, /* KEY_SEMICOLON        -> SDL_SCANCODE_SEMICOLON */
    [ 40] =  52, /* KEY_APOSTROPHE       -> SDL_SCANCODE_APOSTROPHE */
    [ 41] =  53, /* KEY_GRAVE            -> SDL_SCANCODE_GRAVE */
    [ 42] = 225, /* KEY_LEFTSHIFT        -> SDL_SCANCODE_LSHIFT */
    [ 43] =  49, /* KEY_BACKSLASH        -> SDL_SCANCODE_BACKSLASH */
    [ 44] =  29, /* KEY_Z                -> SDL_SCANCODE_Z */
    [ 45] =  27, /* KEY_X                -> SDL_SCANCODE_X */
    [ 46] =   6, /* KEY_C                -> SDL_SCANCODE_C */
    [ 47] =  25, /* KEY_V                -> SDL_SCANCODE_V */
    [ 48] =   5, /* KEY_B                -> SDL_SCANCODE_B */
    [ 49] =  17, /* KEY_N                -> SDL_SCANCODE_N */
    [ 50] =  16, /* KEY_M                -> SDL_SCANCODE_M */
    [ 51] =  54, /* KEY_COMMA            -> SDL_SCANCODE_COMMA */
    [ 52] =  55, /* KEY_DOT              -> SDL_SCANCODE_PERIOD */
    [ 53] =  56, /* KEY_SLASH            -> SDL_SCANCODE_SLASH */
    [ 54] = 229, /* KEY_RIGHTSHIFT       -> SDL_SCANCODE_RSHIFT */
    [ 55] =  85, /* KEY_KPASTERISK       -> SDL_SCANCODE_KP_MULTIPLY */
    [ 56] = 226, /* KEY_LEFTALT          -> SDL_SCANCODE_LALT */
    [ 57] =  44, /* KEY_SPACE            -> SDL_SCANCODE_SPACE */
    [ 58] =  57, /* KEY_CAPSLOCK         -> SDL_SCANCODE_CAPSLOCK */
    [ 59] =  58, /* KEY_F1               -> SDL_SCANCODE_F1 */
    [ 60] =  59, /* KEY_F2               -> SDL_SCANCODE_F2 */
    [ 61] =  60, /* KEY_F3               -> SDL_SCANCODE_F3 */
    [ 62] =  61, /* KEY_F4               -> SDL_SCANCODE_F4 */
    [ 63] =  62, /* KEY_F5               -> SDL_SCANCODE_F5 */
    [ 64] =  63, /* KEY_F6               -> SDL_SCANCODE_F6 */
    [ 65] =  64, /* KEY_F7               -> SDL_SCANCODE_F7 */
    [ 66] =  65, /* KEY_F8               -> SDL_SCANCODE_F8 */
    [ 67] =  66, /* KEY_F9               -> SDL_SCANCODE_F9 */
    [ 68] =  67, /* KEY_F10              -> SDL_SCANCODE_F10 */
    [ 69] =  83, /* KEY_NUMLOCK          -> SDL_SCANCODE_NUMLOCKCLEAR */
    [ 70] =  71, /* KEY_SCROLLLOCK       -> SDL_SCANCODE_SCROLLLOCK */
    [ 71] =  95, /* KEY_KP7              -> SDL_SCANCODE_KP_7 */
    [ 72] =  96, /* KEY_KP8              -> SDL_SCANCODE_KP_8 */
    [ 73] =  97, /* KEY_KP9              -> SDL_SCANCODE_KP_9 */
    [ 74] =  86, /* KEY_KPMINUS          -> SDL_SCANCODE_KP_MINUS */
    [ 75] =  92, /* KEY_KP4              -> SDL_SCANCODE_KP_4 */
    [ 76] =  93, /* KEY_KP5              -> SDL_SCANCODE_KP_5 */
    [ 77] =  94, /* KEY_KP6              -> SDL_SCANCODE_KP_6 */
    [ 78] =  87, /* KEY_KPPLUS           -> SDL_SCANCODE_KP_PLUS */
    [ 79] =  89, /* KEY_KP1              -> SDL_SCANCODE_KP_1 */
    [ 80] =  90, /* KEY_KP2              -> SDL_SCANCODE_KP_2 */
    [ 81] =  91, /* KEY_KP3              -> SDL_SCANCODE_KP_3 */
    [ 82] =  98, /* KEY_KP0              -> SDL_SCANCODE_KP_0 */
    [ 83] =  99, /* KEY_KPDOT            -> SDL_SCANCODE_KP_PERIOD */
    [ 85] = 148, /* KEY_ZENKAKUHANKAKU   -> SDL_SCANCODE_LANG5 */
    [ 86] = 100, /* KEY_102ND            -> SDL_SCANCODE_NONUSBACKSLASH */
    [ 87] =  68, /* KEY_F11              -> SDL_SCANCODE_F11 */
    [ 88] =  69, /* KEY_F12              -> SDL_SCANCODE_F12 */
    [ 89] = 135, /* KEY_RO               -> SDL_SCANCODE_INTERNATIONAL1 */
    [ 90] = 146, /* KEY_KATAKANA         -> SDL_SCANCODE_LANG3 */
    [ 91] = 147, /* KEY_HIRAGANA         -> SDL_SCANCODE_LANG4 */
    [ 92] = 138, /* KEY_HENKAN           -> SDL_SCANCODE_INTERNATIONAL4 */
    [ 93] = 136, /* KEY_KATAKANAHIRAGANA -> SDL_SCANCODE_INTERNATIONAL2 */
    [ 94] = 139, /* KEY_MUHENKAN         -> SDL_SCANCODE_INTERNATIONAL5 */
    [ 95] = 139, /* KEY_KPJPCOMMA        -> SDL_SCANCODE_INTERNATIONAL5 */
    [ 96] =  88, /* KEY_KPENTER          -> SDL_SCANCODE_KP_ENTER */
    [ 97] = 228, /* KEY_RIGHTCTRL        -> SDL_SCANCODE_RCTRL */
    [ 98] =  84, /* KEY_KPSLASH          -> SDL_SCANCODE_KP_DIVIDE */
    [ 99] = 154, /* KEY_SYSRQ            -> SDL_SCANCODE_SYSREQ */
    [100] = 230, /* KEY_RIGHTALT         -> SDL_SCANCODE_RALT */
    [102] =  74, /* KEY_HOME             -> SDL_SCANCODE_HOME */
    [103] =  82, /* KEY_UP               -> SDL_SCANCODE_UP */
    [104] =  75, /* KEY_PAGEUP           -> SDL_SCANCODE_PAGEUP */
    [105] =  80, /* KEY_LEFT             -> SDL_SCANCODE_LEFT */
    [106] =  79, /* KEY_RIGHT            -> SDL_SCANCODE_RIGHT */
    [107] =  77, /* KEY_END              -> SDL_SCANCODE_END */
    [108] =  81, /* KEY_DOWN             -> SDL_SCANCODE_DOWN */
    [109] =  78, /* KEY_PAGEDOWN         -> SDL_SCANCODE_PAGEDOWN */
    [110] =  73, /* KEY_INSERT           -> SDL_SCANCODE_INSERT */
    [111] =  76, /* KEY_DELETE           -> SDL_SCANCODE_DELETE */
    [113] = 127, /* KEY_MUTE             -> SDL_SCANCODE_MUTE */
    [114] = 129, /* KEY_VOLUMEDOWN       -> SDL_SCANCODE_VOLUMEDOWN */
    [115] = 128, /* KEY_VOLUMEUP         -> SDL_SCANCODE_VOLUMEUP */
    [116] = 102, /* KEY_POWER            -> SDL_SCANCODE_POWER */
    [117] = 103, /* KEY_KPEQUAL          -> SDL_SCANCODE_KP_EQUALS */
    [118] = 215, /* KEY_KPPLUSMINUS      -> SDL_SCANCODE_KP_PLUSMINUS */
    [119] =  72, /* KEY_PAUSE            -> SDL_SCANCODE_PAUSE */
    [121] = 133, /* KEY_KPCOMMA          -> SDL_SCANCODE_KP_COMMA */
    [122] = 144, /* KEY_HANGEUL          -> SDL_SCANCODE_LANG1 */
    [123] = 145, /* KEY_HANJA            -> SDL_SCANCODE_LANG2 */
    [124] = 137, /* KEY_YEN              -> SDL_SCANCODE_INTERNATIONAL3 */
    [125] = 227, /* KEY_LEFTMETA         -> SDL_SCANCODE_LGUI */
    [126] = 231, /* KEY_RIGHTMETA        -> SDL_SCANCODE_RGUI */
    [127] = 101, /* KEY_COMPOSE          -> SDL_SCANCODE_APPLICATION */
    [128] = 120, /* KEY_STOP             -> SDL_SCANCODE_STOP */
    [129] = 121, /* KEY_AGAIN            -> SDL_SCANCODE_AGAIN */
    [131] = 122, /* KEY_UNDO             -> SDL_SCANCODE_UNDO */
    [133] = 124, /* KEY_COPY             -> SDL_SCANCODE_COPY */
    [135] = 125, /* KEY_PASTE            -> SDL_SCANCODE_PASTE */
    [136] = 126, /* KEY_FIND             -> SDL_SCANCODE_FIND */
    [137] = 123, /* KEY_CUT              -> SDL_SCANCODE_CUT */
    [138] = 117, /* KEY_HELP             -> SDL_SCANCODE_HELP */
    [139] = 118, /* KEY_MENU             -> SDL_SCANCODE_MENU */
    [179] = 182, /* KEY_KPLEFTPAREN      -> SDL_SCANCODE_KP_LEFTPAREN */
    [180] = 183, /* KEY_KPRIGHTPAREN     -> SDL_SCANCODE_KP_RIGHTPAREN */
    [182] = 121, /* KEY_REDO             -> SDL_SCANCODE_AGAIN */
    [183] = 104, /* KEY_F13              -> SDL_SCANCODE_F13 */
    [184] = 105, /* KEY_F14              -> SDL_SCANCODE_F14 */
    [185] = 106, /* KEY_F15              -> SDL_SCANCODE_F15 */
    [186] = 107, /* KEY_F16              -> SDL_SCANCODE_F16 */
    [187] = 108, /* KEY_F17              -> SDL_SCANCODE_F17 */
    [188] = 109, /* KEY_F18              -> SDL_SCANCODE_F18 */
    [189] = 110, /* KEY_F19              -> SDL_SCANCODE_F19 */
    [190] = 111, /* KEY_F20              -> SDL_SCANCODE_F20 */
    [191] = 112, /* KEY_F21              -> SDL_SCANCODE_F21 */
    [192] = 113, /* KEY_F22              -> SDL_SCANCODE_F22 */
    [193] = 114, /* KEY_F23              -> SDL_SCANCODE_F23 */
    [194] = 115, /* KEY_F24              -> SDL_SCANCODE_F24 */
    [210] =  70, /* KEY_PRINT            -> SDL_SCANCODE_PRINTSCREEN */
    [222] = 153, /* KEY_ALTERASE         -> SDL_SCANCODE_ALTERASE */
    [223] = 155, /* KEY_CANCEL           -> SDL_SCANCODE_CANCEL */
};

/* groovy_input_evdev_to_sdl -- lookup in the table above. Returns -1 (not
 * 0: 0 is the table's internal sentinel, but a public API must return a
 * value outside the domain of valid scancodes) if `evdev_code` has no known
 * SDL scancode within the handled range. External linkage (not `static`):
 * the unit tests call it directly, and groovy_input_set_key_bit is its
 * caller in this file.
 */
int groovy_input_evdev_to_sdl(unsigned short evdev_code)
{
    if ((size_t)evdev_code >= (sizeof groovy_evdev_to_sdl / sizeof groovy_evdev_to_sdl[0]))
        return -1;
    if (groovy_evdev_to_sdl[evdev_code] == 0)
        return -1;
    return (int)groovy_evdev_to_sdl[evdev_code];
}

/* ---------------------------------------------------------------------------
 * groovy_input_set_key_bit -- sets or clears, in the 256-bit field
 * `keys[32]`, the bit matching the SDL scancode of `evdev_code` (table
 * above, through groovy_input_evdev_to_sdl). Returns 1 if the key is within
 * the handled range (bit written), 0 if `evdev_code` has no known SDL
 * scancode OR if that scancode is beyond the 256 positions of the field;
 * the caller (handle_keyboard_event) must log that case rather than ignore
 * it silently.
 * The write is bounded by construction: `sdl` comes from the table filtered
 * at generation (no value >= 256 appears in it), so keys[sdl/8] always
 * stays within [0, 31].
 * ------------------------------------------------------------------------- */
int groovy_input_set_key_bit(uint8_t keys[32], unsigned short evdev_code, int pressed)
{
    int sdl = groovy_input_evdev_to_sdl(evdev_code);
    uint8_t mask;

    if (sdl < 0)
        return 0;

    mask = (uint8_t)(1u << (sdl % 8));
    if (pressed)
        keys[sdl / 8] = (uint8_t)(keys[sdl / 8] | mask);
    else
        keys[sdl / 8] = (uint8_t)(keys[sdl / 8] & (uint8_t)~mask);
    return 1;
}

/* ---------------------------------------------------------------------------
 * groovy_input_encode_mouse_delta -- encodes ONE accumulated relative axis
 * (dx or dy) into the matching PS/2 byte. The exact inverse of the upstream
 * decoder in `input_mister.cpp` (mister_mouse_device::update,
 * antonioginer/GroovyMAME, BSD-3-Clause):
 *     rel = byte - ((bits << n) & 0x100)
 * For any rel in [-256, 255]: if rel >= 0, byte = rel and the sign bit
 * stays 0 (rel - 0 = rel); if rel < 0, byte = rel + 256 and the sign bit is
 * 1 (byte - 256 = rel). Both cases come down to the low byte of `rel`
 * (`rel & 0xFF`, the same result in two's complement) and
 * `sign = (rel < 0)`.
 *
 * Bounds the input to [-256, 255] BEFORE that conversion (a standard PS/2
 * packet carries only 9 signed bits) and reports saturation through
 * *saturated (may be NULL if the caller does not want to count).
 * ------------------------------------------------------------------------- */
uint8_t groovy_input_encode_mouse_delta(int32_t delta, int *sign_bit, int *saturated)
{
    int sat = 0;

    if (delta > 255)       { delta = 255;  sat = 1; }
    else if (delta < -256) { delta = -256; sat = 1; }

    if (saturated) *saturated = sat;
    if (sign_bit)  *sign_bit  = (delta < 0) ? 1 : 0;

    return (uint8_t)(delta & 0xFF);
}

/* ---------------------------------------------------------------------------
 * groovy_input_encode_mouse -- builds the four bytes `mouse[4]`
 * (mouse_bits, mouse_x, mouse_y, mouse_z) consumed by
 * groovy_input_pack_ps2. Bit positions CONFIRMED by direct reading of
 * `mister_mouse_device::update` (`input_mister.cpp`,
 * antonioginer/GroovyMAME, BSD-3-Clause), not assumed by symmetry:
 *   - bits 0-2: left/right/middle buttons (`m_buttons[i] = mouse_bits &
 *     (1 << i)`);
 *   - bit 3: always 1, the standard convention of a PS/2 packet (the
 *     upstream decoder read here never looks at it, it appears in NEITHER
 *     of the two decoding masks; kept for conformance with the format,
 *     documented here rather than assumed);
 *   - bit 4: sign of dx (`(mouse_bits << 4) & 0x100`);
 *   - bit 5: sign of dy (`(mouse_bits << 3) & 0x100`: a shift of 3, NOT 4;
 *     it is bit 5 of mouse_bits, not bit 4, that survives the 0x100 mask
 *     after that shift).
 * `dz` (the wheel) has no upstream decoder read here to cross-check it:
 * written as a raw signed byte, as its place in `struct groovy_ps2_inputs`
 * suggests, without a possible cross-validation.
 * ------------------------------------------------------------------------- */
void groovy_input_encode_mouse(uint8_t mouse[4], int32_t dx, int32_t dy, int32_t dz,
                               uint8_t buttons, int *saturated_x, int *saturated_y)
{
    int sign_x = 0, sign_y = 0;
    uint8_t mx = groovy_input_encode_mouse_delta(dx, &sign_x, saturated_x);
    uint8_t my = groovy_input_encode_mouse_delta(dy, &sign_y, saturated_y);

    mouse[0] = (uint8_t)((buttons & 0x07u) | (1u << 3)
                          | (sign_x ? (1u << 4) : 0u)
                          | (sign_y ? (1u << 5) : 0u));
    mouse[1] = mx;
    mouse[2] = my;
    mouse[3] = (uint8_t)(dz & 0xFF);
}

/* ---------------------------------------------------------------------------
 * input_open_fail_should_log -- "once per path" memory of open() failures
 * on /dev/input/eventN. The three scans (pad, keyboard, mouse) used to pass
 * silently over a failed open(): under the systemd unit, DeviceAllow=
 * without the char-input class refuses ALL nodes (EPERM), and the daemon
 * sent 60 empty reports per second without a single log line. Returns 1
 * when the failure deserves a log line: first failure of this node, or an
 * errno different from the previous one. `err == 0` signals a successful
 * open() and forgets the node, so that a relapse is logged again. A path
 * outside the pattern (or N >= 256) is always logged: the scans only run
 * when /dev/input changes, so the volume stays bounded.
 * Called from the input thread only: no synchronisation.
 * ------------------------------------------------------------------------- */
#define INPUT_OPEN_FAIL_NODES 256

static int input_open_fail_should_log(int16_t seen[INPUT_OPEN_FAIL_NODES],
                                      const char *path, int err)
{
    static const char prefix[] = "/dev/input/event";
    const char *p;
    unsigned n = 0;

    if (strncmp(path, prefix, sizeof prefix - 1) != 0)
        return err != 0;
    p = path + sizeof prefix - 1;
    if (*p < '0' || *p > '9')
        return err != 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        n = n * 10u + (unsigned)(*p - '0');
        if (n >= INPUT_OPEN_FAIL_NODES)
            return err != 0;
    }
    if (*p != '\0')
        return err != 0;

    if (err == 0) {
        seen[n] = 0;
        return 0;
    }
    if (seen[n] == (int16_t)err)
        return 0;
    seen[n] = (int16_t)err;
    return 1;
}

#ifndef INPUT_TEST
/* ---------------------------------------------------------------------------
 * evdev/socket thread -- the 32101/UDP hook (reversed direction: the
 * RECEIVER waits for the byte and becomes the source), reading the pad
 * through evdev with hot-plugging, exclusive grab while a session is active,
 * and the frame/order sending policy.
 *
 * The same secondary-thread pattern as `pcm.c`: priority 70, never above
 * net/blit: "in a conflict, the tube wins" (pcm.c says the same).
 * No allocation and no blocking I/O in the loop; the only exceptions are
 * `poll()`, which IS the intended wait, and the two EVIOCGRAB ioctls (rare
 * transitions, not the hot path).
 * ------------------------------------------------------------------------- */

#include "net.h"       /* struct net_blit_shared -- ack_frame, read-only */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>

/* ---------------------------------------------------------------------------
 * Local constants.
 * ------------------------------------------------------------------------- */
#define INPUT_POLL_MS         16     /* one frame */
#define INPUT_RECV_BUF        64     /* hook: content never read, only the address */
#define INPUT_ORDER_MAX       250u   /* saturation guard of the order counter */
#define INPUT_GRAB_ACTIVE_NS  1000000000ULL   /* 1 s: video session considered active */
#define INPUT_GRAB_RELEASE_NS 5000000000ULL   /* 5 s: still -> release the grab */
#define INPUT_REPORT_NS       1000000000ULL   /* input_1s */

/* ---------------------------------------------------------------------------
 * Private state of the thread. Only one instance ever lives (one input
 * thread in the daemon): allocated once in input_thread_start, freed by the
 * thread itself on exit, the same pattern as `net_internal`/net.c.
 * ------------------------------------------------------------------------- */
struct input_internal {
    struct input_thread_config cfg;

    int                sock;
    int                evdev_fd;
    char               device_path[300];
    char               device_name[256];
    int                have_pad;
    int                with_analog;      /* 17 bytes if true, 9 otherwise; fixed at open */

    struct groovy_axis_cal lx, ly, rx, ry, za, rza;   /* za/rza: ABS_Z/ABS_RZ triggers */

    /* Keyboard and mouse: the second and third device classes (discovery
     * and descriptor lifetime). The matching behavioural state (keys[32],
     * mouse accumulator, grab) is grouped further down, next to its
     * joystick equivalent. */
    int                keyboard_fd;
    char               keyboard_path[300];
    char               keyboard_name[256];
    int                have_keyboard;

    int                mouse_fd;
    char               mouse_path[300];
    char               mouse_name[256];
    int                have_mouse;

    struct sockaddr_in bound_addr;
    socklen_t          bound_addr_len;
    int                have_bound;

    uint16_t           joy1;
    int8_t             axes[8];          /* [0..3] = joy1 lx/ly/rx/ry; [4..7] = joy2, always 0 */
    int                dirty;

    uint32_t           last_frame;
    uint8_t            order;
    uint64_t           last_frame_change_ns;

    int                grabbed;

    /* Keyboard/mouse state. keys[32] carries a STATE (not edges), zeroed at
     * start and when the device is lost: the same discipline as joy1/axes
     * above. */
    uint8_t            keys[32];
    int                keyboard_grabbed;
    int                unmapped_key_traced;   /* "log once" for an unmapped key */

    int32_t            mouse_dx, mouse_dy, mouse_dz;   /* accumulators, zeroed AFTER sending */
    uint8_t            mouse_buttons;
    int                mouse_grabbed;

    /* frame/order counters of the PS2 channel, INDEPENDENT of the joystick
     * ones above (the same policy, applied per channel). */
    uint32_t           ps2_last_frame;
    uint8_t            ps2_order;
    int                ps2_dirty;

    uint64_t           next_rescan_ns;

    /* Last-modification time of /dev/input, cached to avoid repeating the
     * full scan (open+ioctl per device) when nothing has changed since the
     * last scan: a full scan at every rescan period made the frame rate
     * drop. See the comment at the call site. sec == 0 = never measured
     * (the first pass is always full). In seconds AND nanoseconds
     * (st_mtim): the one-second granularity of st_mtime opened a race where
     * a node created in the same second as the cached stat() never moved
     * the date again; rescans skipped for good, hot-plugging dead. */
    time_t             dev_input_mtime_sec;
    long               dev_input_mtime_nsec;

    /* Period counters, zeroed at each input_1s log line. */
    uint64_t           sent;
    uint64_t           order_saturated;
    uint64_t           rescans;
    uint64_t           ps2_sent;
    uint64_t           ps2_order_saturated;
    uint64_t           mouse_saturated;
    uint64_t           log_ns;
};

/* Only one input thread is possible in this daemon: a stop flag, alongside
 * shared->running (which this thread also watches). It lets
 * input_thread_request_stop() take no argument, as input.h declares it,
 * including for a caller (a test) that has no hold on shared->running
 * (shared with net/blit). */
static _Atomic int g_input_stop_requested = 0;

static uint64_t input_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* apply_realtime -- the same gesture as net.c/pcm.c: affinity first,
 * priority next, so that the kernel does not migrate the thread after the
 * promotion. A failure is not fatal (logged, and we go on without a
 * real-time guarantee). */
static void apply_realtime(int cpu, int prio)
{
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET((size_t)cpu, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0) {
            fprintf(stderr,
                    "{\"event\":\"input_affinity_failed\",\"cpu\":%d,\"errno\":%d}\n",
                    cpu, errno);
        }
    }
    if (prio > 0) {
        struct sched_param sp;
        memset(&sp, 0, sizeof sp);
        sp.sched_priority = prio;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
            fprintf(stderr,
                    "{\"event\":\"input_sched_fifo_failed\",\"prio\":%d,\"errno\":%d}\n",
                    prio, errno);
        }
    }
}

/* setup_input_socket -- non-blocking bind on INADDR_ANY:port. No
 * SO_BUSY_POLL nor a wider SO_RCVBUF: this channel only carries a one-byte
 * hook now and then, never a stream. */
static int setup_input_socket(uint16_t port)
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (s < 0) {
        fprintf(stderr, "{\"event\":\"input_socket_failed\",\"errno\":%d}\n", errno);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "{\"event\":\"input_bind_failed\",\"port\":%u,\"errno\":%d}\n",
                (unsigned)port, errno);
        close(s);
        return -1;
    }
    return s;
}

/* ---------------------------------------------------------------------------
 * Pad discovery -- `find_pad_device` and `query_axis`, ported literally
 * from the older bench pad client to `struct groovy_axis_cal`. Its own
 * compilation unit: nothing is called across into that client, which stays
 * a separate binary.
 * ------------------------------------------------------------------------- */
static int input_test_bit(const uint8_t *bits, int bit)
{
    return (bits[bit / 8] >> (bit % 8)) & 1;
}

/* note_open_result -- logs `input_open_failed` once per node (see
 * input_open_fail_should_log). `err` is errno after a failed open(), 0
 * after a successful one. The three scans share the same memory: a node
 * refused is refused for all three, one line is enough. */
static void note_open_result(const char *path, int err)
{
    static int16_t seen[INPUT_OPEN_FAIL_NODES];

    if (input_open_fail_should_log(seen, path, err))
        fprintf(stderr,
                "{\"event\":\"input_open_failed\",\"path\":\"%s\",\"errno\":%d}\n",
                path, err);
}

static int find_pad_device(char *out_path, size_t out_len)
{
    DIR *d = opendir("/dev/input");
    struct dirent *ent;
    int found_fd = -1;

    if (!d) return -1;

    while ((ent = readdir(d)) != NULL) {
        char path[300];
        int fd;
        uint8_t keybits[(KEY_MAX / 8) + 1];

        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        snprintf(path, sizeof path, "/dev/input/%s", ent->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            note_open_result(path, errno);
            continue;
        }
        note_open_result(path, 0);

        memset(keybits, 0, sizeof keybits);
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) >= 0
            && input_test_bit(keybits, BTN_GAMEPAD)) {
            snprintf(out_path, out_len, "%s", path);
            found_fd = fd;
            break;
        }
        close(fd);
    }
    closedir(d);
    return found_fd;
}

/* ---------------------------------------------------------------------------
 * find_keyboard_device -- find_pad_device generalised to a second device
 * class. A keyboard announces EV_KEY with KEY_A AND KEY_Z, and above all NOT
 * BTN_GAMEPAD: without that exclusion, a pad that announces a few keys
 * (rare but it exists: some combined keyboard/pad devices) would be taken
 * for a keyboard.
 * ------------------------------------------------------------------------- */
static int find_keyboard_device(char *out_path, size_t out_len)
{
    DIR *d = opendir("/dev/input");
    struct dirent *ent;
    int found_fd = -1;

    if (!d) return -1;

    while ((ent = readdir(d)) != NULL) {
        char path[300];
        int fd;
        uint8_t keybits[(KEY_MAX / 8) + 1];

        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        snprintf(path, sizeof path, "/dev/input/%s", ent->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            note_open_result(path, errno);
            continue;
        }
        note_open_result(path, 0);

        memset(keybits, 0, sizeof keybits);
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) >= 0
            && input_test_bit(keybits, KEY_A) && input_test_bit(keybits, KEY_Z)
            && !input_test_bit(keybits, BTN_GAMEPAD)) {
            snprintf(out_path, out_len, "%s", path);
            found_fd = fd;
            break;
        }
        close(fd);
    }
    closedir(d);
    return found_fd;
}

/* ---------------------------------------------------------------------------
 * find_mouse_device -- the third class: EV_REL with REL_X and REL_Y. The
 * same pattern, no exclusion needed (no pad nor keyboard announces these two
 * relative axes together).
 * ------------------------------------------------------------------------- */
static int find_mouse_device(char *out_path, size_t out_len)
{
    DIR *d = opendir("/dev/input");
    struct dirent *ent;
    int found_fd = -1;

    if (!d) return -1;

    while ((ent = readdir(d)) != NULL) {
        char path[300];
        int fd;
        uint8_t relbits[(REL_MAX / 8) + 1];

        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        snprintf(path, sizeof path, "/dev/input/%s", ent->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            note_open_result(path, errno);
            continue;
        }
        note_open_result(path, 0);

        memset(relbits, 0, sizeof relbits);
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof relbits), relbits) >= 0
            && input_test_bit(relbits, REL_X) && input_test_bit(relbits, REL_Y)) {
            snprintf(out_path, out_len, "%s", path);
            found_fd = fd;
            break;
        }
        close(fd);
    }
    closedir(d);
    return found_fd;
}

static void query_axis(int fd, unsigned int code, struct groovy_axis_cal *cal)
{
    struct input_absinfo ai;

    memset(cal, 0, sizeof *cal);
    if (ioctl(fd, EVIOCGABS(code), &ai) == 0) {
        cal->min  = ai.minimum;
        cal->max  = ai.maximum;
        cal->have = 1;
    }
}

/* trigger_pressed -- the middle threshold of the calibration cached at open
 * (no ioctl in the loop). */
static int trigger_pressed(const struct groovy_axis_cal *cal, int32_t value)
{
    if (!cal->have || cal->max <= cal->min) return 0;
    int32_t mid = cal->min + (cal->max - cal->min) / 2;
    return value > mid;
}

/* ---------------------------------------------------------------------------
 * open_pad -- opens the device found by find_pad_device, caches the six
 * axis calibrations, decides `with_analog`, zeroes the button/axis state (a
 * clean start), and logs `input_device_opened`.
 * ------------------------------------------------------------------------- */
static void open_pad(struct input_internal *ni, int fd, const char *path)
{
    ni->evdev_fd = fd;
    snprintf(ni->device_path, sizeof ni->device_path, "%s", path);

    snprintf(ni->device_name, sizeof ni->device_name, "unknown");
    (void)ioctl(fd, EVIOCGNAME(sizeof ni->device_name), ni->device_name);

    query_axis(fd, ABS_X,  &ni->lx);
    query_axis(fd, ABS_Y,  &ni->ly);
    query_axis(fd, ABS_RX, &ni->rx);
    query_axis(fd, ABS_RY, &ni->ry);
    query_axis(fd, ABS_Z,  &ni->za);
    query_axis(fd, ABS_RZ, &ni->rza);

    ni->with_analog = ni->lx.have;   /* 17 bytes if the pad announces ABS_X */
    ni->have_pad    = 1;

    /* Clean start: no button "stuck" from a previous pad. */
    ni->joy1 = 0;
    memset(ni->axes, 0, sizeof ni->axes);
    ni->dirty = 1;   /* pushes the neutral state at the next send */

    fprintf(stderr,
            "{\"event\":\"input_device_opened\",\"path\":\"%s\",\"name\":\"%s\","
            "\"analog\":%d}\n",
            ni->device_path, ni->device_name, ni->with_analog);
}

static void close_pad(struct input_internal *ni, const char *why)
{
    if (ni->grabbed) {
        (void)ioctl(ni->evdev_fd, EVIOCGRAB, 0);
        ni->grabbed = 0;
        fprintf(stderr, "{\"event\":\"input_ungrab\",\"reason\":\"%s\"}\n", why);
    }
    close(ni->evdev_fd);
    ni->evdev_fd = -1;
    ni->have_pad = 0;
    /* No button may stay "pressed" on the emitter side after the physical
     * device is lost. */
    ni->joy1 = 0;
    memset(ni->axes, 0, sizeof ni->axes);
    ni->dirty = 1;
    fprintf(stderr, "{\"event\":\"input_device_lost\",\"path\":\"%s\",\"reason\":\"%s\"}\n",
            ni->device_path, why);
}

/* ---------------------------------------------------------------------------
 * open_keyboard/close_keyboard, open_mouse/close_mouse -- the same pattern
 * as open_pad/close_pad: a clean start at open, neutral state + grab
 * released at close, exactly what open_pad/close_pad already do for
 * joy1/axes/grabbed. `keys[32]` and the mouse accumulator are a STATE (not
 * edges), so they are zeroed at every open/close, like joy1 above.
 * ------------------------------------------------------------------------- */
static void open_keyboard(struct input_internal *ni, int fd, const char *path)
{
    ni->keyboard_fd = fd;
    snprintf(ni->keyboard_path, sizeof ni->keyboard_path, "%s", path);

    snprintf(ni->keyboard_name, sizeof ni->keyboard_name, "unknown");
    (void)ioctl(fd, EVIOCGNAME(sizeof ni->keyboard_name), ni->keyboard_name);

    ni->have_keyboard = 1;

    /* Clean start: no key "stuck" from a previous keyboard. */
    memset(ni->keys, 0, sizeof ni->keys);
    ni->ps2_dirty = 1;   /* pushes the neutral state at the next send */

    fprintf(stderr,
            "{\"event\":\"input_keyboard_opened\",\"path\":\"%s\",\"name\":\"%s\"}\n",
            ni->keyboard_path, ni->keyboard_name);
}

static void close_keyboard(struct input_internal *ni, const char *why)
{
    if (ni->keyboard_grabbed) {
        (void)ioctl(ni->keyboard_fd, EVIOCGRAB, 0);
        ni->keyboard_grabbed = 0;
        fprintf(stderr, "{\"event\":\"input_keyboard_ungrab\",\"reason\":\"%s\"}\n", why);
    }
    close(ni->keyboard_fd);
    ni->keyboard_fd = -1;
    ni->have_keyboard = 0;
    /* No key may stay "pressed" on the emitter side after the physical
     * device is lost. */
    memset(ni->keys, 0, sizeof ni->keys);
    ni->ps2_dirty = 1;
    fprintf(stderr, "{\"event\":\"input_keyboard_lost\",\"path\":\"%s\",\"reason\":\"%s\"}\n",
            ni->keyboard_path, why);
}

static void open_mouse(struct input_internal *ni, int fd, const char *path)
{
    ni->mouse_fd = fd;
    snprintf(ni->mouse_path, sizeof ni->mouse_path, "%s", path);

    snprintf(ni->mouse_name, sizeof ni->mouse_name, "unknown");
    (void)ioctl(fd, EVIOCGNAME(sizeof ni->mouse_name), ni->mouse_name);

    ni->have_mouse = 1;

    /* Clean start: no accumulator/button "stuck" from a previous mouse. It
     * also changes the length of the PS2 datagram (37 -> 41, a rule fixed at
     * open). */
    ni->mouse_dx = ni->mouse_dy = ni->mouse_dz = 0;
    ni->mouse_buttons = 0;
    ni->ps2_dirty = 1;

    fprintf(stderr,
            "{\"event\":\"input_mouse_opened\",\"path\":\"%s\",\"name\":\"%s\"}\n",
            ni->mouse_path, ni->mouse_name);
}

static void close_mouse(struct input_internal *ni, const char *why)
{
    if (ni->mouse_grabbed) {
        (void)ioctl(ni->mouse_fd, EVIOCGRAB, 0);
        ni->mouse_grabbed = 0;
        fprintf(stderr, "{\"event\":\"input_mouse_ungrab\",\"reason\":\"%s\"}\n", why);
    }
    close(ni->mouse_fd);
    ni->mouse_fd = -1;
    ni->have_mouse = 0;
    ni->mouse_dx = ni->mouse_dy = ni->mouse_dz = 0;
    ni->mouse_buttons = 0;
    ni->ps2_dirty = 1;
    fprintf(stderr, "{\"event\":\"input_mouse_lost\",\"path\":\"%s\",\"reason\":\"%s\"}\n",
            ni->mouse_path, why);
}

/* ---------------------------------------------------------------------------
 * handle_evdev_event -- the pad translation table.
 * ------------------------------------------------------------------------- */
static void handle_evdev_event(struct input_internal *ni, const struct input_event *ev)
{
    switch (ev->type) {
    case EV_KEY: {
        uint16_t bit;

        if (ev->value == 2) return;   /* auto-repeat, ignored: the field carries a state */
        bit = groovy_input_button_bit(ev->code);
        if (bit == 0) return;
        if (ev->value)
            ni->joy1 = (uint16_t)(ni->joy1 | bit);
        else
            ni->joy1 = (uint16_t)(ni->joy1 & (uint16_t)~bit);
        break;
    }
    case EV_ABS:
        switch (ev->code) {
        case ABS_HAT0X:
            ni->joy1 = (uint16_t)((ev->value < 0)
                          ? (ni->joy1 | GROOVY_JOY_LEFT) : (ni->joy1 & (uint16_t)~GROOVY_JOY_LEFT));
            ni->joy1 = (uint16_t)((ev->value > 0)
                          ? (ni->joy1 | GROOVY_JOY_RIGHT) : (ni->joy1 & (uint16_t)~GROOVY_JOY_RIGHT));
            break;
        case ABS_HAT0Y:
            ni->joy1 = (uint16_t)((ev->value < 0)
                          ? (ni->joy1 | GROOVY_JOY_UP) : (ni->joy1 & (uint16_t)~GROOVY_JOY_UP));
            ni->joy1 = (uint16_t)((ev->value > 0)
                          ? (ni->joy1 | GROOVY_JOY_DOWN) : (ni->joy1 & (uint16_t)~GROOVY_JOY_DOWN));
            break;
        case ABS_X:  ni->axes[0] = groovy_input_scale_axis(&ni->lx, ev->value); break;
        case ABS_Y:  ni->axes[1] = groovy_input_scale_axis(&ni->ly, ev->value); break;
        case ABS_RX: ni->axes[2] = groovy_input_scale_axis(&ni->rx, ev->value); break;
        case ABS_RY: ni->axes[3] = groovy_input_scale_axis(&ni->ry, ev->value); break;
        case ABS_Z:
            /* L2 trigger -> B9, the table composed above
             * groovy_input_button_bit: L2 is a button DISTINCT from L1
             * (BTN_TL, already B7). Found on the bench: L1 and L2 both lit
             * B7, while the documented table says B9. */
            ni->joy1 = (uint16_t)(trigger_pressed(&ni->za, ev->value)
                          ? (ni->joy1 | GROOVY_JOY_B9) : (ni->joy1 & (uint16_t)~GROOVY_JOY_B9));
            break;
        case ABS_RZ:
            /* R2 trigger -> B10, same table, same fix */
            ni->joy1 = (uint16_t)(trigger_pressed(&ni->rza, ev->value)
                          ? (ni->joy1 | GROOVY_JOY_B10) : (ni->joy1 & (uint16_t)~GROOVY_JOY_B10));
            break;
        default:
            return;   /* no SYN to mark for an ignored axis */
        }
        break;
    case EV_SYN:
        if (ev->code == SYN_REPORT)
            ni->dirty = 1;
        return;   /* no needless re-marking */
    default:
        return;
    }
}

/* trace_unmapped_key_once -- a key outside the handled range (evdev with no
 * known SDL scancode, or a scancode >= 256) must be LOGGED, not filed
 * silently in a wrong slot. Once per session (not per occurrence): the
 * physical keyboard does not change layout along the way, and logging again
 * at every keystroke adds no information. */
static void trace_unmapped_key_once(struct input_internal *ni, unsigned short evdev_code)
{
    if (ni->unmapped_key_traced) return;
    ni->unmapped_key_traced = 1;
    fprintf(stderr, "{\"event\":\"input_key_unmapped\",\"evdev_code\":%u}\n",
            (unsigned)evdev_code);
}

/* ---------------------------------------------------------------------------
 * handle_keyboard_event -- sets/clears the bits of `keys[32]` through
 * groovy_input_set_key_bit. `ev->value == 2` (the kernel's auto-repeat) is
 * ignored: the field carries a STATE, not edges, and auto-repeat has no
 * place in it (the same guard as for the joystick).
 * ------------------------------------------------------------------------- */
static void handle_keyboard_event(struct input_internal *ni, const struct input_event *ev)
{
    switch (ev->type) {
    case EV_KEY:
        if (ev->value == 2) return;   /* auto-repeat, ignored */
        if (!groovy_input_set_key_bit(ni->keys, ev->code, ev->value != 0))
            trace_unmapped_key_once(ni, ev->code);
        break;
    case EV_SYN:
        if (ev->code == SYN_REPORT)
            ni->ps2_dirty = 1;
        return;
    default:
        return;
    }
}

/* ---------------------------------------------------------------------------
 * handle_mouse_event -- accumulates the relative moves (REL_X/REL_Y/
 * REL_WHEEL) and sets the button bits in `mouse_buttons`. The axes
 * ACCUMULATE (not a state like the keyboard): a relative move not sent is a
 * move lost, so the accumulator is zeroed only AFTER a successful send
 * (input_thread_main, PS2 sending block).
 * ------------------------------------------------------------------------- */
static void handle_mouse_event(struct input_internal *ni, const struct input_event *ev)
{
    switch (ev->type) {
    case EV_REL:
        switch (ev->code) {
        case REL_X:     ni->mouse_dx += ev->value; break;
        case REL_Y:     ni->mouse_dy += ev->value; break;
        case REL_WHEEL: ni->mouse_dz += ev->value; break;
        default:        return;   /* axis the protocol does not carry: ignored */
        }
        break;
    case EV_KEY: {
        uint8_t bit;

        if (ev->value == 2) return;
        switch (ev->code) {
        case BTN_LEFT:   bit = (uint8_t)(1u << 0); break;
        case BTN_RIGHT:  bit = (uint8_t)(1u << 1); break;
        case BTN_MIDDLE: bit = (uint8_t)(1u << 2); break;
        default:         return;
        }
        if (ev->value)
            ni->mouse_buttons = (uint8_t)(ni->mouse_buttons | bit);
        else
            ni->mouse_buttons = (uint8_t)(ni->mouse_buttons & (uint8_t)~bit);
        break;
    }
    case EV_SYN:
        if (ev->code == SYN_REPORT)
            ni->ps2_dirty = 1;
        return;
    default:
        return;
    }
}

/* ---------------------------------------------------------------------------
 * drain_socket -- empties the queue of the input channel. Every datagram,
 * whatever its size or content (never read, so it cannot be used to attack
 * the parser), replaces the learned address and logs `input_bound`.
 * ------------------------------------------------------------------------- */
static void drain_socket(struct input_internal *ni)
{
    uint8_t buf[INPUT_RECV_BUF];

    for (;;) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof src;
        ssize_t n = recvfrom(ni->sock, buf, sizeof buf, 0,
                             (struct sockaddr *)&src, &src_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            return;   /* unrecoverable error on a UDP socket: stay quiet and keep running */
        }
        if (src_len > sizeof ni->bound_addr) continue;   /* never a write out of bounds */

        memcpy(&ni->bound_addr, &src, src_len);
        ni->bound_addr_len = src_len;
        ni->have_bound = 1;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src.sin_addr, ip, sizeof ip);
        fprintf(stderr, "{\"event\":\"input_bound\",\"ip\":\"%s\",\"port\":%u}\n",
                ip, (unsigned)ntohs(src.sin_port));
    }
}

/* ---------------------------------------------------------------------------
 * report_1s -- logs `input_1s`, only if something happened since the last
 * time (the same discipline as audio_out_report). The counters are zeroed
 * at every line written.
 * ------------------------------------------------------------------------- */
static void report_1s(struct input_internal *ni)
{
    uint64_t now = input_mono_ns();
    if (ni->log_ns == 0u) { ni->log_ns = now; return; }
    if (now - ni->log_ns < INPUT_REPORT_NS) return;
    ni->log_ns = now;

    if (ni->sent == 0u && ni->order_saturated == 0u && ni->rescans == 0u &&
        ni->ps2_sent == 0u && ni->ps2_order_saturated == 0u && ni->mouse_saturated == 0u)
        return;   /* a silent daemon at rest: a property, not an oversight */

    fprintf(stderr,
            "{\"event\":\"input_1s\",\"sent\":%llu,\"order_saturated\":%llu,\"rescans\":%llu,"
            "\"ps2_sent\":%llu,\"ps2_order_saturated\":%llu,\"mouse_saturated\":%llu}\n",
            (unsigned long long)ni->sent,
            (unsigned long long)ni->order_saturated,
            (unsigned long long)ni->rescans,
            (unsigned long long)ni->ps2_sent,
            (unsigned long long)ni->ps2_order_saturated,
            (unsigned long long)ni->mouse_saturated);

    ni->sent = 0u;
    ni->order_saturated = 0u;
    ni->rescans = 0u;
    ni->ps2_sent = 0u;
    ni->ps2_order_saturated = 0u;
    ni->mouse_saturated = 0u;
}

/* ---------------------------------------------------------------------------
 * input_thread_main -- the main loop. A single `pthread`.
 * ------------------------------------------------------------------------- */
static void *input_thread_main(void *arg)
{
    struct input_internal *ni = (struct input_internal *)arg;

    apply_realtime(ni->cfg.cpu_affinity, ni->cfg.sched_fifo_priority);

    fprintf(stderr,
            "{\"event\":\"input_thread_ready\",\"port\":%u,\"cpu\":%d,\"prio\":%d}\n",
            (unsigned)ni->cfg.listen_port, ni->cfg.cpu_affinity, ni->cfg.sched_fifo_priority);

    ni->next_rescan_ns = 0u;   /* first scan at once */

    while (atomic_load(&ni->cfg.shared->running) &&
           !atomic_load(&g_input_stop_requested)) {

        /* fds[] holds up to FOUR descriptors (the socket plus the three
         * device classes). Each class is independent: indices TRACKED
         * explicitly (idx_*), no fixed position. Unlike a 2-entry array
         * where the pad would always be fds[1] when present, here any
         * subset of the three may be missing. */
        struct pollfd fds[4];
        nfds_t nfds = 0;
        int idx_sock = -1, idx_pad = -1, idx_kbd = -1, idx_mouse = -1;

        idx_sock = (int)nfds;
        fds[nfds].fd = ni->sock; fds[nfds].events = POLLIN; fds[nfds].revents = 0; nfds++;
        if (ni->have_pad) {
            idx_pad = (int)nfds;
            fds[nfds].fd = ni->evdev_fd; fds[nfds].events = POLLIN; fds[nfds].revents = 0; nfds++;
        }
        if (ni->have_keyboard) {
            idx_kbd = (int)nfds;
            fds[nfds].fd = ni->keyboard_fd; fds[nfds].events = POLLIN; fds[nfds].revents = 0; nfds++;
        }
        if (ni->have_mouse) {
            idx_mouse = (int)nfds;
            fds[nfds].fd = ni->mouse_fd; fds[nfds].events = POLLIN; fds[nfds].revents = 0; nfds++;
        }

        int pr = poll(fds, nfds, INPUT_POLL_MS);
        if (pr < 0 && errno != EINTR) {
            /* poll() failed hard: sleep one frame so as not to spin. */
            struct timespec t = { .tv_sec = 0, .tv_nsec = INPUT_POLL_MS * 1000000L };
            nanosleep(&t, NULL);
        }

        if (idx_sock >= 0 && (fds[idx_sock].revents & POLLIN))
            drain_socket(ni);

        if (idx_pad >= 0 && (fds[idx_pad].revents & (POLLIN | POLLHUP | POLLERR))) {
            for (;;) {
                struct input_event ev;
                ssize_t n = read(ni->evdev_fd, &ev, sizeof ev);
                if (n == (ssize_t)sizeof ev) {
                    handle_evdev_event(ni, &ev);
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (n < 0 && errno == EINTR) continue;
                /* ENODEV (unplugged) or any other error/end of stream: the
                 * pad is lost, which hot-plugging handles; not a daemon
                 * error. */
                close_pad(ni, (errno == ENODEV) ? "enodev" : "read_error");
                break;
            }
        }

        /* Keyboard and mouse: drain AND handle. */
        if (idx_kbd >= 0 && (fds[idx_kbd].revents & (POLLIN | POLLHUP | POLLERR))) {
            for (;;) {
                struct input_event ev;
                ssize_t n = read(ni->keyboard_fd, &ev, sizeof ev);
                if (n == (ssize_t)sizeof ev) {
                    handle_keyboard_event(ni, &ev);
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (n < 0 && errno == EINTR) continue;
                close_keyboard(ni, (errno == ENODEV) ? "enodev" : "read_error");
                break;
            }
        }

        if (idx_mouse >= 0 && (fds[idx_mouse].revents & (POLLIN | POLLHUP | POLLERR))) {
            for (;;) {
                struct input_event ev;
                ssize_t n = read(ni->mouse_fd, &ev, sizeof ev);
                if (n == (ssize_t)sizeof ev) {
                    handle_mouse_event(ni, &ev);
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (n < 0 && errno == EINTR) continue;
                close_mouse(ni, (errno == ENODEV) ? "enodev" : "read_error");
                break;
            }
        }

        /* --- Hot-plugging: a periodic rescan of the three device classes.
         * Each one is checked independently; `did_scan` counts the rescan
         * only if an opendir() really took place (all three present at
         * once = zero cost).
         *
         * The frame-rate drop (60 -> 36-41 per second, measured on the
         * bench): when at least one class is missing (typically the mouse),
         * the loop used to reopen ALL /dev/input/eventN nodes at EVERY
         * rescan cycle to look for the missing class, including the event
         * nodes of the audio subsystem (HDA jack sensing, 14 on the bench,
         * /proc/bus/input/devices), whose open() wakes the audio codec
         * (runtime PM) and costs several milliseconds each, measured one by
         * one. Measured directly (a temporary log line, removed after the
         * diagnosis): about 352 ms PER RESCAN, once per second, enough to
         * bring the poll() loop down from 60+ to 36-41 iterations per
         * second, swallowing short presses between two iterations (as seen
         * from the sofa: the receiver's pad AND keyboard lost short
         * presses, while the PC's local dinput path was intact).
         *
         * The fix: one stat() on /dev/input itself (a single call, no walk
         * through the nodes) BEFORE the costly scan. The /dev/input
         * directory changes its modification time exactly when udev creates
         * or removes a node: plugging/unplugging a mouse triggers it, the
         * mere passing of time does not. If nothing has changed since the
         * last full scan, the missing classes are still missing for the
         * same reason: skipping the scan this time is correct, not an
         * approximation. Hot-plugging is fully preserved: a NEW device
         * always moves the modification time forward, so the next cycle
         * does the full scan as usual. */
        uint64_t now = input_mono_ns();
        if (now >= ni->next_rescan_ns) {
            int need_scan = !ni->have_pad || !ni->have_keyboard || !ni->have_mouse;

            if (need_scan) {
                struct stat st;
                if (stat("/dev/input", &st) == 0) {
                    /* Compared on st_mtim (tv_sec AND tv_nsec), not
                     * st_mtime alone: with one-second granularity, a node
                     * created by udev in the same second as the cached
                     * stat() left the date equal to the cache for ever,
                     * and ALL later rescans were skipped. In nanoseconds,
                     * a creation after the stat() always yields a strictly
                     * newer mtim at the next cycle. */
                    if (ni->dev_input_mtime_sec != 0 &&
                        st.st_mtim.tv_sec  == ni->dev_input_mtime_sec &&
                        st.st_mtim.tv_nsec == ni->dev_input_mtime_nsec) {
                        need_scan = 0;   /* nothing new since the last full scan */
                    } else {
                        ni->dev_input_mtime_sec  = st.st_mtim.tv_sec;
                        ni->dev_input_mtime_nsec = st.st_mtim.tv_nsec;
                    }
                }
                /* stat() failed (unlikely for /dev/input): need_scan stays
                 * 1; hot-plugging is never lost out of caution, and the
                 * worst cost is the one from before this fix. */
            }

            if (need_scan) {
                int did_scan = 0;

                if (!ni->have_pad) {
                    char path[300];
                    int fd = find_pad_device(path, sizeof path);
                    did_scan = 1;
                    if (fd >= 0) open_pad(ni, fd, path);
                }
                if (!ni->have_keyboard) {
                    char path[300];
                    int fd = find_keyboard_device(path, sizeof path);
                    did_scan = 1;
                    if (fd >= 0) open_keyboard(ni, fd, path);
                }
                if (!ni->have_mouse) {
                    char path[300];
                    int fd = find_mouse_device(path, sizeof path);
                    did_scan = 1;
                    if (fd >= 0) open_mouse(ni, fd, path);
                }
                if (did_scan) ni->rescans++;
            }
            ni->next_rescan_ns = now + (uint64_t)ni->cfg.rescan_ms * 1000000ULL;
        }

        /* --- Exclusive grab while a video session is active, then released
         * after 5 s of stillness; for the pad, the keyboard and the mouse,
         * with the SAME video activity signal (ni->last_frame_change_ns,
         * updated below by the joystick sending block, which runs
         * unconditionally: the signal therefore reflects the activity of the
         * VIDEO SESSION, not only the presence of a pad). This is WHERE the
         * grab really matters for the keyboard: without it, every keystroke
         * meant for the game would ALSO land in the receiver's local tty.
         * The kernel releases the grab when the descriptor is closed
         * (close_keyboard/close_mouse, and the kernel itself if the daemon
         * crashes), so a crash cannot confiscate the local console's
         * keyboard for good. --- */
        if (ni->have_pad) {
            int video_active = ni->last_frame_change_ns != 0u &&
                               (now - ni->last_frame_change_ns) < INPUT_GRAB_ACTIVE_NS;
            if (ni->have_bound && video_active && !ni->grabbed) {
                if (ioctl(ni->evdev_fd, EVIOCGRAB, 1) == 0) {
                    ni->grabbed = 1;
                    fprintf(stderr, "{\"event\":\"input_grab\"}\n");
                }
            } else if (ni->grabbed && ni->last_frame_change_ns != 0u &&
                      (now - ni->last_frame_change_ns) > INPUT_GRAB_RELEASE_NS) {
                (void)ioctl(ni->evdev_fd, EVIOCGRAB, 0);
                ni->grabbed = 0;
                fprintf(stderr, "{\"event\":\"input_ungrab\",\"reason\":\"idle\"}\n");
            }
        }

        if (ni->have_keyboard) {
            int video_active = ni->last_frame_change_ns != 0u &&
                               (now - ni->last_frame_change_ns) < INPUT_GRAB_ACTIVE_NS;
            if (ni->have_bound && video_active && !ni->keyboard_grabbed) {
                if (ioctl(ni->keyboard_fd, EVIOCGRAB, 1) == 0) {
                    ni->keyboard_grabbed = 1;
                    fprintf(stderr, "{\"event\":\"input_keyboard_grab\"}\n");
                }
            } else if (ni->keyboard_grabbed && ni->last_frame_change_ns != 0u &&
                      (now - ni->last_frame_change_ns) > INPUT_GRAB_RELEASE_NS) {
                (void)ioctl(ni->keyboard_fd, EVIOCGRAB, 0);
                ni->keyboard_grabbed = 0;
                fprintf(stderr, "{\"event\":\"input_keyboard_ungrab\",\"reason\":\"idle\"}\n");
            }
        }

        if (ni->have_mouse) {
            int video_active = ni->last_frame_change_ns != 0u &&
                               (now - ni->last_frame_change_ns) < INPUT_GRAB_ACTIVE_NS;
            if (ni->have_bound && video_active && !ni->mouse_grabbed) {
                if (ioctl(ni->mouse_fd, EVIOCGRAB, 1) == 0) {
                    ni->mouse_grabbed = 1;
                    fprintf(stderr, "{\"event\":\"input_mouse_grab\"}\n");
                }
            } else if (ni->mouse_grabbed && ni->last_frame_change_ns != 0u &&
                      (now - ni->last_frame_change_ns) > INPUT_GRAB_RELEASE_NS) {
                (void)ioctl(ni->mouse_fd, EVIOCGRAB, 0);
                ni->mouse_grabbed = 0;
                fprintf(stderr, "{\"event\":\"input_mouse_ungrab\",\"reason\":\"idle\"}\n");
            }
        }

        /* --- Sending policy. Every new ack_frame resets `order` to 0 and
         * repeats the current state once, so the emitter sees the pad state
         * at least once per frame; within a frame, each change is sent with
         * the next `order`. Past INPUT_ORDER_MAX sends in one frame, the
         * state waits for the next frame (counted as order_saturated). --- */
        {
            uint32_t frame = atomic_load(&ni->cfg.shared->ack_frame);
            int repeat_due = 0;

            if (frame != ni->last_frame) {
                ni->last_frame = frame;
                ni->order = 0;
                repeat_due = 1;
                ni->last_frame_change_ns = now;
            }

            if (ni->have_bound && (ni->dirty || repeat_due)) {
                if (ni->order < INPUT_ORDER_MAX) {
                    uint8_t sendbuf[17];
                    size_t n = groovy_input_pack_joy(sendbuf, sizeof sendbuf,
                                                     frame, ni->order,
                                                     ni->joy1, 0u,
                                                     ni->axes, ni->with_analog);
                    if (n > 0u) {
                        (void)sendto(ni->sock, sendbuf, n, 0,
                                    (struct sockaddr *)&ni->bound_addr, ni->bound_addr_len);
                        ni->order++;
                        ni->sent++;
                    }
                } else {
                    ni->order_saturated++;
                }
                ni->dirty = 0;
            }
        }

        /* --- PS2 sending policy (keyboard/mouse): frame/order counters
         * INDEPENDENT of the joystick ones above, same policy. Length: 37
         * bytes if no mouse is open, 41 otherwise; `ni->have_mouse` changes
         * only at open/close (rescan), never at each frame, so the rule is
         * already "fixed at open" with no extra state. --- */
        {
            uint32_t frame = atomic_load(&ni->cfg.shared->ack_frame);
            int repeat_due = 0;

            if (frame != ni->ps2_last_frame) {
                ni->ps2_last_frame = frame;
                ni->ps2_order = 0;
                repeat_due = 1;
            }

            if (ni->have_bound && (ni->ps2_dirty || repeat_due)) {
                if (ni->ps2_order < INPUT_ORDER_MAX) {
                    uint8_t sendbuf[41];
                    uint8_t mouse4[4];
                    const uint8_t *mouse_ptr = NULL;
                    size_t n;

                    if (ni->have_mouse) {
                        int sat_x = 0, sat_y = 0;
                        groovy_input_encode_mouse(mouse4, ni->mouse_dx, ni->mouse_dy,
                                                  ni->mouse_dz, ni->mouse_buttons,
                                                  &sat_x, &sat_y);
                        if (sat_x || sat_y) ni->mouse_saturated++;
                        mouse_ptr = mouse4;
                    }

                    n = groovy_input_pack_ps2(sendbuf, sizeof sendbuf, frame, ni->ps2_order,
                                              ni->keys, mouse_ptr);
                    if (n > 0u) {
                        (void)sendto(ni->sock, sendbuf, n, 0,
                                    (struct sockaddr *)&ni->bound_addr, ni->bound_addr_len);
                        ni->ps2_order++;
                        ni->ps2_sent++;
                        /* A relative move not sent is a move lost: zeroed
                         * ONLY after a successful send. */
                        if (ni->have_mouse) {
                            ni->mouse_dx = 0;
                            ni->mouse_dy = 0;
                            ni->mouse_dz = 0;
                        }
                    }
                } else {
                    ni->ps2_order_saturated++;
                }
                ni->ps2_dirty = 0;
            }
        }

        report_1s(ni);
    }

    if (ni->have_pad)
        close_pad(ni, "shutdown");
    if (ni->have_keyboard)
        close_keyboard(ni, "shutdown");
    if (ni->have_mouse)
        close_mouse(ni, "shutdown");
    close(ni->sock);
    free(ni);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Public interface.
 * ------------------------------------------------------------------------- */
int input_thread_start(struct input_thread_config *cfg, pthread_t *out_tid)
{
    if (!cfg || !cfg->shared || !out_tid) return -1;
    if (cfg->listen_port == 0u) return -1;

    struct input_internal *ni = calloc(1, sizeof *ni);
    if (!ni) return -1;
    ni->cfg        = *cfg;
    ni->evdev_fd    = -1;
    ni->keyboard_fd = -1;
    ni->mouse_fd    = -1;

    ni->sock = setup_input_socket(cfg->listen_port);
    if (ni->sock < 0) {
        free(ni);
        return -1;
    }

    atomic_store(&g_input_stop_requested, 0);

    if (pthread_create(out_tid, NULL, input_thread_main, ni) != 0) {
        fprintf(stderr, "{\"event\":\"input_pthread_create_failed\",\"errno\":%d}\n", errno);
        close(ni->sock);
        free(ni);
        return -1;
    }
    return 0;
}

void input_thread_request_stop(void)
{
    atomic_store(&g_input_stop_requested, 1);
}

#endif /* INPUT_TEST */
