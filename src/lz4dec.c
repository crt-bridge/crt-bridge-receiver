/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * lz4dec.c -- see lz4dec.h for the why.
 *
 * liblz4 1.10.0 is already in the receiver's PXE image -- checked on
 * 2026-08-31, /usr/lib/liblz4.so.1.10.0 with its development symlink.
 * Neither an image rebuild nor a static link is needed: -llz4 is enough.
 */
#include "lz4dec.h"

#include <lz4.h>

size_t lz4dec_bound(uint32_t plain_len)
{
    if (plain_len == 0u)
        return 0u;
    /* LZ4_compressBound returns 0 above LZ4_MAX_INPUT_SIZE: pass it on. */
    int b = LZ4_compressBound((int)plain_len);
    return (b > 0) ? (size_t)b : 0u;
}

int lz4dec_size_ok(uint32_t announced_len, uint32_t plain_len)
{
    if (announced_len == 0u || plain_len == 0u)
        return 0;
    size_t bound_len = lz4dec_bound(plain_len);
    if (bound_len == 0u)
        return 0;
    return ((size_t)announced_len <= bound_len) ? 1 : 0;
}

int lz4dec_block(const uint8_t *src, uint32_t src_len,
                 uint8_t *dst, uint32_t dst_cap)
{
    if (!src || !dst || src_len == 0u || dst_cap == 0u)
        return -1;
    /* LZ4_decompress_safe takes ints: above INT_MAX, refuse rather than
     * truncate silently. The receiver's frames are very far from that. */
    if (src_len > (uint32_t)0x7FFFFFFF || dst_cap > (uint32_t)0x7FFFFFFF)
        return -1;

    /* The "safe" function is the only acceptable one here: the input comes
     * from the network. It bounds its writes to dst_cap whatever the block
     * contains. */
    int n = LZ4_decompress_safe((const char *)src, (char *)dst,
                                (int)src_len, (int)dst_cap);
    return (n > 0) ? n : -1;
}
