/* Network information table parsing. A network search stands or falls on
 * this: it turns a transponder's NIT into the carriers the operator
 * broadcasts, and wrong BCD arithmetic sends the tuner to a frequency that
 * does not exist, with only no signal to show for it.
 *
 * The section is built by hand (a capture carrying a complete NIT is
 * 20 MB) to ETSI EN 300 468; the two transponders are real Turksat
 * carriers, so the expected values can be checked against a channel list. */

#include "ts_services.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL %s\n", what);
        ++g_failures;
    }
}

static uint32_t mpeg_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    for (i = 0; i < size; ++i) {
        unsigned bit;
        crc ^= (uint32_t)data[i] << 24;
        for (bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

/* satellite_delivery_system_descriptor for one carrier. */
static size_t satellite_descriptor(uint8_t *out, const char *frequency,
                                   const char *orbital, int east,
                                   int vertical, const char *symbol_rate)
{
    size_t i;
    out[0] = 0x43;
    out[1] = 11;
    for (i = 0; i < 4; ++i)     /* 8 BCD digits, units of 10 kHz */
        out[2 + i] = (uint8_t)(((frequency[i * 2] - '0') << 4) |
                               (frequency[i * 2 + 1] - '0'));
    for (i = 0; i < 2; ++i)     /* 4 BCD digits, tenths of a degree */
        out[6 + i] = (uint8_t)(((orbital[i * 2] - '0') << 4) |
                               (orbital[i * 2 + 1] - '0'));
    out[8] = (uint8_t)((east ? 0x80 : 0) | (vertical ? 0x20 : 0));
    for (i = 0; i < 3; ++i)     /* 7 BCD digits, units of 100 Hz */
        out[9 + i] = (uint8_t)(((symbol_rate[i * 2] - '0') << 4) |
                               (symbol_rate[i * 2 + 1] - '0'));
    out[12] = (uint8_t)(((symbol_rate[6] - '0') << 4) | 0x05);  /* + FEC */
    return 13;
}

int main(void)
{
    uint8_t section[512];
    uint8_t *p;
    size_t size, loop_start, loop_length;
    uint32_t crc;
    dtv_ts_scan_result result;
    static const char network_name[] = "TURKSAT";

    memset(section, 0, sizeof(section));
    section[0] = 0x40;                      /* NIT actual */
    section[3] = 0x00; section[4] = 0x2a;   /* network_id */
    section[5] = 0xc1;                      /* version, current */
    section[6] = 0x00; section[7] = 0x00;   /* section 0 of 0 */

    /* network descriptor loop: just the name */
    p = section + 10;
    *p++ = 0x40;
    *p++ = (uint8_t)(sizeof(network_name) - 1);
    memcpy(p, network_name, sizeof(network_name) - 1);
    p += sizeof(network_name) - 1;
    section[8] = 0;
    section[9] = (uint8_t)(p - (section + 10));

    /* transport stream loop */
    loop_start = (size_t)(p - section) + 2;
    p += 2;
    {
        /* 12458 H 27500 */
        uint8_t *entry = p;
        size_t dlen;
        entry[0] = 0x0a; entry[1] = 0x01;   /* transport_stream_id */
        entry[2] = 0x00; entry[3] = 0x2a;   /* original_network_id */
        dlen = satellite_descriptor(entry + 6, "01245800", "0420", 1, 0,
                                    "0275000");
        entry[4] = 0;
        entry[5] = (uint8_t)dlen;
        p = entry + 6 + dlen;
    }
    {
        /* 11794 V 30000 */
        uint8_t *entry = p;
        size_t dlen;
        entry[0] = 0x0a; entry[1] = 0x02;
        entry[2] = 0x00; entry[3] = 0x2a;
        dlen = satellite_descriptor(entry + 6, "01179400", "0420", 1, 1,
                                    "0300000");
        entry[4] = 0;
        entry[5] = (uint8_t)dlen;
        p = entry + 6 + dlen;
    }
    loop_length = (size_t)(p - section) - loop_start;
    section[loop_start - 2] = (uint8_t)(loop_length >> 8);
    section[loop_start - 1] = (uint8_t)loop_length;

    size = (size_t)(p - section) + 4;       /* plus CRC */
    section[1] = (uint8_t)(0xb0 | ((size - 3) >> 8));
    section[2] = (uint8_t)((size - 3) & 0xff);
    crc = mpeg_crc32(section, size - 4);
    section[size - 4] = (uint8_t)(crc >> 24);
    section[size - 3] = (uint8_t)(crc >> 16);
    section[size - 2] = (uint8_t)(crc >> 8);
    section[size - 1] = (uint8_t)crc;

    memset(&result, 0, sizeof(result));
    check(dtv_ts_parse_nit(section, size, &result) == 0, "NIT parsed");
    check(strcmp(result.network_name, "TURKSAT") == 0, "network name");
    check(result.network_tp_count == 2, "two transponders found");
    if (result.network_tp_count >= 2) {
        const dtv_ts_network_tp *a = &result.network_tps[0];
        const dtv_ts_network_tp *b = &result.network_tps[1];
        /* 8 BCD digits in units of 10 kHz: reading them as plain bytes, or
         * forgetting the division, is the classic way to end up 100x off. */
        check(a->frequency_mhz == 12458, "TP 1 frequency 12458 MHz");
        check(a->symbol_rate_ksps == 27500, "TP 1 symbol rate 27500");
        check(a->polarization == 'H', "TP 1 horizontal");
        check(a->orbital_tenths == 420, "orbital position 42.0");
        check(a->east == 1, "east");
        check(a->transport_stream_id == 0x0a01, "TP 1 stream id");
        check(b->frequency_mhz == 11794, "TP 2 frequency 11794 MHz");
        check(b->symbol_rate_ksps == 30000, "TP 2 symbol rate 30000");
        check(b->polarization == 'V', "TP 2 vertical");
    }

    /* The same carrier twice must be stored once: a NIT repeats, and a
     * network may list one carrier under several stream ids. */
    check(dtv_ts_parse_nit(section, size, &result) == 0, "second pass");
    check(result.network_tp_count == 2, "not added twice");

    /* Anything malformed has to be refused rather than half-parsed: the
     * result would be a tuner sent to an invented frequency. */
    check(dtv_ts_parse_nit(section, size - 1, &result) != 0, "short section");
    check(dtv_ts_parse_nit(NULL, 0, &result) != 0, "empty input");
    {
        /* Well-formed by every length check but not matching its own CRC:
         * what a torn capture produces when two partial reads splice. Every
         * carrier decoded from it would become a permanent, never-matching
         * new transponder. */
        uint8_t corrupt[512];
        memcpy(corrupt, section, size);
        corrupt[40] ^= 0xff;
        check(dtv_ts_parse_nit(corrupt, size, &result) != 0,
              "bad CRC refused");
    }
    {
        /* NIT other: a real, CRC-valid section, but for a different
         * network. Its carriers must not be attributed to this satellite. */
        uint8_t other[512];
        uint32_t other_crc;
        memcpy(other, section, size);
        other[0] = 0x41;
        other_crc = mpeg_crc32(other, size - 4);
        other[size - 4] = (uint8_t)(other_crc >> 24);
        other[size - 3] = (uint8_t)(other_crc >> 16);
        other[size - 2] = (uint8_t)(other_crc >> 8);
        other[size - 1] = (uint8_t)other_crc;
        check(dtv_ts_parse_nit(other, size, &result) != 0,
              "NIT other refused");
    }
    section[0] = 0x42;
    check(dtv_ts_parse_nit(section, size, &result) != 0, "a table that is not a NIT");

    if (g_failures) {
        printf("nit: %d checks failed\n", g_failures);
        return 1;
    }
    printf("nit: network transponder list parsing -- ok\n");
    return 0;
}
