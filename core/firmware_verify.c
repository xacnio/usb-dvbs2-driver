#include "firmware_verify.h"

#include <string.h>

#include "dtv_sha256.h"

typedef struct firmware_identity {
    size_t size;
    uint8_t sha256[32];
} firmware_identity;

static const firmware_identity g_firmware_identities[] = {
    {
        6498,
        { 0xe4, 0xe9, 0x46, 0x33, 0xb4, 0xac, 0x47, 0xc6,
          0xbb, 0xa5, 0xab, 0x90, 0xee, 0xdd, 0x4a, 0x7e,
          0x5f, 0xfa, 0x2c, 0xe7, 0xe3, 0x42, 0x88, 0x4c,
          0x91, 0x74, 0x9c, 0x74, 0x64, 0x70, 0x4a, 0x70 }
    },
    {
        48960,
        { 0xc4, 0xde, 0x37, 0x7b, 0xd7, 0xec, 0x17, 0xfb,
          0x8d, 0x83, 0x24, 0x50, 0xfa, 0xfe, 0x99, 0x9c,
          0xc4, 0x14, 0xe0, 0xf1, 0xd5, 0x44, 0xdd, 0x4c,
          0x98, 0x0b, 0x3e, 0xee, 0x48, 0x67, 0x98, 0x58 }
    }
};

int dtv_firmware_verify(dtv_firmware_kind kind, const uint8_t *data,
                        size_t size)
{
    const firmware_identity *identity;
    uint8_t digest[32];

    if ((unsigned)kind >= sizeof(g_firmware_identities) /
                          sizeof(g_firmware_identities[0]) || !data)
        return -1;
    identity = &g_firmware_identities[kind];
    if (size != identity->size)
        return -1;
    dtv_sha256(data, size, digest);
    return memcmp(digest, identity->sha256, sizeof(digest)) == 0 ? 0 : -1;
}
