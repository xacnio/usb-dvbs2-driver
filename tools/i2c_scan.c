/* i2c_scan.c - READ-ONLY demod/tuner I2C scan
 *
 * Reads one byte from the usual I2C addresses through the IT9300 bridge (it
 * WRITES nothing and loads NO firmware). The point is to:
 *   - see whether the I2C bridge works at all
 *   - find the demod (CXD2878) and tuner (RDA5815) I2C addresses
 *   - tell whether firmware is really required (an I2C answer means the
 *     demod is reachable)
 *
 * Entirely safe: it only uses CMD_GENERIC_I2C_RD. */
#include <stdio.h>
#include "usb_backend.h"
#include "it9300.h"

#define UDTV_VID 0x048D
#define UDTV_PID 0xF036

/* Known candidate chips - 7-bit I2C addresses (sent as addr<<1 in the
 * protocol). */
static const struct { uint8_t addr; const char *name; } candidates[] = {
    { 0x6C, "Sony CXD28xx demod" },
    { 0x6D, "Sony CXD28xx demod (alt)" },
    { 0x0C, "CXD/generic demod" },
    { 0x0D, "CXD/generic demod (alt)" },
    { 0x60, "RDA5815 tuner" },
    { 0x61, "RDA5815 tuner (alt)" },
    { 0x14, "AVL demod" },
    { 0x62, "tuner" },
    { 0x63, "tuner (alt)" },
    { 0x00, NULL },
};

int main(void)
{
    dtv_usb *usb = NULL;
    it9300 bridge;
    int r;

    r = dtv_usb_open(&usb, UDTV_VID, UDTV_PID);
    if (r != 0) {
        fprintf(stderr, "[ERROR] Device could not be opened: %s\n", dtv_usb_strerror(r));
        return 2;
    }
    it9300_attach(&bridge, usb);

    r = it9300_identify(&bridge);
    if (r == 0)
        printf("[OK] Bridge responds. chip_type=0x%04x version=0x%02x\n",
               bridge.chip_type, bridge.chip_version);
    else
        printf("[WARN] identify r=%d (I2C will be tried anyway)\n", r);

    /* Wake the demod (volatile register/GPIO init - user approved) */
    r = it9300_bridge_init(&bridge);
    printf("[%s] bridge_init (demod power-up + I2C clock) r=%d\n\n",
           r == 0 ? "OK" : "WARN", r);

    printf("=== Known candidate addresses (READ-ONLY) ===\n");
    int hits = 0;
    for (int i = 0; candidates[i].name; i++) {
        uint8_t val = 0;
        r = it9300_i2c_read(&bridge, candidates[i].addr, &val, 1);
        printf("  addr 0x%02x  %-28s -> %s",
               candidates[i].addr, candidates[i].name,
               r == 0 ? "REPLY" : (r == 1 ? "no-reply(fw?)" : "error"));
        if (r == 0) { printf(" (first byte=0x%02x)", val); hits++; }
        else        { printf(" (r=%d)", r); }
        printf("\n");
    }

    printf("\n=== Full scan (7-bit 0x08..0x77, READ-ONLY) ===\n");
    for (int a = 0x08; a <= 0x77; a++) {
        uint8_t val = 0;
        r = it9300_i2c_read(&bridge, (uint8_t)a, &val, 1);
        if (r == 0) {
            printf("  0x%02x -> REPLY (byte=0x%02x)\n", a, val);
            hits++;
        }
    }

    printf("\nTotal addresses that replied: %d\n", hits);
    if (hits == 0)
        printf("No I2C reply at all -> demod firmware/power-up is probably required.\n");

    dtv_usb_close(usb);
    return 0;
}
