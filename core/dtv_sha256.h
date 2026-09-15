/* dtv_sha256.h - SHA-256, for checking the firmware files before upload.
 * Self-contained so no platform crypto library is needed. */
#ifndef DTV_SHA256_H
#define DTV_SHA256_H

#include <stddef.h>
#include <stdint.h>

void dtv_sha256(const uint8_t *data, size_t size, uint8_t digest[32]);

#endif
