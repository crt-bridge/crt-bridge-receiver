/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * proto.h -- the Groovy v2 wire protocol (psakhis), as this receiver reads it.
 *
 * UDP/32100 carries commands, video and audio; the input channel is
 * UDP/32101 (input.h). The decisions declared here are pure: no socket, no
 * allocation, no global state. net.c calls them and never copies them, so
 * each rule lives in one place and can be tested on any host.
 *
 * Upstream compatibility. "Upstream" is not a single source: the GroovyMAME
 * emitter and the groovymister library (Groovy_MiSTer) disagree in places,
 * and our reference is GroovyMAME. Three things look wrong and are not; do
 * NOT "fix" them:
 *   - 240p travels as CMD_BLIT_VSYNC (0x06), the only blit GroovyMAME uses.
 *     Do not route it to CMD_BLIT_FIELD_VSYNC (0x07), even though
 *     groovymister marks 0x06 as deprecated;
 *   - the CMD_INIT our emitter SENDS is 4 bytes, as GroovyMAME sends it. Do
 *     not add a fifth byte to it;
 *   - there is no delta mode to handle: GroovyMAME 0.289 has none, and a
 *     delta frame is refused openly (net.c logs it once).
 * What the receiver ACCEPTS is a separate matter, and wider: CMD_INIT at 4,
 * 5 or 8 bytes, frame_dup in both session states (see below).
 */
#ifndef RECEIVER_PROTO_H
#define RECEIVER_PROTO_H

#include <stdint.h>
#include <stddef.h>

/* Opcodes (Groovy v2, psakhis). */
#define CMD_CLOSE             0x01
#define CMD_INIT              0x02
#define CMD_SWITCHRES         0x03
#define CMD_AUDIO             0x04
#define CMD_GET_STATUS        0x05
#define CMD_BLIT_VSYNC        0x06
#define CMD_BLIT_FIELD_VSYNC  0x07
#define CMD_GET_VERSION       0x08

/* Status flags of the 13-byte ACK packet. FLAG_AUDIO carries a SESSION
 * state (the CMD_INIT announced a non-zero rate and channel count), not
 * "audio bytes were received": GroovyMAME waits for this bit before it sends
 * any audio, so tying it to received audio would lock both sides forever.
 * FLAG_VGA_F1 carries the parity of the acknowledged frame. */
#define FLAG_VRAM_READY      0x01
#define FLAG_VRAM_END_FRAME  0x02
#define FLAG_VRAM_SYNCED     0x04
#define FLAG_VGA_FRAMESKIP   0x08
#define FLAG_VGA_VBLANK      0x10
#define FLAG_VGA_F1          0x20
#define FLAG_AUDIO           0x40
#define FLAG_VRAM_QUEUE      0x80

#pragma pack(push, 1)

/* CMD_INIT wire format (GroovyMAME 0.287 with Switchres 2.22c):
 *   byte 0: cmd  (0x02)
 *   byte 1: compression. ON THE WIRE, 0 = raw and 1 = LZ4, AND NOTHING ELSE.
 *           Checked against the upstream source: groovymister.cpp writes
 *           `(lz4Frames) ? 1 : 0`, and groovy.cpp overwrites any value
 *           above 1 with 0. The values 0 to 6 that psakhis documents are the
 *           LOCAL setting of his emitter, which picks a strategy (LZ4, LZ4HC,
 *           delta, adaptive); they never travel. Reading "2 = lz4hc,
 *           3 = adaptive" off the wire would be wrong.
 *   byte 2: audio sample_rate code
 *   byte 3: audio channels (0=disabled, 1=mono, 2=stereo)
 * Total 4 bytes. The `rgb_mode` field that some specs document is NOT sent
 * by current GroovyMAME: a capture on real hardware shows `02 00 03 02`.
 *
 * The wire may also carry this same shape followed by four ZERO padding
 * bytes (8 bytes in all), which opens a PADDED session; the table and the
 * reasoning are in the comment above proto_is_command_header below. The
 * structure does not change: bytes 4 to 7 are NEVER fields, only padding
 * that must be zero. */
typedef struct {
    uint8_t cmd;
    uint8_t compression;
    uint8_t sample_rate;
    uint8_t channels;
} cmd_init_t;
_Static_assert(sizeof(cmd_init_t) == 4, "CMD_INIT must be 4 bytes");

typedef struct {
    uint8_t  cmd;
    double   pclock;      /* IEEE 754, 8 bytes LE, unaligned: always memcpy */
    uint16_t h_active;
    uint16_t h_begin;
    uint16_t h_end;
    uint16_t h_total;
    uint16_t v_active;
    uint16_t v_begin;
    uint16_t v_end;
    uint16_t v_total;
    uint8_t  interlace;
} cmd_switchres_t;
_Static_assert(sizeof(cmd_switchres_t) == 26, "CMD_SWITCHRES must be 26 bytes");

