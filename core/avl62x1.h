/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef AVL62X1_H
#define AVL62X1_H

#include <stddef.h>
#include <stdint.h>
#include "it9300.h"

#define AVL62X1_I2C_ADDR 0x14
#define AVL62X1_CHIP_ID  0x62615ca8u

typedef struct avl62x1_patch_info {
    uint32_t words;
    uint32_t standard;
    uint32_t args_addr;
    uint32_t data_offset_words;
    uint8_t major;
    uint8_t minor;
    uint16_t build;
    uint32_t chip_id;
} avl62x1_patch_info;

typedef struct avl62x1_signal_status {
    uint8_t locked;
    uint8_t fec_locked;
    uint8_t frame_locked;
    int16_t snr_db_x100;
    uint32_t symbol_rate_hz;
} avl62x1_signal_status;

typedef struct avl62x1_ts_config {
    uint8_t mode;              /* 0=parallel, 1=serial, 2=2-bit serial */
    uint8_t format;            /* 0=188-byte TS, 1=TS plus parity */
    uint8_t clock_rising;
    uint8_t clock_phase;       /* 0..3 */
    uint8_t adaptive_clock;
    uint8_t error_inverted;
    uint8_t valid_inverted;
    uint8_t serial_data_pin;   /* 0=MPEG_DATA_0, 1=MPEG_DATA_7 */
    uint8_t serial_msb_first;
} avl62x1_ts_config;

int avl62x1_read8(it9300 *bridge, uint8_t addr, uint32_t reg, uint8_t *value);
int avl62x1_read16(it9300 *bridge, uint8_t addr, uint32_t reg, uint16_t *value);
int avl62x1_read32(it9300 *bridge, uint8_t addr, uint32_t reg, uint32_t *value);
int avl62x1_write8(it9300 *bridge, uint8_t addr, uint32_t reg, uint8_t value);
int avl62x1_write16(it9300 *bridge, uint8_t addr, uint32_t reg, uint16_t value);
int avl62x1_write32(it9300 *bridge, uint8_t addr, uint32_t reg, uint32_t value);

int avl62x1_parse_patch(const uint8_t *patch, size_t patch_size,
                        avl62x1_patch_info *info);
int avl62x1_load_patch(it9300 *bridge, uint8_t addr,
                       const uint8_t *patch, size_t patch_size);

/* Pace the short USB/I2C requests made while a receiver is cold-starting.
 * It is deliberately off during normal tuning and signal polling. */
void avl62x1_set_cold_init_pacing(int enabled);
int avl62x1_wait_ready(it9300 *bridge, uint8_t addr,
                       unsigned retries, unsigned delay_ms);
int avl62x1_get_running_version(it9300 *bridge, uint8_t addr,
                                uint8_t *major, uint8_t *minor,
                                uint16_t *build);
int avl62x1_load_defaults(it9300 *bridge, uint8_t addr,
                          uint8_t ref_clock, uint32_t requested_mpeg_clock_hz,
                          uint32_t *core_clock_hz, uint32_t *fec_clock_hz,
                          uint32_t *mpeg_clock_hz);
int avl62x1_init_tuner_i2c(it9300 *bridge, uint8_t addr,
                           uint32_t core_clock_hz);
int avl62x1_set_tuner_i2c(it9300 *bridge, uint8_t addr, int enable);
int avl62x1_init_demod_input(it9300 *bridge, uint8_t addr,
                             int spectrum_inverted);
int avl62x1_acquire(it9300 *bridge, uint8_t addr,
                    uint32_t symbol_rate_hz, int32_t carrier_offset_hz,
                    int blind_symbol_rate);
int avl62x1_get_signal_status(it9300 *bridge, uint8_t addr,
                              avl62x1_signal_status *status);
int avl62x1_configure_ts(it9300 *bridge, uint8_t addr,
                         const avl62x1_ts_config *config, int enable_output);
int avl62x1_set_lnb_voltage(it9300 *bridge, uint8_t addr, unsigned volts);
int avl62x1_init_diseqc(it9300 *bridge, uint8_t addr,
                        uint32_t core_clock_hz);
int avl62x1_set_22khz_tone(it9300 *bridge, uint8_t addr, int enable);
int avl62x1_send_diseqc(it9300 *bridge, uint8_t addr,
                        const uint8_t *message, size_t message_size);
int avl62x1_select_diseqc_1_0_port(it9300 *bridge, uint8_t addr,
                                   unsigned port, int horizontal,
                                   int high_band);
/* DiSEqC 1.0 "committed" switch (4 ports) and 1.1 "uncommitted" switch (16
 * ports). Cascaded they reach up to 64 inputs. repeated=1 sends the repeat
 * frame (0xE1): the standard remedy for switches that miss the first
 * command in a cascaded setup. */
int avl62x1_select_diseqc_committed(it9300 *bridge, uint8_t addr,
                                    unsigned port, int horizontal,
                                    int high_band, int repeated);
int avl62x1_select_diseqc_uncommitted(it9300 *bridge, uint8_t addr,
                                      unsigned port, int repeated);
/* Tone burst (mini DiSEqC): 0 = A (tone0), 1 = B (tone1). */
int avl62x1_send_tone_burst(it9300 *bridge, uint8_t addr, int second_port);

#endif
