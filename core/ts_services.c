/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ts_services.h"

#include "dvb_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSI_MAX_SECTION 4096
#define PSI_MAX_CONTEXTS (DTV_TS_MAX_SERVICES + 2)

typedef struct psi_context {
    uint16_t pid;
    uint8_t data[PSI_MAX_SECTION];
    size_t length;
    size_t expected;
    int active;
} psi_context;

typedef struct parser_state {
    dtv_ts_scan_result *result;
    psi_context contexts[PSI_MAX_CONTEXTS];
    size_t context_count;
} parser_state;

static dtv_ts_service *find_service(dtv_ts_scan_result *result,
                                    uint16_t service_id, int create)
{
    size_t i;
    for (i = 0; i < result->service_count; ++i) {
        if (result->services[i].service_id == service_id)
            return &result->services[i];
    }
    if (!create || result->service_count >= DTV_TS_MAX_SERVICES)
        return NULL;
    dtv_ts_service *service = &result->services[result->service_count++];
    memset(service, 0, sizeof(*service));
    service->service_id = service_id;
    service->pmt_pid = DTV_TS_NO_PID;
    service->video_pid = DTV_TS_NO_PID;
    service->audio_pid = DTV_TS_NO_PID;
    service->teletext_pid = DTV_TS_NO_PID;
    snprintf(service->name, sizeof(service->name), "Service %u", service_id);
    return service;
}

static psi_context *find_context(parser_state *state, uint16_t pid, int create)
{
    size_t i;
    for (i = 0; i < state->context_count; ++i) {
        if (state->contexts[i].pid == pid)
            return &state->contexts[i];
    }
    if (!create || state->context_count >= PSI_MAX_CONTEXTS)
        return NULL;
    psi_context *context = &state->contexts[state->context_count++];
    memset(context, 0, sizeof(*context));
    context->pid = pid;
    return context;
}

static void copy_dvb_text(char *target, size_t target_size,
                          const uint8_t *source, size_t source_size)
{
    size_t out;
    if (target_size == 0)
        return;
    /* The selector in front of the string says which character table the
     * broadcaster used. Skipping it and keeping the bytes -- which is what
     * this did -- hands the rest of the program text that is not UTF-8. */
    out = dtv_dvb_text_to_utf8(target, target_size, source, source_size);
    while (out && target[out - 1] == ' ')
        --out;
    target[out] = '\0';
}

static int is_video_type(uint8_t stream_type)
{
    return stream_type == 0x01 || stream_type == 0x02 ||
           stream_type == 0x10 || stream_type == 0x1b ||
           stream_type == 0x24 || stream_type == 0x42;
}

static int is_audio_type(uint8_t stream_type)
{
    return stream_type == 0x03 || stream_type == 0x04 ||
           stream_type == 0x0f || stream_type == 0x11 ||
           stream_type == 0x81 || stream_type == 0x87;
}

static void parse_pat(parser_state *state, const uint8_t *section, size_t size)
{
    size_t pos;
    if (size < 12 || section[0] != 0x00)
        return;
    for (pos = 8; pos + 4 <= size - 4; pos += 4) {
        uint16_t service_id = (uint16_t)((section[pos] << 8) | section[pos + 1]);
        uint16_t pid = (uint16_t)(((section[pos + 2] & 0x1f) << 8) |
                                  section[pos + 3]);
        if (service_id != 0) {
            dtv_ts_service *service = find_service(state->result,
                                                   service_id, 1);
            if (service) {
                service->pmt_pid = pid;
                service->on_this_transport = 1;
                find_context(state, pid, 1);
            }
        }
    }
}

static uint32_t mpeg_crc32(const uint8_t *data, size_t size);