/* CMD_AUDIO (0x04) wire format. A 3-byte header:
 *   byte 0     : cmd = 0x04
 *   bytes 1-2  : size of the audio block, u16 little-endian
 * The header travels ALONE on the wire; the PCM bytes follow in later
 * datagrams WITHOUT an opcode prefix, exactly like BLIT payloads. The
 * receiver joins them by counting the bytes it expects.
 * Source: psakhis/Groovy_MiSTer api/groovymister.cpp. Our emitter sends it
 * from libgm, gm_send_audio. */
typedef struct {
    uint8_t  cmd;
    uint16_t size;
} cmd_audio_t;
_Static_assert(sizeof(cmd_audio_t) == 3, "CMD_AUDIO header must be 3 bytes");

typedef struct {
    uint8_t  cmd;
    uint32_t frame_id;
    uint8_t  field;
    uint16_t vsync;
} cmd_blit_field_vsync_base_t;

/* What the long variants of the 0x07 header carry beyond the 8 base bytes.
 * Kept apart because the structure above is a copy of the wire, locked by a
 * _Static_assert.
 *
 * The four lengths, and what THIS receiver does with them:
 *
 *   len  compression  meaning
 *    8   either       raw, payload = expected_bytes bytes
 *    9   either       frame_dup: [8] = 0x01, NO payload, the frame repeats
 *                     the previous one. The CONTENT CHECK stays whole: the
 *                     length alone is not enough.
 *   12   on           [8..11] = compressed size, u32 little-endian
 *   13   on           same + [12] = 0x01: the payload is a DELTA (XOR)
 *                     against the previous frame
 *
 * THIS receiver accepts `len == 9` WHATEVER the compression state of the
 * session, like the upstream groovymister library (groovy.cpp never tests
 * `blitCompression` for this length). The reason upstream needs it: it
 * sizes its LZ4 output buffer at the raw size, not at LZ4_compressBound, so
 * on content that compresses poorly its compression fails and it
 * legitimately falls back to 8 or 9 bytes IN A COMPRESSED SESSION.
 *
 * KNOWN AND ACCEPTED DIVERGENCE, not a defect to fix here: the upstream
 * MiSTer FPGA receiver (the real hardware that Groovy_MiSTer drives) is
 * STRICTER. Its rule is `if (len == 9 && !blitCompression)`: it reads the
 * frame_dup flag ONLY outside compression, and expects the long form
 * (`len == 13`) in a compressed session. No MiSTer FPGA sits on this bench,
 * and GroovyMAME 0.289 (the reference emitter of this project) sends no
 * form of `frame_dup`, so the divergence has no visible effect here.
 *
 * CAREFUL, the point that corrupts silently if missed: bytes 8 to 11 are
 * the compressed size ONLY when compression is on. Our emitter packs the
 * header and the payload into the SAME datagram where upstream sends the
 * header alone, so for us an uncompressed `len == 12` is an 8-byte header
 * followed by 4 bytes of picture. The upstream rule
 * (`if ((len == 12 || len == 13) && blitCompression)`) removes the
 * ambiguity, and we adopt it as is. */
typedef struct {
    uint32_t lz4_size;   /* 0 when the header carries none */
    uint8_t  delta;      /* 1: the payload is a delta (len 13) */
    uint8_t  dup;        /* 1: frame_dup (len 9), there is no payload */
} blit_field_extras_t;
_Static_assert(sizeof(cmd_blit_field_vsync_base_t) == 8,
               "BLIT_FIELD_VSYNC base header must be 8 bytes");

/* CMD_BLIT_VSYNC (0x06) wire format. Per psakhis/Groovy_MiSTer
 * hps_linux/src/support/groovy/groovy.cpp (its 0x06 handler), the on-wire
 * packet is EITHER 7 bytes (raw mode, no compression metadata) OR 11 bytes
 * (with a trailing u32 LE lz4_compressed_size at offset 7-10). NOTE: there
 * is NO field byte; vsync sits at offset 5-6. Upstream calls this the
 * legacy progressive BLIT path, yet GroovyMAME 0.287 emits it for
 * 240p/288p/480p games (seen in a capture on real hardware), so it is a
 * first-class path here. The header packet stands alone on the wire;
 * payload bytes arrive in later UDP datagrams with NO opcode prefix and are
 * routed to proto_reassemble_push by the continuation guard in net.c. */
typedef struct {
    uint8_t  cmd;        /* CMD_BLIT_VSYNC = 0x06 */
    uint32_t frame_id;
    uint16_t vsync;
    uint32_t lz4_size;   /* 0 when len==7 or compression == raw; populated only when len==11 */
} cmd_blit_vsync_base_t;

