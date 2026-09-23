/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * AVL62x1 patch-script loader for the IT9303 userspace bridge.
 * The command format and register definitions follow Availink's GPL driver:
 * https://github.com/availink/dvb-frontends-availink
 */
#include "avl62x1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dtv_platform.h"

#define AVL_REG_COMMAND       0x000200u
#define AVL_REG_ARGS_ADDR     0x000258u
#define AVL_REG_PATCH_VERSION 0x0000a8u
#define AVL_REG_CPU_RESET     0x110840u
#define AVL_REG_DMA_STATUS    0x110048u
#define AVL_REG_DMA_COMMAND   0x110050u
#define AVL_REG_READY_WORD    0x0000a0u
#define AVL_REG_XTAL          0x000205u
#define AVL_REG_SYS_CLOCK     0x00083cu
#define AVL_REG_FEC_CLOCK     0x000840u
#define AVL_REG_MPEG_CLOCK    0x000848u

#define AVL_REG_TUNER_SRST    0x118000u
#define AVL_REG_TUNER_CONTROL 0x118004u
#define AVL_REG_TUNER_DIVIDER 0x118018u
#define AVL_REG_TUNER_REPEATER 0x11801cu
#define AVL_REG_GPIO_I2C_DATA 0x120014u
#define AVL_REG_GPIO_I2C_CLOCK 0x120018u
#define AVL_REG_GPIO_M3_SCL   0x12009cu
#define AVL_REG_GPIO_M3_SDA   0x1200a0u

#define AVL_REG_INPUT_FORMAT        0x00082du
#define AVL_REG_TUNER_SPECTRUM      0x00082eu
#define AVL_REG_INPUT_SELECT        0x00082fu
#define AVL_REG_RF_AGC_POLARITY     0x00080fu
#define AVL_REG_RF_AGC_MAX_GAIN     0x000864u
#define AVL_REG_RF_AGC_MIN_GAIN     0x000866u
#define AVL_REG_AAGC_SD_CONTROL     0x160900u
#define AVL_REG_GPIO_AGC1           0x120000u
#define AVL_REG_GPIO_AGC2           0x120010u

#define AVL_REG_BLIND_SYMBOL_RATE   0x000851u
#define AVL_REG_BLIND_CFO           0x000852u
#define AVL_REG_NOM_SYMBOL_RATE     0x000804u
#define AVL_REG_NOM_CARRIER_OFFSET  0x000830u
#define AVL_REG_PL_SCRAMBLE         0x00085cu
#define AVL_REG_STREAM_RAW_T2MI     0xa00604u
#define AVL_REG_STREAM_TYPE         0xa00605u
#define AVL_REG_STREAM_ID           0xa0060au

#define AVL_REG_LOCK                0x000408u
#define AVL_REG_FEC_LOCK            0x00040au
#define AVL_REG_FRAME_LOCK          0x00040bu
#define AVL_REG_SNR_DB_X100         0x000412u
#define AVL_REG_SYMBOL_RATE         0x00041cu

#define AVL_REG_TS_FORMAT           0xa00209u
#define AVL_REG_TS_CONTINUOUS       0xa0020au
#define AVL_REG_TS_CLOCK_EDGE       0xa0020bu
#define AVL_REG_TS_MODE             0xa0020cu
#define AVL_REG_TS_BIT_ORDER        0xa0020eu
#define AVL_REG_TS_SERIAL_PIN       0xa0020fu
#define AVL_REG_TS_VALID_POLARITY   0xa00210u
#define AVL_REG_TS_ERROR_POLARITY   0xa00211u
#define AVL_REG_TS_DRIVER_STRENGTH  0xa00236u
#define AVL_REG_TS_CLOCK_ADAPT      0xa00235u
#define AVL_REG_TS_CLOCK_PHASE      0xa00237u
#define AVL_REG_TS_BUS_OFF          0xb08410u
#define AVL_REG_TS_BUS_ENABLE_BITS  0xb08420u

#define AVL_REG_LNB_POWER_SELECT    0x120008u
#define AVL_REG_LNB_POWER_ENABLE    0x12000cu

#define AVL_REG_DISEQC_TX_CONTROL   0x16c000u
#define AVL_REG_DISEQC_TONE_FRAC_N  0x16c004u
#define AVL_REG_DISEQC_TONE_FRAC_D  0x16c008u
#define AVL_REG_DISEQC_TX_STATUS    0x16c00cu
#define AVL_REG_DISEQC_RX_TIMEOUT   0x16c018u
#define AVL_REG_DISEQC_RX_CONTROL   0x16c01cu
#define AVL_REG_DISEQC_RESET        0x16c020u
#define AVL_REG_DISEQC_SAMPLE_N     0x16c028u
#define AVL_REG_DISEQC_SAMPLE_D     0x16c02cu
#define AVL_REG_DISEQC_TX_FIFO      0x16c080u

#define AVL_READY_VALUE       0x5aa57ff7u

#define CMD_IDLE       0u
#define CMD_LD_DEFAULT 1u
#define CMD_ACQUIRE    2u
#define CMD_HALT       3u
#define CMD_DMA        21u
#define CMD_CALC_CRC   22u
#define CMD_PING       23u
#define CMD_DECOMPRESS 24u
#define CMD_FAILED     255u

#define PATCH_VALIDATE_CRC       0u
#define PATCH_PING               1u
#define PATCH_LOAD               2u
#define PATCH_DMA                3u
#define PATCH_EXTRACT            4u
#define PATCH_ASSERT_RESET       5u
#define PATCH_RELEASE_RESET      6u
#define PATCH_LOAD_IMMEDIATE     7u
#define PATCH_READ               8u
#define PATCH_DMA_HW             9u
#define PATCH_SET_CONDITION     10u
#define PATCH_EXIT              11u
#define PATCH_POLL              12u
#define PATCH_LOAD_PACKED       13u

