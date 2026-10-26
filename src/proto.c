/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * proto.c -- Groovy v2 wire protocol parser, and the pure decisions that
 * net.c calls (see proto.h for each contract).
 *
 * Reference: jcramer83/Mistglow GroovyProtocol.swift v1.08, checked byte for
 * byte for every opcode 0x01-0x08, then against the psakhis/Groovy_MiSTer
 * sources.
 *
 * Scope:
 *   - CMD_AUDIO carries a 3-byte header, parsed by parse_cmd_audio; joining
 *     the payload back together lives in net.c;
 *   - compression is 0 (raw) or 1 (LZ4); anything above is refused
 *     (PARSE_BAD_COMPRESSION);
 *   - CMD_BLIT_VSYNC (0x06) is accepted by the dispatcher, and parsed by
 *     parse_cmd_blit_vsync: GroovyMAME sends its progressive frames that way.
 *
 * Unaligned fields: cmd_switchres_t.pclock is a double at offset 1. memcpy is
 * mandatory; do NOT cast and dereference. The memcpy of the whole packed
 * cmd_switchres_t covers this: memcpy makes no alignment claim about the
 * destination, and the destination struct is packed too.
 *
 * UDP fragmentation: the payload arrives in MTU-sized chunks (1472 by
 * default, up to 3800 with jumbo frames). proto_reassemble_push accumulates
 * against frame->expected_bytes with a running counter, NOT a slice-count
 * field. The destination buf must NEVER overflow, even if a misbehaving
 * emitter sends extra bytes after expected_bytes is reached.
 *
 * Trust boundary: every function in this file treats `buf` as untrusted
 * input from recvmmsg(). Length is bounded against the expected struct size
 * BEFORE any memcpy. Those bound checks, the refusal of unknown compression
 * modes and the memcpy-not-dereference discipline are what keep a hostile or
 * broken emitter from reading or writing out of bounds.
 */

#include "proto.h"
#include <string.h>
#include <stdio.h>

/*
 * tail_is_zero -- are the bytes [from, to) of `b` all zero? Used by the five
 * opcodes whose 8-byte padded form requires a zero padding tail (0x01, 0x02,
 * 0x04, 0x05, 0x08); see the table above proto_is_command_header in
 * proto.h.
 *
 * The bounds are always LITERAL CONSTANTS at the call site, never a value
 * from the wire: it is called only AFTER an exact length check (`len == 8`)
 * has already made sure that [from, to) lies inside the buffer.
 */
static int tail_is_zero(const uint8_t *b, size_t from, size_t to)
{
    size_t i;
    for (i = from; i < to; i++) {
        if (b[i] != 0u) return 0;
    }
    return 1;
}

/*
 * parse_cmd_init -- opcode 0x02, 4, 5 or 8 bytes (the 8-byte form opens a
 * padded session; GroovyMAME 0.287 sends the 4-byte form).
 *   pre:  buf points to >= len bytes; out is writable.
 *   post: PARSE_OK with out populated when buf[0]==CMD_INIT, len in
 *         {4,5,8}, compression<=1, and (for len==8) b[4..7] all zero.
 *         PARSE_TOO_SHORT when buf is NULL, out is NULL, len not in
 *         {4,5,8}, or (for len==8) b[4..7] carries a non-zero byte (no
 *         memcpy in any of these cases: the tail is checked first).
 *         PARSE_BAD_OPCODE when buf[0] != CMD_INIT.
 *         PARSE_BAD_COMPRESSION when compression > 1.
 *
 *   Mode 1 (LZ4) is accepted. The wire carries ONLY 0 or 1
 *   (groovymister.cpp writes `(lz4Frames) ? 1 : 0`), so anything above 1 is
 *   either a broken emitter or a protocol we do not know: refusing stays
 *   the right answer. Upstream, for its part, silently overwrites it with 0,
 *   which would give a corrupted picture if a mode 2 ever meant something.
 *
 *   Length 8 opens a PADDED session. CMD_INIT DOES NOT DEPEND on the mode of
 *   the current session; it is what chooses it (see proto.h). Bytes 4 to 7
 *   of the 8-byte form are PADDING, never fields: the memcpy stays bounded
 *   to sizeof(cmd_init_t) (4 bytes). They are REQUIRED TO BE ZERO: a
 *   non-zero tail is a malformed shape and returns PARSE_TOO_SHORT, reused
 *   here in the general sense of "no known shape matches", just as
 *   parse_cmd_blit_field_vsync_ex reuses PARSE_BAD_COMPRESSION for a
 *   different reason. A unit test of the session-mode state machine checks
 *   it: an 8-byte INIT with a non-zero tail must NEVER return PARSE_OK.
 */
