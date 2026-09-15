/* Hand-written replacement for the autoconf-generated config.h.
 *
 * libdvbcsa upstream configures itself with autotools; the project builds
 * with CMake and only needs one fixed configuration, so the handful of
 * macros the sources actually read are written out here instead of running
 * a configure pass.
 *
 * The bitslice word size is the important one: DVBCSA_USE_SSE selects the
 * 128-bit SSE2 implementation (dvbcsa_bs_sse.h, 128 packets per batch) and
 * therefore also decides that dvbcsa_bs_transpose128.c is the transpose
 * unit that gets compiled. x86-64 always has SSE2, so no runtime check is
 * needed. Changing this line means changing the transpose source in
 * CMakeLists.txt as well:
 *
 *   DVBCSA_USE_SSE    -> dvbcsa_bs_transpose128.c
 *   DVBCSA_USE_UINT64 -> dvbcsa_bs_transpose64.c
 *   DVBCSA_USE_UINT32 -> dvbcsa_bs_transpose32.c
 */
#ifndef DVBCSA_CONFIG_H_
#define DVBCSA_CONFIG_H_

#define STDC_HEADERS 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_ASSERT_H 1

/* x86 and x86-64 take the SSE2 word. Every other processor takes the same
 * 128-bit word in plain C (dvbcsa_bs_portable128.h, added in this copy), so
 * dvbcsa_bs_transpose128.c stays the transpose unit on all of them.
 * DVBCSA_FORCE_PORTABLE128 selects it on x86 too, to test it there. */
#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64)) && \
    !defined(DVBCSA_FORCE_PORTABLE128)
/* _mm_malloc()/_mm_free() come with the x86 intrinsics headers; the
 * bitslice key needs its buffers aligned to the word size. */
#define HAVE_MM_MALLOC 1
#define DVBCSA_USE_SSE 1
#else
#if !defined(_WIN32)
#define HAVE_POSIX_MEMALIGN 1
#endif
#define DVBCSA_USE_PORTABLE128 1
#endif

#endif
