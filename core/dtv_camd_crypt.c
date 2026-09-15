/* DES, triple DES CBC and MD5/crypt(3) for the CAM protocols. See
 * dtv_camd_crypt.h for why they are here rather than linked in. The DES is
 * the textbook bit-permutation one: it runs on login messages and ECM
 * requests only, so clarity beats speed. The cipher that has to keep up
 * with the stream is DVB-CSA, in libdvbcsa. */
#include "dtv_camd_crypt.h"

#include <string.h>

/* ---- DES tables (FIPS 46-3) ---------------------------------------- */

/* All tables are 1-based bit positions as printed in the standard, and
 * are converted on use, so they stay checkable against the document. */

static const uint8_t des_ip[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17,  9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7
};

static const uint8_t des_fp[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41,  9, 49, 17, 57, 25
};

static const uint8_t des_e[48] = {
    32,  1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,
     8,  9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32,  1
};

static const uint8_t des_p[32] = {
    16,  7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
     2,  8, 24, 14, 32, 27,  3,  9, 19, 13, 30,  6, 22, 11,  4, 25
};

static const uint8_t des_pc1[56] = {
    57, 49, 41, 33, 25, 17,  9,  1, 58, 50, 42, 34, 26, 18,
    10,  2, 59, 51, 43, 35, 27, 19, 11,  3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15,  7, 62, 54, 46, 38, 30, 22,
    14,  6, 61, 53, 45, 37, 29, 21, 13,  5, 28, 20, 12,  4
};

static const uint8_t des_pc2[48] = {
    14, 17, 11, 24,  1,  5,  3, 28, 15,  6, 21, 10,
    23, 19, 12,  4, 26,  8, 16,  7, 27, 20, 13,  2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32
};

static const uint8_t des_shifts[16] = {
    1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};

static const uint8_t des_sbox[8][64] = {
    { 14,  4, 13,  1,  2, 15, 11,  8,  3, 10,  6, 12,  5,  9,  0,  7,
       0, 15,  7,  4, 14,  2, 13,  1, 10,  6, 12, 11,  9,  5,  3,  8,
       4,  1, 14,  8, 13,  6,  2, 11, 15, 12,  9,  7,  3, 10,  5,  0,
      15, 12,  8,  2,  4,  9,  1,  7,  5, 11,  3, 14, 10,  0,  6, 13 },
    { 15,  1,  8, 14,  6, 11,  3,  4,  9,  7,  2, 13, 12,  0,  5, 10,
       3, 13,  4,  7, 15,  2,  8, 14, 12,  0,  1, 10,  6,  9, 11,  5,
       0, 14,  7, 11, 10,  4, 13,  1,  5,  8, 12,  6,  9,  3,  2, 15,
      13,  8, 10,  1,  3, 15,  4,  2, 11,  6,  7, 12,  0,  5, 14,  9 },
    { 10,  0,  9, 14,  6,  3, 15,  5,  1, 13, 12,  7, 11,  4,  2,  8,
      13,  7,  0,  9,  3,  4,  6, 10,  2,  8,  5, 14, 12, 11, 15,  1,
      13,  6,  4,  9,  8, 15,  3,  0, 11,  1,  2, 12,  5, 10, 14,  7,
       1, 10, 13,  0,  6,  9,  8,  7,  4, 15, 14,  3, 11,  5,  2, 12 },
    {  7, 13, 14,  3,  0,  6,  9, 10,  1,  2,  8,  5, 11, 12,  4, 15,
      13,  8, 11,  5,  6, 15,  0,  3,  4,  7,  2, 12,  1, 10, 14,  9,
      10,  6,  9,  0, 12, 11,  7, 13, 15,  1,  3, 14,  5,  2,  8,  4,
       3, 15,  0,  6, 10,  1, 13,  8,  9,  4,  5, 11, 12,  7,  2, 14 },
    {  2, 12,  4,  1,  7, 10, 11,  6,  8,  5,  3, 15, 13,  0, 14,  9,
      14, 11,  2, 12,  4,  7, 13,  1,  5,  0, 15, 10,  3,  9,  8,  6,
       4,  2,  1, 11, 10, 13,  7,  8, 15,  9, 12,  5,  6,  3,  0, 14,
      11,  8, 12,  7,  1, 14,  2, 13,  6, 15,  0,  9, 10,  4,  5,  3 },
    { 12,  1, 10, 15,  9,  2,  6,  8,  0, 13,  3,  4, 14,  7,  5, 11,
      10, 15,  4,  2,  7, 12,  9,  5,  6,  1, 13, 14,  0, 11,  3,  8,
       9, 14, 15,  5,  2,  8, 12,  3,  7,  0,  4, 10,  1, 13, 11,  6,
       4,  3,  2, 12,  9,  5, 15, 10, 11, 14,  1,  7,  6,  0,  8, 13 },
    {  4, 11,  2, 14, 15,  0,  8, 13,  3, 12,  9,  7,  5, 10,  6,  1,
      13,  0, 11,  7,  4,  9,  1, 10, 14,  3,  5, 12,  2, 15,  8,  6,
       1,  4, 11, 13, 12,  3,  7, 14, 10, 15,  6,  8,  0,  5,  9,  2,
       6, 11, 13,  8,  1,  4, 10,  7,  9,  5,  0, 15, 14,  2,  3, 12 },
    { 13,  2,  8,  4,  6, 15, 11,  1, 10,  9,  3, 14,  5,  0, 12,  7,
       1, 15, 13,  8, 10,  3,  7,  4, 12,  5,  6, 11,  0, 14,  9,  2,
       7, 11,  4,  1,  9, 12, 14,  2,  0,  6, 10, 13, 15,  3,  5,  8,
       2,  1, 14,  7,  4, 10,  8, 13, 15, 12,  9,  0,  3,  5,  6, 11 }
};

