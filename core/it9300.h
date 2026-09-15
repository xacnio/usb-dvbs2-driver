/* it9300.h - ITE IT9300/IT930x USB bridge driver
 *
 * A libusb port of the Linux kernel dvb-usb-v2 it930x/af9035 driver.
 * Reference: nxdong520/avl6381 (GPL-2.0), torvalds/linux af9035.c.
 *
 * The bridge talks to the host over a command protocol: OUT:
 * [len][mbox][cmd][seq][payload...][cksum_hi][cksum_lo] IN :
 * [len][seq][status][payload...][cksum_hi][cksum_lo] and provides an I2C
 * bridge to the demod/tuner chips behind it. */
#ifndef IT9300_H
#define IT9300_H

#include <stdint.h>
#include "usb_backend.h"

/* USB commands (identical to the kernel it930x.h) */
#define CMD_MEM_RD          0x00
#define CMD_MEM_WR          0x01
#define CMD_I2C_RD          0x02
#define CMD_I2C_WR          0x03
#define CMD_IR_GET          0x18
#define CMD_FW_DL           0x21
#define CMD_FW_QUERYINFO    0x22
#define CMD_FW_BOOT         0x23
#define CMD_FW_DL_BEGIN     0x24
#define CMD_FW_DL_END       0x25
#define CMD_FW_SCATTER_WR   0x29
#define CMD_GENERIC_I2C_RD  0x2a
#define CMD_GENERIC_I2C_WR  0x2b

typedef struct it9300 {
    dtv_usb *usb;
    uint8_t  seq;             /* packet sequence number */
    uint8_t  chip_version;
    uint16_t chip_type;
    uint8_t  fw_ver[4];       /* firmware version after boot */
    uint8_t  i2c_bus;         /* generic I2C sub-header bus byte (IT930x: 0x03) */
} it9300;

/* Low-level command exchange.
 *   cmd       : a CMD_* constant
 *   mbox      : processor/mailbox selection (reg>>16 & 0xff, or the I2C
 *               address bit)
 *   wbuf/wlen : payload to send
 *   rbuf/rlen : expected reply payload (0 = no reply)
 * Returns 0 on success, <0 on error, 1 when the firmware reports no data
 * (IR and the like). */
int it9300_ctrl_msg(it9300 *b, uint8_t cmd, uint8_t mbox,
                    const uint8_t *wbuf, int wlen,
                    uint8_t *rbuf, int rlen);

/* Register access (reg = 16-bit address plus mbox in the high byte). */
int it9300_wr_regs(it9300 *b, uint32_t reg, const uint8_t *val, int len);
int it9300_rd_regs(it9300 *b, uint32_t reg, uint8_t *val, int len);
int it9300_wr_reg (it9300 *b, uint32_t reg, uint8_t val);
int it9300_rd_reg (it9300 *b, uint32_t reg, uint8_t *val);
int it9300_wr_reg_mask(it9300 *b, uint32_t reg, uint8_t val, uint8_t mask);

/* Demod/tuner I2C bridge. addr is the 7-bit I2C address. */
int it9300_i2c_read (it9300 *b, uint8_t addr, uint8_t *val, int len);
int it9300_i2c_write(it9300 *b, uint8_t addr, const uint8_t *data, int len);

/* Repeated-start write+read: wbuf (usually a register pointer) first, then
 * rbuf. The correct read path for register-based demod/tuner chips. */
int it9300_i2c_wr_rd(it9300 *b, uint8_t addr,
                     const uint8_t *wbuf, int wlen,
                     uint8_t *rbuf, int rlen);

/* Load the firmware (an ITE .fw file: consecutive
 * [core,addr,len,cksum]+data blocks), then boot it and read the version. */
int it9300_download_firmware(it9300 *b, const uint8_t *fw, size_t fw_size);
int it9300_boot(it9300 *b);

/* Version of the running firmware, without sending BOOT. 0 = running
 * (b->fw_ver filled in), <0 = not running or an error. */
int it9300_query_fw_version(it9300 *b);

/* Open the bridge: attach the USB backend and read the chip ID. It loads NO
 * firmware. */
int  it9300_attach(it9300 *b, dtv_usb *usb);

/* Read the chip identity (prechip/chip version, chip_type). */
int it9300_identify(it9300 *b);

/* Bridge init AFTER the firmware (kernel it930x_init): I2C master clocks,
 * TS port (ts0), EP4 bulk, frame/packet size, power config. */
int it9300_bridge_init(it9300 *b);

/* Demod power cycle: GPIO1 LOW (reset) -> 30 ms -> HIGH -> 150 ms boot
 * wait. The demod does not answer on I2C before this completes. */
int it9300_demod_power(it9300 *b);

/* Power-cycle one GPIO (0=GPIO1 .. 15=GPIO16) with LOW, 30 ms, HIGH, 150
 * ms. For finding the GPIO that feeds the demod empirically. */
int it9300_gpio_power_cycle(it9300 *b, int gpio_idx);

/* Set one GPIO (0=GPIO1..15=GPIO16) to OUT+ENABLE and drive a level (no
 * power cycle). */
int it9300_gpio_set(it9300 *b, int gpio_idx, int high);

/* Read the GPIO input level (0/1). For detecting the board straps. */
int it9300_gpio_read(it9300 *b, int gpio_idx, uint8_t *level);

#endif /* IT9300_H */
