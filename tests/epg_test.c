/* EIT event parsing. Three of the fields use formats nothing else in the
 * project does -- Modified Julian Day, BCD clock and BCD duration -- and
 * reading one wrong draws every grid block in the wrong place rather than
 * crashing. The arithmetic is pinned against dates whose answers are known
 * independently, over a synthetic section rather than a shipped capture. */
#include <stdio.h>
#include <string.h>

#include "ts_epg.h"

static int g_failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
        ++g_failures; \
    } \
} while (0)

static unsigned char g_section[4096];
static size_t g_size;

static void put(unsigned value)
{
    if (g_size < sizeof(g_section))
        g_section[g_size++] = (unsigned char)value;
}

static void put16(unsigned value)
{
    put((value >> 8) & 0xff);
    put(value & 0xff);
}

static void put_bcd(unsigned value)
{
    put(((value / 10) << 4) | (value % 10));
}

/* Section header: table id, length placeholder, service id, then the
 * fields every EIT section carries before its first event. */
static void begin_section(unsigned table_id, unsigned service_id,
                          unsigned version)
{
    g_size = 0;
    put(table_id);
    put16(0);                      /* length, filled in by end_section */
    put16(service_id);
    put(0xc1u | (version << 1));   /* version, current/next */
    put(0);                        /* section number */
    put(0);                        /* last section number */
    put16(0);                      /* transport stream id */
    put16(0);                      /* original network id */
    put(0);                        /* segment last section number */
    put(table_id);                 /* last table id */
}

/* One event: id, start, duration, status, then a short_event_descriptor
 * carrying the name and the one-line description. */
static void put_event(unsigned event_id, unsigned mjd, unsigned hour,
                      unsigned minute, unsigned second,
                      unsigned dur_hour, unsigned dur_minute,
                      const char *title, const char *text)
{
    size_t name_length = strlen(title);
    size_t text_length = strlen(text);
    size_t descriptor_length = 5u + name_length + text_length;
    put16(event_id);
    put16(mjd);
    put_bcd(hour);
    put_bcd(minute);
    put_bcd(second);
    put_bcd(dur_hour);
    put_bcd(dur_minute);
    put_bcd(0);
    /* running_status 4 (running), not scrambled, descriptor loop length */
    put(0x80u | ((descriptor_length + 2u) >> 8));
    put((descriptor_length + 2u) & 0xff);
    put(0x4d);                     /* short_event_descriptor */
    put(descriptor_length);
    put('t'); put('u'); put('r');
    put(name_length);
    while (*title)
        put((unsigned char)*title++);
    put(text_length);
    while (*text)
        put((unsigned char)*text++);
}

static void end_section(void)
{
    size_t length;
    put(0); put(0); put(0); put(0);   /* CRC */
    length = g_size - 3u;
    g_section[1] = (unsigned char)(0xb0u | ((length >> 8) & 0x0fu));
    g_section[2] = (unsigned char)(length & 0xffu);
}