parse_result_t parse_cmd_init(const uint8_t *buf, size_t len, cmd_init_t *out)
{
    if (!buf || !out)                        return PARSE_TOO_SHORT;
    if (len != 4u && len != 5u && len != 8u) return PARSE_TOO_SHORT;
    if (buf[0] != CMD_INIT)                  return PARSE_BAD_OPCODE;
    if (len == 8u && !tail_is_zero(buf, 4u, 8u)) return PARSE_TOO_SHORT;
    memcpy(out, buf, sizeof(cmd_init_t));
    if (out->compression > 1)                return PARSE_BAD_COMPRESSION;
    return PARSE_OK;
}

/*
 * parse_cmd_switchres -- opcode 0x03, 26 bytes total.
 *   pre:  buf points to >= len bytes; out is writable.
 *   post: PARSE_OK with out populated when buf[0]==CMD_SWITCHRES.
 *         PARSE_TOO_SHORT when buf is NULL, out is NULL, or len < 26 (no memcpy).
 *         PARSE_BAD_OPCODE when buf[0] != CMD_SWITCHRES.
 *
 *   The pclock double sits at offset 1, unaligned. memcpy of the whole packed
 *   struct is the safe path on every supported architecture.
 */
parse_result_t parse_cmd_switchres(const uint8_t *buf, size_t len, cmd_switchres_t *out)
{
    if (!buf || !out)                    return PARSE_TOO_SHORT;
    if (len < sizeof(cmd_switchres_t))   return PARSE_TOO_SHORT;
    if (buf[0] != CMD_SWITCHRES)         return PARSE_BAD_OPCODE;
    memcpy(out, buf, sizeof(cmd_switchres_t));  /* memcpy handles unaligned double safely */
    return PARSE_OK;
}

/*
 * parse_cmd_blit_field_vsync -- opcode 0x07, 8-byte base header + payload.
 *   pre:  buf points to >= len bytes; out, payload, payload_len all writable.
 *   post: PARSE_OK with out populated, *payload = buf+8, *payload_len = len-8.
 *         PARSE_TOO_SHORT when any pointer is NULL or len < 8 (no memcpy).
 *         PARSE_BAD_OPCODE when buf[0] != CMD_BLIT_FIELD_VSYNC.
 *
 *   The payload pointer aliases into the caller's buf; the caller must not
 *   free buf before consuming *payload. In the daemon, net.c owns the
 *   receive buffer that backs buf.
 */
parse_result_t parse_cmd_blit_field_vsync(const uint8_t *buf, size_t len,
                                          cmd_blit_field_vsync_base_t *out,
                                          const uint8_t **payload,
                                          size_t *payload_len)
{
    if (!buf || !out || !payload || !payload_len)            return PARSE_TOO_SHORT;
    if (len < sizeof(cmd_blit_field_vsync_base_t))           return PARSE_TOO_SHORT;
    if (buf[0] != CMD_BLIT_FIELD_VSYNC)                      return PARSE_BAD_OPCODE;
    memcpy(out, buf, sizeof(cmd_blit_field_vsync_base_t));
    *payload     = buf + sizeof(cmd_blit_field_vsync_base_t);
    *payload_len = len - sizeof(cmd_blit_field_vsync_base_t);
    return PARSE_OK;
}

