/* bridge_test.c - phase 1 go/no-go: IT9300 bridge verification
 *
 * 1) Open the device (WinUSB required)
 * 2) Read the chip id before firmware
 * 3) Load and boot the firmware
 * 4) Report the firmware version and the chip id
 *
 * Usage: usb-dvbs2-bridge-test [--force] [firmware.fw]
 *   Without a firmware path the bridge firmware of the receiver's hardware
 *   profile is used. --force reloads the RAM firmware even when one is
 *   running. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dtv_platform.h"
#include "usb_backend.h"
#include "it9300.h"
#include "firmware_verify.h"
#include "tool_util.h"

int main(int argc, char **argv)
{
    const dtv_hardware_profile *profile;
    const char *fw_path = NULL;
    char fw_resolved[DTV_PATH_MAX];
    int force = 0;
    int fw_path_set = 0;
    dtv_usb *usb = NULL;
    it9300 bridge;
    int r;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else if (!fw_path_set) {
            fw_path = argv[i];
            fw_path_set = 1;
        } else {
            fprintf(stderr, "Usage: %s [--force] [firmware.fw]\n", argv[0]);
            return 1;
        }
    }

    profile = open_connected_device(&usb);
    if (!profile)
        return 2;
    printf("[OK] Device opened (%04x:%04x)\n", profile->vid, profile->pid);
    if (!fw_path_set)
        fw_path = profile->bridge_firmware;

    it9300_attach(&bridge, usb);

    /* --- identity before firmware --- */
    r = it9300_identify(&bridge);
    if (r < 0) {
        printf("[WARN] Identify before firmware failed (r=%d). "
               "This can be normal in the bootstrap state.\n", r);
    } else {
        printf("[OK] Before firmware: chip_type=0x%04x version=0x%02x\n",
               bridge.chip_type, bridge.chip_version);
    }

    /* --- is the firmware already running? --- */
    r = it9300_query_fw_version(&bridge);
    if (r == 0 && !force) {
        printf("[OK] Firmware is ALREADY running. Version: %d.%d.%d.%d\n",
               bridge.fw_ver[0], bridge.fw_ver[1],
               bridge.fw_ver[2], bridge.fw_ver[3]);
    } else {
        if (force && r == 0)
            printf("[WARN] --force: replacing the running %d.%d.%d.%d firmware "
                   "in RAM.\n",
                   bridge.fw_ver[0], bridge.fw_ver[1],
                   bridge.fw_ver[2], bridge.fw_ver[3]);
        else
            printf("[..] No firmware running (r=%d). Loading into RAM...\n", r);
        size_t fw_size = 0;
        resolve_data_path(fw_path, fw_resolved, sizeof(fw_resolved));
        uint8_t *fw = read_file(fw_resolved, &fw_size);
        if (!fw) {
            fprintf(stderr, "[ERROR] Firmware could not be read: %s\n", fw_path);
            dtv_usb_close(usb);
            return 3;
        }
        if (dtv_firmware_verify(DTV_FIRMWARE_IT9303, fw, fw_size) != 0) {
            fprintf(stderr, "[ERROR] Firmware SHA-256 verification failed.\n");
            free(fw);
            dtv_usb_close(usb);
            return 3;
        }
        printf("[..] Firmware: %s (%zu bytes, format=%s)\n",
               fw_resolved, fw_size, fw[0] == 0x01 ? "old" : "scatter");
        r = it9300_download_firmware(&bridge, fw, fw_size);
        free(fw);
        if (r < 0) {
            fprintf(stderr, "[ERROR] Firmware load/boot: r=%d\n", r);
            dtv_usb_close(usb);
            return 4;
        }
        printf("[OK] Firmware running. Version: %d.%d.%d.%d\n",
               bridge.fw_ver[0], bridge.fw_ver[1],
               bridge.fw_ver[2], bridge.fw_ver[3]);
    }

    /* --- identity after boot --- */
    r = it9300_identify(&bridge);
    if (r == 0)
        printf("[OK] After boot: chip_type=0x%04x version=0x%02x\n",
               bridge.chip_type, bridge.chip_version);
    else
        printf("[WARN] Identify after boot r=%d\n", r);

    printf("\n=== PHASE 1 PASSED: the bridge can be driven ===\n");
    dtv_usb_close(usb);
    return 0;
}
