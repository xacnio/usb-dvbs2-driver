#ifndef DTV_TOOLS_TS_UDP_H
#define DTV_TOOLS_TS_UDP_H

#include "dtv_net.h"
#include <stddef.h>
#include <stdint.h>

/* Sending a filtered MPEG-TS over UDP, shared by the probe and the daemon,
 * which each used to carry their own copy: a fix in one (the PMT crossing
 * a packet boundary, say) silently missed the other. */

/* Finds the start of a TS packet: the offset where five consecutive sync
 * bytes line up. Returns (size_t)-1 when there is none. */
size_t find_ts_sync(const uint8_t *data, size_t size, size_t start);

/* Datagrams carry seven packets, the usual MPEG-TS-over-UDP payload.
 *
 * A second destination (has_alt) lets the daemon send to two ports so the
 * player can move to the other on a channel change: on Windows two sockets
 * may bind the same UDP port but only the first receives, so with one port
 * a new player saw nothing until the old one closed. */
typedef struct udp_ts_writer {
    dtv_socket socket_handle;
    struct sockaddr_in destination;
    struct sockaddr_in destination_alt;
    int has_alt;
    uint8_t datagram[7 * 188];
    size_t size;
    unsigned long packets;
} udp_ts_writer;

/* A programme normally has a handful of elementary streams, but
 * multilingual services carry several audio and subtitle PIDs. */
#define UDP_TS_MAX_PROGRAM_PIDS 32

int udp_ts_flush(udp_ts_writer *writer);
int udp_ts_send_packet(udp_ts_writer *writer, const uint8_t *packet);

/* Reassembles a PSI section that may span several TS packets. */
typedef struct psi_section_assembler {
    uint8_t data[1024];
    size_t length;
    size_t expected;
} psi_section_assembler;

/* Returns 1 and fills in the section pointer and size once a section is
 * complete. */
int psi_assemble(psi_section_assembler *assembler, const uint8_t *packet,
                 const uint8_t **section, size_t *size);

/* MPEG-2 CRC32, for the sections we rebuild ourselves. */
uint32_t mpeg2_crc32(const uint8_t *data, size_t size);

/* Emits one PSI section as TS packets on the given pid. */
int udp_ts_send_section(udp_ts_writer *writer, uint16_t pid,
                        uint8_t *continuity_counter, const uint8_t *section,
                        size_t section_size);

/* PMT pid of a service from the PAT; 0x1fff when it is not listed. */
uint16_t pat_lookup_pmt_pid(const uint8_t *section, size_t size,
                            uint16_t service_id);

/* Rewrites the PAT and the PMT down to the selected service, so the player
 * sees a single-program stream instead of the whole multiplex. */
int udp_ts_send_single_pat(udp_ts_writer *writer, const uint8_t *source,
                           size_t source_size, uint16_t service_id,
                           uint16_t pmt_pid, uint8_t *continuity_counter);
int udp_ts_send_selected_pmt(udp_ts_writer *writer, const uint8_t *source,
                             size_t source_size, uint16_t pmt_pid,
                             uint16_t service_id,
                             /* In/out: the teletext pid to keep. The table
                              * is trusted over the passed-in value, which
                              * is written back for the caller to forward. */
                             uint16_t *teletext_pid,
                             uint16_t *pcr_pid,
                             /* Filled from every ES entry in the PMT. The
                              * caller forwards these PIDs so alternate
                              * audio and subtitle tracks reach the player. */
                             uint16_t *stream_pids, size_t stream_capacity,
                             size_t *stream_count,
                             uint8_t *continuity_counter,
                             /* Drop the CA descriptors from the rewritten
                              * table. Set while descrambling: the outgoing
                              * stream is in the clear, so a table claiming
                              * otherwise is a lie the player acts on. */
                             int strip_ca);

int udp_ts_pid_in_list(uint16_t pid, const uint16_t *pids, size_t count);

#endif