#define OP_ADDR_VARIABLE 0u
#define OP_UNARY_NOT     1u
#define OP_UNARY_INVERT  2u
#define OP_LOAD           0u
#define OP_AND            1u
#define OP_OR             2u
#define OP_BIT_AND        3u
#define OP_BIT_OR         4u
#define OP_EQUALS         5u
#define OP_STORE          6u
#define OP_NOT_EQUALS     7u

#define PATCH_VARIABLES 32u
/* The vendor IT9303 firmware rejects the 61-byte I2C messages allowed by the
 * host protocol. Keep AVL bursts at the conservative 32-byte device payload
 * used by the native path; the 24-bit address is advanced for every chunk. */
#define AVL_WRITE_DATA_MAX 32u

/* The AVL patch is a second firmware upload after the IT9303 bridge is up.
 * It consists of many short I2C transactions through that same USB bridge.
 * Keep this deliberately scoped to patch loading: signal polling and normal
 * tuning must not acquire sleeps just because they write a register. */
#define AVL_PATCH_BREATHE_EVERY 8u
#define AVL_PATCH_BREATHE_MS    1u
static int avl_patch_upload_active;
static int avl_cold_init_pacing;
static unsigned avl_patch_write_count;

void avl62x1_set_cold_init_pacing(int enabled)
{
    avl_cold_init_pacing = enabled != 0;
    if (avl_cold_init_pacing)
        avl_patch_write_count = 0;
}

typedef struct patch_reader {
    const uint8_t *data;
    size_t size;
    size_t pos;
} patch_reader;

