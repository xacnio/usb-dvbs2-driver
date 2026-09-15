#include "ts_udp.h"

#include <string.h>

size_t find_ts_sync(const uint8_t *data, size_t size, size_t start)
{
    size_t offset;
    for (offset = start; offset + 4u * 188u < size; ++offset) {
        if (data[offset] == 0x47 && data[offset + 188] == 0x47 &&
            data[offset + 376] == 0x47 && data[offset + 564] == 0x47 &&
            data[offset + 752] == 0x47)
            return offset;
    }
    return (size_t)-1;
}

int udp_ts_flush(udp_ts_writer *writer)
{
    int rc = 0;
    if (writer->size == 0)
        return 0;
    if (sendto(writer->socket_handle, (const char *)writer->datagram,
               (int)writer->size, 0,
               (const struct sockaddr *)&writer->destination,
               sizeof(writer->destination)) == DTV_SOCKET_ERROR)
        rc = -1;
    /* The second port is best effort: the player may not be listening on
     * it yet, and that must not stop the primary stream. */
    if (writer->has_alt)
        sendto(writer->socket_handle, (const char *)writer->datagram,
               (int)writer->size, 0,
               (const struct sockaddr *)&writer->destination_alt,
               sizeof(writer->destination_alt));
    writer->size = 0;
    return rc;
}

int udp_ts_send_packet(udp_ts_writer *writer, const uint8_t *packet)
{
    memcpy(writer->datagram + writer->size, packet, 188);
    writer->size += 188;
    ++writer->packets;
    return writer->size == sizeof(writer->datagram)
               ? udp_ts_flush(writer) : 0;
}

uint32_t mpeg2_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    for (i = 0; i < size; ++i) {
        unsigned bit;
        crc ^= (uint32_t)data[i] << 24;
        for (bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u
                                      : crc << 1;
    }
    return crc;
}

int udp_ts_send_section(udp_ts_writer *writer, uint16_t pid,
                               uint8_t *continuity_counter,
                               const uint8_t *section, size_t section_size)
{
    size_t used = 0;
    int first = 1;
    while (used < section_size) {
        uint8_t packet[188];
        size_t payload_offset = first ? 5u : 4u;
        size_t capacity = 188u - payload_offset;
        size_t chunk = section_size - used;
        if (chunk > capacity)
            chunk = capacity;
        memset(packet, 0xff, sizeof(packet));
        packet[0] = 0x47;
        packet[1] = (uint8_t)((pid >> 8) & 0x1fu);
        if (first)
            packet[1] |= 0x40;
        packet[2] = (uint8_t)pid;
        packet[3] = (uint8_t)(0x10u | (*continuity_counter & 0x0fu));
        *continuity_counter = (uint8_t)((*continuity_counter + 1u) & 0x0fu);
        if (first)
            packet[4] = 0;
        memcpy(packet + payload_offset, section + used, chunk);
        if (udp_ts_send_packet(writer, packet) != 0)
            return -1;
        used += chunk;
        first = 0;
    }
    return 0;
}

int psi_assemble(psi_section_assembler *assembler,
                        const uint8_t *packet, const uint8_t **section,
                        size_t *size)
{
    unsigned afc = (packet[3] >> 4) & 3u;
    size_t offset, take;
    if (afc == 1) offset = 4;
    else if (afc == 3) offset = 5u + packet[4];
    else return 0;
    if (offset >= 188u) return 0;
    if (packet[1] & 0x40u) {
        unsigned pointer = packet[offset++];
        if (offset + pointer > 188u) return 0;
        offset += pointer;
        assembler->length = assembler->expected = 0;
    } else if (!assembler->length) {
        return 0;
    }
    if (offset >= 188u) return 0;
    take = 188u - offset;
    if (assembler->length + take > sizeof(assembler->data))
        take = sizeof(assembler->data) - assembler->length;
    if (!take) { assembler->length = assembler->expected = 0; return 0; }
    memcpy(assembler->data + assembler->length, packet + offset, take);
    assembler->length += take;
    if (!assembler->expected && assembler->length >= 3) {
        assembler->expected = 3u +
            (((size_t)(assembler->data[1] & 0x0fu) << 8) | assembler->data[2]);
        if (assembler->expected > sizeof(assembler->data)) {
            assembler->length = assembler->expected = 0;
            return 0;
        }
    }
    if (assembler->expected && assembler->length >= assembler->expected) {
        *section = assembler->data;
        *size = assembler->expected;
        assembler->length = assembler->expected = 0;
        return 1;
    }
    return 0;
}

