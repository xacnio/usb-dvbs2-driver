/* AVL6261 firmware patch boot/ready diagnostic. */
#include <stdio.h>
#include <stdlib.h>
#include "dtv_platform.h"

#include "usb_backend.h"
#include "it9300.h"
#include "avl62x1.h"
#include "firmware_verify.h"

#define UDTV_VID 0x048d
#define UDTV_PID 0xf036
#define BRIDGE_FW "../firmware/hiremco-it9303.fw"
#define DEMOD_FW  "../firmware/hiremco-avl62x1.fw"

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) { fclose(file); return NULL; }
    uint8_t *data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(file);
    if (data) *size = (size_t)length;
    return data;
}

static int ensure_bridge_firmware(it9300 *bridge)
{
    if (it9300_query_fw_version(bridge) == 0)
        return 0;
    size_t size = 0;
    uint8_t *firmware = read_file(BRIDGE_FW, &size);
    if (!firmware) return -1;
    if (dtv_firmware_verify(DTV_FIRMWARE_IT9303, firmware, size) != 0) {
        free(firmware);
        return -2;
    }
    int rc = it9300_download_firmware(bridge, firmware, size);
    free(firmware);
    return rc;
}

int main(int argc, char **argv)
{
    const char *demod_path = argc > 1 ? argv[1] : DEMOD_FW;
    dtv_usb *usb = NULL;
    it9300 bridge;
    int result = 1;

    if (dtv_usb_open(&usb, UDTV_VID, UDTV_PID) != 0) {
        fprintf(stderr, "USB device could not be opened.\n");
        return 2;
    }
    if (it9300_attach(&bridge, usb) != 0 || it9300_identify(&bridge) != 0 ||
        ensure_bridge_firmware(&bridge) != 0) {
        fprintf(stderr, "IT9303 firmware could not be prepared.\n");
        goto done;
    }
    printf("IT9303 firmware: %u.%u.%u.%u\n", bridge.fw_ver[0], bridge.fw_ver[1],
           bridge.fw_ver[2], bridge.fw_ver[3]);
    if (it9300_bridge_init(&bridge) != 0) {
        fprintf(stderr, "Bridge init failed.\n");
        goto done;
    }

    /* libudtv board sequence: native GPIO2 HIGH -> LOW -> HIGH. */
    it9300_gpio_set(&bridge, 1, 1); dtv_sleep_ms(100);
    it9300_gpio_set(&bridge, 1, 0); dtv_sleep_ms(200);
    it9300_gpio_set(&bridge, 1, 1); dtv_sleep_ms(100);
    bridge.i2c_bus = 3;

    uint32_t chip_id = 0;
    int rc = avl62x1_read32(&bridge, AVL62X1_I2C_ADDR, 0x040000, &chip_id);
    printf("AVL chip ID: 0x%08lx (rc=%d)\n", (unsigned long)chip_id, rc);
    if (rc != 0 || chip_id != AVL62X1_CHIP_ID) {
        fprintf(stderr, "AVL62x1 not found.\n");
        goto done;
    }

    size_t patch_size = 0;
    uint8_t *patch = read_file(demod_path, &patch_size);
    if (!patch) {
        fprintf(stderr, "Demod firmware could not be opened: %s\n", demod_path);
        goto done;
    }
    if (dtv_firmware_verify(DTV_FIRMWARE_AVL62X1, patch, patch_size) != 0) {
        fprintf(stderr, "Demod firmware SHA-256 verification failed.\n");
        free(patch);
        goto done;
    }
    avl62x1_patch_info info;
    rc = avl62x1_parse_patch(patch, patch_size, &info);
    if (rc != 0) {
        fprintf(stderr, "Invalid AVL patch file (rc=%d).\n", rc);
        free(patch);
        goto done;
    }
    printf("Patch: %lu bytes, version %u.%u.%u, chip 0x%08lx\n",
           (unsigned long)info.words * 4, info.major, info.minor, info.build,
           (unsigned long)info.chip_id);

    rc = avl62x1_load_patch(&bridge, AVL62X1_I2C_ADDR, patch, patch_size);
    free(patch);
    printf("Patch load result: %d\n", rc);
    if (rc != 0) goto done;

    rc = avl62x1_wait_ready(&bridge, AVL62X1_I2C_ADDR, 100, 50);
    printf("Boot/ready result: %d\n", rc);
    if (rc != 0) goto done;

    uint8_t major = 0, minor = 0;
    uint16_t build = 0;
    rc = avl62x1_get_running_version(&bridge, AVL62X1_I2C_ADDR,
                                     &major, &minor, &build);
    printf("Running AVL firmware: %u.%u.%u (rc=%d)\n",
           major, minor, build, rc);
    if (rc == 0) {
        puts("[OK] AVL6261 firmware booted and the ready word was verified.");
        result = 0;
    }

done:
    dtv_usb_close(usb);
    return result;
}
