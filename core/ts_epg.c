/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ts_epg.h"

#include "dvb_text.h"

#include <string.h>

/* Two BCD digits, as the table writes hours, minutes and seconds. */
static unsigned bcd_value(uint8_t value)
{
    return (unsigned)((value >> 4) * 10u + (value & 0x0fu));
}

/* Modified Julian Day to a civil date, by the conversion in ETSI EN 300
 * 468 annex C. */
static void mjd_to_date(unsigned mjd, int *year, int *month, int *day)
{
    int y = (int)((mjd - 15078.2) / 365.25);
    int m = (int)((mjd - 14956.1 - (int)(y * 365.25)) / 30.6001);
    int d = (int)mjd - 14956 - (int)(y * 365.25) - (int)(m * 30.6001);
    int k = (m == 14 || m == 15) ? 1 : 0;
    *year = y + k + 1900;
    *month = m - 1 - k * 12;
    *day = d;
}

/* Days since the Unix epoch for a civil date, by Howard Hinnant's
 * days_from_civil. No time zone anywhere: the table is UTC and so is
 * everything downstream. */
static int64_t days_from_civil(int year, int month, int day)
{
    int64_t y = year;
    int64_t era, doe, yoe, doy;
    y -= month <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Start time of an event: 16 bit MJD then three BCD bytes. All ones means
 * "undefined", which the standard allows for an event whose time is not
 * known yet. */
static int event_start_utc(const uint8_t *start, int64_t *out)
{
    int year, month, day;
    unsigned mjd;
    if (start[0] == 0xffu && start[1] == 0xffu)
        return -1;
    mjd = (unsigned)((start[0] << 8) | start[1]);
    if (!mjd)
        return -1;
    mjd_to_date(mjd, &year, &month, &day);
    if (month < 1 || month > 12 || day < 1 || day > 31)
        return -1;
    *out = days_from_civil(year, month, day) * 86400 +
           (int64_t)bcd_value(start[2]) * 3600 +
           (int64_t)bcd_value(start[3]) * 60 +
           (int64_t)bcd_value(start[4]);
    return 0;
}

/* DVB text into UTF-8, then the tidying this table wants: these strings
 * travel down a pipe of |-separated fields, so the separator cannot survive
 * in one, and operators pad their titles. */
static void copy_text(char *target, size_t target_size,
                      const uint8_t *source, size_t source_size)
{
    char decoded[512];
    size_t in = 0, out = 0, length;
    if (!target_size)
        return;
    length = dtv_dvb_text_to_utf8(decoded, sizeof(decoded), source,
                                  source_size);
    while (in < length && out + 1 < target_size) {
        char ch = decoded[in++];
        if (ch == '|')
            ch = ' ';
        /* Several Turkish channels send a leading space, which shifts
         * every title right of its grid block. */
        if (ch == ' ' && !out)
            continue;
        target[out++] = ch;
    }
    while (out && target[out - 1] == ' ')
        --out;
    /* Never cut a multi-byte character in half on the way out. */
    while (out && ((unsigned char)target[out] & 0xC0u) == 0x80u)
        --out;
    target[out] = 0;
}

int dtv_epg_is_event_table(uint8_t table_id)
{
    return table_id == 0x4eu || table_id == 0x4fu ||
           (table_id >= 0x50u && table_id <= 0x6fu);
}

int dtv_epg_section_version(const uint8_t *section, size_t size)
{
    if (!section || size < 6)
        return -1;
    return (int)((section[5] >> 1) & 0x1fu);
}

size_t dtv_epg_parse_section(const uint8_t *section, size_t size,
                             dtv_epg_event *out, size_t max_events)
{
    size_t pos, end, count = 0;
    uint16_t service_id;
    if (!section || !out || !max_events || size < 18)
        return 0;
    if (!dtv_epg_is_event_table(section[0]))
        return 0;
    /* The last four bytes are the CRC, never event data. */
    end = size - 4u;
    service_id = (uint16_t)((section[3] << 8) | section[4]);
    pos = 14;
    while (pos + 12u <= end && count < max_events) {
        size_t descriptor_size =
            ((size_t)(section[pos + 10] & 0x0fu) << 8) | section[pos + 11];
        size_t desc = pos + 12u;
        size_t desc_end = desc + descriptor_size;
        dtv_epg_event *event = &out[count];
        int have_start;
        if (desc_end > end)
            break;
        memset(event, 0, sizeof(*event));
        event->service_id = service_id;
        event->event_id = (uint16_t)((section[pos] << 8) | section[pos + 1]);
        event->running_status = (uint8_t)((section[pos + 10] >> 5) & 7u);
        event->scrambled = (uint8_t)((section[pos + 10] >> 4) & 1u);
        have_start = event_start_utc(section + pos + 2u,
                                     &event->start_utc) == 0;
        event->duration_seconds =
            (uint32_t)(bcd_value(section[pos + 7]) * 3600u +
                       bcd_value(section[pos + 8]) * 60u +
                       bcd_value(section[pos + 9]));
        while (desc + 2u <= desc_end) {
            uint8_t tag = section[desc];
            size_t length = section[desc + 1];
            if (desc + 2u + length > desc_end)
                break;
            /* short_event_descriptor: language, name, one-line
             * description. The grid needs the name, the panel the rest. */
            if (tag == 0x4du && length >= 5u) {
                const uint8_t *data = section + desc + 2u;
                size_t name_length = data[3];
                if (4u + name_length + 1u <= length) {
                    size_t text_length = data[4u + name_length];
                    copy_text(event->title, sizeof(event->title),
                              data + 4u, name_length);
                    if (5u + name_length + text_length <= length)
                        copy_text(event->text, sizeof(event->text),
                                  data + 5u + name_length, text_length);
                } else if (4u + name_length <= length) {
                    copy_text(event->title, sizeof(event->title),
                              data + 4u, name_length);
                }
            }
            desc += 2u + length;
        }
        pos = desc_end;
        /* No clock means no place in the table; no name means a blank
         * block. */
        if (have_start && event->title[0])
            ++count;
    }
    return count;
}
