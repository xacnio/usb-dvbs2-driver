/* gpio_scan.c - empirical scan for the GPIO feeding the demod and the right
 * I2C bus/address
 *
 * The reference board uses GPIO1, but this board may differ. Every GPIO is
 * power-cycled and the candidate demod/tuner addresses are read on every
 * bus; the combination that breaks the echo pattern (addr<<1|1) points at
 * the real chip.
 *
 * Loads firmware (into RAM, volatile). Registers and I2C only - no flash
 * writes. */
#include <stdio.h>
#include <stdlib.h>
#include "usb_backend.h"
#include "it9300.h"
#include "tool_util.h"

int main(void)
{
    dtv_usb *usb = NULL;
    it9300 b;
    int r;

    const dtv_hardware_profile *profile = open_connected_device(&usb);
    if (!profile)
        return 2;
    it9300_attach(&b, usb);
    it9300_identify(&b);
    printf("chip_type=0x%04x\n", b.chip_type);

    if (it9300_query_fw_version(&b) != 0 &&
        load_file_to_bridge(&b, profile->bridge_firmware, 0) != 0) {
        fprintf(stderr, "[ERROR] firmware could not be loaded\n");
        dtv_usb_close(usb); return 3;
    }
    printf("firmware=%d.%d.%d.%d\n", b.fw_ver[0], b.fw_ver[1], b.fw_ver[2], b.fw_ver[3]);
    it9300_bridge_init(&b);

    /* 7-bit candidate addresses (demod + tuner) */
    uint8_t addrs[] = { 0x6C, 0x6E, 0x0C, 0x0D, 0x14, 0x18, 0x1A, 0x60, 0x61, 0x63 };
    uint8_t buses[] = { 0x03, 0x01, 0x02 };

    printf("\n=== Scan: GPIO x bus x address (non-echo = REAL) ===\n");
    int found = 0;
    for (int g = 0; g < 16; g++) {
        it9300_gpio_power_cycle(&b, g);
        for (size_t bi = 0; bi < sizeof(buses); bi++) {
            b.i2c_bus = buses[bi];
            for (size_t ai = 0; ai < sizeof(addrs); ai++) {
                uint8_t reg = 0, v = 0;
                r = it9300_i2c_wr_rd(&b, addrs[ai], &reg, 1, &v, 1);
                uint8_t echo = (uint8_t)((addrs[ai] << 1) | 1);
                if (r == 0 && v != echo) {
                    /* read two more registers; a real chip returns varying
                     * values */
                    uint8_t r1 = 1, r2 = 2, v1 = 0, v2 = 0;
                    it9300_i2c_wr_rd(&b, addrs[ai], &r1, 1, &v1, 1);
                    it9300_i2c_wr_rd(&b, addrs[ai], &r2, 1, &v2, 1);
                    printf("  >>> GPIO%d bus=0x%02x addr=0x%02x : reg0=%02x reg1=%02x reg2=%02x  (REAL CHIP?)\n",
                           g + 1, b.i2c_bus, addrs[ai], v, v1, v2);
                    found++;
                }
            }
        }
    }

    if (!found)
        printf("\nNo combination broke the echo. Demod access may be outside generic I2C.\n");
    else
        printf("\n%d candidate(s) found.\n", found);

    dtv_usb_close(usb);
    return 0;
}