/* Channels sourced from the TKGS table carry their elementary stream PIDs
 * but no PMT pid; the PAT is what maps service_id -> PMT pid, so learn it
 * from the stream. Returns 0x1fff when this service isn't in the PAT. */
uint16_t pat_lookup_pmt_pid(const uint8_t *section, size_t size,
                                   uint16_t service_id)
{
    size_t pos;
    if (size < 12 || section[0] != 0x00)
        return 0x1fffu;
    for (pos = 8; pos + 4 <= size - 4; pos += 4) {
        uint16_t sid = (uint16_t)((section[pos] << 8) | section[pos + 1]);
        if (sid == service_id)
            return (uint16_t)(((section[pos + 2] & 0x1fu) << 8) |
                              section[pos + 3]);
    }
    return 0x1fffu;
}

int udp_ts_send_single_pat(udp_ts_writer *writer,
                                  const uint8_t *source, size_t source_size,
                                  uint16_t service_id, uint16_t pmt_pid,
                                  uint8_t *continuity_counter)
{
    uint8_t section[16];
    uint32_t crc;
    if (source_size < 12 || source[0] != 0x00)
        return -1;
    section[0] = 0x00;
    section[1] = 0xb0;
    section[2] = 13;
    section[3] = source[3];
    section[4] = source[4];
    section[5] = source[5];
    section[6] = 0;
    section[7] = 0;
    section[8] = (uint8_t)(service_id >> 8);
    section[9] = (uint8_t)service_id;
    section[10] = (uint8_t)(0xe0u | ((pmt_pid >> 8) & 0x1fu));
    section[11] = (uint8_t)pmt_pid;
    crc = mpeg2_crc32(section, 12);
    section[12] = (uint8_t)(crc >> 24);
    section[13] = (uint8_t)(crc >> 16);
    section[14] = (uint8_t)(crc >> 8);
    section[15] = (uint8_t)crc;
    return udp_ts_send_section(writer, 0, continuity_counter,
                               section, sizeof(section));
}

