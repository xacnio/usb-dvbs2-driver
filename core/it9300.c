/* it9300.c - ITE IT9300/IT930x USB bridge driver (libusb port)
 *
 * Ported from the Linux kernel dvb-usb-v2 it930x/af9035 driver. Reference:
 * nxdong520/avl6381 it930x.c (GPL-2.0-or-later). */
#include "it9300.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include "dtv_platform.h"
static void it9300_msleep(unsigned ms) { dtv_sleep_ms(ms); }
#else
#include <unistd.h>
static void it9300_msleep(unsigned ms) { usleep(ms * 1000u); }
#endif

/* GPIO register tables (kernel it930x, index 0 = GPIO1 .. 15 = GPIO16) */
static const uint16_t gpio_mode_regs[16] = {
    0xd8b0,0xd8b8,0xd8b4,0xd8c0,0xd8bc,0xd8c8,0xd8c4,0xd8d0,
    0xd8cc,0xd8d8,0xd8d4,0xd8e0,0xd8dc,0xd8e4,0xd8e8,0xd8ec };
static const uint16_t gpio_en_regs[16] = {
    0xd8b1,0xd8b9,0xd8b5,0xd8c1,0xd8bd,0xd8c9,0xd8c5,0xd8d1,
    0xd8cd,0xd8d9,0xd8d5,0xd8e1,0xd8dd,0xd8e5,0xd8e9,0xd8ed };
static const uint16_t gpio_out_regs[16] = {
    0xd8af,0xd8b7,0xd8b3,0xd8bf,0xd8bb,0xd8c7,0xd8c3,0xd8cf,
    0xd8cb,0xd8d7,0xd8d3,0xd8df,0xd8db,0xd8e3,0xd8e7,0xd8eb };
static const uint16_t gpio_in_regs[16] = {
    0xd8ae,0xd8b6,0xd8b2,0xd8be,0xd8ba,0xd8c6,0xd8c2,0xd8ce,
    0xd8ca,0xd8d6,0xd8d2,0xd8de,0xd8da,0xd8e2,0xd8e6,0xd8ea };

#define GPIO1_MODE_REG   0xd8b0
#define GPIO1_EN_REG     0xd8b1
#define GPIO1_OUT_REG    0xd8af
#define I2C_SPEED_366K   7

#define BUF_LEN       255
#define REQ_HDR_LEN   4    /* [len][mbox][cmd][seq] */
#define ACK_HDR_LEN   3    /* [len][seq][status]    */
#define CHECKSUM_LEN  2
#define USB_TIMEOUT   2000
#define MAX_XFER_SIZE 64

/* Identical to the kernel it930x_checksum: starting at index 1 it adds odd
 * bytes into the high half and even bytes into the low half, then takes the
 * complement. */
static uint16_t it9300_checksum(const uint8_t *buf, size_t len)
{
    uint16_t checksum = 0;
    for (size_t i = 1; i < len; i++) {
        if (i % 2)
            checksum += (uint16_t)buf[i] << 8;
        else
            checksum += buf[i];
    }
    return (uint16_t)~checksum;
}