static void parse_sdt(parser_state *state, const uint8_t *section, size_t size)
{
    size_t pos;
    if (size < 15 || (section[0] != 0x42 && section[0] != 0x46))
        return;
    pos = 11;
    while (pos + 5 <= size - 4) {
        uint16_t service_id = (uint16_t)((section[pos] << 8) | section[pos + 1]);
        size_t descriptor_length = (size_t)(((section[pos + 3] & 0x0f) << 8) |
                                            section[pos + 4]);
        size_t desc = pos + 5;
        size_t desc_end = desc + descriptor_length;
        dtv_ts_service *service;
        if (desc_end > size - 4)
            break;
        service = find_service(state->result, service_id, 1);
        /* free_CA_mode only ever adds; it never clears.
         *
         * The rule this file already states -- a table naming a CA system
         * beats the SDT flag, which some multiplexes leave clear on services
         * they do scramble -- was defeated by section order. The SDT repeats
         * every couple of seconds, so a section arriving after the PMT
         * overwrote what read_ca_descriptors had found and the channel came
         * back free. Measured on Turksat 11807 H 10000, where FX HD and its
         * neighbours are all marked free in the SDT and all carry a CA
         * descriptor in their PMT. */
        if (service && ((section[pos + 3] >> 4) & 1u))
            service->scrambled = 1u;
        while (service && desc + 2 <= desc_end) {
            uint8_t tag = section[desc];
            size_t length = section[desc + 1];
            const uint8_t *data = section + desc + 2;
            if (desc + 2 + length > desc_end)
                break;
            if (tag == 0x48 && length >= 3) {
                size_t provider_length = data[1];
                if (2 + provider_length < length) {
                    size_t name_length = data[2 + provider_length];
                    size_t available = length - 3 - provider_length;
                    service->service_type = data[0];
                    copy_dvb_text(service->provider,
                                  sizeof(service->provider), data + 2,
                                  provider_length);
                    if (name_length > available)
                        name_length = available;
                    copy_dvb_text(service->name, sizeof(service->name),
                                  data + 3 + provider_length, name_length);
                }
            }
            desc += 2 + length;
        }
        pos = desc_end;
    }
}

/* Registered conditional access system identifiers (ETSI TS 101 162). Each
 * system holds a block of 256 ids -- the operator picks the low byte -- so
 * the high byte names the system. Only the systems seen on the satellites
 * this receiver points at are listed; the rest show as a bare number. */
const char *dtv_ts_ca_system_name(uint16_t ca_system_id)
{
    switch (ca_system_id >> 8) {
    case 0x01: return "Seca/Mediaguard";
    case 0x05: return "Viaccess";
    case 0x06: return "Irdeto";
    case 0x09: return "NDS/Videoguard";
    case 0x0b: return "Conax";
    case 0x0d: return "Cryptoworks";
    case 0x0e: return "PowerVu";
    case 0x10: return "Tandberg";
    case 0x11: return "BISS";
    case 0x17: return "BetaCrypt";
    case 0x18: return "Nagravision";
    case 0x26: return "BISS";
    case 0x27: return "BISS-CA";
    case 0x4a: return "DRE-Crypt";
    case 0x56: return "Verimatrix";
    default:   return NULL;
    }
}

/* One CA_descriptor (tag 0x09): the system id and its EMM/ECM pid.
 * Descriptors appear once for the programme and once per stream; the first
 * found is taken, since a scrambled service is scrambled by that system. */
static void read_ca_descriptors(dtv_ts_service *service,
                                const uint8_t *data, size_t start,
                                size_t end)
{
    size_t d = start;
    if (service->ca_system_id)
        return;
    while (d + 2 <= end) {
        size_t length = data[d + 1];
        if (d + 2 + length > end)
            break;
        if (data[d] == 0x09 && length >= 4) {
            service->ca_system_id =
                (uint16_t)((data[d + 2] << 8) | data[d + 3]);
            service->ca_pid =
                (uint16_t)(((data[d + 4] & 0x1f) << 8) | data[d + 5]);
            /* A table naming a system beats the SDT flag, which some
             * multiplexes leave clear on services they do scramble. */
            service->scrambled = 1;
            return;
        }
        d += 2 + length;
    }
}

