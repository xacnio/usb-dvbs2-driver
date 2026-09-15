/*

    This file is part of the usb-dvbs2-driver copy of libdvbcsa.

    libdvbcsa is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published
    by the Free Software Foundation; either version 2 of the License,
    or (at your option) any later version.

    The 128-bit bitslice word of dvbcsa_bs_sse.h, written in plain C for the
    processors without SSE2 (ARM, among them Apple silicon and the Raspberry
    Pi). It is the same word, byte for byte on a little-endian machine, so
    the SSE2 sources -- the batch size, dvbcsa_bs_transpose128.c and
    BS_EXTRACT8 -- work on it unchanged. The compiler vectorises most of it.

*/

#ifndef DVBCSA_PORTABLE128_H_
#define DVBCSA_PORTABLE128_H_

#include <stdint.h>

/* Low word first: byte 0 of the struct is byte 0 of the 128-bit value, as
 * in an __m128i on a little-endian machine. */
typedef struct
{
  uint64_t lo;
  uint64_t hi;
} dvbcsa_bs_word_t;

#define BS_BATCH_SIZE 128
#define BS_BATCH_BYTES 16

static inline dvbcsa_bs_word_t dvbcsa_bs_val(uint64_t hi, uint64_t lo)
{
  dvbcsa_bs_word_t w;
  w.lo = lo;
  w.hi = hi;
  return w;
}

static inline dvbcsa_bs_word_t dvbcsa_bs_and(dvbcsa_bs_word_t a, dvbcsa_bs_word_t b)
{
  return dvbcsa_bs_val(a.hi & b.hi, a.lo & b.lo);
}

static inline dvbcsa_bs_word_t dvbcsa_bs_or(dvbcsa_bs_word_t a, dvbcsa_bs_word_t b)
{
  return dvbcsa_bs_val(a.hi | b.hi, a.lo | b.lo);
}

static inline dvbcsa_bs_word_t dvbcsa_bs_xor(dvbcsa_bs_word_t a, dvbcsa_bs_word_t b)
{
  return dvbcsa_bs_val(a.hi ^ b.hi, a.lo ^ b.lo);
}

static inline dvbcsa_bs_word_t dvbcsa_bs_not(dvbcsa_bs_word_t a)
{
  return dvbcsa_bs_val(~a.hi, ~a.lo);
}

/* _mm_slli_epi64 / _mm_srli_epi64: each 64-bit half on its own. */
static inline dvbcsa_bs_word_t dvbcsa_bs_shl(dvbcsa_bs_word_t a, unsigned n)
{
  return n >= 64 ? dvbcsa_bs_val(0, 0) : dvbcsa_bs_val(a.hi << n, a.lo << n);
}

static inline dvbcsa_bs_word_t dvbcsa_bs_shr(dvbcsa_bs_word_t a, unsigned n)
{
  return n >= 64 ? dvbcsa_bs_val(0, 0) : dvbcsa_bs_val(a.hi >> n, a.lo >> n);
}

/* _mm_slli_si128 / _mm_srli_si128: the whole 128 bits, by n bytes. */
static inline dvbcsa_bs_word_t dvbcsa_bs_shl8(dvbcsa_bs_word_t a, unsigned n)
{
  unsigned bits = n * 8;
  if (bits == 0)
    return a;
  if (bits >= 128)
    return dvbcsa_bs_val(0, 0);
  if (bits >= 64)
    return dvbcsa_bs_val(a.lo << (bits - 64), 0);
  return dvbcsa_bs_val((a.hi << bits) | (a.lo >> (64 - bits)), a.lo << bits);
}

static inline dvbcsa_bs_word_t dvbcsa_bs_shr8(dvbcsa_bs_word_t a, unsigned n)
{
  unsigned bits = n * 8;
  if (bits == 0)
    return a;
  if (bits >= 128)
    return dvbcsa_bs_val(0, 0);
  if (bits >= 64)
    return dvbcsa_bs_val(0, a.hi >> (bits - 64));
  return dvbcsa_bs_val(a.hi >> bits, (a.lo >> bits) | (a.hi << (64 - bits)));
}

#define BS_VAL(n, m)	dvbcsa_bs_val((uint64_t)(n), (uint64_t)(m))
#define BS_VAL64(n)	BS_VAL(0x##n##ULL, 0x##n##ULL)
#define BS_VAL32(n)	BS_VAL64(n##n)
#define BS_VAL16(n)	BS_VAL32(n##n)
#define BS_VAL8(n)	BS_VAL16(n##n)

#define BS_AND(a, b)	dvbcsa_bs_and((a), (b))
#define BS_OR(a, b)	dvbcsa_bs_or((a), (b))
#define BS_XOR(a, b)	dvbcsa_bs_xor((a), (b))
#define BS_NOT(a)	dvbcsa_bs_not(a)

#define BS_SHL(a, n)	dvbcsa_bs_shl((a), (n))
#define BS_SHR(a, n)	dvbcsa_bs_shr((a), (n))
#define BS_SHL8(a, n)	dvbcsa_bs_shl8((a), (n))
#define BS_SHR8(a, n)	dvbcsa_bs_shr8((a), (n))

#define BS_EXTRACT8(a, n) ((uint8_t*)&(a))[n]

#define BS_EMPTY()

#endif
