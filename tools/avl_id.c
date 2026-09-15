/* avl_id.c - read the AVL62x1 / AVL68xx demod identity
 *
 * AVL demods use a 24-bit register address with a big-endian 32-bit value.
 * The AVL62x1 Linux driver reads the chip ID from 0x040000. On the Hiremco
 * hardware the value 0x62615ca8 confirms the AVL6261A/C family. */
#include <stdio.h>
#include <stdlib.h>
#include "dtv_platform.h"
#include "usb_backend.h"
#include "it9300.h"
#include "firmware_verify.h"

#define UDTV_VID 0x048D
#define UDTV_PID 0xF036
#define DEFAULT_FW "../firmware/hiremco-it9303.fw"
#define DEMOD_ADDR 0x14

static uint8_t *read_file(const char *path, size_t *sz) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc(n);
    if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f); if (b) *sz = (size_t)n; return b;
}

/* Read an AVL 32-bit register: write the 24-bit address, read 4 big-endian
 * bytes. */
static int avl_rd32(it9300 *b, uint32_t reg, uint32_t *out)
{
    uint8_t w[3] = { (reg >> 16) & 0xff, (reg >> 8) & 0xff, reg & 0xff };
    uint8_t r[4] = { 0 };
    int rc = it9300_i2c_wr_rd(b, DEMOD_ADDR, w, 3, r, 4);
    if (rc != 0) return rc;
    *out = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) |
           ((uint32_t)r[2] << 8) | r[3];
    return 0;
}

int main(void)
{
    dtv_usb *usb = NULL; it9300 b;
    if (dtv_usb_open(&usb, UDTV_VID, UDTV_PID) != 0) { fprintf(stderr,"could not open device\n"); return 2; }
    it9300_attach(&b, usb);
    it9300_identify(&b);

    if (it9300_query_fw_version(&b) != 0) {
        size_t sz = 0; uint8_t *fw = read_file(DEFAULT_FW, &sz);
        if (!fw || dtv_firmware_verify(DTV_FIRMWARE_IT9303, fw, sz) != 0 ||
            it9300_download_firmware(&b, fw, sz) != 0) {
            fprintf(stderr,"firmware missing or SHA-256 invalid\n"); free(fw); return 3;
        }
        free(fw);
    }
    printf("firmware=%d.%d.%d.%d\n", b.fw_ver[0], b.fw_ver[1], b.fw_ver[2], b.fw_ver[3]);
    it9300_bridge_init(&b);

    /* libudtv.so board reset: native GPIO2, HIGH->LOW->HIGH. */
    it9300_gpio_set(&b, 1, 1); dtv_sleep_ms(100);
    it9300_gpio_set(&b, 1, 0); dtv_sleep_ms(200);
    it9300_gpio_set(&b, 1, 1); dtv_sleep_ms(100);

    b.i2c_bus = 3;
    uint32_t legacy_id = 0, family_id = 0;
    int r1 = avl_rd32(&b, 0x108000, &legacy_id);
    int r2 = avl_rd32(&b, 0x040000, &family_id);
    printf("bus=3 addr=0x14 reg108000=0x%08x (r=%d) chip_id=0x%08x (r=%d)\n",
           legacy_id, r1, family_id, r2);
    if (r2 == 0 && family_id == 0x62615ca8) {
        printf("[OK] Demod positively identified: AVL6261A/C (AVL62x1)\n");
    } else if (r1 == 0 && r2 == 0 && family_id == 0x68624955) {
        const char *name = "AVL68xx";
        switch (legacy_id) {
        case 0x0b: name = "AVL6882"; break;
        case 0x0d: name = "AVL6812"; break;
        case 0x0e: name = "AVL6762"; break;
        case 0x0f: name = "AVL6862"; break;
        }
        printf("[OK] Demod positively identified: %s\n", name);
    } else {
        printf("[NO] No known AVL chip ID matched.\n");
    }
    dtv_usb_close(usb);
    return 0;
}