static void parse_pmt(parser_state *state, const uint8_t *section, size_t size)
{
    uint16_t service_id;
    size_t pos, program_info_length;
    dtv_ts_service *service;
    if (size < 16 || section[0] != 0x02)
        return;
    service_id = (uint16_t)((section[3] << 8) | section[4]);
    service = find_service(state->result, service_id, 1);
    if (!service)
        return;
    service->on_this_transport = 1;
    program_info_length = (size_t)(((section[10] & 0x0f) << 8) | section[11]);
    pos = 12 + program_info_length;
    if (pos + 4 <= size)
        read_ca_descriptors(service, section, 12, pos);
    while (pos + 5 <= size - 4) {
        uint8_t stream_type = section[pos];
        uint16_t pid = (uint16_t)(((section[pos + 1] & 0x1f) << 8) |
                                  section[pos + 2]);
        size_t info_length = (size_t)(((section[pos + 3] & 0x0f) << 8) |
                                      section[pos + 4]);
        if (pos + 5 + info_length > size - 4)
            break;
        /* Teletext has no stream type of its own: it is private data
         * (0x06) carrying a teletext descriptor (0x56, or 0x46 for VBI).
         * On the TRT multiplex all three services share one such stream. */
        if (stream_type == 0x06 && service->teletext_pid >= DTV_TS_NO_PID) {
            size_t d = pos + 5;
            size_t end = pos + 5 + info_length;
            while (d + 2 <= end) {
                uint8_t tag = section[d];
                uint8_t length = section[d + 1];
                if (d + 2 + (size_t)length > end)
                    break;
                if (tag == 0x56 || tag == 0x46)
                    service->teletext_pid = pid;
                d += 2 + (size_t)length;
            }
        }
        read_ca_descriptors(service, section, pos + 5,
                            pos + 5 + info_length);
        if (!service->has_video && is_video_type(stream_type)) {
            service->video_pid = pid;
            service->video_stream_type = stream_type;
            service->has_video = 1;
        } else if (!service->has_audio && is_audio_type(stream_type)) {
            service->audio_pid = pid;
            service->audio_stream_type = stream_type;
            service->has_audio = 1;
        }
        pos += 5 + info_length;
    }
}

static uint32_t mpeg_crc32(const uint8_t *data, size_t size);

/* Test entry: CRC-check a standalone PMT section and parse it through the
 * exact code path the live scan uses. */
int dtv_ts_parse_pmt(const uint8_t *section, size_t size,
                     dtv_ts_scan_result *result)
{
    /* parser_state carries a reassembly context per possible service and is
     * far too large for the stack; parse_pmt only reads state->result. */
    parser_state *state;
    if (!section || !result || size < 16 || section[0] != 0x02)
        return -1;
    if (mpeg_crc32(section, size - 4) !=
            (((uint32_t)section[size - 4] << 24) |
             ((uint32_t)section[size - 3] << 16) |
             ((uint32_t)section[size - 2] << 8) |
             (uint32_t)section[size - 1]))
        return -1;
    state = calloc(1, sizeof(*state));
    if (!state)
        return -1;
    state->result = result;
    parse_pmt(state, section, size);
    free(state);
    return 0;
}

/* Test entry: CRC-check a standalone SDT section and parse it through the
 * exact code path the live scan uses. Public for the same reason as the PMT
 * entry, and for one more: the interesting case is an SDT read *after* a
 * PMT, which is only reachable by feeding the two in order. */
