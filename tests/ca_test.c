/* Reading conditional-access and teletext descriptors from a PMT. A
 * hand-built section carries exactly the descriptors under test, so a
 * failure names the descriptor rather than the transponder. It goes
 * through the same dtv_ts_parse_pmt the live scan uses, CRC and all. */
#include "ts_services.h"
#include <stdio.h>
#include <string.h>

static uint32_t mpeg_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    int b;
    for (i = 0; i < size; ++i) {
        crc ^= (uint32_t)data[i] << 24;
        for (b = 0; b < 8; ++b)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

/* Builds a one-service PMT. es_type/es_pid describe a single elementary
 * stream; es_desc (es_desc_len bytes) is copied into that stream's
 * descriptor loop, and prog_desc into the programme loop. */
static size_t build_pmt(uint8_t *out, uint16_t service_id,
                        const uint8_t *prog_desc, size_t prog_desc_len,
                        uint8_t es_type, uint16_t es_pid,
                        const uint8_t *es_desc, size_t es_desc_len)
{
    uint8_t *p = out;
    size_t body, i;
    uint32_t crc;
    out[0] = 0x02;                          /* table_id: PMT */
    out[3] = (uint8_t)(service_id >> 8);
    out[4] = (uint8_t)service_id;
    out[5] = 0xc1;                          /* version 0, current */
    out[6] = 0x00; out[7] = 0x00;           /* section 0 of 0 */
    out[8] = 0xe0; out[9] = 0x00;           /* PCR pid = 0 */
    out[10] = (uint8_t)(0xf0 | ((prog_desc_len >> 8) & 0x0f));
    out[11] = (uint8_t)prog_desc_len;
    p = out + 12;
    memcpy(p, prog_desc, prog_desc_len);
    p += prog_desc_len;
    *p++ = es_type;
    *p++ = (uint8_t)(0xe0 | ((es_pid >> 8) & 0x1f));
    *p++ = (uint8_t)es_pid;
    *p++ = (uint8_t)(0xf0 | ((es_desc_len >> 8) & 0x0f));
    *p++ = (uint8_t)es_desc_len;
    memcpy(p, es_desc, es_desc_len);
    p += es_desc_len;
    body = (size_t)(p - out) + 4;           /* + CRC */
    out[1] = (uint8_t)(0xb0 | ((body - 3) >> 8));
    out[2] = (uint8_t)(body - 3);
    crc = mpeg_crc32(out, (size_t)(p - out));
    *p++ = (uint8_t)(crc >> 24);
    *p++ = (uint8_t)(crc >> 16);
    *p++ = (uint8_t)(crc >> 8);
    *p++ = (uint8_t)crc;
    (void)i;
    return (size_t)(p - out);
}

static int failures;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

/* Builds a one-service SDT. free_ca is the free_CA_mode bit: 1 says the
 * broadcaster declares the service scrambled, 0 says it does not. No
 * descriptors, since only the flag is under test. */
static size_t build_sdt(uint8_t *out, uint16_t service_id, int free_ca)
{
    size_t size = 20;                       /* 11 header + 5 service + 4 CRC */
    uint32_t crc;
    memset(out, 0, size);
    out[0] = 0x42;                          /* table_id: SDT actual */
    out[1] = (uint8_t)(0xb0 | ((size - 3) >> 8));
    out[2] = (uint8_t)((size - 3) & 0xff);
    out[3] = 0x0a; out[4] = 0x01;           /* transport_stream_id */
    out[5] = 0xc1;                          /* version 0, current */
    out[6] = 0x00; out[7] = 0x00;           /* section 0 of 0 */
    out[8] = 0x00; out[9] = 0x2a;           /* original_network_id */
    out[10] = 0xff;                         /* reserved */
    out[11] = (uint8_t)(service_id >> 8);
    out[12] = (uint8_t)service_id;
    out[13] = 0x00;                         /* EIT flags */
    /* running_status 4 (running), free_CA_mode, descriptors_loop_length 0 */
    out[14] = (uint8_t)(0x80 | (free_ca ? 0x10 : 0x00));
    out[15] = 0x00;
    crc = mpeg_crc32(out, size - 4);
    out[size - 4] = (uint8_t)(crc >> 24);
    out[size - 3] = (uint8_t)(crc >> 16);
    out[size - 2] = (uint8_t)(crc >> 8);
    out[size - 1] = (uint8_t)crc;
    return size;
}

int main(void)
{
    uint8_t section[512];
    size_t size;

    /* 1. Irdeto (system 0x0602) in the programme-level CA descriptor. */
    {
        static dtv_ts_scan_result r;
        const uint8_t prog[] = { 0x09, 0x04, 0x06, 0x02, 0xe1, 0x00 };
        const uint8_t es[] = { 0x28, 0x02, 0x00, 0x00 };   /* AVC, no CA */
        memset(&r, 0, sizeof(r));
        size = build_pmt(section, 0x0001, prog, sizeof(prog),
                         0x1b, 0x0201, es, sizeof(es));
        check("Irdeto PMT parses", dtv_ts_parse_pmt(section, size, &r) == 0);
        check("service found", r.service_count == 1);
        check("marked scrambled", r.services[0].scrambled == 1);
        check("system id 0x0602",
              r.services[0].ca_system_id == 0x0602);
        check("ca pid 0x0100", r.services[0].ca_pid == 0x0100);
        check("named Irdeto",
              dtv_ts_ca_system_name(r.services[0].ca_system_id) != NULL &&
              strcmp(dtv_ts_ca_system_name(r.services[0].ca_system_id),
                     "Irdeto") == 0);
    }

    /* 2. BISS (0x2600) named, and taken from a stream-level CA descriptor
     *    when the programme loop has none. */
    {
        static dtv_ts_scan_result r;
        const uint8_t es[] = { 0x09, 0x04, 0x26, 0x00, 0xe1, 0xf4 };
        memset(&r, 0, sizeof(r));
        size = build_pmt(section, 0x0002, NULL, 0,
                         0x1b, 0x0201, es, sizeof(es));
        check("BISS PMT parses", dtv_ts_parse_pmt(section, size, &r) == 0);
        check("stream-level CA seen", r.services[0].ca_system_id == 0x2600);
        check("ca pid 0x01f4", r.services[0].ca_pid == 0x01f4);
        check("named BISS",
              dtv_ts_ca_system_name(0x2600) != NULL &&
              strcmp(dtv_ts_ca_system_name(0x2600), "BISS") == 0);
        check("BISS-CA distinct",
              strcmp(dtv_ts_ca_system_name(0x2700), "BISS-CA") == 0);
    }

    /* 3. Free-to-air: no CA descriptor, so no system and not scrambled. A
     *    teletext descriptor on the same stream is still read. */
    {
        static dtv_ts_scan_result r;
        const uint8_t es[] = { 0x56, 0x00 };   /* teletext descriptor */
        memset(&r, 0, sizeof(r));
        size = build_pmt(section, 0x0003, NULL, 0,
                         0x06, 0x0460, es, sizeof(es));
        check("FTA PMT parses", dtv_ts_parse_pmt(section, size, &r) == 0);
        check("no system", r.services[0].ca_system_id == 0x0000);
        check("not scrambled", r.services[0].scrambled == 0);
        check("teletext pid 0x0460",
              r.services[0].teletext_pid == 0x0460);
        check("unknown id has no name",
              dtv_ts_ca_system_name(0x3f00) == NULL);
    }

    /* 4. The two tables disagreeing, in the order a live scan meets them.
     *
     *    A PMT with a CA descriptor, then an SDT that says the service is
     *    free. The SDT repeats every couple of seconds, so on any real
     *    carrier it is read after the PMT, and assigning free_CA_mode
     *    outright wiped what the descriptor had found: every scrambled
     *    channel on Turksat 11807 H came back free. The flag may add; it
     *    may not clear. */
    {
        static dtv_ts_scan_result r;
        uint8_t sdt[64];
        const uint8_t prog[] = { 0x09, 0x04, 0x18, 0x01, 0xe1, 0x00 };
        size_t sdt_size;
        memset(&r, 0, sizeof(r));
        size = build_pmt(section, 0x0004, prog, sizeof(prog),
                         0x1b, 0x0641, NULL, 0);
        check("PMT parses", dtv_ts_parse_pmt(section, size, &r) == 0);
        check("CA descriptor says scrambled", r.services[0].scrambled == 1);

        sdt_size = build_sdt(sdt, 0x0004, 0);
        check("SDT parses", dtv_ts_parse_sdt(sdt, sdt_size, &r) == 0);
        check("a free-marked SDT does not clear it",
              r.services[0].scrambled == 1);
        check("the system is still named",
              r.services[0].ca_system_id == 0x1801);
    }

    /* 5. The other way round: the SDT alone is still believed when nothing
     *    contradicts it, or a scrambled channel with no CA descriptor in
     *    reach would read as free. */
    {
        static dtv_ts_scan_result r;
        uint8_t sdt[64];
        size_t sdt_size;
        memset(&r, 0, sizeof(r));
        sdt_size = build_sdt(sdt, 0x0005, 1);
        check("SDT alone parses", dtv_ts_parse_sdt(sdt, sdt_size, &r) == 0);
        check("SDT alone marks it scrambled", r.services[0].scrambled == 1);
    }

    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