static uint32_t be32_at(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int read_word(patch_reader *r, uint32_t *value)
{
    if (!r || !value || r->pos > r->size || r->size - r->pos < 4)
        return -2;
    *value = be32_at(r->data + r->pos);
    r->pos += 4;
    return 0;
}

static int skip_words(patch_reader *r, uint32_t words)
{
    size_t bytes = (size_t)words * 4;
    if (r->pos > r->size || bytes > r->size - r->pos)
        return -2;
    r->pos += bytes;
    return 0;
}

static int avl_write(it9300 *bridge, uint8_t addr, uint32_t reg,
                     const uint8_t *data, size_t size)
{
    uint8_t packet[3 + AVL_WRITE_DATA_MAX];

    if (!bridge || (!data && size))
        return -1;

    while (size) {
        size_t chunk = size > AVL_WRITE_DATA_MAX ? AVL_WRITE_DATA_MAX : size;
        packet[0] = (uint8_t)(reg >> 16);
        packet[1] = (uint8_t)(reg >> 8);
        packet[2] = (uint8_t)reg;
        memcpy(packet + 3, data, chunk);
        int rc = it9300_i2c_write(bridge, addr, packet, (int)chunk + 3);
        if (rc != 0)
            return rc;
        if ((avl_patch_upload_active || avl_cold_init_pacing) &&
            (++avl_patch_write_count % AVL_PATCH_BREATHE_EVERY) == 0)
            dtv_sleep_ms(AVL_PATCH_BREATHE_MS);
        reg += (uint32_t)chunk;
        data += chunk;
        size -= chunk;
    }
    return 0;
}

static int avl_read(it9300 *bridge, uint8_t addr, uint32_t reg,
                    uint8_t *data, size_t size)
{
    uint8_t pointer[3] = {
        (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg
    };
    if (!bridge || !data || size == 0 || size > 60)
        return -1;
    return it9300_i2c_wr_rd(bridge, addr, pointer, 3, data, (int)size);
}

int avl62x1_read8(it9300 *bridge, uint8_t addr, uint32_t reg, uint8_t *value)
{
    return avl_read(bridge, addr, reg, value, 1);
}

int avl62x1_read16(it9300 *bridge, uint8_t addr, uint32_t reg, uint16_t *value)
{
    uint8_t data[2];
    int rc = avl_read(bridge, addr, reg, data, sizeof(data));
    if (rc == 0)
        *value = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    return rc;
}

int avl62x1_read32(it9300 *bridge, uint8_t addr, uint32_t reg, uint32_t *value)
{
    uint8_t data[4];
    int rc = avl_read(bridge, addr, reg, data, sizeof(data));
    if (rc == 0)
        *value = be32_at(data);
    return rc;
}

int avl62x1_write8(it9300 *bridge, uint8_t addr, uint32_t reg, uint8_t value)
{
    return avl_write(bridge, addr, reg, &value, 1);
}

int avl62x1_write16(it9300 *bridge, uint8_t addr, uint32_t reg, uint16_t value)
{
    uint8_t data[2] = { (uint8_t)(value >> 8), (uint8_t)value };
    return avl_write(bridge, addr, reg, data, sizeof(data));
}

int avl62x1_write32(it9300 *bridge, uint8_t addr, uint32_t reg, uint32_t value)
{
    uint8_t data[4] = {
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8), (uint8_t)value
    };
    return avl_write(bridge, addr, reg, data, sizeof(data));
}

int avl62x1_parse_patch(const uint8_t *patch, size_t patch_size,
                        avl62x1_patch_info *info)
{
    if (!patch || !info || patch_size < 40)
        return -1;

    memset(info, 0, sizeof(*info));
    info->words = be32_at(patch + 4);
    info->standard = be32_at(patch + 8);
    info->args_addr = be32_at(patch + 12);
    info->data_offset_words = be32_at(patch + 16);
    info->major = patch[24];
    info->minor = patch[25];
    info->build = (uint16_t)(((uint16_t)patch[26] << 8) | patch[27]);
    info->chip_id = be32_at(patch + 28);

    if ((size_t)info->words * 4 > patch_size ||
        info->data_offset_words >= info->words)
        return -2;
    return 0;
}

static int get_command_status(it9300 *bridge, uint8_t addr)
{
    uint16_t command = 0;
    int rc = avl62x1_read16(bridge, addr, AVL_REG_COMMAND, &command);
    if (rc != 0)
        return rc;
    if (command == CMD_FAILED)
        return -4;
    return command == CMD_IDLE ? 0 : 1;
}

static int wait_command_idle(it9300 *bridge, uint8_t addr)
{
    for (unsigned i = 0; i <= 50; ++i) {
        int rc = get_command_status(bridge, addr);
        if (rc <= 0)
            return rc;
        dtv_sleep_ms(10);
    }
    return -3;
}

static int send_command(it9300 *bridge, uint8_t addr, uint8_t command)
{
    int rc = wait_command_idle(bridge, addr);
    if (rc != 0)
        return rc;
    rc = avl62x1_write16(bridge, addr, AVL_REG_COMMAND, command);
    if (rc != 0)
        return rc;
    return wait_command_idle(bridge, addr);
}

int avl62x1_wait_ready(it9300 *bridge, uint8_t addr,
                       unsigned retries, unsigned delay_ms)
{
    for (unsigned i = 0; i <= retries; ++i) {
        uint32_t reset = 1, ready = 0;
        int rc = avl62x1_read32(bridge, addr, AVL_REG_CPU_RESET, &reset);
        if (rc != 0)
            return rc;
        rc = avl62x1_read32(bridge, addr, AVL_REG_READY_WORD, &ready);
        if (rc != 0)
            return rc;
        if (reset == 0 && ready == AVL_READY_VALUE)
            return 0;
        if (i != retries)
            dtv_sleep_ms(delay_ms);
    }
    return -3;
}

int avl62x1_get_running_version(it9300 *bridge, uint8_t addr,
                                uint8_t *major, uint8_t *minor,
                                uint16_t *build)
{
    uint32_t value = 0;
    int rc = avl62x1_read32(bridge, addr, AVL_REG_PATCH_VERSION, &value);
    if (rc == 0) {
        if (major) *major = (uint8_t)(value >> 24);
        if (minor) *minor = (uint8_t)(value >> 16);
        if (build) *build = (uint16_t)value;
    }
    return rc;
}

int avl62x1_load_defaults(it9300 *bridge, uint8_t addr,
                          uint8_t ref_clock, uint32_t requested_mpeg_clock_hz,
                          uint32_t *core_clock_hz, uint32_t *fec_clock_hz,
                          uint32_t *mpeg_clock_hz)
{
    uint32_t core = 0, fec = 0, mpeg = 0;
    int rc;

    if (ref_clock > 3 || requested_mpeg_clock_hz == 0)
        return -1;
    if ((rc = avl62x1_write32(bridge, addr, AVL_REG_MPEG_CLOCK,
                              requested_mpeg_clock_hz)) != 0 ||
        (rc = avl62x1_write8(bridge, addr, AVL_REG_XTAL, ref_clock)) != 0 ||
        (rc = send_command(bridge, addr, CMD_LD_DEFAULT)) != 0 ||
        (rc = avl62x1_read32(bridge, addr, AVL_REG_SYS_CLOCK, &core)) != 0 ||
        (rc = avl62x1_read32(bridge, addr, AVL_REG_FEC_CLOCK, &fec)) != 0 ||
        (rc = avl62x1_read32(bridge, addr, AVL_REG_MPEG_CLOCK, &mpeg)) != 0)
        return rc;

    if (core_clock_hz) *core_clock_hz = core;
    if (fec_clock_hz) *fec_clock_hz = fec;
    if (mpeg_clock_hz) *mpeg_clock_hz = mpeg;
    return 0;
}

int avl62x1_init_tuner_i2c(it9300 *bridge, uint8_t addr,
                           uint32_t core_clock_hz)
{
    uint32_t control = 0;
    uint32_t divider;
    int rc;

    if (core_clock_hz == 0)
        return -1;
    divider = 0x2au * (core_clock_hz / 1000u) / (240u * 1000u);

    if ((rc = avl62x1_write32(bridge, addr, AVL_REG_TUNER_SRST, 1)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_TUNER_SRST, 0)) != 0 ||
        (rc = avl62x1_read32(bridge, addr, AVL_REG_TUNER_CONTROL,
                             &control)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_TUNER_CONTROL,
                              control & 0xfffffffeu)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_TUNER_REPEATER, 6)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_TUNER_DIVIDER,
                              divider)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_GPIO_I2C_CLOCK, 7)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_GPIO_I2C_DATA, 8)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_GPIO_M3_SCL, 6)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_GPIO_M3_SDA, 5)) != 0)
        return rc;
    return 0;
}

int avl62x1_set_tuner_i2c(it9300 *bridge, uint8_t addr, int enable)
{
    uint32_t value = enable ? 7u : 6u;
    int rc = 0;

    /* The Availink SDK intentionally writes this latch three times. */
    for (unsigned i = 0; i < 3; ++i) {
        rc = avl62x1_write32(bridge, addr, AVL_REG_TUNER_REPEATER, value);
        if (rc != 0)
            return rc;
    }
    return 0;
}