int main(void)
{
    dtv_epg_event events[8];
    size_t count;

    /* --- which tables carry events --- */
    CHECK(dtv_epg_is_event_table(0x4e));   /* present/following, this TS */
    CHECK(dtv_epg_is_event_table(0x4f));   /* present/following, other TS */
    CHECK(dtv_epg_is_event_table(0x50));   /* schedule, this TS */
    CHECK(dtv_epg_is_event_table(0x5f));
    CHECK(dtv_epg_is_event_table(0x60));   /* schedule, other TS */
    CHECK(dtv_epg_is_event_table(0x6f));
    /* Neighbours in the table id space that are something else entirely. */
    CHECK(!dtv_epg_is_event_table(0x42));  /* SDT */
    CHECK(!dtv_epg_is_event_table(0x70));  /* TDT */
    CHECK(!dtv_epg_is_event_table(0x40));  /* NIT */

    /* One event with known times: MJD 60543 is 2024-08-21, and
     * 2024-08-21 20:00:00 UTC is 1724270400 seconds since the epoch, both
     * worked out independently of the code under test. */
    begin_section(0x4e, 10601, 3);
    put_event(0x1234, 60543, 20, 0, 0, 1, 30, "Evening News", "The day in brief");
    end_section();
    count = dtv_epg_parse_section(g_section, g_size, events, 8);
    CHECK(count == 1);
    if (count == 1) {
        CHECK(events[0].service_id == 10601);
        CHECK(events[0].event_id == 0x1234);
        CHECK(events[0].start_utc == 1724270400);
        CHECK(events[0].duration_seconds == 90u * 60u);
        CHECK(events[0].running_status == 4);
        CHECK(!events[0].scrambled);
        CHECK(strcmp(events[0].title, "Evening News") == 0);
        CHECK(strcmp(events[0].text, "The day in brief") == 0);
    }
    CHECK(dtv_epg_section_version(g_section, g_size) == 3);

    /* Midnight, and a day that only exists in a leap year: the MJD
     * conversion is where those go wrong. MJD 60369 is 2024-02-29. */
    begin_section(0x50, 42, 0);
    put_event(1, 60369, 0, 0, 0, 0, 45, "Late Night", "");
    end_section();
    count = dtv_epg_parse_section(g_section, g_size, events, 8);
    CHECK(count == 1);
    /* 2024-02-29 00:00:00 UTC */
    if (count == 1)
        CHECK(events[0].start_utc == 1709164800);

    /* --- several events in one section, in order --- */
    begin_section(0x60, 777, 1);
    put_event(1, 60543, 8, 0, 0, 0, 30, "Morning", "");
    put_event(2, 60543, 8, 30, 0, 1, 0, "Mid-morning", "");
    put_event(3, 60543, 9, 30, 0, 2, 15, "Noon", "");
    end_section();
    count = dtv_epg_parse_section(g_section, g_size, events, 8);
    CHECK(count == 3);
    if (count == 3) {
        CHECK(strcmp(events[0].title, "Morning") == 0);
        CHECK(strcmp(events[2].title, "Noon") == 0);
        /* The blocks have to meet end to end, or the grid shows gaps. */
        CHECK(events[0].start_utc + events[0].duration_seconds ==
              events[1].start_utc);
        CHECK(events[1].start_utc + events[1].duration_seconds ==
              events[2].start_utc);
        CHECK(events[2].duration_seconds == 2u * 3600u + 15u * 60u);
    }

    /* Capacity: a section with more events than there is room for must
     * fill what there is and stop. */
    count = dtv_epg_parse_section(g_section, g_size, events, 2);
    CHECK(count == 2);

    /* An empty section: the standard uses these to delimit the segments of
     * a schedule, so an operator broadcasting none still sends them. */
    begin_section(0x60, 50560, 5);
    end_section();
    count = dtv_epg_parse_section(g_section, g_size, events, 8);
    CHECK(count == 0);

    /* --- rubbish must not be read as programmes --- */
    begin_section(0x42, 1, 0);      /* SDT, not an event table */
    put_event(1, 60543, 8, 0, 0, 0, 30, "Never", "");
    end_section();
    CHECK(dtv_epg_parse_section(g_section, g_size, events, 8) == 0);

    /* An event whose start time is "undefined" (all ones) cannot be put on
     * a time axis, so it is dropped rather than drawn at the epoch. */
    begin_section(0x4e, 5, 0);
    put_event(1, 0xffff, 0, 0, 0, 1, 0, "Unknown", "");
    end_section();
    CHECK(dtv_epg_parse_section(g_section, g_size, events, 8) == 0);

    /* Operators pad their titles. A leading space would push every title
     * in the grid out of line with the block it names. */
    begin_section(0x4e, 7, 0);
    put_event(1, 60543, 8, 0, 0, 0, 30, "  EVENING NEWS  ", " summary ");
    end_section();
    count = dtv_epg_parse_section(g_section, g_size, events, 8);
    CHECK(count == 1);
    if (count == 1) {
        CHECK(strcmp(events[0].title, "EVENING NEWS") == 0);
        CHECK(strcmp(events[0].text, "summary") == 0);
    }

    /* Truncated input must be refused, not walked off the end of. */
    begin_section(0x4e, 5, 0);
    put_event(1, 60543, 8, 0, 0, 0, 30, "Truncated", "");
    end_section();
    CHECK(dtv_epg_parse_section(g_section, 10, events, 8) == 0);
    CHECK(dtv_epg_parse_section(NULL, g_size, events, 8) == 0);
    CHECK(dtv_epg_parse_section(g_section, g_size, events, 0) == 0);

    if (g_failures) {
        printf("\n%d failed\n", g_failures);
        return 1;
    }
    printf("epg: table selection, MJD/BCD time and event fields -- ok\n");
    return 0;
}
