/* dtv_sha256.c - FIPS 180-4 SHA-256; see dtv_sha256.h. */
#include "dtv_sha256.h"

#include <string.h>

static const uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; ++i)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    for (i = 16; i < 64; ++i) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];
    for (i = 0; i < 64; ++i) {
        uint32_t t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) +
                      ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) +
                      ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void dtv_sha256(const uint8_t *data, size_t size, uint8_t digest[32])
{
    uint32_t state[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t tail[128];
    size_t whole = size - size % 64u, rest = size - whole, tail_size;
    uint64_t bits = (uint64_t)size * 8u;
    size_t i;

    for (i = 0; i < whole; i += 64u)
        compress(state, data + i);

    memset(tail, 0, sizeof(tail));
    if (rest)
        memcpy(tail, data + whole, rest);
    tail[rest] = 0x80;
    tail_size = rest + 1u + 8u <= 64u ? 64u : 128u;
    for (i = 0; i < 8u; ++i)
        tail[tail_size - 1u - i] = (uint8_t)(bits >> (8u * i));
    compress(state, tail);
    if (tail_size == 128u)
        compress(state, tail + 64);

    for (i = 0; i < 8u; ++i) {
        digest[i * 4] = (uint8_t)(state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)state[i];
    }
}