int avl62x1_init_demod_input(it9300 *bridge, uint8_t addr,
                             int spectrum_inverted)
{
    uint32_t agc_control = 0;
    int rc;

    /* RDA5815M has the default negative AGC slope used by Availink when the
     * tuner does not expose gain-voltage callbacks. The gain limits below are
     * the SDK defaults for 3200 mV minimum and 100 mV maximum gain. */
    const uint16_t min_gain = (uint16_t)((100u * 65536u) / 3300u);
    const uint16_t max_gain = (uint16_t)((3200u * 65536u) / 3300u);

    if ((rc = avl62x1_write8(bridge, addr, AVL_REG_INPUT_FORMAT, 0)) != 0 ||
        (rc = avl62x1_write8(bridge, addr, AVL_REG_INPUT_SELECT, 1)) != 0 ||
        (rc = avl62x1_write8(bridge, addr, AVL_REG_TUNER_SPECTRUM,
                             spectrum_inverted ? 1 : 0)) != 0 ||
        (rc = avl62x1_write8(bridge, addr, AVL_REG_RF_AGC_POLARITY, 1)) != 0 ||
        (rc = avl62x1_write16(bridge, addr, AVL_REG_RF_AGC_MIN_GAIN,
                              min_gain)) != 0 ||
        (rc = avl62x1_write16(bridge, addr, AVL_REG_RF_AGC_MAX_GAIN,
                              max_gain)) != 0 ||
        (rc = avl62x1_read32(bridge, addr, AVL_REG_AAGC_SD_CONTROL,
                             &agc_control)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_AAGC_SD_CONTROL,
                              agc_control | (1u << 1))) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_GPIO_AGC1, 6)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_GPIO_AGC2, 6)) != 0)
        return rc;
    return 0;
}

int avl62x1_acquire(it9300 *bridge, uint8_t addr,
                    uint32_t symbol_rate_hz, int32_t carrier_offset_hz,
                    int blind_symbol_rate)
{
    int rc;

    if (symbol_rate_hz == 0)
        return -1;
    if ((rc = send_command(bridge, addr, CMD_HALT)) != 0 ||
        (rc = avl62x1_write8(bridge, addr, AVL_REG_BLIND_SYMBOL_RATE,
                             blind_symbol_rate ? 1 : 0)) != 0 ||
        (rc = avl62x1_write8(bridge, addr, AVL_REG_BLIND_CFO, 1)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_NOM_SYMBOL_RATE,
                              symbol_rate_hz)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_NOM_CARRIER_OFFSET,
                              (uint32_t)carrier_offset_hz)) != 0 ||
        /* Bit 23 asks firmware to discover the PL scrambling sequence. */
        (rc = avl62x1_write32(bridge, addr, AVL_REG_PL_SCRAMBLE,
                              0x00800000u)) != 0 ||
        /* No MIS/T2MI selection: discover the ordinary/default stream. */
        (rc = avl62x1_write8(bridge, addr, AVL_REG_STREAM_TYPE, 0xff)) != 0 ||
        (rc = avl62x1_write8(bridge, addr, AVL_REG_STREAM_ID, 0)) != 0 ||
        (rc = avl62x1_write8(bridge, addr, AVL_REG_STREAM_RAW_T2MI, 0)) != 0)
        return rc;
    return send_command(bridge, addr, CMD_ACQUIRE);
}

int avl62x1_get_signal_status(it9300 *bridge, uint8_t addr,
                              avl62x1_signal_status *status)
{
    uint16_t snr = 0;
    int rc;

    if (!status)
        return -1;
    memset(status, 0, sizeof(*status));
    if ((rc = avl62x1_read8(bridge, addr, AVL_REG_LOCK,
                            &status->locked)) != 0 ||
        (rc = avl62x1_read8(bridge, addr, AVL_REG_FEC_LOCK,
                            &status->fec_locked)) != 0 ||
        (rc = avl62x1_read8(bridge, addr, AVL_REG_FRAME_LOCK,
                            &status->frame_locked)) != 0 ||
        (rc = avl62x1_read16(bridge, addr, AVL_REG_SNR_DB_X100,
                             &snr)) != 0 ||
        (rc = avl62x1_read32(bridge, addr, AVL_REG_SYMBOL_RATE,
                             &status->symbol_rate_hz)) != 0)
        return rc;
    status->locked = status->locked != 0;
    status->fec_locked = status->fec_locked != 0;
    status->frame_locked = status->frame_locked != 0;
    status->snr_db_x100 = (int16_t)snr;
    return 0;
}

int avl62x1_configure_ts(it9300 *bridge, uint8_t addr,
                         const avl62x1_ts_config *config, int enable_output)
{
    int rc;

    if (!config || config->mode > 2 || config->format > 1 ||
        config->clock_phase > 3 || config->serial_data_pin > 1)
        return -1;

    if ((rc = avl62x1_write8(bridge, addr, AVL_REG_TS_MODE,
                             config->mode)) != 0 ||
        (rc = avl62x1_write8(bridge, addr, AVL_REG_TS_FORMAT,
                             config->format)) != 0 ||
        (rc = avl62x1_write8(bridge, addr, AVL_REG_TS_CLOCK_EDGE,
                             config->clock_rising ? 1 : 0)) != 0 ||
        (rc = avl62x1_write8(bridge, addr, AVL_REG_TS_CLOCK_PHASE,
                             config->clock_phase)) != 0 ||
        (rc = avl62x1_write8(bridge, addr, AVL_REG_TS_CLOCK_ADAPT,
                             config->adaptive_clock ? 1 : 0)) != 0 ||
        /* This is the public SDK's recommended stronger MPEG pad drive. */
        (rc = avl62x1_write8(bridge, addr, AVL_REG_TS_DRIVER_STRENGTH,
                             3)) != 0 ||
        (rc = avl62x1_write8(bridge, addr, AVL_REG_TS_ERROR_POLARITY,
                             config->error_inverted ? 1 : 0)) != 0 ||
        (rc = avl62x1_write8(bridge, addr, AVL_REG_TS_VALID_POLARITY,
                             config->valid_inverted ? 1 : 0)) != 0)
        return rc;

    if (config->mode == 1) {
        if ((rc = avl62x1_write8(bridge, addr, AVL_REG_TS_SERIAL_PIN,
                                 config->serial_data_pin)) != 0 ||
            (rc = avl62x1_write8(bridge, addr, AVL_REG_TS_BIT_ORDER,
                                 config->serial_msb_first ? 1 : 0)) != 0 ||
            (rc = avl62x1_write8(bridge, addr, AVL_REG_TS_CONTINUOUS,
                                 1)) != 0)
            return rc;
    } else if (config->mode == 0) {
        if ((rc = avl62x1_write8(bridge, addr, AVL_REG_TS_CLOCK_ADAPT,
                                 1)) != 0 ||
            (rc = avl62x1_write8(bridge, addr, AVL_REG_TS_CONTINUOUS,
                                 0)) != 0)
            return rc;
    }

    if ((rc = avl62x1_write8(bridge, addr, AVL_REG_TS_BUS_OFF,
                             enable_output ? 0x00 : 0xff)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_TS_BUS_ENABLE_BITS,
                              enable_output ? 0x000 : 0xfff)) != 0)
        return rc;
    return 0;
}