int dtv_ts_parse_sdt(const uint8_t *section, size_t size,
                     dtv_ts_scan_result *result)
{
    parser_state *state;
    if (!section || !result || size < 15 ||
        (section[0] != 0x42 && section[0] != 0x46))
        return -1;
    if (size != 3u + (size_t)(((section[1] & 0x0f) << 8) | section[2]))
        return -1;
    if (mpeg_crc32(section, size - 4) !=
            (((uint32_t)section[size - 4] << 24) |
             ((uint32_t)section[size - 3] << 16) |
             ((uint32_t)section[size - 2] << 8) |
             (uint32_t)section[size - 1]))
        return -1;
    state = calloc(1, sizeof(*state));
    if (!state)
        return -1;
    state->result = result;
    parse_sdt(state, section, size);
    free(state);
    return 0;
}

/* BCD helper: the satellite descriptor stores frequency, orbital position
 * and symbol rate as packed decimal. */
static unsigned bcd_value(const uint8_t *data, size_t nibbles)
{
    unsigned value = 0;
    size_t i;
    for (i = 0; i < nibbles; ++i) {
        unsigned digit = (i & 1) ? (data[i / 2] & 0x0f) : (data[i / 2] >> 4);
        if (digit > 9)
            digit = 0;
        value = value * 10u + digit;
    }
    return value;
}

/* One satellite_delivery_system_descriptor (tag 0x43, ETSI EN 300 468).
 *
 * frequency  : 8 BCD digits, in units of 10 kHz  -> 0124 5800 = 12458.00 MHz
 * orbital    : 4 BCD digits, tenths of a degree  -> 0420     = 42.0
 * flags byte : bit7 east/west, bits 6-5 polarization
 * symbol rate: 7 BCD digits, in units of 100 Hz  -> 0275000  = 27500 kSym/s
 */
static void parse_satellite_descriptor(const uint8_t *data, size_t length,
                                       dtv_ts_network_tp *out)
{
    if (length < 11)
        return;
    out->frequency_mhz = bcd_value(data, 8) / 100u;
    out->orbital_tenths = bcd_value(data + 4, 4);
    out->east = (data[6] >> 7) & 1;
    out->polarization = (((data[6] >> 5) & 3) == 0) ? 'H' : 'V';
    out->symbol_rate_ksps = bcd_value(data + 7, 7) / 10u;
}

/* MPEG-2 CRC32, verifying a section before anything in it is trusted.
 *
 * PAT, PMT and SDT are short and repeat several times a second, so a torn
 * read is quickly overwritten. The NIT is not: one section takes about ten
 * seconds across five packets, and its content goes straight into the
 * satellite transponder list and the scan queue. A frame reassembled from
 * two overlapping partial reads can pass the section_length check with
 * garbage content, and every carrier read out of it is a random number
 * that matches nothing known, so it is added as new and inherited by the
 * next search. */
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