/* ---- DES bit plumbing ---------------------------------------------- */

static unsigned bit_at(const uint8_t *data, unsigned index)
{
    return (data[index >> 3] >> (7u - (index & 7u))) & 1u;
}

static void bit_set(uint8_t *data, unsigned index, unsigned value)
{
    uint8_t mask = (uint8_t)(0x80u >> (index & 7u));
    if (value)
        data[index >> 3] |= mask;
    else
        data[index >> 3] &= (uint8_t)~mask;
}

/* table holds 1-based source positions, as in the standard. */
static void permute(uint8_t *out, const uint8_t *in, const uint8_t *table,
                    unsigned bits)
{
    unsigned i;
    memset(out, 0, (bits + 7u) / 8u);
    for (i = 0; i < bits; ++i)
        bit_set(out, i, bit_at(in, (unsigned)table[i] - 1u));
}

/* Rotates a 28-bit half of the key schedule left. The halves are stored
 * left-aligned in four bytes, so the four spare low bits stay zero. */
static void rotate28(uint8_t *half, unsigned count)
{
    unsigned i, j;
    for (j = 0; j < count; ++j) {
        unsigned first = bit_at(half, 0);
        for (i = 0; i < 27u; ++i)
            bit_set(half, i, bit_at(half, i + 1u));
        bit_set(half, 27u, first);
    }
}

void dtv_des_set_key(dtv_des_key *schedule, const uint8_t key[8])
{
    uint8_t permuted[7];
    uint8_t c[4], d[4], cd[7];
    unsigned round, i;

    permute(permuted, key, des_pc1, 56);
    memset(c, 0, sizeof(c));
    memset(d, 0, sizeof(d));
    for (i = 0; i < 28u; ++i) {
        bit_set(c, i, bit_at(permuted, i));
        bit_set(d, i, bit_at(permuted, i + 28u));
    }

    for (round = 0; round < 16u; ++round) {
        rotate28(c, des_shifts[round]);
        rotate28(d, des_shifts[round]);
        memset(cd, 0, sizeof(cd));
        for (i = 0; i < 28u; ++i) {
            bit_set(cd, i, bit_at(c, i));
            bit_set(cd, i + 28u, bit_at(d, i));
        }
        permute(schedule->subkey[round], cd, des_pc2, 48);
    }
}

/* Feistel function: expand R to 48 bits, mix in the subkey, fold back to
 * 32 bits through the S-boxes and permute. */