int avl62x1_set_lnb_voltage(it9300 *bridge, uint8_t addr, unsigned volts)
{
    uint32_t enable, select;
    int rc;

    switch (volts) {
    case 0:  enable = 0; select = 0; break;
    case 13: enable = 1; select = 0; break;
    case 18: enable = 1; select = 1; break;
    default: return -1;
    }

    /* AVL GPIO37 is LNB power-enable and GPIO38 selects 13/18 V in the
     * reference frontend. These writes only drive the two external control
     * pins; the high-voltage regulator remains board-side. */
    if ((rc = avl62x1_write32(bridge, addr, AVL_REG_LNB_POWER_ENABLE,
                              enable)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_LNB_POWER_SELECT,
                              select)) != 0)
        return rc;
    return 0;
}

int avl62x1_init_diseqc(it9300 *bridge, uint8_t addr,
                        uint32_t core_clock_hz)
{
    uint32_t tx_control = 0;
    int rc;

    if (core_clock_hz == 0)
        return -1;
    if ((rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_RESET, 1)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_SAMPLE_N,
                              2000000u)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_SAMPLE_D,
                              core_clock_hz)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_TONE_FRAC_N,
                              44u)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_TONE_FRAC_D,
                              core_clock_hz / 1000u)) != 0 ||
        (rc = avl62x1_read32(bridge, addr, AVL_REG_DISEQC_TX_CONTROL,
                             &tx_control)) != 0)
        return rc;

    /* 15 ms gap, normal waveform, gap enabled; pulse FIFO held in reset
     * during setup exactly as in the public SDK. */
    tx_control = (tx_control & 0x00000300u) | 0x28u;
    if ((rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_TX_CONTROL,
                              tx_control)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_TX_CONTROL,
                              tx_control & ~0x20u)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_RX_CONTROL,
                              0x0au)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_RX_TIMEOUT,
                              0u)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_RESET, 0)) != 0)
        return rc;
    return 0;
}

int avl62x1_set_22khz_tone(it9300 *bridge, uint8_t addr, int enable)
{
    uint32_t control = 0;
    int rc = avl62x1_read32(bridge, addr, AVL_REG_DISEQC_TX_CONTROL,
                            &control);
    if (rc != 0)
        return rc;
    if (enable) {
        control = (control & 0xfffffff8u) | 0x03u;
        if ((rc = avl62x1_write32(bridge, addr,
                                  AVL_REG_DISEQC_TX_CONTROL,
                                  control)) != 0)
            return rc;
        control |= (1u << 10);
    } else {
        control &= 0xfffff3ffu;
    }
    return avl62x1_write32(bridge, addr, AVL_REG_DISEQC_TX_CONTROL,
                           control);
}

int avl62x1_send_diseqc(it9300 *bridge, uint8_t addr,
                        const uint8_t *message, size_t message_size)
{
    uint32_t control = 0, rx_control = 0, status = 0;
    int resume_tone = 0;
    int rc;

    if (!message || message_size == 0 || message_size > 8)
        return -1;

    if ((rc = avl62x1_read32(bridge, addr, AVL_REG_DISEQC_TX_CONTROL,
                             &control)) != 0)
        return rc;

    /* A continuous 22 kHz wave must be stopped before a modulated command.
     * Preserve it for callers that use this low-level function directly. */
    if ((control & (1u << 10)) != 0) {
        resume_tone = 1;
        control &= 0xfffff3ffu;
        if ((rc = avl62x1_write32(bridge, addr,
                                  AVL_REG_DISEQC_TX_CONTROL,
                                  control)) != 0)
            return rc;
        dtv_sleep_ms(20);
    }

    /* Reset the receive FIFO, select modulation/FIFO-load mode, then append
     * one byte per 32-bit FIFO write as prescribed by the public AVL SDK. */
    if ((rc = avl62x1_read32(bridge, addr, AVL_REG_DISEQC_RX_CONTROL,
                             &rx_control)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_RX_CONTROL,
                              rx_control | 1u)) != 0 ||
        (rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_RX_CONTROL,
                              rx_control & ~1u)) != 0)
        return rc;

    control &= 0xfffffff8u;
    if ((rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_TX_CONTROL,
                              control)) != 0)
        return rc;
    for (size_t i = 0; i < message_size; ++i) {
        if ((rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_TX_FIFO,
                                  message[i])) != 0)
            return rc;
    }

    if ((rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_TX_CONTROL,
                              control | (1u << 2))) != 0)
        return rc;
    for (unsigned elapsed = 0; elapsed < 500; ++elapsed) {
        dtv_sleep_ms(1);
        if ((rc = avl62x1_read32(bridge, addr, AVL_REG_DISEQC_TX_STATUS,
                                 &status)) != 0)
            return rc;
        if ((status & 0x40u) != 0)
            break;
        if (elapsed == 499)
            return -2;
    }
    dtv_sleep_ms(20);

    if (resume_tone)
        return avl62x1_set_22khz_tone(bridge, addr, 1);
    return 0;
}

