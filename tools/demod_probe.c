/* demod_probe.c - demod/tuner register reads (repeated-start wr+rd)
 *
 * Writes a register pointer to the candidate I2C addresses and reads it
 * back (it9300_i2c_wr_rd). With a real chip the registers return
 * meaningful, varying values; otherwise the echo pattern (constant) shows
 * up.
 *
 * Bridge init (demod power-up) is performed - user approved, volatile. */
#include <stdio.h>
#include <stdlib.h>
#include "usb_backend.h"
#include "it9300.h"
#include "tool_util.h"

static int ensure_firmware(it9300 *b, const char *path)
{
    if (it9300_query_fw_version(b) == 0) {
        printf("[OK] Firmware already loaded: %d.%d.%d.%d\n",
               b->fw_ver[0], b->fw_ver[1], b->fw_ver[2], b->fw_ver[3]);
        return 0;
    }
    int r = load_file_to_bridge(b, path, 0);
    if (r == -1) { printf("[ERROR] firmware could not be read\n"); return -1; }
    if (r == -2) {
        printf("[ERROR] firmware SHA-256 verification failed\n");
        return -1;
    }
    if (r == 0)
        printf("[OK] Firmware loaded (RAM): %d.%d.%d.%d\n",
               b->fw_ver[0], b->fw_ver[1], b->fw_ver[2], b->fw_ver[3]);
    else
        printf("[ERROR] firmware load r=%d\n", r);
    return r;
}

static void dump_regs(it9300 *b, uint8_t addr, const char *name)
{
    printf("--- addr 0x%02x (%s) : reg[0x00..0x0F] ---\n", addr, name);
    printf("   ");
    int nonecho = 0, ok = 0;
    uint8_t vals[16];
    for (int reg = 0; reg < 16; reg++) {
        uint8_t rp = (uint8_t)reg;
        uint8_t v = 0;
        int r = it9300_i2c_wr_rd(b, addr, &rp, 1, &v, 1);
        if (r == 0) {
            ok++;
            vals[reg] = v;
            /* echo detection: v == (addr<<1)|1 means it is fake */
            if (v != (uint8_t)((addr << 1) | 1))
                nonecho++;
        } else {
            vals[reg] = 0xFF;
        }
    }
    for (int reg = 0; reg < 16; reg++)
        printf("%02x ", vals[reg]);
    printf("\n   -> %d/16 read, %d of them non-echo (%s)\n\n",
           ok, nonecho,
           nonecho > 0 ? "MAY BE A REAL CHIP" : "echo/empty");
}

int main(void)
{
    dtv_usb *usb = NULL;
    it9300 bridge;
    int r;

    const dtv_hardware_profile *profile = open_connected_device(&usb);
    if (!profile)
        return 2;
    it9300_attach(&bridge, usb);

    it9300_identify(&bridge);
    printf("[OK] chip_type=0x%04x version=0x%02x\n", bridge.chip_type, bridge.chip_version);

    /* Kernel order: firmware -> bridge init (TS/EP/I2C) -> demod power-up */
    if (ensure_firmware(&bridge, profile->bridge_firmware) != 0) {
        dtv_usb_close(usb);
        return 5;
    }
    r = it9300_bridge_init(&bridge);
    printf("[%s] bridge_init (it930x_init) r=%d\n", r == 0 ? "OK" : "WARN", r);
    r = it9300_demod_power(&bridge);
    printf("[%s] demod_power (GPIO1 + 150ms) r=%d\n\n", r == 0 ? "OK" : "WARN", r);

    uint8_t buses[] = { 0x03, 0x01, 0x02 };
    for (size_t i = 0; i < sizeof(buses); i++) {
        bridge.i2c_bus = buses[i];
        printf("############## I2C BUS = 0x%02x ##############\n", bridge.i2c_bus);
        dump_regs(&bridge, 0x6C, "Sony CXD28xx demod SLVT");
        dump_regs(&bridge, 0x6E, "Sony CXD28xx demod SLVX");
        dump_regs(&bridge, 0x0C, "generic demod");
        dump_regs(&bridge, 0x18, "AVL/generic demod");
        dump_regs(&bridge, 0x60, "RDA5815 tuner");
    }

    dtv_usb_close(usb);
    return 0;
}