typedef struct {
    uint32_t frame_echo;
    uint16_t vcount_echo;
    uint32_t frame;
    uint16_t vcount;
    uint8_t  flags;
} status_pkt_t;
_Static_assert(sizeof(status_pkt_t) == 13, "ACK must be 13 bytes");

#pragma pack(pop)

typedef enum {
    PARSE_OK              = 0,
    PARSE_TOO_SHORT       = 1,
    PARSE_BAD_OPCODE      = 2,
    PARSE_BAD_COMPRESSION = 3
} parse_result_t;

parse_result_t parse_cmd_init(const uint8_t *buf, size_t len, cmd_init_t *out);
parse_result_t parse_cmd_switchres(const uint8_t *buf, size_t len, cmd_switchres_t *out);
parse_result_t parse_cmd_blit_field_vsync(const uint8_t *buf, size_t len,
                                          cmd_blit_field_vsync_base_t *out,
                                          const uint8_t **payload,
                                          size_t *payload_len);

/* The variant that understands the long headers. `compressed` says whether
 * the session's CMD_INIT announced LZ4; without it, `len == 12` is
 * ambiguous.
 *
 * `extras` is always written, cleared first. On frame_dup, *payload is NULL
 * and *payload_len is 0.
 *
 * parse_cmd_blit_field_vsync above stays the uncompressed case and is left
 * unchanged: it is the one the original unit tests exercise. */
parse_result_t parse_cmd_blit_field_vsync_ex(const uint8_t *buf, size_t len,
                                             int compressed,
                                             cmd_blit_field_vsync_base_t *out,
                                             blit_field_extras_t *extras,
                                             const uint8_t **payload,
                                             size_t *payload_len);
parse_result_t parse_cmd_blit_vsync(const uint8_t *buf, size_t len,
                                    cmd_blit_vsync_base_t *out);

/* Parses the 3-byte header of CMD_AUDIO. PARSE_TOO_SHORT if the datagram is
 * shorter than 3 bytes. The announced size needs no bound here: it fits in
 * a u16, so at most 65535, and the audio reassembly buffer is sized for that
 * maximum. */
parse_result_t parse_cmd_audio(const uint8_t *buf, size_t len,
                               cmd_audio_t *out);

/* Dispatch by opcode at buf[0]. Returns PARSE_BAD_OPCODE for unknown. */
parse_result_t parse_dispatch(const uint8_t *buf, size_t len);

