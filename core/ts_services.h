/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TS_SERVICES_H
#define TS_SERVICES_H

#include <stddef.h>
#include <stdint.h>

/* A single TKGS list can carry 583 channels (the Turksat HD profile). At
 * 512 the table overflowed with leftovers from old scans and dropped the
 * rest of the list silently. 1024 fits a full list plus leftovers. */
#define DTV_TS_MAX_SERVICES 1024
#define DTV_TS_NAME_SIZE 128
#define DTV_TS_NO_PID 0x1fffu

/* Name of a conditional access system from its id. Ids are handed out in
 * blocks of 256, so the high byte identifies the system. NULL when the
 * block has no name here -- the number is still worth showing. */
const char *dtv_ts_ca_system_name(uint16_t ca_system_id);

typedef struct dtv_ts_service {
    uint16_t service_id;
    uint16_t pmt_pid;
    uint16_t video_pid;
    uint16_t audio_pid;
    /* Teletext pid, 0x1fff for none. Its own field because it is not a
     * stream anyone plays: it is a page service riding in the private data
     * of the transport and has to be let through as itself. */
    uint16_t teletext_pid;
    uint8_t service_type;
    uint8_t video_stream_type;
    uint8_t audio_stream_type;
    uint8_t has_video;
    uint8_t has_audio;
    uint8_t scrambled;
    /* Which CA system scrambles this service, from the CA descriptors of
     * its PMT, and that system's message stream. 0 means none named. Shown
     * only: knowing a channel is locked says nothing about which package
     * would open it, and the system name does. */
    uint16_t ca_system_id;
    uint16_t ca_pid;
    /* Is this service carried by the transport stream just scanned?
     *
     * SDT actual describes this multiplex, SDT other the rest of the
     * network, and both land in the same table. Turksat sends hundreds of
     * the second kind; taking them as channels of the scanned carrier
     * filled the pool on the first transponder and listed channels this
     * frequency does not carry. The PAT is the authority. Services seen
     * only in an SDT are still kept -- TKGS needs them for the scrambled
     * flag -- but are not merged as channels. */
    uint8_t on_this_transport;
    char name[DTV_TS_NAME_SIZE];
    char provider[DTV_TS_NAME_SIZE];
} dtv_ts_service;

/* Transponders announced by the network, read from the NIT. It lists every
 * transponder of the operator with a satellite_delivery_system_descriptor,
 * which is what makes a network search possible: tune one known carrier,
 * read the list, scan the carriers it names. */
#define DTV_TS_MAX_NETWORK_TPS 128

typedef struct dtv_ts_network_tp {
    unsigned frequency_mhz;
    unsigned symbol_rate_ksps;
    char polarization;          /* 'H' or 'V' */
    unsigned orbital_tenths;    /* 420 = 42.0 degrees */
    int east;                   /* 1 = east, 0 = west */
    uint16_t transport_stream_id;
} dtv_ts_network_tp;

typedef struct dtv_ts_scan_result {
    dtv_ts_service services[DTV_TS_MAX_SERVICES];
    size_t service_count;
    size_t packet_count;
    size_t sync_errors;
    /* Filled in from the NIT when the capture contains one. */
    dtv_ts_network_tp network_tps[DTV_TS_MAX_NETWORK_TPS];
    size_t network_tp_count;
    char network_name[DTV_TS_NAME_SIZE];
} dtv_ts_scan_result;

int dtv_ts_scan_file(const char *path, dtv_ts_scan_result *result);

/* Reads one NIT section into the result. Public so a test can feed it a
 * synthetic section. CRC-checked, NIT actual (table_id 0x40) only -- NIT
 * other describes a different network. 0 = parsed. */
int dtv_ts_parse_nit(const uint8_t *section, size_t size,
                     dtv_ts_scan_result *result);

/* Reads one PMT section into the result, creating the service. Public for
 * the same reason as the NIT entry. CRC-checked. 0 = parsed. */
int dtv_ts_parse_pmt(const uint8_t *section, size_t size,
                     dtv_ts_scan_result *result);

/* Reads one SDT section into the result. CRC-checked, table_id 0x42 (actual)
 * or 0x46 (other). 0 = parsed. Public so a test can feed a PMT and an SDT in
 * order, which is the only way to reach the case where the two disagree
 * about whether a service is scrambled. */
int dtv_ts_parse_sdt(const uint8_t *section, size_t size,
                     dtv_ts_scan_result *result);

#endif
