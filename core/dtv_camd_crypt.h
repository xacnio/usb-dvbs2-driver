/* Cryptographic primitives the CAM protocols need.
 *
 * newcamd authenticates with two-key triple DES in CBC mode and the MD5
 * crypt(3) ($1$) hash. Both are small and fully specified, so they are
 * implemented here from FIPS 46-3 and RFC 1321 rather than pulling in
 * OpenSSL, which would be the project's only crypto dependency and has to
 * move to Android later. Verified in tests/camd_crypt_test.c. */
#ifndef DTV_CAMD_CRYPT_H
#define DTV_CAMD_CRYPT_H

#include <stddef.h>
#include <stdint.h>

/* ---- DES ---------------------------------------------------------- */

typedef struct dtv_des_key {
    /* One 48-bit subkey per round, kept as six bytes each. */
    uint8_t subkey[16][6];
} dtv_des_key;

void dtv_des_set_key(dtv_des_key *schedule, const uint8_t key[8]);
void dtv_des_encrypt_block(const dtv_des_key *schedule, const uint8_t in[8],
                           uint8_t out[8]);
void dtv_des_decrypt_block(const dtv_des_key *schedule, const uint8_t in[8],
                           uint8_t out[8]);

/* DES ignores the low bit of every key byte; it carries odd parity. Key
 * material derived by shifting bits (as newcamd does) needs it fixed. */
void dtv_des_set_odd_parity(uint8_t *key, size_t size);

/* Two-key triple DES in CBC mode: E(k1) D(k2) E(k1), the OpenSSL
 * DES_ede2_cbc_encrypt() newcamd implementations call. size must be a
 * multiple of 8; iv is updated in place. Works in place (in == out). */
void dtv_des_ede2_cbc(const uint8_t key1[8], const uint8_t key2[8],
                      uint8_t iv[8], const uint8_t *in, uint8_t *out,
                      size_t size, int encrypt);

/* ---- MD5 ---------------------------------------------------------- */

typedef struct dtv_md5_ctx {
    uint32_t state[4];
    uint64_t count;      /* message length in bytes */
    uint8_t buffer[64];
} dtv_md5_ctx;

void dtv_md5_init(dtv_md5_ctx *ctx);
void dtv_md5_update(dtv_md5_ctx *ctx, const void *data, size_t size);
void dtv_md5_final(dtv_md5_ctx *ctx, uint8_t digest[16]);
void dtv_md5(const void *data, size_t size, uint8_t digest[16]);

/* crypt(3) with the $1$ (MD5) scheme. salt may include or omit the leading
 * $1$ and trailing $ -- newcamd always uses $1$abcdefgh$. Returns output,
 * the complete crypt string, which needs DTV_MD5_CRYPT_SIZE bytes. */
#define DTV_MD5_CRYPT_SIZE 64
char *dtv_md5_crypt(const char *password, const char *salt, char *output,
                    size_t output_size);

/* ---- AES-128 and CRC-32 ------------------------------------------- */

/* cs378x (camd35 over TCP) encrypts every message with AES-128 in ECB mode
 * keyed with the MD5 of the password, and authenticates it with a CRC-32
 * of the payload. Single blocks at a time; that protocol has no chaining. */

typedef struct dtv_aes_key {
    /* 11 round keys of four words; decryption walks the same schedule
     * backwards (the FIPS-197 inverse cipher). */
    uint32_t round_key[44];
} dtv_aes_key;

void dtv_aes128_set_key(dtv_aes_key *schedule, const uint8_t key[16]);
void dtv_aes128_encrypt_block(const dtv_aes_key *schedule,
                              const uint8_t in[16], uint8_t out[16]);
void dtv_aes128_decrypt_block(const dtv_aes_key *schedule,
                              const uint8_t in[16], uint8_t out[16]);

/* The usual reflected CRC-32 (zlib's), which is what camd35 sends. */
uint32_t dtv_crc32(uint32_t seed, const void *data, size_t size);

#endif