/*
 * parse_cmd_blit_field_vsync_ex -- opcode 0x07, headers of 8, 9, 12 or 13
 * bytes. See blit_field_extras_t in proto.h for the table of lengths and for
 * the ambiguity of `len == 12` that the `compressed` argument removes.
 *
 * Upstream reference: hps_linux/src/support/groovy/groovy.cpp, its 0x07
 * handler.
 */
parse_result_t parse_cmd_blit_field_vsync_ex(const uint8_t *buf, size_t len,
                                             int compressed,
                                             cmd_blit_field_vsync_base_t *out,
                                             blit_field_extras_t *extras,
                                             const uint8_t **payload,
                                             size_t *payload_len)
{
    if (!buf || !out || !extras || !payload || !payload_len) return PARSE_TOO_SHORT;
    if (len < sizeof(cmd_blit_field_vsync_base_t))           return PARSE_TOO_SHORT;
    if (buf[0] != CMD_BLIT_FIELD_VSYNC)                      return PARSE_BAD_OPCODE;

    memcpy(out, buf, sizeof(cmd_blit_field_vsync_base_t));
    extras->lz4_size = 0u;
    extras->delta    = 0u;
    extras->dup      = 0u;
    *payload     = buf + sizeof(cmd_blit_field_vsync_base_t);
    *payload_len = len - sizeof(cmd_blit_field_vsync_base_t);

    /* frame_dup -- 9 bytes, [8] == 0x01, NO payload. Tested BEFORE the
     * compression branch: upstream (groovy.cpp) accepts 8 and 9 without
     * looking at blitCompression, and our bench runs compressed by default.
     * The CONTENT CHECK stays whole: the length alone is not enough, [8]
     * must be 0x01; any 9-byte datagram is not a frame_dup. A 9-byte header
     * whose [8] != 0x01 goes on below: refused in a compressed session (the
     * if (compressed) branch below), handled as an 8-byte header plus 1
     * payload byte outside compression. */
    if (len == 9u && buf[8] == 0x01u) {
        extras->dup  = 1u;
        *payload     = NULL;
        *payload_len = 0u;
        return PARSE_OK;
    }

    if (compressed) {
        /* With compression, the header travels ALONE: the bytes after the 8
         * base bytes are metadata, never picture. Any length other than 8,
         * 12 or 13 is a packet we cannot read. */
        if (len == 12u || len == 13u) {
            memcpy(&extras->lz4_size, buf + 8, sizeof(uint32_t));
            if (len == 13u && buf[12] == 0x01u)
                extras->delta = 1u;
            *payload     = NULL;
            *payload_len = 0u;
        } else if (len != sizeof(cmd_blit_field_vsync_base_t)) {
            return PARSE_BAD_COMPRESSION;
        } else {
            /* 8 bytes in a compressed session: no announced size, so nothing
             * to decompress. The groovymister library can send this when its
             * compression fails (it sizes its output at the raw size);
             * GroovyMAME, our reference, sizes it with LZ4_COMPRESSBOUND and
             * never does. We refuse rather than guess. */
            return PARSE_BAD_COMPRESSION;
        }
    }

    return PARSE_OK;
}

/*
 * parse_cmd_blit_vsync -- opcode 0x06, 7 or 11 bytes total. NO PAYLOAD.
 *   pre:  buf points to >= len bytes; out is writable.
 *   post: PARSE_OK with out populated when buf[0]==CMD_BLIT_VSYNC and
 *         len is 7 or 11. lz4_size = 0 for len==7, parsed from offset 7-10 LE
 *         for len==11.
 *         PARSE_TOO_SHORT for buf=NULL, out=NULL, or len not 7 or 11 (no memcpy).
 *         PARSE_BAD_OPCODE when buf[0] != CMD_BLIT_VSYNC.
 *
 *   Wire reference: psakhis/Groovy_MiSTer hps_linux/src/support/groovy/groovy.cpp,
 *   its 0x06 handler (the FPGA-side reference implementation). The strict
 *   length check matches the reference's `if (len == 7 || len == 11)` guard;
 *   anything else is dropped.
 *
 *   The header packet stands alone on the wire: there is NO payload in this
 *   datagram. Later UDP packets carry pure RGB888 bytes without an opcode
 *   prefix; the continuation guard in net.c routes them to
 *   proto_reassemble_push directly. That is why this parser, unlike
 *   parse_cmd_blit_field_vsync, returns no payload pointer.
 *
 *   Wire layout differs from CMD_BLIT_FIELD_VSYNC (0x07) in two important ways:
 *   no field byte (so vsync at offset 5-6 not 6-7), and the header arrives
 *   alone with no trailing payload bytes in the same datagram.
 */