static void des_f(const uint8_t right[4], const uint8_t subkey[6],
                  uint8_t out[4])
{
    uint8_t expanded[6], substituted[4];
    unsigned i;

    permute(expanded, right, des_e, 48);
    for (i = 0; i < 6u; ++i)
        expanded[i] ^= subkey[i];

    memset(substituted, 0, sizeof(substituted));
    for (i = 0; i < 8u; ++i) {
        unsigned base = i * 6u;
        unsigned row = (bit_at(expanded, base) << 1) |
                       bit_at(expanded, base + 5u);
        unsigned column = (bit_at(expanded, base + 1u) << 3) |
                          (bit_at(expanded, base + 2u) << 2) |
                          (bit_at(expanded, base + 3u) << 1) |
                          bit_at(expanded, base + 4u);
        unsigned value = des_sbox[i][row * 16u + column];
        unsigned bit;
        for (bit = 0; bit < 4u; ++bit)
            bit_set(substituted, i * 4u + bit,
                    (value >> (3u - bit)) & 1u);
    }

    permute(out, substituted, des_p, 32);
}

static void des_crypt_block(const dtv_des_key *schedule, const uint8_t in[8],
                            uint8_t out[8], int encrypt)
{
    uint8_t block[8], left[4], right[4], f_out[4], preoutput[8];
    unsigned round, i;

    permute(block, in, des_ip, 64);
    memcpy(left, block, 4);
    memcpy(right, block + 4, 4);

    for (round = 0; round < 16u; ++round) {
        const uint8_t *subkey =
            schedule->subkey[encrypt ? round : 15u - round];
        uint8_t previous_right[4];
        memcpy(previous_right, right, 4);
        des_f(right, subkey, f_out);
        for (i = 0; i < 4u; ++i)
            right[i] = (uint8_t)(left[i] ^ f_out[i]);
        memcpy(left, previous_right, 4);
    }

    /* The halves are swapped once more before the final permutation. */
    memcpy(preoutput, right, 4);
    memcpy(preoutput + 4, left, 4);
    permute(out, preoutput, des_fp, 64);
}

void dtv_des_encrypt_block(const dtv_des_key *schedule, const uint8_t in[8],
                           uint8_t out[8])
{
    des_crypt_block(schedule, in, out, 1);
}

void dtv_des_decrypt_block(const dtv_des_key *schedule, const uint8_t in[8],
                           uint8_t out[8])
{
    des_crypt_block(schedule, in, out, 0);
}

void dtv_des_set_odd_parity(uint8_t *key, size_t size)
{
    size_t i;
    for (i = 0; i < size; ++i) {
        uint8_t value = (uint8_t)(key[i] & 0xfeu);
        unsigned ones = 0, bit;
        for (bit = 1; bit < 8u; ++bit)
            ones += (value >> bit) & 1u;
        key[i] = (uint8_t)(value | ((ones & 1u) ? 0u : 1u));
    }
}

void dtv_des_ede2_cbc(const uint8_t key1[8], const uint8_t key2[8],
                      uint8_t iv[8], const uint8_t *in, uint8_t *out,
                      size_t size, int encrypt)
{
    dtv_des_key schedule1, schedule2;
    uint8_t chain[8];
    size_t offset;

    dtv_des_set_key(&schedule1, key1);
    dtv_des_set_key(&schedule2, key2);
    memcpy(chain, iv, 8);

    for (offset = 0; offset + 8u <= size; offset += 8u) {
        uint8_t block[8], temp[8];
        unsigned i;
        if (encrypt) {
            for (i = 0; i < 8u; ++i)
                block[i] = (uint8_t)(in[offset + i] ^ chain[i]);
            dtv_des_encrypt_block(&schedule1, block, temp);
            dtv_des_decrypt_block(&schedule2, temp, block);
            dtv_des_encrypt_block(&schedule1, block, temp);
            memcpy(chain, temp, 8);
            memcpy(out + offset, temp, 8);
        } else {
            /* The ciphertext block is the next chaining value and in may
             * alias out, so keep a copy. */
            uint8_t cipher[8];
            memcpy(cipher, in + offset, 8);
            dtv_des_decrypt_block(&schedule1, cipher, temp);
            dtv_des_encrypt_block(&schedule2, temp, block);
            dtv_des_decrypt_block(&schedule1, block, temp);
            for (i = 0; i < 8u; ++i)
                out[offset + i] = (uint8_t)(temp[i] ^ chain[i]);
            memcpy(chain, cipher, 8);
        }
    }

    memcpy(iv, chain, 8);
}

/* ---- MD5 (RFC 1321) ------------------------------------------------ */