int avl62x1_select_diseqc_committed(it9300 *bridge, uint8_t addr,
                                    unsigned port, int horizontal,
                                    int high_band, int repeated)
{
    uint8_t message[4];

    if (port < 1 || port > 4)
        return -1;
    /* 0xe0 is the first transmission and 0xe1 a repeat: cascaded switches
     * sometimes miss the first command, which is what the repeat frame is
     * for. */
    message[0] = repeated ? 0xe1 : 0xe0;
    message[1] = 0x10; /* any LNB/switch */
    message[2] = 0x38; /* write committed switch N0 */
    message[3] = (uint8_t)(0xf0u | ((port - 1u) << 2) |
                           (horizontal ? 0x02u : 0u) |
                           (high_band ? 0x01u : 0u));
    return avl62x1_send_diseqc(bridge, addr, message, sizeof(message));
}

int avl62x1_select_diseqc_uncommitted(it9300 *bridge, uint8_t addr,
                                      unsigned port, int repeated)
{
    uint8_t message[4];

    if (port < 1 || port > 16)
        return -1;
    message[0] = repeated ? 0xe1 : 0xe0;
    message[1] = 0x10;
    message[2] = 0x39; /* write uncommitted switch N1 (DiSEqC 1.1) */
    message[3] = (uint8_t)(0xf0u | (port - 1u));
    return avl62x1_send_diseqc(bridge, addr, message, sizeof(message));
}

int avl62x1_send_tone_burst(it9300 *bridge, uint8_t addr, int second_port)
{
    /* Bits 0-2 of TX control select the waveform: 0 modulation, 1 tone0,
     * 2 tone1, 3 continuous 22 kHz. The tone burst is the pair between the
     * two used elsewhere in this file. */
    uint32_t control = 0, status = 0;
    int resume_tone = 0;
    int rc = avl62x1_read32(bridge, addr, AVL_REG_DISEQC_TX_CONTROL,
                            &control);
    if (rc != 0)
        return rc;
    if ((control & (1u << 10)) != 0) {
        resume_tone = 1;
        control &= 0xfffff3ffu;
        if ((rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_TX_CONTROL,
                                  control)) != 0)
            return rc;
        dtv_sleep_ms(20);
    }
    control = (control & 0xfffffff8u) | (second_port ? 0x02u : 0x01u);
    if ((rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_TX_CONTROL,
                              control)) != 0)
        return rc;
    if ((rc = avl62x1_write32(bridge, addr, AVL_REG_DISEQC_TX_CONTROL,
                              control | (1u << 2))) != 0)
        return rc;
    for (unsigned elapsed = 0; elapsed < 500; ++elapsed) {
        dtv_sleep_ms(1);
        if ((rc = avl62x1_read32(bridge, addr, AVL_REG_DISEQC_TX_STATUS,
                                 &status)) != 0)
            return rc;
        if ((status & 0x40u) != 0)
            break;
        if (elapsed == 499)
            return -2;
    }
    dtv_sleep_ms(20);
    if (resume_tone)
        return avl62x1_set_22khz_tone(bridge, addr, 1);
    return 0;
}

int avl62x1_select_diseqc_1_0_port(it9300 *bridge, uint8_t addr,
                                   unsigned port, int horizontal,
                                   int high_band)
{
    return avl62x1_select_diseqc_committed(bridge, addr, port, horizontal,
                                           high_band, 0);
}

static int read_result_value(it9300 *bridge, uint8_t addr, uint32_t length,
                             uint32_t reg, uint32_t *value)
{
    if (length == 4)
        return avl62x1_read32(bridge, addr, reg, value);
    if (length == 2) {
        uint16_t v = 0;
        int rc = avl62x1_read16(bridge, addr, reg, &v);
        *value = v;
        return rc;
    }
    if (length == 1) {
        uint8_t v = 0;
        int rc = avl62x1_read8(bridge, addr, reg, &v);
        *value = v;
        return rc;
    }
    return -2;
}

static int load_packed(it9300 *bridge, uint8_t addr,
                       const uint8_t *patch, size_t patch_size,
                       uint32_t data_offset_words, uint32_t source_words)
{
    size_t pos = ((size_t)data_offset_words + source_words) * 4;
    if (pos > patch_size || patch_size - pos < 8)
        return -2;

    pos += 2; /* address-offset field length; AVL62x1 format uses two */
    uint16_t records = (uint16_t)(((uint16_t)patch[pos] << 8) | patch[pos + 1]);
    pos += 2;
    uint32_t destination = be32_at(patch + pos);
    pos += 4;

    for (uint16_t i = 0; i < records; ++i) {
        if (pos > patch_size || patch_size - pos < 4)
            return -2;
        uint16_t offset = (uint16_t)(((uint16_t)patch[pos] << 8) | patch[pos + 1]);
        uint16_t length = (uint16_t)(((uint16_t)patch[pos + 2] << 8) | patch[pos + 3]);
        pos += 4;
        if (pos > patch_size || length > patch_size - pos)
            return -2;
        int rc = avl_write(bridge, addr, destination + offset, patch + pos, length);
        if (rc != 0) {
            fprintf(stderr, "AVL62x1 packed record %u could not be written: addr=0x%06lx len=%u rc=%d\n",
                    i, (unsigned long)(destination + offset), length, rc);
            return rc;
        }
        pos += length;
    }
    return 0;
}