parse_result_t parse_cmd_blit_vsync(const uint8_t *buf, size_t len,
                                    cmd_blit_vsync_base_t *out)
{
    if (!buf || !out)                       return PARSE_TOO_SHORT;
    if (len != 7 && len != 11)              return PARSE_TOO_SHORT;
    if (buf[0] != CMD_BLIT_VSYNC)           return PARSE_BAD_OPCODE;
    out->cmd = buf[0];
    /* frame_id at offset 1-4, u32 LE. memcpy is safe vs alignment. */
    memcpy(&out->frame_id, buf + 1, sizeof(uint32_t));
    /* vsync at offset 5-6, u16 LE. Note: no field byte before vsync (contrast
     * with 0x07 which puts vsync at offset 6). */
    memcpy(&out->vsync, buf + 5, sizeof(uint16_t));
    if (len == 11) {
        /* lz4_compressed_size at offset 7-10, u32 LE. Only present when the
         * session is compressed; net.c uses it to arm the decompression of
         * the frame. */
        memcpy(&out->lz4_size, buf + 7, sizeof(uint32_t));
    } else {
        out->lz4_size = 0u;
    }
    return PARSE_OK;
}

/*
 * parse_cmd_audio -- the 3-byte header of CMD_AUDIO (0x04).
 *   pre:  buf points to >= len bytes, out != NULL.
 *   post: PARSE_OK and *out filled when len >= 3 and buf[0] == CMD_AUDIO;
 *         PARSE_BAD_OPCODE when buf[0] != CMD_AUDIO;
 *         PARSE_TOO_SHORT otherwise.
 *
 * The size is read with memcpy and not by dereferencing a u16: the field
 * falls at offset 1, so it is unaligned, and an unaligned access is
 * undefined behaviour. Same discipline as the pclock field of CMD_SWITCHRES.
 */
parse_result_t parse_cmd_audio(const uint8_t *buf, size_t len,
                               cmd_audio_t *out)
{
    if (!buf || !out || len < sizeof(cmd_audio_t)) return PARSE_TOO_SHORT;
    if (buf[0] != CMD_AUDIO) return PARSE_BAD_OPCODE;
    out->cmd = buf[0];
    memcpy(&out->size, buf + 1, sizeof(uint16_t));
    return PARSE_OK;
}

/*
 * proto_audio_rate_hz -- see proto.h.
 */
unsigned proto_audio_rate_hz(uint8_t code)
{
    switch (code) {
        case 1u: return 22050u;
        case 2u: return 44100u;
        case 3u: return 48000u;
        default: return 0u;
    }
}

/*
 * proto_is_command_header -- see proto.h.
 *
 * The list lives here, in one place, because two copies drifting apart
 * would be a delayed bug: the video and the audio exits from continuation
 * must recognise exactly the same shapes.
 */