int udp_ts_send_selected_pmt(udp_ts_writer *writer,
                                    const uint8_t *source,
                                    size_t source_size, uint16_t pmt_pid,
                                    uint16_t service_id,
                                    uint16_t *teletext_pid,
                                    uint16_t *pcr_pid,
                                    uint16_t *stream_pids,
                                    size_t stream_capacity,
                                    size_t *stream_count,
                                    uint8_t *continuity_counter,
                                    int strip_ca)
{
    uint8_t section[1024];
    size_t program_info_size, source_pos, source_end, output_pos;
    uint32_t crc;
    uint16_t source_service;
    if (source_size < 16 || source[0] != 0x02)
        return -1;
    source_service = (uint16_t)((source[3] << 8) | source[4]);
    if (source_service != service_id)
        return -1;
    if (!teletext_pid || !pcr_pid || !stream_pids || !stream_count)
        return -1;
    *stream_count = 0;
    program_info_size = ((size_t)(source[10] & 0x0fu) << 8) | source[11];
    source_pos = 12u + program_info_size;
    source_end = source_size - 4u;
    if (source_pos > source_end || source_pos > sizeof(section) - 4u)
        return -1;
    memcpy(section, source, 12u);
    output_pos = 12u;
    /* Once the stream is descrambled on the way out, the CA descriptors
     * name ECM pids that are no longer forwarded for a payload that now
     * arrives in the clear. A player reading them announces a scrambled
     * program and may refuse the streams, so they are dropped and the
     * descriptor loop length rewritten. */
    {
        size_t descriptor = 12u, kept = 0;
        while (descriptor + 2u <= source_pos) {
            size_t length = 2u + source[descriptor + 1];
            if (descriptor + length > source_pos)
                break;
            if (!(strip_ca && source[descriptor] == 0x09u)) {
                if (output_pos + length + 4u > sizeof(section))
                    return -1;
                memcpy(section + output_pos, source + descriptor, length);
                output_pos += length;
                kept += length;
            }
            descriptor += length;
        }
        section[10] = (uint8_t)((source[10] & 0xf0u) |
                                ((kept >> 8) & 0x0fu));
        section[11] = (uint8_t)kept;
    }
    *pcr_pid = (uint16_t)(((source[8] & 0x1fu) << 8) | source[9]);
    while (source_pos + 5u <= source_end) {
        uint16_t pid = (uint16_t)(((source[source_pos + 1] & 0x1fu) << 8) |
                                  source[source_pos + 2]);
        size_t info_size = ((size_t)(source[source_pos + 3] & 0x0fu) << 8) |
                           source[source_pos + 4];
        size_t entry_size = 5u + info_size;
        if (source_pos + entry_size > source_end)
            return -1;
        /* Keep every elementary stream of this service, not only the first
         * video/audio pair: alternate audio and subtitle tracks need their
         * PMT entries and their packets to reach libVLC.
         *
         * Which stream is which comes from the table, not the caller: a
         * channel list made before teletext was understood says every
         * service has none. Teletext is private data (0x06) carrying a
         * teletext descriptor (0x56, or 0x46 for VBI). */
        if (source[source_pos] == 0x06u) {
            size_t d = source_pos + 5u;
            size_t d_end = source_pos + entry_size;
            while (d + 2u <= d_end) {
                size_t d_len = source[d + 1];
                if (d + 2u + d_len > d_end)
                    break;
                if (source[d] == 0x56u || source[d] == 0x46u)
                    *teletext_pid = pid;
                d += 2u + d_len;
            }
        }
        if (*stream_count < stream_capacity)
            stream_pids[(*stream_count)++] = pid;
        if (output_pos + entry_size + 4u > sizeof(section))
            return -1;
        if (strip_ca) {
            /* As in the program loop: copy the five byte stream header,
             * then every descriptor except the CA ones, and write the new
             * descriptor length back. */
            size_t descriptor = source_pos + 5u;
            size_t entry_end = source_pos + entry_size;
            size_t header = output_pos, kept = 0;
            memcpy(section + output_pos, source + source_pos, 5u);
            output_pos += 5u;
            while (descriptor + 2u <= entry_end) {
                size_t length = 2u + source[descriptor + 1];
                if (descriptor + length > entry_end)
                    break;
                if (source[descriptor] != 0x09u) {
                    memcpy(section + output_pos, source + descriptor, length);
                    output_pos += length;
                    kept += length;
                }
                descriptor += length;
            }
            section[header + 3] = (uint8_t)((source[source_pos + 3] & 0xf0u) |
                                            ((kept >> 8) & 0x0fu));
            section[header + 4] = (uint8_t)kept;
        } else {
            memcpy(section + output_pos, source + source_pos, entry_size);
            output_pos += entry_size;
        }
        source_pos += entry_size;
    }
    {
        size_t section_length = (output_pos - 3u) + 4u;
        section[1] = (uint8_t)((section[1] & 0xf0u) |
                               ((section_length >> 8) & 0x0fu));
        section[2] = (uint8_t)section_length;
    }
    crc = mpeg2_crc32(section, output_pos);
    section[output_pos++] = (uint8_t)(crc >> 24);
    section[output_pos++] = (uint8_t)(crc >> 16);
    section[output_pos++] = (uint8_t)(crc >> 8);
    section[output_pos++] = (uint8_t)crc;
    return udp_ts_send_section(writer, pmt_pid, continuity_counter,
                               section, output_pos);
}

int udp_ts_pid_in_list(uint16_t pid, const uint16_t *pids, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i)
        if (pids[i] == pid)
            return 1;
    return 0;
}
