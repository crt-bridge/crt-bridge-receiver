/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * lz4dec.h -- LZ4 decompression of frames, and the guards that go with it.
 *
 * The Groovy protocol carries LZ4 in **block format**, not frame format: one
 * block per BLIT header, whose compressed size is announced in the header and
 * whose plain size is known in advance (h_active * v_active * 3). That is the
 * simplest case of LZ4_decompress_safe.
 *
 * This module exists for one reason only: **the compressed size comes from
 * the network**. A broken or hostile emitter can announce anything, while the
 * staging buffer that receives the reassembled bytes is sized on a computed
 * bound. The guards therefore live here, isolated and testable, rather than
 * scattered through the hot path of net.c.
 *
 * Format authority, checked on 2026-08-31 against psakhis/Groovy_MiSTer:
 *   - api/groovymister.cpp, in the blit path --
 *     LZ4_compress_default(src, dst, RGBSize, RGBSize): one block per field,
 *     no frame format, no dictionary chaining;
 *   - hps_linux/src/support/groovy/groovy.cpp -- the compressed size is read
 *     as a little-endian u32 from the header.
 */
#ifndef RECEIVER_LZ4DEC_H
#define RECEIVER_LZ4DEC_H

#include <stddef.h>
#include <stdint.h>

/* Upper bound of what an honest LZ4 compression can produce for `plain_len`
 * plain bytes -- an all-literal block costs a little more than its content.
 * This is the size to give the staging buffer.
 *
 * Returns 0 if `plain_len` is zero or large enough for the computation to
 * overflow. */
size_t lz4dec_bound(uint32_t plain_len);

/* Is the size announced on the wire plausible for `plain_len` plain bytes?
 * Returns 1 if so, 0 otherwise.
 *
 * Refuses zero (nothing to decompress is not a frame) and anything above the
 * bound -- beyond it, this is not compression, it is an overflow. */
int lz4dec_size_ok(uint32_t announced_len, uint32_t plain_len);

/* Decompresses one block.
 *
 * Returns the number of bytes written to `dst`, or -1 on failure -- corrupt
 * input, output larger than `dst_cap`, or an absurd argument. A failure
 * writes nothing usable: the caller must drop the frame.
 *
 * NEVER writes beyond dst_cap: that is the guarantee of LZ4_decompress_safe,
 * and the reason this function is used and no other. */
int lz4dec_block(const uint8_t *src, uint32_t src_len,
                 uint8_t *dst, uint32_t dst_cap);

#endif /* RECEIVER_LZ4DEC_H */
