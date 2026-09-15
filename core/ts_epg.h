/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TS_EPG_H
#define TS_EPG_H

#include <stddef.h>
#include <stdint.h>

/* Programme events from the DVB event information table (PID 0x12).
 *
 * Four table ids matter and differ only in reach:
 *
 *   0x4E  present/following of THIS transport   now and next
 *   0x4F  present/following of another one
 *   0x50..0x5F  the schedule of THIS transport
 *   0x60..0x6F  the schedule of another one
 *
 * They parse the same way and nothing here cares which arrived: Turksat
 * fills present/following everywhere and the schedule only on some
 * services, so every event that turns up is kept.
 *
 * Pure data work -- bytes in, events out, no state or I/O -- so it can be
 * checked against a real capture from a test. */

#define DTV_EPG_TITLE_SIZE 192
#define DTV_EPG_TEXT_SIZE  384

typedef struct dtv_epg_event {
    uint16_t service_id;
    uint16_t event_id;
    /* Programme start in seconds since the Unix epoch, UTC. The table
     * carries Modified Julian Day plus BCD time. */
    int64_t start_utc;
    uint32_t duration_seconds;
    /* 4 = running. Such a programme is on air whatever the clock says,
     * which matters when the receiver time is off. */
    uint8_t running_status;
    uint8_t scrambled;
    char title[DTV_EPG_TITLE_SIZE];
    char text[DTV_EPG_TEXT_SIZE];
} dtv_epg_event;

/* Is this table id one of the four above? */
int dtv_epg_is_event_table(uint8_t table_id);

/* Reads one EIT section into out (up to max_events); returns how many were
 * written. 0 is normal: the standard uses empty sections to delimit the
 * segments of a schedule. */
size_t dtv_epg_parse_section(const uint8_t *section, size_t size,
                             dtv_epg_event *out, size_t max_events);

/* Section version, so a repeat can be skipped. -1 when too short. */
int dtv_epg_section_version(const uint8_t *section, size_t size);

#endif