/* Is the datagram a recognised command header?
 *
 * Detection: buf[0] is a known opcode AND len is exactly the wire size of
 * that opcode. Payload chunks are raw bytes with no opcode prefix; this is
 * the only way to tell them apart.
 *
 * Used by both exits from continuation mode, video and audio, so that they
 * cannot drift apart.
 *
 * `compressed` says whether CMD_INIT announced LZ4 for the current session;
 * without it, the CMD_BLIT_FIELD_VSYNC (0x07) header is ambiguous. Its four
 * lengths, under the same conditions as parse_cmd_blit_field_vsync_ex (do
 * not let the two lists diverge):
 *
 *   len  compression  meaning
 *    8   either       raw
 *    9   either       frame_dup, [8] = 0x01, no payload; in BOTH session
 *                     states, like upstream (groovy.cpp, see
 *                     blit_field_extras_t above)
 *   12   on           [8..11] = compressed size, u32 little-endian
 *   13   on           same + [12] = 0x01: delta
 *
 * Widening the list widens the false-positive window, and the figure is
 * known. A continuation chunk is GM_MTU_DEFAULT (1472) bytes except at the
 * tail of a field: only that tail can measure exactly 9, 12 or 13 bytes AND
 * start with 0x07, and, for 9, end with 0x01: the length alone does not
 * make a frame_dup, here as in parse_cmd_blit_field_vsync_ex (a 9-byte
 * header accepted here but refused by the parser would cost a frame). Outside
 * compression the list does not move (7, 8, 9, 11, 12, 13 were already
 * sensitive); IN A COMPRESSED SESSION, accepting frame_dup there takes the
 * sensitive lengths from five (7, 8, 11, 12, 13) to six (7, 8, 9, 11, 12,
 * 13), so the false-positive window grows by one length in that state
 * only. The cost of a false positive stays one lost picture and one log
 * line: strictly better than the behaviour before this guard existed,
 * which cost about 273 unknown_opcode events and a corrupted picture for a
 * single lost datagram (measured on the bench). The trade-off is accepted
 * knowingly.
 *
 * ---------------------------------------------------------------------
 * `padded`: is the session PADDED? Set once per CMD_INIT (8 bytes at the
 * opening instead of 4 or 5), never re-evaluated per datagram. Padding
 * exists because some Wi-Fi links drop very short datagrams: in a padded
 * session no command is shorter than 8 bytes and no payload datagram is
 * shorter than 256. TABLE OF LENGTHS PER SESSION MODE, shared by this file
 * and by is_command_header in the software client (client/gmclient.c); the
 * two MUST NOT diverge:
 *
 *   opcode                     classic      padded      ZERO tail required (8-byte form)
 *   0x01 CMD_CLOSE              len==1       len==8      b[1..7]
 *   0x02 CMD_INIT               4, 5 or 8    4, 5 or 8   b[4..7], DOES NOT DEPEND ON THE MODE;
 *                                                        b[1] <= 1 and b[2] <= 3
 *   0x03 CMD_SWITCHRES          len==26      len==26     -
 *   0x04 CMD_AUDIO              len==3       len==8      b[3..7]
 *   0x05 CMD_GET_STATUS         len==1       len==8      b[1..7]
 *   0x06 CMD_BLIT_VSYNC         7 or 11      same        -
 *   0x07 CMD_BLIT_FIELD_VSYNC   8/9/12/13    same        -
 *   0x08 CMD_GET_VERSION        len==1       len==8      b[1..7]
 *
 * For the FOUR short commands (0x01, 0x04, 0x05, 0x08): the CLASSIC length
 * is NOT accepted in a PADDED session, and the other way round: the exact
 * length FOR THE SESSION'S MODE. One session, one mode, one table: that is
 * what keeps the false-positive window from growing on both sides at once.
 *
 * A DECIDED RULE, NOT A PENDING EXCEPTION: CMD_INIT is recognised at 4, 5
 * AND 8 bytes IN BOTH MODES, with bytes 4 to 7 required to be zero in the
 * 8-byte form. The INIT chooses the mode; it cannot depend on the table it
 * installs. CMD_INIT is the datagram that NEGOTIATES the mode; when it
 * arrives, the session is still in the OLD mode (classic for the very first
 * session, the previous mode for any reopening). Making it follow the table
 * of the CURRENT mode would make it invisible in the one case that matters.
 * The cost of forgetting this has already been paid once: a CMD_INIT that
 * arrived WHILE A FRAME WAS IN FLIGHT was swallowed as pixels and its
 * session settings lost, a real case since the emitter is restarted for
 * every run. With padding, the first datagram of a padded session arriving
 * during a frame of a previous classic session is EXACTLY that case.
 *
 * THE CASE THAT MUST BE WRITTEN DOWN: the 8-byte payload tail that starts
 * with 0x01..0x08. In a classic session, an 8-byte block tail is taken for
 * a header only if its first byte is 0x07 (and, for len==9, if its last is
 * 0x01). In a padded session, FIVE more opcodes claim length 8 (0x01, 0x02,
 * 0x04, 0x05, 0x08): with no further guard, the probability that an 8-byte
 * tail is taken for a header would rise from 1/256 to 6/256, that is 2.3 %.
 *
 * THE ZERO-TAIL REQUIREMENT TAKES IT WELL BELOW TODAY'S LEVEL:
 *
 *   shape, padded mode                             free bytes  zero bytes  spoofing probability
 *   0x01, 0x05, 0x08 at 8 bytes                     0           7           2^-8 x 2^-56 = 2^-64
 *   0x04 at 8 bytes                                 2 (size)    5           2^-8 x 2^-40 = 2^-48
 *   0x02 at 8 bytes                                 3 (comp/rate/channels) 4  2^-8 x 2^-32 = 2^-40, less still with the field bounds
 *   for comparison: 0x04 at 3 bytes, CLASSIC, today  2          0           2^-8
 *
 * In other words: the five new claims on length 8 are, TOGETHER, at least
 * 2^32 times less likely than the aliasing ALREADY ACCEPTED today on the
 * 3-byte CMD_AUDIO. The padded session is SAFER than the classic one, not
 * less safe.
 *
 * The residual cost, bounded and known: a real 8-byte padded header that
 * arrives during an audio block with EXACTLY 8 bytes left to receive is
 * classed as payload by the second condition of proto_audio_escape, which
 * does not change. Cost: one damaged audio block, never a picture. Same
 * price as today for lengths 1, 3, 4, 5, 7, 8, 11 and 26.
 *
 * ---------------------------------------------------------------------
 * FIELD BOUNDS OF CMD_INIT (one table: this one AND is_command_header in
 * client/gmclient.c). At all three lengths, byte 1 (compression) is 0 or 1
 * and byte 2 (rate code) is 0 to 3, exactly as in the client. The wire
 * carries nothing else: compression is 0 or 1 (see cmd_init_t above), and
 * proto_audio_rate_hz knows only 1, 2 and 3. A CMD_INIT outside these
 * bounds IS NO LONGER A COMMAND: inside a frame it is payload, outside a
 * frame it is a counted orphan (the orphan guard of net.c). The window for
 * a false INIT inside a frame shrinks by about 2,000 (2/256 x 4/256).
 * --------------------------------------------------------------------- */