int proto_is_command_header(const uint8_t *buf, size_t len, int compressed, int padded)
{
    if (!buf || len < 1) return 0;

    /* --- (a) What depends on NO session mode: 0x03, 0x06, 0x07. The
     * lengths of 0x07 follow EXACTLY the rule of
     * parse_cmd_blit_field_vsync_ex: 8 always; 9 (frame_dup) in BOTH
     * session states, AND ONLY if [8] is 0x01, the same content check as
     * the parser, never the length alone; 12 and 13 only with compression.
     * A divergence between these two places would swallow a legitimate
     * header as pixels, or the reverse: a 9-byte datagram whose [8] !=
     * 0x01, recognised here as a header but refused by the parser, used to
     * cost a frame in flight in a compressed session (continuation_escape)
     * and then a silent PARSE_BAD_COMPRESSION. */
    const int blit_field = (buf[0] == CMD_BLIT_FIELD_VSYNC) &&
        (len == 8U
         || (len == 9U && buf[8] == 0x01u)  /* frame_dup: both states, content checked */
         || (compressed && (len == 12U || len == 13U)));
    if ((buf[0] == CMD_SWITCHRES  && len == 26U) ||
        (buf[0] == CMD_BLIT_VSYNC && (len == 7U || len == 11U)) ||
        blit_field) {
        return 1;
    }

    /* --- (b) CMD_INIT: recognised at 4, 5, and 8-with-zero-tail, IN BOTH
     * MODES; it negotiates the mode, it cannot depend on it. 4 is the real
     * format: cmd_init_t, what gm_send_init writes, what GroovyMAME 0.289,
     * our reference emitter, sends. 5 stays accepted because the
     * groovymister library of the upstream RetroArch fork, which is not our
     * target, sends that variant with rgbMode; refusing it would break an
     * emitter that works today. 8 opens the padded session; the tail
     * b[4..7] must be zero; see the table and the false-positive arithmetic
     * in proto.h. The price of recognising it inside a frame, at all three
     * lengths: one lost picture and one log line, or one damaged audio
     * block, never a corrupted picture. */
    if (buf[0] == CMD_INIT) {
        /* Field bounds, the same as is_command_header in the software
         * client (one table). Tested AFTER the length check: buf[1] and
         * buf[2] exist only from 4 bytes on. */
        if (len != 4U && len != 5U && len != 8U) return 0;
        if (buf[1] > 1U || buf[2] > 3U) return 0;
        if (len == 8U && !tail_is_zero(buf, 4U, 8U)) return 0;
        return 1;
    }

    /* --- (c) The four short commands: the TABLE depends on the mode. The
     * classic length is not accepted in a padded session, and the other
     * way round: one session, one mode, one table. */
    if (padded) {
        if (buf[0] == CMD_CLOSE       && len == 8U && tail_is_zero(buf, 1U, 8U)) return 1;
        if (buf[0] == CMD_GET_STATUS  && len == 8U && tail_is_zero(buf, 1U, 8U)) return 1;
        if (buf[0] == CMD_GET_VERSION && len == 8U && tail_is_zero(buf, 1U, 8U)) return 1;
        if (buf[0] == CMD_AUDIO       && len == 8U && tail_is_zero(buf, 3U, 8U)) return 1;
        return 0;
    } else {
        if (buf[0] == CMD_CLOSE       && len == 1U) return 1;
        if (buf[0] == CMD_GET_STATUS  && len == 1U) return 1;
        if (buf[0] == CMD_GET_VERSION && len == 1U) return 1;
        if (buf[0] == CMD_AUDIO       && len == 3U) return 1;
        return 0;
    }
}

/*
 * proto_audio_escape -- see proto.h for the measurement that forced the
 * second condition.
 */
int proto_audio_escape(const uint8_t *buf, size_t len,
                       uint32_t expected, uint32_t received, int compressed,
                       int padded)
{
    /* `padded` is passed unchanged to proto_is_command_header, exactly as
     * `compressed` is: the padded mode changes the table, not this rule. */
    if (!proto_is_command_header(buf, len, compressed, padded)) return 0;
    if (received > expected) return 1;          /* inconsistent state: get out */
    /* Would this datagram end the block to the byte? Then it is sound. */
    return (len == (size_t)(expected - received)) ? 0 : 1;
}

/*
 * proto_video_escape -- see proto.h: the exact-remainder rule, its exception
 * (a real CMD_INIT always escapes) and the case it swallows.
 */