int dtv_ts_parse_nit(const uint8_t *section, size_t size,
                     dtv_ts_scan_result *result)
{
    size_t network_descriptor_length, pos, loop_length, end;
    if (!section || !result || size < 16)
        return -1;
    /* NIT actual only. NIT other (table_id 0x41) describes a different
     * network, possibly at another orbital position, so its carriers are
     * not scannable transponders of this satellite. */
    if (section[0] != 0x40)
        return -1;
    if (size != 3u + (size_t)(((section[1] & 0x0f) << 8) | section[2]))
        return -1;
    if (size < 4 ||
        mpeg_crc32(section, size - 4) !=
            ((uint32_t)section[size - 4] << 24 |
             (uint32_t)section[size - 3] << 16 |
             (uint32_t)section[size - 2] << 8 |
             (uint32_t)section[size - 1]))
        return -1;
    network_descriptor_length = (size_t)(((section[8] & 0x0f) << 8) |
                                         section[9]);
    pos = 10;
    end = pos + network_descriptor_length;
    if (end + 2 > size - 4)
        return -1;
    while (pos + 2 <= end) {
        uint8_t tag = section[pos];
        size_t length = section[pos + 1];
        if (pos + 2 + length > end)
            break;
        if (tag == 0x40 && length && !result->network_name[0]) {
            size_t copy = length < DTV_TS_NAME_SIZE - 1
                        ? length : DTV_TS_NAME_SIZE - 1;
            memcpy(result->network_name, section + pos + 2, copy);
            result->network_name[copy] = 0;
        }
        pos += 2 + length;
    }
    pos = end;
    loop_length = (size_t)(((section[pos] & 0x0f) << 8) | section[pos + 1]);
    pos += 2;
    end = pos + loop_length;
    if (end > size - 4)
        end = size - 4;
    while (pos + 6 <= end) {
        uint16_t tsid = (uint16_t)((section[pos] << 8) | section[pos + 1]);
        size_t descriptors = (size_t)(((section[pos + 4] & 0x0f) << 8) |
                                      section[pos + 5]);
        size_t desc = pos + 6;
        size_t desc_end = desc + descriptors;
        if (desc_end > end)
            break;
        while (desc + 2 <= desc_end) {
            uint8_t tag = section[desc];
            size_t length = section[desc + 1];
            if (desc + 2 + length > desc_end)
                break;
            if (tag == 0x43 &&
                result->network_tp_count < DTV_TS_MAX_NETWORK_TPS) {
                dtv_ts_network_tp entry;
                size_t i;
                int duplicate = 0;
                memset(&entry, 0, sizeof(entry));
                entry.transport_stream_id = tsid;
                parse_satellite_descriptor(section + desc + 2, length, &entry);
                if (!entry.frequency_mhz || !entry.symbol_rate_ksps)
                    break;
                /* The same carrier appears once per section repetition, and
                 * a network may also list it under several stream ids. */
                for (i = 0; i < result->network_tp_count; ++i) {
                    const dtv_ts_network_tp *known = &result->network_tps[i];
                    if (known->frequency_mhz == entry.frequency_mhz &&
                        known->polarization == entry.polarization) {
                        duplicate = 1;
                        break;
                    }
                }
                if (!duplicate)
                    result->network_tps[result->network_tp_count++] = entry;
            }
            desc += 2 + length;
        }
        pos += 6 + descriptors;
    }
    return 0;
}

static void parse_section(parser_state *state, uint16_t pid,
                          const uint8_t *section, size_t size)
{
    (void)pid;
    if (size < 3 || size != 3u +
        (size_t)(((section[1] & 0x0f) << 8) | section[2]))
        return;
    /* A capture off the demodulator contains torn and noisy stretches, and
     * arbitrary bytes pass the length check often enough to matter. Every
     * table ends in an MPEG-2 CRC so a receiver can tell a real section
     * from an accident; without it, noise became services with made-up
     * ids. */
    if (size < 4 ||
        mpeg_crc32(section, size - 4) !=
            ((uint32_t)section[size - 4] << 24 |
             (uint32_t)section[size - 3] << 16 |
             (uint32_t)section[size - 2] << 8 |
             (uint32_t)section[size - 1]))
        return;
    switch (section[0]) {
    case 0x00: parse_pat(state, section, size); break;
    case 0x02: parse_pmt(state, section, size); break;
    case 0x42:
    case 0x46: parse_sdt(state, section, size); break;
    case 0x40:
    case 0x41: dtv_ts_parse_nit(section, size, state->result); break;
    default: break;
    }
}

static void context_reset(psi_context *context)
{
    context->length = 0;
    context->expected = 0;
    context->active = 0;
}

static size_t append_section(parser_state *state, psi_context *context,
                             const uint8_t *data, size_t size)
{
    size_t consumed = 0;
    if (!context->active && size) {
        if (data[0] == 0xff)
            return size;
        context->active = 1;
    }
    while (consumed < size && context->active) {
        if (context->length < 3) {
            context->data[context->length++] = data[consumed++];
            if (context->length == 3) {
                context->expected = 3u +
                    (size_t)(((context->data[1] & 0x0f) << 8) |
                             context->data[2]);
                if (context->expected < 3 ||
                    context->expected > PSI_MAX_SECTION) {
                    context_reset(context);
                    return consumed;
                }
            }
        } else {
            size_t need = context->expected - context->length;
            size_t take = need < size - consumed ? need : size - consumed;
            memcpy(context->data + context->length, data + consumed, take);
            context->length += take;
            consumed += take;
        }
        if (context->expected && context->length == context->expected) {
            parse_section(state, context->pid, context->data,
                          context->expected);
            context_reset(context);
        }
    }
    return consumed;
}