static const uint32_t md5_k[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

static const uint8_t md5_shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static uint32_t md5_rotate(uint32_t value, unsigned count)
{
    return (value << count) | (value >> (32u - count));
}

static void md5_transform(uint32_t state[4], const uint8_t block[64])
{
    uint32_t m[16], a = state[0], b = state[1], c = state[2], d = state[3];
    unsigned i;

    for (i = 0; i < 16u; ++i)
        m[i] = (uint32_t)block[i * 4u] |
               ((uint32_t)block[i * 4u + 1u] << 8) |
               ((uint32_t)block[i * 4u + 2u] << 16) |
               ((uint32_t)block[i * 4u + 3u] << 24);

    for (i = 0; i < 64u; ++i) {
        uint32_t f;
        unsigned g;
        if (i < 16u)      { f = (b & c) | (~b & d);        g = i; }
        else if (i < 32u) { f = (d & b) | (~d & c);        g = (5u * i + 1u) & 15u; }
        else if (i < 48u) { f = b ^ c ^ d;                 g = (3u * i + 5u) & 15u; }
        else              { f = c ^ (b | ~d);              g = (7u * i) & 15u; }
        f += a + md5_k[i] + m[g];
        a = d;
        d = c;
        c = b;
        b += md5_rotate(f, md5_shift[i]);
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void dtv_md5_init(dtv_md5_ctx *ctx)
{
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xefcdab89u;
    ctx->state[2] = 0x98badcfeu;
    ctx->state[3] = 0x10325476u;
    ctx->count = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

void dtv_md5_update(dtv_md5_ctx *ctx, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t used = (size_t)(ctx->count & 63u);
    ctx->count += size;
    if (used) {
        size_t space = 64u - used;
        if (size < space) {
            memcpy(ctx->buffer + used, bytes, size);
            return;
        }
        memcpy(ctx->buffer + used, bytes, space);
        md5_transform(ctx->state, ctx->buffer);
        bytes += space;
        size -= space;
    }
    while (size >= 64u) {
        md5_transform(ctx->state, bytes);
        bytes += 64u;
        size -= 64u;
    }
    memcpy(ctx->buffer, bytes, size);
}

void dtv_md5_final(dtv_md5_ctx *ctx, uint8_t digest[16])
{
    uint64_t bits = ctx->count * 8u;
    size_t used = (size_t)(ctx->count & 63u);
    uint8_t tail[72];
    size_t pad = (used < 56u) ? (56u - used) : (120u - used);
    unsigned i;

    memset(tail, 0, sizeof(tail));
    tail[0] = 0x80u;
    for (i = 0; i < 8u; ++i)
        tail[pad + i] = (uint8_t)(bits >> (8u * i));
    dtv_md5_update(ctx, tail, pad + 8u);

    for (i = 0; i < 4u; ++i) {
        digest[i * 4u]      = (uint8_t)(ctx->state[i]);
        digest[i * 4u + 1u] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4u + 2u] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4u + 3u] = (uint8_t)(ctx->state[i] >> 24);
    }
    memset(ctx, 0, sizeof(*ctx));
}

void dtv_md5(const void *data, size_t size, uint8_t digest[16])
{
    dtv_md5_ctx ctx;
    dtv_md5_init(&ctx);
    dtv_md5_update(&ctx, data, size);
    dtv_md5_final(&ctx, digest);
}

/* ---- crypt(3) "$1$" ------------------------------------------------ */

static const char md5_crypt_alphabet[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/* The hash is emitted as four bit-groups from scattered digest bytes, then
 * the last byte alone -- an ordering the original simply had. */
static char *md5_crypt_encode(const uint8_t digest[16], char *out)
{
    static const uint8_t order[5][3] = {
        { 0, 6, 12 }, { 1, 7, 13 }, { 2, 8, 14 }, { 3, 9, 15 }, { 4, 10, 5 }
    };
    unsigned group, i;
    for (group = 0; group < 5u; ++group) {
        uint32_t value = ((uint32_t)digest[order[group][0]] << 16) |
                         ((uint32_t)digest[order[group][1]] << 8) |
                         digest[order[group][2]];
        for (i = 0; i < 4u; ++i) {
            *out++ = md5_crypt_alphabet[value & 0x3fu];
            value >>= 6;
        }
    }
    {
        uint32_t value = digest[11];
        for (i = 0; i < 2u; ++i) {
            *out++ = md5_crypt_alphabet[value & 0x3fu];
            value >>= 6;
        }
    }
    *out = 0;
    return out;
}

char *dtv_md5_crypt(const char *password, const char *salt, char *output,
                    size_t output_size)
{
    static const char magic[] = "$1$";
    char clean_salt[9];
    size_t salt_length = 0, password_length, i;
    dtv_md5_ctx ctx, alt_ctx;
    uint8_t digest[16];
    char *p;

    if (!password || !salt || !output || output_size < DTV_MD5_CRYPT_SIZE)
        return NULL;

    /* Accept "$1$abcdefgh$", "abcdefgh$" and "abcdefgh" alike. */
    if (strncmp(salt, magic, 3) == 0)
        salt += 3;
    while (salt_length < 8u && salt[salt_length] && salt[salt_length] != '$')
        ++salt_length;
    memcpy(clean_salt, salt, salt_length);
    clean_salt[salt_length] = 0;

    password_length = strlen(password);

    dtv_md5_init(&ctx);
    dtv_md5_update(&ctx, password, password_length);
    dtv_md5_update(&ctx, magic, 3);
    dtv_md5_update(&ctx, clean_salt, salt_length);

    /* Inner hash of password+salt+password, folded in one digest at a
     * time for as many bytes as the password is long. */
    dtv_md5_init(&alt_ctx);
    dtv_md5_update(&alt_ctx, password, password_length);
    dtv_md5_update(&alt_ctx, clean_salt, salt_length);
    dtv_md5_update(&alt_ctx, password, password_length);
    dtv_md5_final(&alt_ctx, digest);

    for (i = password_length; i > 16u; i -= 16u)
        dtv_md5_update(&ctx, digest, 16);
    dtv_md5_update(&ctx, digest, i);

    /* Then the password length bit by bit: a zero byte for a set bit, the
     * first password byte for a clear one. */
    memset(digest, 0, sizeof(digest));
    for (i = password_length; i; i >>= 1) {
        if (i & 1u)
            dtv_md5_update(&ctx, digest, 1);
        else
            dtv_md5_update(&ctx, password, 1);
    }
    dtv_md5_final(&ctx, digest);

    /* 1000 rounds of deliberate slowness. */
    for (i = 0; i < 1000u; ++i) {
        dtv_md5_init(&ctx);
        if (i & 1u)
            dtv_md5_update(&ctx, password, password_length);
        else
            dtv_md5_update(&ctx, digest, 16);
        if (i % 3u)
            dtv_md5_update(&ctx, clean_salt, salt_length);
        if (i % 7u)
            dtv_md5_update(&ctx, password, password_length);
        if (i & 1u)
            dtv_md5_update(&ctx, digest, 16);
        else
            dtv_md5_update(&ctx, password, password_length);
        dtv_md5_final(&ctx, digest);
    }

    p = output;
    memcpy(p, magic, 3);
    p += 3;
    memcpy(p, clean_salt, salt_length);
    p += salt_length;
    *p++ = '$';
    md5_crypt_encode(digest, p);
    return output;
}

/* ---- AES-128 (FIPS-197) -------------------------------------------- */

/* Byte oriented, no lookup tables: cs378x encrypts a couple of 16 byte
 * blocks per ECM. The state is laid out as the standard writes it,
 * state[r][c] at index 4*c + r, which is also the arrival order. */

static const uint8_t aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static const uint8_t aes_inv_sbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

/* Multiplication in GF(2^8) with the AES polynomial. */
static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    uint8_t result = 0;
    while (b) {
        if (b & 1u)
            result ^= a;
        a = (uint8_t)((a << 1) ^ ((a & 0x80u) ? 0x1bu : 0u));
        b >>= 1;
    }
    return result;
}

void dtv_aes128_set_key(dtv_aes_key *schedule, const uint8_t key[16])
{
    uint8_t rcon = 1;
    unsigned i;

    for (i = 0; i < 4u; ++i)
        schedule->round_key[i] = ((uint32_t)key[4u * i] << 24) |
                                 ((uint32_t)key[4u * i + 1u] << 16) |
                                 ((uint32_t)key[4u * i + 2u] << 8) |
                                 key[4u * i + 3u];

    for (i = 4u; i < 44u; ++i) {
        uint32_t word = schedule->round_key[i - 1u];
        if (i % 4u == 0) {
            /* Rotate, substitute, then fold in the round constant. */
            word = (word << 8) | (word >> 24);
            word = ((uint32_t)aes_sbox[(word >> 24) & 0xffu] << 24) |
                   ((uint32_t)aes_sbox[(word >> 16) & 0xffu] << 16) |
                   ((uint32_t)aes_sbox[(word >> 8) & 0xffu] << 8) |
                   aes_sbox[word & 0xffu];
            word ^= (uint32_t)rcon << 24;
            rcon = gf_mul(rcon, 2);
        }
        schedule->round_key[i] = schedule->round_key[i - 4u] ^ word;
    }
}

static void aes_add_round_key(uint8_t state[16], const uint32_t *round_key)
{
    unsigned column, row;
    for (column = 0; column < 4u; ++column)
        for (row = 0; row < 4u; ++row)
            state[4u * column + row] ^=
                (uint8_t)(round_key[column] >> (24u - 8u * row));
}

void dtv_aes128_encrypt_block(const dtv_aes_key *schedule,
                              const uint8_t in[16], uint8_t out[16])
{
    uint8_t state[16];
    unsigned round, i, column;

    memcpy(state, in, 16);
    aes_add_round_key(state, schedule->round_key);

    for (round = 1; round <= 10u; ++round) {
        uint8_t shifted[16];
        for (i = 0; i < 16u; ++i)
            state[i] = aes_sbox[state[i]];
        /* Row r rotates left by r positions. */
        for (column = 0; column < 4u; ++column)
            for (i = 0; i < 4u; ++i)
                shifted[4u * column + i] =
                    state[4u * ((column + i) & 3u) + i];
        memcpy(state, shifted, 16);
        if (round != 10u) {
            for (column = 0; column < 4u; ++column) {
                uint8_t *c = state + 4u * column;
                uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
                c[0] = (uint8_t)(gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3);
                c[1] = (uint8_t)(a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3);
                c[2] = (uint8_t)(a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3));
                c[3] = (uint8_t)(gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2));
            }
        }
        aes_add_round_key(state, schedule->round_key + 4u * round);
    }
    memcpy(out, state, 16);
}