int proto_video_escape(const uint8_t *buf, size_t len,
                       uint32_t expected, uint32_t received, int compressed,
                       int padded)
{
    if (!proto_is_command_header(buf, len, compressed, padded)) return 0;
    if (buf[0] == CMD_INIT) return 1;            /* bounded by the table */
    if (received >= expected) return 1;          /* inconsistent state: get out */
    /* Would this datagram end the frame to the byte? Then it is the tail. */
    return (len == (size_t)(expected - received)) ? 0 : 1;
}

/*
 * proto_switchres_plausible -- see proto.h. The fields are read from a copy:
 * the structure is packed, and the double falls at offset 1.
 */
int proto_switchres_plausible(const cmd_switchres_t *sw)
{
    if (!sw) return 0;
    const cmd_switchres_t s = *sw;
    const double pclock = s.pclock;
    if (s.h_active == 0u || s.v_active == 0u) return 0;
    if (!(s.h_active <= s.h_begin && s.h_begin <= s.h_end && s.h_end <= s.h_total))
        return 0;
    if (!(s.v_active <= s.v_begin && s.v_begin <= s.v_end && s.v_end <= s.v_total))
        return 0;
    if (s.interlace > 1u) return 0;
    /* Written so that NaN fails: every comparison with NaN is false. */
    if (!(pclock >= PROTO_PCLOCK_MIN_MHZ && pclock <= PROTO_PCLOCK_MAX_MHZ))
        return 0;
    return 1;
}

/*
 * proto_audio_size_ok -- see proto.h.
 */
int proto_audio_size_ok(uint32_t size, uint8_t rate_code, uint8_t channels)
{
    if (proto_audio_rate_hz(rate_code) == 0u) return 0;
    if (channels != 1u && channels != 2u) return 0;
    if (size == 0u || size > AUDIO_MAX_BLOCK_BYTES) return 0;
    return (size % (2u * (uint32_t)channels)) == 0u;
}

/*
 * proto_out_of_frame_accepts -- see proto.h. The table is that of
 * proto_is_command_header; only the exception of the 0x07 header with an
 * inline payload is added, and it goes through the parser, never through a
 * copied rule.
 */
int proto_out_of_frame_accepts(const uint8_t *buf, size_t len, int compressed,
                               int padded)
{
    if (proto_is_command_header(buf, len, compressed, padded)) return 1;
    if (!buf || compressed || len <= 9u || buf[0] != CMD_BLIT_FIELD_VSYNC)
        return 0;
    cmd_blit_field_vsync_base_t hdr;
    blit_field_extras_t         xtra;
    const uint8_t              *inline_payload;
    size_t                      inline_payload_len;
    return parse_cmd_blit_field_vsync_ex(buf, len, 0, &hdr, &xtra,
                                         &inline_payload, &inline_payload_len) == PARSE_OK;
}

/*
 * proto_session_on_init / proto_session_on_close -- the session mode.
 *
 * The state transition of the session mode LIVES HERE, and nowhere else:
 * net.c CALLS these functions, it does not rewrite them. That is what a
 * unit test checks by playing these same functions on the host; see
 * proto.h for the full comment on the contract, including the reasoning
 * "the last CMD_INIT wins" and its limit "per network thread, not per
 * peer".
 */
int proto_session_on_init(uint8_t *padded, parse_result_t r, size_t len)
{
    if (r != PARSE_OK) return 0;
    uint8_t next = (len == 8u) ? 1u : 0u;
    if (*padded == next) return 0;
    *padded = next;
    return 1;
}

/* ---------------------------------------------------------------------------
 * proto_sight_* and proto_guard_*: the two blindness decisions. The contract
 * and the reasons for the thresholds are in proto.h.
 */
void proto_sight_key(struct proto_sight *v, uint32_t plain_bytes, int is_lz4)
{
    if (!v) return;
    const uint8_t c = is_lz4 ? 1u : 0u;
    if (v->key_set && v->key_bytes == plain_bytes && v->key_compressed == c)
        return;
    v->key_bytes      = plain_bytes;
    v->key_compressed = c;
    v->key_set        = 1u;
    v->reject_count   = 0u;
    v->first_ms       = 0u;
}