int proto_is_command_header(const uint8_t *buf, size_t len, int compressed, int padded);

/* Should the audio block in progress be abandoned on seeing this datagram?
 *
 * Returns 1 when the datagram is a command header AND it cannot be the end
 * of the expected audio block.
 *
 * The second condition is not a refinement: without it, an 8-byte audio
 * block tail whose first byte is 0x07 is taken for a BLIT_FIELD_VSYNC
 * header. Measured on the bench: nine blocks lost in four minutes of Chrono
 * Cross, one every twenty-five seconds, each worth sixteen milliseconds of
 * sound. The collision is not unlikely: RetroArch blocks regularly end on a
 * tail of 3, 7, 8 or 11 bytes, and one sound byte in 256 is 0x07.
 *
 * The rule: a datagram whose length is EXACTLY what remains to be received
 * is payload. To fool the rule, a real header arriving at that very moment
 * would have to measure exactly the number of missing bytes, which happens
 * only if an audio datagram was lost AND the shortfall is exactly 1, 3, 4
 * (the 4-byte CMD_INIT), 5, 7, 8, 11 or 26. The cost of that error stays
 * one damaged audio block, never a picture.
 *
 * `compressed` is passed unchanged to proto_is_command_header: the two
 * continuation exits cannot diverge. It extends the list of sensitive
 * lengths above in the same way: +9 outside compression (frame_dup), +12
 * and +13 in a compressed session; see proto_is_command_header for the
 * exact figure of the wider window.
 *
 * `padded` is passed unchanged for the same reason: an audio block and a
 * video frame can never be active at the same time, but the guard that
 * tells them apart must stay IDENTICAL on the video and audio sides. The
 * residual cost of padded mode is bounded and known: a real 8-byte padded
 * header that arrives during an audio block with EXACTLY 8 bytes left to
 * receive is classed as payload by the second condition below, which does
 * not change. Cost: one damaged audio block, never a picture; see
 * proto_is_command_header for the full table and arithmetic. */
int proto_audio_escape(const uint8_t *buf, size_t len,
                       uint32_t expected, uint32_t received, int compressed,
                       int padded);

/* Should the video frame in progress be abandoned on seeing this datagram?
 *
 * `expected` and `received` are those of the slot in flight: in an LZ4
 * session, the COMPRESSED size to reassemble (arm_compression writes it
 * there), not the raw size. Returns:
 *   0 if proto_is_command_header refuses it: a payload chunk;
 *   1 if buf[0] == CMD_INIT: a real CMD_INIT, bounded by the table above,
 *     ALWAYS interrupts a frame in flight, even if it equals exactly what
 *     remains. It is the datagram that opens a session; taking it for
 *     pixels lost its settings, and with them the emitter's re-announce;
 *   1 if received >= expected: inconsistent state, get out;
 *   0 if len == expected - received: the datagram would end the frame to
 *     the byte, so it is the tail;
 *   1 otherwise: a loss is established.
 *
 * Same rule as proto_audio_escape, which proved it on sound. Why "a length
 * different from what remains" means an established loss: a continuation
 * chunk is one MTU, except the tail (gm_next_chunk, libgm). A command shape
 * that does not end the frame exactly is therefore a real command, or the
 * proof that a datagram is already missing.
 *
 * THE CASE THE RULE SWALLOWS, written down on purpose: ONLY the tail of the
 * frame is lost, AND the next datagram is a command shape whose length is
 * exactly that lost tail (the next header: 0x06 at 7 or 11 bytes, 0x07 at
 * 8, 9, 12 or 13; a 3-byte audio header; a 26-byte CMD_SWITCHRES). If more
 * than the tail is missing, what remains is at least 1472 bytes plus the
 * tail, and no command matches it. Raw, the tail is fixed by the geometry
 * (320 x 240: 768 bytes, never swallowed); in LZ4 it varies with every
 * frame; in a padded session, never: no tail goes below 256 bytes there
 * (GM_TAIL_MIN). Accepted cost: the frame that swallows is damaged (raw) or
 * lost (LZ4), then the chunks of the next one arrive without a slot and are
 * dropped as orphans by the guard in net.c: one or two pictures, never the
 * cascade of false commands read from picture bytes that this rule ends. */
int proto_video_escape(const uint8_t *buf, size_t len,
                       uint32_t expected, uint32_t received, int compressed,
                       int padded);