static int run_patch_command(it9300 *bridge, uint8_t addr,
                             patch_reader *reader, size_t script_end,
                             uint32_t command, uint32_t data_offset_words,
                             uint32_t args_addr, uint32_t variables[PATCH_VARIABLES],
                             int *exit_seen)
{
    uint32_t a, b, c, d, count, index, value;
    int rc;

#define WORD(v) do { rc = read_word(reader, &(v)); if (rc != 0) return rc; } while (0)
#define SKIP(v) do { rc = skip_words(reader, (v)); if (rc != 0) return rc; } while (0)

    switch (command) {
    case PATCH_PING:
        rc = send_command(bridge, addr, CMD_PING);
        WORD(count); WORD(index);
        if (index >= PATCH_VARIABLES || count == 0) return -2;
        variables[index] = (rc == 0);
        SKIP(count - 1);
        return 0;

    case PATCH_VALIDATE_CRC:
        WORD(a); WORD(b); WORD(c); /* expected CRC, start, length */
        if ((rc = avl62x1_write32(bridge, addr, AVL_REG_ARGS_ADDR, args_addr)) != 0 ||
            (rc = avl62x1_write32(bridge, addr, args_addr, b)) != 0 ||
            (rc = avl62x1_write32(bridge, addr, args_addr + 4, c)) != 0 ||
            (rc = send_command(bridge, addr, CMD_CALC_CRC)) != 0 ||
            (rc = avl62x1_read32(bridge, addr, args_addr + 8, &value)) != 0)
            return rc;
        WORD(count); WORD(index);
        if (index >= PATCH_VARIABLES || count == 0) return -2;
        variables[index] = (value == a);
        SKIP(count - 1);
        return 0;

    case PATCH_LOAD: {
        WORD(a); WORD(b); WORD(c); /* words, destination, source word offset */
        size_t source = ((size_t)data_offset_words + c) * 4;
        size_t bytes = (size_t)a * 4;
        if (source > reader->size || bytes > reader->size - source) return -2;
        rc = avl_write(bridge, addr, b, reader->data + source, bytes);
        if (rc != 0) return rc;
        WORD(count); SKIP(count);
        return 0;
    }

    case PATCH_LOAD_IMMEDIATE:
        WORD(a); WORD(b); WORD(c); /* byte width, destination, value */
        if (a == 4) rc = avl62x1_write32(bridge, addr, b, c);
        else if (a == 2) rc = avl62x1_write16(bridge, addr, b, (uint16_t)c);
        else if (a == 1) rc = avl62x1_write8(bridge, addr, b, (uint8_t)c);
        else return -2;
        if (rc != 0) return rc;
        WORD(count); SKIP(count);
        return 0;

    case PATCH_LOAD_PACKED:
        WORD(a); WORD(b); /* word length, source word offset */
        (void)a;
        rc = load_packed(bridge, addr, reader->data, reader->size,
                         data_offset_words, b);
        if (rc != 0) return rc;
        WORD(count); SKIP(count);
        return 0;

    case PATCH_READ:
        WORD(a); WORD(b); WORD(count); WORD(index);
        if (index >= PATCH_VARIABLES || count == 0) return -2;
        rc = read_result_value(bridge, addr, a, b, &variables[index]);
        if (rc != 0) return rc;
        SKIP(count - 1);
        return 0;

    case PATCH_DMA:
        WORD(a); WORD(b); /* descriptor address/count */
        if (reader->pos > reader->size ||
            (size_t)b * 12 > reader->size - reader->pos) return -2;
        rc = avl_write(bridge, addr, a, reader->data + reader->pos, (size_t)b * 12);
        if (rc != 0) return rc;
        reader->pos += (size_t)b * 12;
        if ((rc = avl62x1_write32(bridge, addr, AVL_REG_ARGS_ADDR, a)) != 0 ||
            (rc = send_command(bridge, addr, CMD_DMA)) != 0) return rc;
        WORD(count); SKIP(count);
        return 0;

    case PATCH_EXTRACT:
        WORD(a); WORD(b); WORD(c); /* compression type, source, destination */
        if ((rc = avl62x1_write32(bridge, addr, AVL_REG_ARGS_ADDR, args_addr)) != 0 ||
            (rc = avl62x1_write32(bridge, addr, args_addr, a)) != 0 ||
            (rc = avl62x1_write32(bridge, addr, args_addr + 4, b)) != 0 ||
            (rc = avl62x1_write32(bridge, addr, args_addr + 8, c)) != 0 ||
            (rc = send_command(bridge, addr, CMD_DECOMPRESS)) != 0) return rc;
        WORD(count); SKIP(count);
        return 0;

    case PATCH_ASSERT_RESET:
    case PATCH_RELEASE_RESET:
        rc = avl62x1_write32(bridge, addr, AVL_REG_CPU_RESET,
                             command == PATCH_ASSERT_RESET ? 1 : 0);
        if (rc != 0) return rc;
        WORD(count); SKIP(count);
        return 0;

    case PATCH_DMA_HW:
        WORD(a); WORD(b); /* descriptor address/count */
        if (reader->pos > reader->size ||
            (size_t)b * 12 > reader->size - reader->pos) return -2;
        if (b) {
            rc = avl_write(bridge, addr, a, reader->data + reader->pos,
                           (size_t)b * 12);
            if (rc != 0) return rc;
        }
        reader->pos += (size_t)b * 12;
        for (unsigned tries = 0;; ++tries) {
            dtv_sleep_ms(10);
            rc = avl62x1_read32(bridge, addr, AVL_REG_DMA_STATUS, &value);
            if (rc != 0) return rc;
            if (value & 1) break;
            if (tries >= 20) return -3;
        }
        if ((rc = avl62x1_write32(bridge, addr, AVL_REG_DMA_COMMAND, a)) != 0)
            return rc;
        for (unsigned tries = 0;; ++tries) {
            dtv_sleep_ms(10);
            rc = avl62x1_read32(bridge, addr, AVL_REG_DMA_STATUS, &value);
            if (rc != 0) return rc;
            if (!(value & 0x100)) break;
            if (tries >= 20) return -3;
        }
        WORD(count); SKIP(count);
        return 0;

    case PATCH_SET_CONDITION:
        WORD(value); WORD(count); WORD(index);
        if (index >= PATCH_VARIABLES || count == 0) return -2;
        variables[index] = value;
        SKIP(count - 1);
        return 0;

    case PATCH_EXIT:
        WORD(count);
        (void)count;
        reader->pos = script_end;
        *exit_seen = 1;
        return 0;

    case PATCH_POLL:
        WORD(a); WORD(b); WORD(c); WORD(d); /* width, address, match, polls */
        for (uint32_t tries = 0;; ++tries) {
            rc = read_result_value(bridge, addr, a, b, &value);
            if (rc != 0) return rc;
            if (value == c) break;
            if (tries >= d) return -3;
            dtv_sleep_ms(10);
        }
        WORD(count); SKIP(count);
        return 0;

    default:
        fprintf(stderr, "AVL62x1: unknown patch command %lu\n",
                (unsigned long)command);
        return -2;
    }

#undef WORD
#undef SKIP
}