static void process_payload(parser_state *state, psi_context *context,
                            const uint8_t *payload, size_t size,
                            int payload_start)
{
    size_t pos = 0;
    if (!size)
        return;
    if (payload_start) {
        size_t pointer = payload[0];
        pos = 1;
        if (pointer > size - pos) {
            context_reset(context);
            return;
        }
        if (context->active && pointer)
            append_section(state, context, payload + pos, pointer);
        context_reset(context);
        pos += pointer;
    }
    while (pos < size) {
        size_t used = append_section(state, context, payload + pos,
                                     size - pos);
        if (!used)
            break;
        pos += used;
    }
}

int dtv_ts_scan_file(const char *path, dtv_ts_scan_result *result)
{
    FILE *file;
    uint8_t packet[188];
    uint8_t probe[65536];
    size_t probe_size, sync_offset = (size_t)-1;
    /* PSI_MAX_CONTEXTS contexts of 4096 bytes add up to several MB, well
     * over a default 1 MB thread stack, so this is heap-allocated. */
    parser_state *state;
    int rc;
    if (!path || !result)
        return -1;
    file = fopen(path, "rb");
    if (!file)
        return -2;
    /* A USB capture may begin with a partial transfer or stale FIFO bytes,
     * so acquire the first phase with five consecutive packets rather than
     * assuming byte zero is a sync byte. */
    probe_size = fread(probe, 1, sizeof(probe), file);
    if (probe_size >= 5u * sizeof(packet)) {
        size_t offset;
        for (offset = 0; offset + 4u * sizeof(packet) < probe_size; ++offset) {
            if (probe[offset] == 0x47 &&
                probe[offset + 188] == 0x47 &&
                probe[offset + 376] == 0x47 &&
                probe[offset + 564] == 0x47 &&
                probe[offset + 752] == 0x47) {
                sync_offset = offset;
                break;
            }
        }
    }
    if (sync_offset == (size_t)-1 ||
        fseek(file, (long)sync_offset, SEEK_SET) != 0) {
        fclose(file);
        return -3;
    }
    state = (parser_state *)calloc(1, sizeof(*state));
    if (!state) {
        fclose(file);
        return -5;
    }
    memset(result, 0, sizeof(*result));
    state->result = result;
    find_context(state, 0x0000, 1); /* PAT */
    find_context(state, 0x0011, 1); /* SDT */
    find_context(state, 0x0010, 1); /* NIT */

    while (fread(packet, 1, sizeof(packet), file) == sizeof(packet)) {
        uint16_t pid;
        unsigned adaptation_control;
        size_t offset = 4;
        psi_context *context;
        ++result->packet_count;
        if (packet[0] != 0x47) {
            ++result->sync_errors;
            continue;
        }
        pid = (uint16_t)(((packet[1] & 0x1f) << 8) | packet[2]);
        context = find_context(state, pid, 0);
        if (!context)
            continue;
        adaptation_control = (packet[3] >> 4) & 3u;
        if (adaptation_control == 0 || adaptation_control == 2)
            continue;
        if (adaptation_control == 3) {
            offset += 1u + packet[4];
            if (offset > sizeof(packet))
                continue;
        }
        process_payload(state, context, packet + offset,
                        sizeof(packet) - offset, (packet[1] & 0x40) != 0);
    }
    fclose(file);
    rc = result->service_count ? 0 : -4;
    free(state);
    return rc;
}