/* Bounds of the pclock of a CMD_SWITCHRES, in MHz as the wire carries it
 * (GroovyMAME: 6.90625 for gridlee, quoted in net.c; our emitter computes
 * pclock_mhz = h_total x v_total x fps / 1e6, groovy_modeline.h).
 *
 * Measured over EVERY CMD_SWITCHRES of the test emitter's suite: from
 * 4.74498 MHz (233 x 238, derived timings) to 24.27168 MHz (1280 x 480,
 * 1544 x 525). The fastest mode a slot can carry (MAX_FRAME_BYTES, 1280
 * wide, 480p at 31 kHz, about 1600 x 525 x 60) is about 50 MHz.
 *
 * Lower bound 1 MHz: a 15 kHz mode goes down at most to h_total ~ 64, about
 * 1 MHz; below that it is not a pclock, it is picture bytes. Upper bound
 * 200 MHz: four times the fastest a slot accepts. A pclock sent in Hz
 * (13,156,000 "MHz", which an old test emitter once sent) is out of bounds:
 * that was a bug in the sender, fixed there, not a reason to widen. */
#define PROTO_PCLOCK_MIN_MHZ   1.0
#define PROTO_PCLOCK_MAX_MHZ 200.0

/* Is a CMD_SWITCHRES of exact shape a plausible geometry? (Rejects
 * "absurd geometries", the shape picture bytes take when read as a
 * command.)
 *
 * Returns 1 if h_active and v_active are non-zero, h_active <= h_begin <=
 * h_end <= h_total, the same vertically, interlace is 0 or 1, and pclock
 * (MHz) is within [PROTO_PCLOCK_MIN_MHZ, PROTO_PCLOCK_MAX_MHZ], NaN and
 * infinities included in the refusal. Otherwise 0: net.c refuses the
 * command, counts it `absurd` and draws NOTHING from it (no expected size,
 * no regime).
 *
 * Does NOT bound the dimensions: the size guard of net.c (MAX_FRAME_BYTES)
 * does, after this one. Of random 26-byte buffers starting with 0x03, more
 * than 99 % are refused (measured by a unit test): the order of the eight
 * positions alone holds only once in 576. */
int proto_switchres_plausible(const cmd_switchres_t *sw);

/* Is the size of a CMD_AUDIO possible for the session?
 *
 * Returns 1 if the session has sound (proto_audio_rate_hz(rate_code) != 0
 * and channels is 1 or 2), if `size` is non-zero, at most
 * AUDIO_MAX_BLOCK_BYTES, and a multiple of 2 x channels (whole S16
 * samples: an odd block would swap left and right for the rest of the
 * session, see pcm.c). Otherwise 0: no block is opened, counted `absurd`.
 *
 * An EMPTY block (size 0) is refused: libgm never sends one (gm_send_audio
 * is only called with at least one frame of sound, record_groovy.c), but
 * GroovyMAME 0.289 can send one (checked). net.c counts it `absurd` without
 * feeding the safeguard: it never makes the receiver blind. */
int proto_audio_size_ok(uint32_t size, uint8_t rate_code, uint8_t channels);

/* Outside a frame (no video slot and no audio block in flight), is this
 * datagram a command to handle? (The orphan guard.)
 *
 * Returns 1 if proto_is_command_header accepts it (EXACT shape from the
 * table of the session's mode). Also returns 1, as the single exception,
 * for an uncompressed 0x07 header LONGER than 9 bytes that
 * parse_cmd_blit_field_vsync_ex accepts: the header and its payload in the
 * same datagram. libgm never sends it (gm_send_blit_field sends the header
 * alone), but the test emitter's pack_blit_payload does, and three bench
 * tests depend on it. Returns 0 otherwise: the datagram is an ORPHAN, net.c
 * counts it and drops it, and never reads it as a command. Same rule as
 * the software client (`stray_payload`, client/gmclient.c).
 *
 * What the guard closes: a 1472-byte picture chunk starting with 0x01, 0x03
 * or 0x04 read as CMD_CLOSE, CMD_SWITCHRES or CMD_AUDIO (parse_dispatch and
 * the parsers accept loose lengths), which used to cascade: a false close,
 * a geometry taken from the picture, white noise. A CMD_INIT outside its
 * field bounds (compression > 1, rate code > 3) is an orphan too: it no
 * longer sets any lock (the old `compression_blocked` is gone from net.c).
 *
 * What it lets through, known: in an UNCOMPRESSED session, an orphan whose
 * first byte is 0x07 has the shape of the exception and opens a slot. It
 * then swallows the following orphans as payload until the next real
 * header interrupts it: one damaged picture at most, never a command. In an
 * LZ4 session (the bench default), the exception does not apply. */
int proto_out_of_frame_accepts(const uint8_t *buf, size_t len, int compressed,
                               int padded);

