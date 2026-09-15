#ifndef DTV_FIRMWARE_VERIFY_H
#define DTV_FIRMWARE_VERIFY_H

#include <stddef.h>
#include <stdint.h>

typedef enum dtv_firmware_kind {
    DTV_FIRMWARE_IT9303,
    DTV_FIRMWARE_AVL62X1
} dtv_firmware_kind;

/* Returns 0 only when both the size and SHA-256 digest match the firmware
 * approved for this hardware profile. */
int dtv_firmware_verify(dtv_firmware_kind kind, const uint8_t *data,
                        size_t size);

#endif