int it9300_ctrl_msg(it9300 *b, uint8_t cmd, uint8_t mbox,
                    const uint8_t *wbuf, int wlen,
                    uint8_t *rbuf, int rlen)
{
    uint8_t buf[BUF_LEN];
    int wtot, rtot;
    uint16_t checksum, resp_checksum;
    int r;

    if (wlen > (BUF_LEN - REQ_HDR_LEN - CHECKSUM_LEN) ||
        rlen > (BUF_LEN - ACK_HDR_LEN - CHECKSUM_LEN))
        return -1;

    buf[0] = (uint8_t)(REQ_HDR_LEN + wlen + CHECKSUM_LEN - 1);
    buf[1] = mbox;
    buf[2] = cmd;
    buf[3] = b->seq++;
    if (wlen)
        memcpy(&buf[REQ_HDR_LEN], wbuf, wlen);

    wtot = REQ_HDR_LEN + wlen + CHECKSUM_LEN;
    rtot = ACK_HDR_LEN + rlen + CHECKSUM_LEN;

    /* checksum over len-1 bytes (written at index [buf[0]-1] and [buf[0]]) */
    checksum = it9300_checksum(buf, buf[0] - 1);
    buf[buf[0] - 1] = (uint8_t)(checksum >> 8);
    buf[buf[0] - 0] = (uint8_t)(checksum & 0xff);

    /* Only CMD_FW_DL (old format) has no reply. Every other command,
     * CMD_FW_SCATTER_WR included, returns a 5-byte ACK -- if it is not read
     * the replies shift. */
    if (cmd == CMD_FW_DL)
        rtot = 0;

    static int dbg = -1;
    if (dbg < 0) { dbg = getenv("DTV_DEBUG") ? 1 : 0; }
    if (dbg) {
        fprintf(stderr, "[TX cmd=%02x mbox=%02x wlen=%d] ", cmd, mbox, wlen);
        for (int i = 0; i < wtot; i++) fprintf(stderr, "%02x ", buf[i]);
        fprintf(stderr, "\n");
    }

    r = dtv_usb_bulk_out(b->usb, DTV_EP_CMD_OUT, buf, wtot, USB_TIMEOUT);
    if (r < 0)
        return r;

    if (rtot == 0)
        return 0;

    r = dtv_usb_bulk_in(b->usb, DTV_EP_CMD_IN, buf, rtot, USB_TIMEOUT);
    if (r < 0)
        return r;

    if (dbg) {
        fprintf(stderr, "[RX rtot=%d got=%d] ", rtot, r);
        for (int i = 0; i < r; i++) fprintf(stderr, "%02x ", buf[i]);
        fprintf(stderr, "\n");
    }

    resp_checksum = it9300_checksum(buf, rtot - 2);
    checksum = ((uint16_t)buf[rtot - 2] << 8) | buf[rtot - 1];
    if (checksum != resp_checksum)
        return -2;

    /* status = buf[2]. The firmware returns 1 when there is no IR code. */
    if (buf[2]) {
        if (cmd == CMD_IR_GET || buf[2] == 1)
            return 1;
        return -3;
    }

    if (rlen)
        memcpy(rbuf, &buf[ACK_HDR_LEN], rlen);
    return 0;
}

/* ---- register access ---- */

int it9300_wr_regs(it9300 *b, uint32_t reg, const uint8_t *val, int len)
{
    uint8_t wbuf[MAX_XFER_SIZE];
    uint8_t mbox = (reg >> 16) & 0xff;

    if (6 + len > (int)sizeof(wbuf))
        return -1;

    wbuf[0] = (uint8_t)len;
    wbuf[1] = 2;
    wbuf[2] = 0;
    wbuf[3] = 0;
    wbuf[4] = (reg >> 8) & 0xff;
    wbuf[5] = (reg >> 0) & 0xff;
    memcpy(&wbuf[6], val, len);

    return it9300_ctrl_msg(b, CMD_MEM_WR, mbox, wbuf, 6 + len, NULL, 0);
}

int it9300_rd_regs(it9300 *b, uint32_t reg, uint8_t *val, int len)
{
    uint8_t wbuf[6];
    uint8_t mbox = (reg >> 16) & 0xff;

    wbuf[0] = (uint8_t)len;
    wbuf[1] = 2;
    wbuf[2] = 0;
    wbuf[3] = 0;
    wbuf[4] = (reg >> 8) & 0xff;
    wbuf[5] = (reg >> 0) & 0xff;

    return it9300_ctrl_msg(b, CMD_MEM_RD, mbox, wbuf, sizeof(wbuf), val, len);
}

int it9300_wr_reg(it9300 *b, uint32_t reg, uint8_t val)
{
    return it9300_wr_regs(b, reg, &val, 1);
}

int it9300_rd_reg(it9300 *b, uint32_t reg, uint8_t *val)
{
    return it9300_rd_regs(b, reg, val, 1);
}

int it9300_wr_reg_mask(it9300 *b, uint32_t reg, uint8_t val, uint8_t mask)
{
    if (mask != 0xff) {
        uint8_t tmp;
        int ret = it9300_rd_regs(b, reg, &tmp, 1);
        if (ret)
            return ret;
        val &= mask;
        tmp &= (uint8_t)~mask;
        val |= tmp;
    }
    return it9300_wr_regs(b, reg, &val, 1);
}

/* ---- demod/tuner I2C bridge ---- 'addr' is the 7-bit I2C address (written
 * as addr<<1 in the protocol). Identical to the kernel
 * it930x_i2c_master_xfer:
 *   chip_type == 0x9306 -> CMD_GENERIC_I2C (buf: [len][bus=1][addr<<1][data])
 *   otherwise           -> CMD_I2C (buf: [len][addr<<1][reglen][regMSB][regLSB][data])
 */