/* --- The receiver declares itself blind ------------------------------------
 *
 * Two pure decisions, with the clock passed as an argument (milliseconds,
 * any monotonic clock): net.c calls them, it does not copy them. Their
 * state lives in struct net_internal (net.c), NEVER in struct
 * net_blit_shared nor in struct frame_slot (those two are defined twice, in
 * net.h and blit.c, and must not grow). A zeroed structure ({0}) is the
 * starting state; assigning a zeroed structure is the only reset: net.c
 * does it on entering blindness.
 *
 * proto_sight: has the daemon lost the picture? A REJECT is a frame whose
 * slot is lost or unreadable against the expected geometry
 * (continuation_escape or frame_id_mismatch of a slot that had received
 * bytes, lz4_decompress_failed, lz4_size_rejected, and, raw, an episode of
 * orphans following a "complete" frame: the expected size was smaller than
 * the real one). A GOOD frame: LZ4 decompression to the exact size, or, raw,
 * a complete frame followed by an accepted header with no orphan between
 * them. The KEY is the expected raw size and the compression state, read at
 * every slot acquisition.
 *
 * Threshold: at least PROTO_SIGHT_REJECTS_MIN rejects on the same key, spread
 * over at least PROTO_SIGHT_SPAN_MS, with no good frame between them. Why a
 * duration and not a count: on a still scene, the emitter delivers only one
 * full frame per second with frame_dup in between (main.c, --idle-after:
 * "2 at least"). A count alone ("20 rejects") would never trigger there;
 * two rejects one second apart trigger on the second full frame. On a
 * moving scene: 60 rejects per second, triggering 1000 ms after the first
 * reject. 1000 ms is the "about one second" this rule was given; a single
 * reject (one lost datagram) is never enough, and a good frame
 * between two rejects restarts the run: a healthy session that loses a
 * datagram now and then does not go blind.
 *
 * proto_guard: has the stream turned into garbage? A FIXED one-second
 * window, opened by the first event; PROTO_GUARD_ABSURD_MAX absurd
 * commands (implausible geometry, impossible audio size) or
 * PROTO_GUARD_EPISODES_MAX orphan episodes in the same window, and the
 * daemon declares itself blind, cause `garbage` (mute the sound and wait).
 * Why episodes and not orphans: a single lost header on its own yields a
 * whole frame of orphans (158 at 320 x 240 LZ4, measured) and ONE episode;
 * it must not be enough. Why 3 and 5: a healthy emitter sends no absurd
 * command, and a header lost now and then makes one episode per second at
 * most; three absurd commands or five stream losses in one second are no
 * longer a transport accident. Absurd commands and episodes are counted
 * separately.
 *
 * A clock that goes backwards never triggers: it reopens the run or the
 * window at the time read.
 *
 * Blind status: while the daemon is blind, it sends a status with
 * frame_echo = 0 at least every PROTO_BLIND_STATUS_MS, the first one on
 * entry, modelled on maybe_status_nogeom in the software client
 * (client/gmclient.c). The wake-up of the network thread (100 ms at most)
 * is included in that period: net.c sends from 150 ms on, so that the gap
 * stays under 250 ms even in silence. Our emitter reads it
 * (groovy_reannounce_should_fire) and announces the session again after
 * one second of blindness. */
#define PROTO_SIGHT_REJECTS_MIN      2u
#define PROTO_SIGHT_SPAN_MS        1000u
#define PROTO_BLIND_STATUS_MS   250u
#define PROTO_GUARD_WINDOW_MS    1000u
#define PROTO_GUARD_ABSURD_MAX  3u
#define PROTO_GUARD_EPISODES_MAX  5u

struct proto_sight {
    uint32_t key_bytes;       /* expected raw size */
    uint8_t  key_compressed;  /* compression state */
    uint8_t  key_set;         /* 0 until a key has been read */
    uint32_t reject_count;    /* rejects of the current run */
    uint64_t first_ms;        /* time of the first reject of the run */
};

/* Reads the key of a slot just acquired. A key different from the previous
 * one restarts the run; the same key changes nothing. */
void proto_sight_key(struct proto_sight *v, uint32_t plain_bytes, int is_lz4);

/* A reject at time now_ms. Returns 1 when it is time to declare blindness
 * (at least PROTO_SIGHT_REJECTS_MIN rejects, the first one at least
 * PROTO_SIGHT_SPAN_MS ago), 0 otherwise. Does not reset itself when it
 * returns 1: the caller reads `reject_count` and `first_ms` for its log line,
 * then zeroes the structure. */
int proto_sight_reject(struct proto_sight *v, uint64_t now_ms);

/* A good frame: the run starts again from zero. The key is kept. */
void proto_sight_good(struct proto_sight *v);

struct proto_guard {
    uint64_t start_ms;       /* opening of the window */
    uint32_t absurd_count;
    uint32_t episodes;
    uint8_t  is_open;
};

/* An absurd command, the start of an orphan episode. They return 1 when the
 * counter reaches its threshold within the current window, 0 otherwise.
 * Like proto_sight_reject, they do not reset themselves when they return 1. */
int proto_guard_absurd(struct proto_guard *g, uint64_t now_ms);
int proto_guard_episode(struct proto_guard *g, uint64_t now_ms);