int proto_sight_reject(struct proto_sight *v, uint64_t now_ms)
{
    if (!v) return 0;
    if (v->reject_count == 0u || now_ms < v->first_ms) {
        /* First of the run, or a clock going backwards: start again here. */
        v->reject_count = 1u;
        v->first_ms     = now_ms;
        return 0;
    }
    if (v->reject_count < UINT32_MAX) v->reject_count++;
    return (v->reject_count >= PROTO_SIGHT_REJECTS_MIN &&
            now_ms - v->first_ms >= (uint64_t)PROTO_SIGHT_SPAN_MS) ? 1 : 0;
}

void proto_sight_good(struct proto_sight *v)
{
    if (!v) return;
    v->reject_count = 0u;
    v->first_ms     = 0u;
}

/* Opens a new window if none is open, if the current one has lived its
 * second, or if the clock goes backwards. */
static void guard_window(struct proto_guard *g, uint64_t now_ms)
{
    if (!g->is_open || now_ms < g->start_ms ||
        now_ms - g->start_ms >= (uint64_t)PROTO_GUARD_WINDOW_MS) {
        g->start_ms     = now_ms;
        g->absurd_count = 0u;
        g->episodes     = 0u;
        g->is_open      = 1u;
    }
}

int proto_guard_absurd(struct proto_guard *g, uint64_t now_ms)
{
    if (!g) return 0;
    guard_window(g, now_ms);
    if (g->absurd_count < UINT32_MAX) g->absurd_count++;
    return g->absurd_count >= PROTO_GUARD_ABSURD_MAX ? 1 : 0;
}

int proto_guard_episode(struct proto_guard *g, uint64_t now_ms)
{
    if (!g) return 0;
    guard_window(g, now_ms);
    if (g->episodes < UINT32_MAX) g->episodes++;
    return g->episodes >= PROTO_GUARD_EPISODES_MAX ? 1 : 0;
}

int proto_session_on_close(uint8_t *padded)
{
    if (*padded == 0u) return 0;
    *padded = 0u;
    return 1;
}

/*
 * parse_dispatch -- opcode-by-opcode router, called once per UDP datagram.
 *   pre:  buf points to >= len bytes (typically MTU-sized).
 *   post: PARSE_OK for any well-formed known opcode (including the 3-byte
 *         CMD_AUDIO header and the bare-opcode commands CLOSE / GET_STATUS /
 *         GET_VERSION / BLIT_VSYNC).
 *         PARSE_TOO_SHORT for buf=NULL or len<1, or when a routed parser
 *         bound-checks the datagram and finds it short or malformed.
 *         PARSE_BAD_OPCODE for any opcode outside 0x01-0x08.
 *         PARSE_BAD_COMPRESSION when CMD_INIT carries a compression byte above 1.
 *
 *   The bare-opcode commands intentionally fall through one switch arm: the
 *   caller only needs to know the opcode was recognised; the response (e.g.
 *   building the status ACK for GET_STATUS) is the caller's job, not the
 *   parser's.
 */
parse_result_t parse_dispatch(const uint8_t *buf, size_t len)
{
    if (!buf || len < 1) return PARSE_TOO_SHORT;
    switch (buf[0]) {
    case CMD_CLOSE:
    case CMD_GET_STATUS:
    case CMD_GET_VERSION:
    case CMD_BLIT_VSYNC:    /* progressive; net.c parses it with parse_cmd_blit_vsync */
        return PARSE_OK;
    case CMD_INIT: {
        cmd_init_t out;
        return parse_cmd_init(buf, len, &out);
    }
    case CMD_SWITCHRES: {
        cmd_switchres_t out;
        return parse_cmd_switchres(buf, len, &out);
    }
    case CMD_AUDIO: {
        /* The header is bounded like the others. A datagram shorter than 3
         * bytes carries an 0x04 opcode with no size: that is noise, not a
         * command. */
        cmd_audio_t out;
        return parse_cmd_audio(buf, len, &out);
    }
    case CMD_BLIT_FIELD_VSYNC: {
        cmd_blit_field_vsync_base_t hdr;
        const uint8_t *p; size_t pl;
        return parse_cmd_blit_field_vsync(buf, len, &hdr, &p, &pl);
    }
    default:
        return PARSE_BAD_OPCODE;
    }
}