int it9300_i2c_read(it9300 *b, uint8_t addr, uint8_t *val, int len)
{
    uint8_t mbox = (uint8_t)((addr & 0x80) >> 3);

    if (b->chip_type == 0x9306) {
        uint8_t wbuf[3];
        wbuf[0] = (uint8_t)len;
        wbuf[1] = b->i2c_bus;         /* I2C bus */
        wbuf[2] = (uint8_t)(addr << 1);
        return it9300_ctrl_msg(b, CMD_GENERIC_I2C_RD, mbox, wbuf, 3, val, len);
    } else {
        uint8_t wbuf[5];
        wbuf[0] = (uint8_t)len;
        wbuf[1] = (uint8_t)(addr << 1);
        wbuf[2] = 0x00;               /* reg addr len */
        wbuf[3] = 0x00;               /* reg addr MSB */
        wbuf[4] = 0x00;               /* reg addr LSB */
        return it9300_ctrl_msg(b, CMD_I2C_RD, mbox, wbuf, 5, val, len);
    }
}

int it9300_i2c_write(it9300 *b, uint8_t addr, const uint8_t *data, int len)
{
    uint8_t wbuf[MAX_XFER_SIZE];
    uint8_t mbox = (uint8_t)((addr & 0x80) >> 3);

    if (b->chip_type == 0x9306) {
        if (3 + len > (int)sizeof(wbuf))
            return -1;
        wbuf[0] = (uint8_t)len;
        wbuf[1] = b->i2c_bus;         /* I2C bus */
        wbuf[2] = (uint8_t)(addr << 1);
        memcpy(&wbuf[3], data, len);
        return it9300_ctrl_msg(b, CMD_GENERIC_I2C_WR, mbox, wbuf, 3 + len, NULL, 0);
    } else {
        if (5 + len > (int)sizeof(wbuf))
            return -1;
        wbuf[0] = (uint8_t)len;
        wbuf[1] = (uint8_t)(addr << 1);
        wbuf[2] = 0x00;
        wbuf[3] = 0x00;
        wbuf[4] = 0x00;
        memcpy(&wbuf[5], data, len);
        return it9300_ctrl_msg(b, CMD_I2C_WR, mbox, wbuf, 5 + len, NULL, 0);
    }
}

int it9300_i2c_wr_rd(it9300 *b, uint8_t addr,
                     const uint8_t *wbuf, int wlen,
                     uint8_t *rbuf, int rlen)
{
    uint8_t buf[MAX_XFER_SIZE];
    uint8_t mbox = (uint8_t)((addr & 0x80) >> 3);

    if (b->chip_type == 0x9306) {
        /* libudtv.so uses two bridge commands for register reads:
         *   GENERIC_I2C_WR [wlen,bus,addr<<1,wdata...]
         *   GENERIC_I2C_RD [rlen,bus,addr<<1]
         * GENERIC_I2C_RD never carries register bytes after byte 2. */
        if (wlen > 0) {
            int ret = it9300_i2c_write(b, addr, wbuf, wlen);
            if (ret != 0)
                return ret;
        }
        return it9300_i2c_read(b, addr, rbuf, rlen);
    } else {
        /* CMD_I2C_RD: buf[0]=len, [1]=addr<<1, [2]=reglen, [3]=MSB, [4]=LSB */
        if (5 + wlen > (int)sizeof(buf))
            return -1;
        buf[0] = (uint8_t)rlen;
        buf[1] = (uint8_t)(addr << 1);
        buf[2] = (uint8_t)wlen;
        buf[3] = (wlen == 2) ? wbuf[0] : 0x00;
        buf[4] = (wlen == 2) ? wbuf[1] : (wlen == 1 ? wbuf[0] : 0x00);
        return it9300_ctrl_msg(b, CMD_I2C_RD, mbox, buf, 5, rbuf, rlen);
    }
}

/* ---- firmware download ---- */

#define FW_HDR_SIZE 7
#define FW_MAX_DATA 58

static int fw_download_old(it9300 *b, const uint8_t *fw, size_t size)
{
    size_t i = size;
    while (i > FW_HDR_SIZE) {
        const uint8_t *p = fw + (size - i);
        uint8_t  core     = p[0];
        uint16_t data_len = ((uint16_t)p[3] << 8) | p[4];

        if ((core != 1 && core != 2) || data_len > i)
            break;

        int ret = it9300_ctrl_msg(b, CMD_FW_DL_BEGIN, 0, NULL, 0, NULL, 0);
        if (ret < 0) return ret;

        for (int j = FW_HDR_SIZE + data_len; j > 0; j -= FW_MAX_DATA) {
            int len = j > FW_MAX_DATA ? FW_MAX_DATA : j;
            const uint8_t *chunk = p + (FW_HDR_SIZE + data_len - j);
            ret = it9300_ctrl_msg(b, CMD_FW_DL, 0, chunk, len, NULL, 0);
            if (ret < 0) return ret;
        }

        ret = it9300_ctrl_msg(b, CMD_FW_DL_END, 0, NULL, 0, NULL, 0);
        if (ret < 0) return ret;

        i -= data_len + FW_HDR_SIZE;
    }
    return 0;
}