/* The session-mode state machine.
 *
 * `padded`: 0 = classic session, 1 = padded session. It lives in struct
 * net_internal on the daemon side (net.c), and on the stack in tests;
 * NEVER in struct net_blit_shared nor struct frame_slot (see above).
 *
 * Pure: no socket, no allocation, no global state. That is what lets a
 * unit test play them on any host, and that test is the ONLY EXECUTED
 * proof of this path: a run on the real tube stays in a classic session,
 * so it does not exercise it.
 */

/* CMD_INIT. `r` is the verdict of parse_cmd_init on THIS datagram, `len` its
 * length. A refused INIT (length outside {4,5,8}, non-zero tail, compression
 * out of bounds) changes NOTHING: neither the compression nor the mode.
 * Returns 1 if the mode CHANGED; net.c logs padding_announced on that
 * condition, and on it alone, like compression_announced.
 *
 * The behaviour on a change of mode, WRITTEN DOWN BEFORE being tested: the
 * LAST CMD_INIT wins, the log says so, nothing is refused; strictly
 * symmetrical with `compression`. Three facts make it tenable, and its limit
 * is real:
 *   - our emitter CANNOT change mode within a session: the mode lives on
 *     the handle, and the re-announce sends again on the same handle (pinned
 *     by a test);
 *   - `padded`, like `compression`, is a state PER NETWORK THREAD, not PER
 *     PEER: with two emitters of different modes aimed at the same daemon,
 *     the last CMD_INIT governs both. That is already true of compression
 *     today; this does not make it worse and does not fix it;
 *   - a change of mode invalidates the frame and the audio block in flight:
 *     nothing special to do, the existing escape guards handle it on the
 *     next datagram. */
int proto_session_on_init(uint8_t *padded, parse_result_t r, size_t len);

/* CMD_CLOSE: a new session does not inherit the mode of the previous one.
 * Returns 1 if the mode changed. */
int proto_session_on_close(uint8_t *padded);

/* Rate in Hz carried by the CMD_INIT code.
 *   0 = off, 1 = 22050, 2 = 44100, 3 = 48000
 * Returns 0 for 0 and for any unknown code: an unknown code means the
 * emitter speaks a version of the protocol we do not know, and staying
 * silent is the right answer. Exact mirror of groovy_audio_rate_hz on the
 * emitter side. */
unsigned proto_audio_rate_hz(uint8_t code);

/* Largest audio block: the size on the wire is a u16. At 44100 Hz, 16-bit
 * stereo, one video frame is worth 2940 bytes: the margin is twentyfold,
 * and the reassembly buffer is sized once and for all on this maximum
 * rather than following the announced size. */
#define AUDIO_MAX_BLOCK_BYTES 65535U

/* Frame reassembly state. expected_bytes = h_active * v_active * 3 (RGB888).
 * No explicit slice-count field: count payload bytes against expected. */
struct frame_reassembly {
    uint32_t frame_id;
    uint32_t expected_bytes;
    uint32_t received_bytes;
    uint8_t *buf;
};

/* Push one BLIT payload chunk.
 *
 * Returns 1 if the frame is complete after this push, 0 otherwise.
 *
 * Robust to misbehaving inputs: NULL pointers, zero-length payloads, and
 * over-long payloads (more bytes than `expected_bytes - received_bytes`)
 * all return 0 / 1 without overflowing the destination buffer. Excess
 * bytes from an emitter that overshoots are silently clamped; a frame_id
 * mismatch must be detected by the caller before calling this function
 * (net.c checks it). The function never returns a negative
 * value: earlier doc-strings claimed -1 on overflow, but the
 * implementation has always defended by clamping. */
int proto_reassemble_push(struct frame_reassembly *f,
                          const uint8_t *payload, size_t len);

/* Field-aware reassembly wrapper for CMD_BLIT_FIELD_VSYNC (0x07).
 *
 * Writes payload bytes into a single 480-row framebuffer at:
 *   - even rows (0, 2, 4, ..., fb_h-2) when field == 0
 *   - odd  rows (1, 3, 5, ..., fb_h-1) when field == 1
 * The radeon kernel module's DRM_MODE_FLAG_INTERLACE plumbing alternates
 * scan-out from this single buffer by itself.
 *
 * Reuses struct frame_reassembly verbatim: the caller sets expected_bytes
 * to the per-field size (fb_w * (fb_h/2) * 3) and resets received_bytes to
 * 0 between field 0 and field 1 (see the CMD_BLIT_FIELD_VSYNC handler in
 * net.c). Returns 1 when the field is complete, 0 otherwise.
 *
 * Rejects field > 1 (returns 0 with no copy).
 */
int proto_reassemble_push_field(struct frame_reassembly *f,
                                uint8_t field, uint32_t fb_w,
                                const uint8_t *payload, size_t len);

#endif /* RECEIVER_PROTO_H */