/*
 * proto_reassemble_push -- accumulate one BLIT payload chunk into the frame buffer.
 *   pre:  f != NULL, f->buf points to >= f->expected_bytes writable bytes,
 *         payload points to >= len readable bytes.
 *   post: returns 1 when f->received_bytes has reached f->expected_bytes,
 *         0 when more bytes are still expected,
 *         0 (with no copy) when any input pointer is NULL.
 *         The destination buffer is never written past f->expected_bytes: extra
 *         bytes from a misbehaving emitter are silently dropped.
 *
 *   This implementation runs a byte counter against expected_bytes (RGB888 frame
 *   size = h_active * v_active * 3). Tracking a slice count would be wrong:
 *   the wire protocol has no slice-count field.
 */
int proto_reassemble_push(struct frame_reassembly *f, const uint8_t *payload, size_t len)
{
    if (!f || !f->buf || !payload) return 0;
    if (f->received_bytes >= f->expected_bytes) return 1;  /* already complete */
    size_t remaining = (size_t)(f->expected_bytes - f->received_bytes);
    size_t to_copy   = (len < remaining) ? len : remaining;  /* bound to prevent overflow */
    memcpy(f->buf + f->received_bytes, payload, to_copy);
    f->received_bytes += (uint32_t)to_copy;
    return (f->received_bytes >= f->expected_bytes) ? 1 : 0;
}

/* proto_reassemble_push_field -- field-aware reassembly wrapper.
 *
 * Wire side: the CMD_BLIT_FIELD_VSYNC handler of net.c delegates to this
 * function when the daemon's current_regime == REGIME_480I. The progressive
 * regime (240p super-res, CMD_BLIT_VSYNC 0x06) keeps calling
 * proto_reassemble_push.
 *
 * Row math: destination row = (bytes_into_field / row_bytes) * 2U + field.
 * Branch-free in the hot path: the field byte just shifts the starting
 * offset; stride is always 2 * row_bytes. The `2U` literal is the
 * unsigned-clean form required under `-Wconversion`.
 *
 * Caller contract (net.c):
 *   - f->expected_bytes = fb_w * (fb_h/2) * 3   (per-field size)
 *   - f->received_bytes = 0                      (reset between field 0 and field 1)
 *   - f->buf            = slot->buf              (the 480-row framebuffer slot)
 *   - fb_w              = h_active_announced     (live atomic from CMD_SWITCHRES)
 *
 * Both fields share one 480-row buffer because the radeon kernel module
 * scans an interlaced mode out of a single framebuffer, alternating rows.
 */
int proto_reassemble_push_field(struct frame_reassembly *f,
                                uint8_t field, uint32_t fb_w,
                                const uint8_t *payload, size_t len)
{
    if (!f || !f->buf || !payload || field > 1) return 0;
    size_t row_bytes = (size_t)fb_w * 3U;
    if (row_bytes == 0) return 0;  /* defensive: guards % and / */
    size_t bytes_into_field = f->received_bytes;
    if (bytes_into_field >= f->expected_bytes) return 1;
    size_t remaining = (size_t)(f->expected_bytes - bytes_into_field);
    size_t to_copy   = (len < remaining) ? len : remaining;
    size_t written = 0;
    while (written < to_copy) {
        size_t field_row = (bytes_into_field + written) / row_bytes;
        size_t row_off   = (bytes_into_field + written) % row_bytes;
        size_t dst_row   = field_row * 2U + (size_t)field;
        size_t row_remaining = row_bytes - row_off;
        size_t chunk = (to_copy - written < row_remaining)
                       ? to_copy - written : row_remaining;
        memcpy(f->buf + dst_row * row_bytes + row_off, payload + written, chunk);
        written += chunk;
    }
    f->received_bytes += (uint32_t)written;
    return (f->received_bytes >= f->expected_bytes) ? 1 : 0;
}