static int fw_download_new(it9300 *b, const uint8_t *fw, size_t size)
{
    size_t i_prev = 0;
    for (size_t i = FW_HDR_SIZE; i <= size; i++) {
        int boundary = (i == size) ||
            (fw[i + 0] == 0x03 &&
             (fw[i + 1] == 0x00 || fw[i + 1] == 0x01) &&
             fw[i + 2] == 0x00);
        if (boundary) {
            int wlen = (int)(i - i_prev);
            int ret = it9300_ctrl_msg(b, CMD_FW_SCATTER_WR, 0,
                                      fw + i_prev, wlen, NULL, 0);
            if (ret < 0) return ret;
            i_prev = i;
        }
    }
    return 0;
}

int it9300_download_firmware(it9300 *b, const uint8_t *fw, size_t size)
{
    int ret;

    if (size < FW_HDR_SIZE)
        return -1;

    if (fw[0] == 0x01)
        ret = fw_download_old(b, fw, size);
    else
        ret = fw_download_new(b, fw, size);
    if (ret < 0)
        return ret;

    return it9300_boot(b);
}

int it9300_query_fw_version(it9300 *b)
{
    uint8_t wbuf[1] = { 1 };
    uint8_t rbuf[4] = { 0 };
    int ret = it9300_ctrl_msg(b, CMD_FW_QUERYINFO, 0, wbuf, 1, rbuf, 4);
    if (ret < 0)
        return ret;
    if (!(rbuf[0] || rbuf[1] || rbuf[2] || rbuf[3]))
        return -4; /* firmware is not running (all zero) */
    memcpy(b->fw_ver, rbuf, 4);
    return 0;
}

int it9300_boot(it9300 *b)
{
    int ret = it9300_ctrl_msg(b, CMD_FW_BOOT, 0, NULL, 0, NULL, 0);
    if (ret < 0)
        return ret;
    return it9300_query_fw_version(b);
}

/* ---- identification ---- */

int it9300_identify(it9300 *b)
{
    int ret;
    uint8_t prechip = 0;
    uint8_t rbuf[3] = { 0, 0, 0 };

    /* Identical to the kernel it930x_identify_state: 3 bytes from reg
     * 0x1222 -> [chip_version][type_lo][type_hi] */
    ret = it9300_rd_regs(b, 0x1222, rbuf, 3);
    if (ret < 0)
        return ret;
    b->chip_version = rbuf[0];
    b->chip_type    = ((uint16_t)rbuf[2] << 8) | rbuf[1];

    ret = it9300_rd_reg(b, 0x384f, &prechip);
    if (ret < 0)
        return ret;
    (void)prechip;
    return 0;
}