void dtv_aes128_decrypt_block(const dtv_aes_key *schedule,
                              const uint8_t in[16], uint8_t out[16])
{
    uint8_t state[16];
    unsigned round, i, column;

    memcpy(state, in, 16);
    aes_add_round_key(state, schedule->round_key + 40u);

    for (round = 10u; round >= 1u; --round) {
        uint8_t shifted[16];
        /* Inverse shift rows: row r rotates right by r. */
        for (column = 0; column < 4u; ++column)
            for (i = 0; i < 4u; ++i)
                shifted[4u * ((column + i) & 3u) + i] =
                    state[4u * column + i];
        for (i = 0; i < 16u; ++i)
            state[i] = aes_inv_sbox[shifted[i]];
        aes_add_round_key(state, schedule->round_key + 4u * (round - 1u));
        if (round != 1u) {
            for (column = 0; column < 4u; ++column) {
                uint8_t *c = state + 4u * column;
                uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
                c[0] = (uint8_t)(gf_mul(a0, 14) ^ gf_mul(a1, 11) ^
                                 gf_mul(a2, 13) ^ gf_mul(a3, 9));
                c[1] = (uint8_t)(gf_mul(a0, 9) ^ gf_mul(a1, 14) ^
                                 gf_mul(a2, 11) ^ gf_mul(a3, 13));
                c[2] = (uint8_t)(gf_mul(a0, 13) ^ gf_mul(a1, 9) ^
                                 gf_mul(a2, 14) ^ gf_mul(a3, 11));
                c[3] = (uint8_t)(gf_mul(a0, 11) ^ gf_mul(a1, 13) ^
                                 gf_mul(a2, 9) ^ gf_mul(a3, 14));
            }
        }
    }
    memcpy(out, state, 16);
}

/* ---- CRC-32 -------------------------------------------------------- */

uint32_t dtv_crc32(uint32_t seed, const void *data, size_t size)
{
    /* Reflected polynomial 0xedb88320, pre- and post-inverted: the value
     * zlib crc32() returns, which is what camd35 expects. */
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = seed ^ 0xffffffffu;
    size_t i;
    unsigned bit;
    for (i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (bit = 0; bit < 8u; ++bit)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xedb88320u : (crc >> 1);
    }
    return crc ^ 0xffffffffu;
}