int avl62x1_load_patch(it9300 *bridge, uint8_t addr,
                       const uint8_t *patch, size_t patch_size)
{
    avl62x1_patch_info info;
    patch_reader reader;
    uint32_t reserved_words, script_words;
    uint32_t variables[PATCH_VARIABLES] = { 0 };
    int rc, exit_seen = 0;

    /* No other thread touches the receiver before cold init finishes. The
     * flag therefore lets avl_write pace only this dense upload, including
     * packed records that are otherwise individual I2C round trips. */
    avl_patch_upload_active = 1;
    avl_patch_write_count = 0;

    rc = avl62x1_parse_patch(patch, patch_size, &info);
    if (rc != 0 || info.chip_id != AVL62X1_CHIP_ID || info.minor != 8)
        { rc = -2; goto done; }

    reader.data = patch;
    reader.size = (size_t)info.words * 4;
    reader.pos = 20;
    if ((rc = read_word(&reader, &reserved_words)) != 0 ||
        (rc = skip_words(&reader, reserved_words)) != 0 ||
        (rc = read_word(&reader, &script_words)) != 0)
        goto done;

    size_t script_start = reader.pos;
    size_t script_end = script_start + (size_t)script_words * 4;
    if (script_end > reader.size ||
        script_end != (size_t)info.data_offset_words * 4)
        { rc = -2; goto done; }

    unsigned command_number = 0;
    while (reader.pos < script_end && !exit_seen) {
        size_t record_start = reader.pos;
        uint32_t record_words, condition_words, condition = 0;
        if ((rc = read_word(&reader, &record_words)) != 0 || record_words == 0 ||
            (rc = read_word(&reader, &condition_words)) != 0)
            { rc = -2; goto done; }
        size_t next_record = record_start + (size_t)record_words * 4;
        if (next_record > script_end)
            { rc = -2; goto done; }

        if (condition_words == 0) {
            condition = 1;
        } else {
            for (uint32_t i = 0; i < condition_words; ++i) {
                uint32_t operation, operand;
                if ((rc = read_word(&reader, &operation)) != 0 ||
                    (rc = read_word(&reader, &operand)) != 0)
                    goto done;
                uint8_t unary = (uint8_t)(operation >> 8);
                uint8_t binary = (uint8_t)operation;
                uint8_t mode = (uint8_t)((operation >> 16) & 3);
                if (mode == OP_ADDR_VARIABLE && binary != OP_STORE) {
                    if (operand >= PATCH_VARIABLES) { rc = -2; goto done; }
                    operand = variables[operand];
                }
                if (unary == OP_UNARY_NOT) operand = !operand;
                else if (unary == OP_UNARY_INVERT) operand = ~operand;
                else if (unary != 0) { rc = -2; goto done; }

                switch (binary) {
                case OP_LOAD: condition = operand; break;
                case OP_STORE:
                    if (operand >= PATCH_VARIABLES) { rc = -2; goto done; }
                    variables[operand] = condition; break;
                case OP_AND: condition = condition && operand; break;
                case OP_OR: condition = condition || operand; break;
                case OP_BIT_AND: condition &= operand; break;
                case OP_BIT_OR: condition |= operand; break;
                case OP_EQUALS: condition = condition == operand; break;
                case OP_NOT_EQUALS: condition = condition != operand; break;
                default: rc = -2; goto done;
                }
            }
        }

        if (!condition) {
            reader.pos = next_record;
            continue;
        }

        uint32_t command;
        if ((rc = read_word(&reader, &command)) != 0)
            goto done;
        printf("AVL62x1 patch command %u: %lu\n", command_number++,
               (unsigned long)command);
        rc = run_patch_command(bridge, addr, &reader, script_end, command,
                               info.data_offset_words, info.args_addr,
                               variables, &exit_seen);
        if (rc != 0)
            goto done;
    }

    /* AVL6261 S2X patches end at data_offset with no PATCH_EXIT record, so
     * reaching the validated script boundary is a success too. */
    rc = reader.pos == script_end ? 0 : -2;
done:
    avl_patch_upload_active = 0;
    return rc;
}