int it9300_bridge_init(it9300 *b)
{
    int ret = 0;
    /* Assumes USB high speed (512-byte bulk). */
    uint16_t frame_size = 816u * 188u / 4u;   /* = 38352 */
    uint8_t  packet_size = 512u / 4u;         /* = 128   */
    uint8_t  fs[2];

    /* I2C master clocks (bus 2, and buses 1 and 3) */
    ret |= it9300_wr_reg(b, 0xf6a7, I2C_SPEED_366K);
    ret |= it9300_wr_reg(b, 0xf103, I2C_SPEED_366K);

    ret |= it9300_wr_reg(b, 0xda1a, 0x00);            /* ignore sync byte: no */
    ret |= it9300_wr_reg_mask(b, 0xf41f, 0x04, 0x04); /* dvb-t interrupt: enable */
    ret |= it9300_wr_reg_mask(b, 0xda10, 0x00, 0x00); /* mpeg full speed */
    ret |= it9300_wr_reg_mask(b, 0xf41a, 0x05, 0x05); /* dvb-t mode: enable */
    ret |= it9300_wr_reg_mask(b, 0xda1d, 0x01, 0x01);

    /* EP4 (bulk TS) enable plus NAK disable */
    ret |= it9300_wr_reg_mask(b, 0xdd11, 0x0F, 0x0F);
    ret |= it9300_wr_reg_mask(b, 0xdd13, 0x1b, 0x1b);
    ret |= it9300_wr_reg_mask(b, 0xdd11, 0x2F, 0x2F);

    fs[0] = frame_size & 0xff;
    fs[1] = (frame_size >> 8) & 0xff;
    ret |= it9300_wr_regs(b, 0xdd88, fs, 2);          /* frame size */
    ret |= it9300_wr_reg(b, 0xdd0c, packet_size);     /* max bulk packet size */

    ret |= it9300_wr_reg_mask(b, 0xda05, 0x00, 0x01);
    ret |= it9300_wr_reg_mask(b, 0xda06, 0x00, 0x01);
    ret |= it9300_wr_reg_mask(b, 0xda1d, 0x00, 0x01);

    ret |= it9300_wr_reg(b, 0xd920, 0x00);            /* reverse: no */
    /* libudtv.so FUN_002aa754: the Hiremco board enables all four power-
     * config registers. The 1/0/1/0 values of the Linux reference do not
     * belong to this board and leave the I2C sub-circuits switched off. */
    ret |= it9300_wr_reg(b, 0xd833, 0x01);
    ret |= it9300_wr_reg(b, 0xd830, 0x01);
    ret |= it9300_wr_reg(b, 0xd831, 0x01);
    ret |= it9300_wr_reg(b, 0xd832, 0x01);
    ret |= it9300_wr_reg(b, 0x4976, 0x01);

    it9300_msleep(20);
    ret |= it9300_wr_reg_mask(b, 0xda58, 0x00, 0x01); /* ts_in_src: serial */
    it9300_msleep(8);
    ret |= it9300_wr_reg(b, 0xda51, 0x00);            /* in ts pkt len */
    it9300_msleep(8);
    ret |= it9300_wr_reg(b, 0xda73, 0x01);            /* ts0_aggre_mode */
    ret |= it9300_wr_reg(b, 0xda78, 0x47);            /* ts0_sync_byte */
    it9300_msleep(30);
    ret |= it9300_wr_reg(b, 0xda4c, 0x01);            /* ts0_en */
    it9300_msleep(8);
    ret |= it9300_wr_reg(b, 0xda5a, 0x1F);            /* ts_fail_ignore */

    /* GPIO5 switches on the external tuner/LNB power rail. A USB cold boot
     * clears it, so the GPIO2 demod reset alone boots the firmware but
     * leaves the RF path dead. */
    ret |= it9300_gpio_set(b, 4 /* GPIO5 */, 1);
    it9300_msleep(100);

    return ret ? -11 : 0;
}

int it9300_demod_power(it9300 *b)
{
    return it9300_gpio_power_cycle(b, 0 /* GPIO1 */);
}

int it9300_gpio_read(it9300 *b, int gpio_idx, uint8_t *level)
{
    if (gpio_idx < 0 || gpio_idx > 15)
        return -1;
    int ret = 0;
    /* native FUN_002a9334 read: mode=1, enable=0, read the input */
    ret |= it9300_wr_reg(b, gpio_mode_regs[gpio_idx], 1);
    ret |= it9300_wr_reg(b, gpio_en_regs[gpio_idx],   0);
    uint8_t v = 0;
    ret |= it9300_rd_reg(b, gpio_in_regs[gpio_idx], &v);
    *level = v ? 1 : 0;
    return ret ? -1 : 0;
}

int it9300_gpio_set(it9300 *b, int gpio_idx, int high)
{
    if (gpio_idx < 0 || gpio_idx > 15)
        return -1;
    int ret = 0;
    ret |= it9300_wr_reg(b, gpio_mode_regs[gpio_idx], 1);   /* OUT */
    ret |= it9300_wr_reg(b, gpio_en_regs[gpio_idx],   1);   /* ENABLE */
    ret |= it9300_wr_reg(b, gpio_out_regs[gpio_idx],  high ? 1 : 0);
    return ret ? -1 : 0;
}

int it9300_gpio_power_cycle(it9300 *b, int gpio_idx)
{
    if (gpio_idx < 0 || gpio_idx > 15)
        return -1;
    int ret = 0;
    ret |= it9300_wr_reg(b, gpio_mode_regs[gpio_idx], 1);  /* OUT */
    ret |= it9300_wr_reg(b, gpio_en_regs[gpio_idx],   1);  /* ENABLE */
    ret |= it9300_wr_reg(b, gpio_out_regs[gpio_idx],  0);  /* LOW */
    it9300_msleep(30);
    ret |= it9300_wr_reg(b, gpio_out_regs[gpio_idx],  1);  /* HIGH */
    it9300_msleep(150);
    return ret ? -12 : 0;
}

int it9300_attach(it9300 *b, dtv_usb *usb)
{
    memset(b, 0, sizeof(*b));
    b->usb = usb;
    b->i2c_bus = 0x03;   /* default I2C bus of the IT930x (kernel comment) */
    return 0;
}
