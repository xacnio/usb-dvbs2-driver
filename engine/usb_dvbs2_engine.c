/* The tuner engine; see usb_dvbs2_engine.h. usb-dvbs2-daemon puts it behind
 * a named pipe. */
#include "dtv_net.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dtv_platform.h"

#include "usb_backend.h"
#include "dtv_thread.h"
#include "hardware_profile.h"
#include "it9300.h"
#include "avl62x1.h"
#include "rda5815m.h"
#include "ts_udp.h"
#include "tool_util.h"
#include "dtv_camd.h"
#include "dtv_csa.h"
#include "ts_epg.h"
#include "usb_dvbs2_engine.h"

/* Does this payload start a picture the decoder can begin from?
 *   MPEG-2  00 00 01 B3  sequence header (or B8, GOP)
 *   H.264   00 00 01 x5  IDR slice / x7 sequence parameter set
 * A parameter set only describes what follows and is repeated before
 * nearly every access unit, so it is reported separately and is not a
 * valid entry point. The whole payload is searched: a fixed window looks
 * past intra pictures whose preamble runs longer than it. */
#define VIDEO_START_NONE    0
#define VIDEO_START_PARAMS  1
#define VIDEO_START_CLEAN   2
static int video_access_unit_starts(const uint8_t *data, size_t size)
{
    size_t i, limit;
    int best = VIDEO_START_NONE;
    if (!data || size < 4u)
        return VIDEO_START_NONE;
    /* Step over the PES header when the payload starts with one. */
    if (size >= 9u && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        size_t header = 9u + data[8];
        if (header >= size)
            return VIDEO_START_NONE;
        data += header;
        size -= header;
    }
    /* Whole payload: the preamble (delimiter, SEI, parameter sets) is
     * routinely longer than a fixed window. */
    limit = size;
    for (i = 0; i + 3u < limit; ++i) {
        uint8_t code;
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
            continue;
        code = data[i + 3];
        if (code == 0xb3u || code == 0xb8u)
            return VIDEO_START_CLEAN;       /* MPEG-2 sequence / GOP */
        if ((code & 0x1fu) == 5u)
            return VIDEO_START_CLEAN;       /* H.264 IDR */
        if ((code & 0x1fu) == 7u)
            best = VIDEO_START_PARAMS;      /* keep looking for the slice */
    }
    return best;
}

/* First video/audio elementary PIDs from the PMT, for channels with no
 * TKGS PID record. */
static void pmt_lookup_stream_pids(const uint8_t *section, size_t size,
                                   uint16_t service_id, uint16_t *video_pid,
                                   uint16_t *audio_pid)
{
    size_t info_len, pos, end;
    if (size < 16 || section[0] != 0x02)
        return;
    if (((uint16_t)((section[3] << 8) | section[4])) != service_id)
        return;
    info_len = ((size_t)(section[10] & 0x0fu) << 8) | section[11];
    pos = 12u + info_len;
    end = size - 4u;
    while (pos + 5u <= end) {
        uint8_t stream_type = section[pos];
        uint16_t pid = (uint16_t)(((section[pos + 1] & 0x1fu) << 8) |
                                  section[pos + 2]);
        size_t es_len = ((size_t)(section[pos + 3] & 0x0fu) << 8) |
                        section[pos + 4];
        int is_video = stream_type == 0x01 || stream_type == 0x02 ||
                       stream_type == 0x10 || stream_type == 0x1b ||
                       stream_type == 0x24 || stream_type == 0x42;
        int is_audio = stream_type == 0x03 || stream_type == 0x04 ||
                       stream_type == 0x0f || stream_type == 0x11 ||
                       stream_type == 0x81 || stream_type == 0x87;
        if (is_video && video_pid && *video_pid >= 0x1fffu)
            *video_pid = pid;
        else if (is_audio && audio_pid && *audio_pid >= 0x1fffu)
            *audio_pid = pid;
        pos += 5u + es_len;
    }
}

/* Collects the CA descriptors (tag 0x09) of a service; their presence
 * means it is scrambled. A service usually runs under several CA systems,
 * so all are kept as candidates and the client moves on when one fails.
 * The provider id stays zero: most systems omit it and the server reads
 * it from the ECM, as tsdecrypt does. */
static void ca_descriptor_candidate(const uint8_t *descriptor,
                                    dtv_camd_candidate *candidates,
                                    size_t capacity, size_t *count)
{
    uint16_t ca_system_id, ecm_pid;
    size_t i;
    if (descriptor[1] < 4 || *count >= capacity)
        return;
    ca_system_id = (uint16_t)((descriptor[2] << 8) | descriptor[3]);
    ecm_pid = (uint16_t)(((descriptor[4] & 0x1fu) << 8) | descriptor[5]);
    if (!ca_system_id)
        return;
    /* pid 0x1fff means no ECM stream: useless for ordinary CA systems,
     * but exactly what BISS says, so the candidate is kept. */
    if (ecm_pid >= 0x1fffu && !DTV_CA_IS_BISS(ca_system_id))
        return;
    /* The same pair repeats in the program and stream loops. */
    for (i = 0; i < *count; ++i)
        if (candidates[i].ca_system_id == ca_system_id &&
            candidates[i].ecm_pid == ecm_pid)
            return;
    candidates[*count].ca_system_id = ca_system_id;
    candidates[*count].ecm_pid = ecm_pid;
    candidates[*count].provider_id = 0;
    ++*count;
}

/* Every CA descriptor of this service; returns 0 for a free service. */
static size_t pmt_collect_ca(const uint8_t *section, size_t size,
                             uint16_t service_id,
                             dtv_camd_candidate *candidates, size_t capacity)
{
    size_t info_len, pos, end, d, count = 0;
    if (size < 16 || section[0] != 0x02)
        return 0;
    if (((uint16_t)((section[3] << 8) | section[4])) != service_id)
        return 0;
    info_len = ((size_t)(section[10] & 0x0fu) << 8) | section[11];
    end = size - 4u;
    if (12u + info_len > end)
        return 0;
    for (d = 12u; d + 2u <= 12u + info_len; d += 2u + section[d + 1])
        if (section[d] == 0x09)
            ca_descriptor_candidate(section + d, candidates, capacity,
                                    &count);
    pos = 12u + info_len;
    while (pos + 5u <= end) {
        size_t es_len = ((size_t)(section[pos + 3] & 0x0fu) << 8) |
                        section[pos + 4];
        size_t es_end = pos + 5u + es_len;
        if (es_end > end)
            break;
        for (d = pos + 5u; d + 2u <= es_end; d += 2u + section[d + 1])
            if (section[d] == 0x09)
                ca_descriptor_candidate(section + d, candidates, capacity,
                                        &count);
        pos = es_end;
    }
    return count;
}

/* ================= tuner engine =================
 *
 * Cold init runs once (firmware, GPIO reset, AVL/RDA init, DiSEqC). The TS
 * stream and the signal monitor run on their own threads, and what they find
 * goes to the host's callbacks. One engine per process. */
static it9300 g_bridge;
static dtv_usb *g_usb;
static const dtv_hardware_profile *g_profile;
#define DEMOD_ADDR (g_profile->demod_i2c_address)
#define TUNER_ADDR (g_profile->tuner_i2c_address)

/* A blind acquisition (TUNE with symbol rate 0): the tuner's widest channel
 * filter, 40 MHz, and a mid-range starting point for the demodulator's own
 * symbol rate search. */
#define BLIND_FILTER_KSPS 40000u
#define BLIND_NOMINAL_KSPS 20000u
static dtv_mutex g_lock;
static dtv_atomic32 g_quit;
static dtv_atomic32 g_tuned;      /* is the demod locked (stream active) */
static dtv_atomic32 g_pid_epoch;  /* incremented on CHANNEL -> PSI cc reset */
static uint32_t g_avl_core;
static int g_lnb_enabled;
static int g_lnb_cold_done;
static unsigned g_udp_port;
static dtv_atomic32 g_status_locked;
/* Asks the next tune to power-cycle the LNB and resend DiSEqC. */
static dtv_atomic32 g_lnb_relight;
static dtv_atomic32 g_status_fec;
static dtv_atomic32 g_status_frame;
static dtv_atomic32 g_status_snr_x100 = -10000;
static dtv_atomic32 g_status_sr_hz;
static char g_sat_date[16];
static char g_sat_time[16];
static uint64_t g_sat_utc_filetime;
static uint64_t g_sat_received_tick;
static char g_epg_now[192];
static char g_epg_next[192];
static char g_epg_now_start[16];
static char g_epg_now_end[16];
static char g_epg_next_start[16];

/* CAM client and descrambler, both optional. g_camd is swapped by the
 * command thread and read by the stream thread under g_camd_lock; the
 * descrambler belongs to the stream thread alone. */
static dtv_mutex g_camd_lock;
static dtv_camd *g_camd;
static dtv_csa *g_csa;
static dtv_camd_config g_camd_config;
static int g_camd_configured;

typedef struct metadata_assembler {
    uint8_t data[4096];
    size_t length;
    size_t expected;
} metadata_assembler;

static metadata_assembler g_eit_assembler;
static metadata_assembler g_time_assembler;
/* active channel (guarded by g_lock) */
static uint16_t g_svc, g_pmt, g_vpid, g_apid;
/* Teletext pid, forwarded like video and audio when the service has it. */
static uint16_t g_ttxpid;
/* Scanner CA hint, a fallback for BISS services whose live PMT omits the
 * CA descriptor. Ordinary CA systems still need a live ECM pid. */
static uint16_t g_hint_ca_system_id, g_hint_ca_pid;
/* active transponder (written by the command thread only) */
static uint32_t g_freq_khz, g_sr_ksps, g_lnb_v, g_tone, g_diseqc;
/* DiSEqC 1.1 uncommitted port, tone burst (0 none, 1 A, 2 B) and repeat
 * count; optional TUNE fields, zero in the old format. */
static uint32_t g_uncommitted, g_burst, g_repeat;

static usb_dvbs2_engine_host g_host;
static dtv_thread *g_stream_thread;

static void log_line(const char *s)
{
    if (g_host.log)
        g_host.log(s);
}

static void log_linef(const char *format, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_line(buffer);
}

static void error_linef(const char *format, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (g_host.error)
        g_host.error(buffer);
    else
        log_line(buffer);
}

static void report_camd(int state, const char *message)
{
    if (g_host.camd_state)
        g_host.camd_state(state, message);
}

/* Borrows the CAM client for one call: the command thread may replace it
 * at any moment, so it is never held across an iteration. */
static dtv_camd *camd_acquire(void)
{
    dtv_camd *camd;
    dtv_mutex_lock(&g_camd_lock);
    camd = g_camd;
    dtv_mutex_unlock(&g_camd_lock);
    return camd;
}

/* Starts, restarts or stops the CAM client. Command thread only. */
static void camd_apply_config(int enabled)
{
    dtv_camd *previous;
    dtv_mutex_lock(&g_camd_lock);
    previous = g_camd;
    g_camd = NULL;
    dtv_mutex_unlock(&g_camd_lock);

    /* Outside the lock: it joins the worker, and the stream thread has to
     * keep draining the USB endpoint meanwhile. */
    if (previous)
        dtv_camd_stop(previous);

    if (enabled && g_camd_configured && !dtv_camd_available()) {
        report_camd(-1, "This driver was built without the card server client");
    } else if (enabled && g_camd_configured) {
        dtv_camd *camd = dtv_camd_start(&g_camd_config);

        dtv_mutex_lock(&g_camd_lock);
        g_camd = camd;
        dtv_mutex_unlock(&g_camd_lock);
        if (camd) {
            char message[160];
            snprintf(message, sizeof(message), "%s %s:%u",
                     dtv_camd_proto_name(g_camd_config.proto),
                     g_camd_config.host, g_camd_config.port);
            report_camd(1, message);
        } else {
            report_camd(-1, "The CAM client could not be started");
        }
    } else {
        report_camd(0, "CAM off");
    }
}

/* Boot screen progress. */
static void log_stage(int percent, const char *text)
{
    if (g_host.stage)
        g_host.stage(percent, text);
}

static unsigned bcd_value(uint8_t value)
{
    return ((value >> 4) & 0x0fu) * 10u + (value & 0x0fu);
}

static void mjd_to_date(uint16_t mjd, int *year, int *month, int *day)
{
    long j = (long)mjd + 2400001L + 68569L;
    long n = 4L * j / 146097L;
    long i;
    j -= (146097L * n + 3L) / 4L;
    i = 4000L * (j + 1L) / 1461001L;
    j = j - 1461L * i / 4L + 31L;
    *month = (int)(80L * j / 2447L);
    *day = (int)(j - 2447L * *month / 80L);
    j = *month / 11L;
    *month = (int)(*month + 2L - 12L * j);
    *year = (int)(100L * (n - 49L) + i + j);
}

static void copy_epg_text(char *target, size_t target_size,
                          const uint8_t *source, size_t source_size)
{
    size_t in = 0, out = 0;
    if (source_size && source[0] < 0x20u)
        in = 1; /* DVB character set selector byte */
    while (in < source_size && out + 1 < target_size) {
        uint8_t ch = source[in++];
        if (ch == '\t' || ch == '\r' || ch == '\n' || ch == '|')
            ch = ' ';
        if (ch >= 0x20u)
            target[out++] = (char)ch;
    }
    target[out] = 0;
}

static void format_event_time(const uint8_t *start, char *text,
                              size_t text_size)
{
    snprintf(text, text_size, "%02u:%02u",
             bcd_value(start[2]), bcd_value(start[3]));
}

static void format_event_end(const uint8_t *start, const uint8_t *duration,
                             char *text, size_t text_size)
{
    unsigned minutes = bcd_value(start[2]) * 60u + bcd_value(start[3]);
    minutes += bcd_value(duration[0]) * 60u + bcd_value(duration[1]);
    minutes %= 24u * 60u;
    snprintf(text, text_size, "%02u:%02u", minutes / 60u, minutes % 60u);
}

static int dvb_event_timestamp(const uint8_t *start, uint64_t *timestamp)
{
    int year, month, day;
    uint16_t mjd;
    if (!start || !timestamp ||
        (start[0] == 0xffu && start[1] == 0xffu))
        return -1;
    mjd = (uint16_t)((start[0] << 8) | start[1]);
    mjd_to_date(mjd, &year, &month, &day);
    *timestamp = dtv_utc_ticks(year, month, day, (int)bcd_value(start[2]),
                               (int)bcd_value(start[3]),
                               (int)bcd_value(start[4]));
    return 0;
}

static uint64_t current_utc_timestamp(void)
{
    return dtv_utc_now_ticks();
}

static uint64_t event_duration_ticks(const uint8_t *duration)
{
    uint64_t seconds = (uint64_t)bcd_value(duration[0]) * 3600u +
                        (uint64_t)bcd_value(duration[1]) * 60u +
                        bcd_value(duration[2]);
    return seconds * 10000000u;
}

static void parse_time_section(const uint8_t *section, size_t size)
{
    int year, month, day;
    uint16_t mjd;
    if (size < 8 || (section[0] != 0x70 && section[0] != 0x73))
        return;
    mjd = (uint16_t)((section[3] << 8) | section[4]);
    mjd_to_date(mjd, &year, &month, &day);
    dtv_mutex_lock(&g_lock);
    snprintf(g_sat_date, sizeof(g_sat_date), "%04d-%02d-%02d",
             year, month, day);
    snprintf(g_sat_time, sizeof(g_sat_time), "%02u:%02u:%02u",
             bcd_value(section[5]), bcd_value(section[6]),
             bcd_value(section[7]));
    g_sat_utc_filetime = dtv_utc_ticks(year, month, day,
                                       (int)bcd_value(section[5]),
                                       (int)bcd_value(section[6]),
                                       (int)bcd_value(section[7]));
    g_sat_received_tick = dtv_tick_ms();
    dtv_mutex_unlock(&g_lock);
}

/* Forwards EIT events to the host, the only place that keeps them
 * across transponders and runs. Sections repeat every few seconds, so
 * each event is remembered by service, event and table version; a version
 * change is what marks a real revision. */
#define EPG_SEEN_SLOTS 8192u

static uint64_t g_epg_seen[EPG_SEEN_SLOTS];
static size_t g_epg_seen_count;

static int epg_already_sent(uint16_t service_id, uint16_t event_id,
                            int version)
{
    /* The key is never zero, so zero can mean "empty slot". */
    uint64_t key = ((uint64_t)service_id << 24) |
                   ((uint64_t)event_id << 8) |
                   (uint64_t)(version & 0x1f) | ((uint64_t)1 << 40);
    size_t slot = (size_t)((key * 1099511628211ull) >> 40) %
                  EPG_SEEN_SLOTS;
    size_t tries = 0;
    /* Full: start over rather than stop sending. Events are stored by
     * service and start time, so repeats are harmless. */
    if (g_epg_seen_count * 4u >= EPG_SEEN_SLOTS * 3u) {
        memset(g_epg_seen, 0, sizeof(g_epg_seen));
        g_epg_seen_count = 0;
    }
    while (tries < EPG_SEEN_SLOTS) {
        if (g_epg_seen[slot] == key)
            return 1;
        if (!g_epg_seen[slot]) {
            g_epg_seen[slot] = key;
            ++g_epg_seen_count;
            return 0;
        }
        slot = (slot + 1u) % EPG_SEEN_SLOTS;
        ++tries;
    }
    return 0;
}

static void forward_epg_section(const uint8_t *section, size_t size)
{
    dtv_epg_event events[64];
    size_t count, i;
    int version = dtv_epg_section_version(section, size);
    if (version < 0)
        return;
    count = dtv_epg_parse_section(section, size, events,
                                  sizeof(events) / sizeof(events[0]));
    for (i = 0; i < count; ++i) {
        const dtv_epg_event *event = &events[i];
        if (epg_already_sent(event->service_id, event->event_id, version))
            continue;
        if (g_host.epg_event)
            g_host.epg_event(event);
    }
}

static void parse_eit_section(const uint8_t *section, size_t size)
{
    uint16_t service_id;
    size_t pos, end;
    uint64_t now = current_utc_timestamp();
    uint64_t next_start_value = ~(uint64_t)0;
    char now_title[192] = "", next_title[192] = "";
    char now_start[16] = "", now_end[16] = "", next_start[16] = "";
    int have_now = 0, have_next = 0;
    unsigned section_number;
    /* p/f may use section 0 for present and 1 for following; section 1
     * must never overwrite the now field. */
    if (size < 18 || section[0] != 0x4e || section[6] > 1)
        return;
    section_number = section[6];
    service_id = (uint16_t)((section[3] << 8) | section[4]);
    dtv_mutex_lock(&g_lock);
    if (service_id != g_svc) {
        dtv_mutex_unlock(&g_lock);
        return;
    }
    dtv_mutex_unlock(&g_lock);
    end = size - 4u;
    pos = 14;
    while (pos + 12u <= end) {
        size_t descriptor_size =
            ((size_t)(section[pos + 10] & 0x0fu) << 8) | section[pos + 11];
        size_t desc = pos + 12u;
        size_t desc_end = desc + descriptor_size;
        char title[192] = "";
        uint64_t start_value = 0, end_value = 0;
        unsigned running_status = (section[pos + 10] >> 5) & 7u;
        int has_time;
        if (desc_end > end)
            break;
        while (desc + 2u <= desc_end) {
            uint8_t tag = section[desc];
            size_t length = section[desc + 1];
            if (desc + 2u + length > desc_end)
                break;
            if (tag == 0x4d && length >= 5u) {
                const uint8_t *data = section + desc + 2u;
                size_t name_length = data[3];
                if (4u + name_length <= length)
                    copy_epg_text(title, sizeof(title), data + 4u,
                                  name_length);
            }
            desc += 2u + length;
        }
        if (!title[0])
            snprintf(title, sizeof(title), "Waiting for programme information");
        has_time = dvb_event_timestamp(section + pos + 2u, &start_value) == 0;
        if (has_time)
            end_value = start_value + event_duration_ticks(section + pos + 7u);
        if (section_number == 0 && !have_now &&
            (running_status == 4u ||
             (has_time && now >= start_value && now < end_value))) {
            snprintf(now_title, sizeof(now_title), "%s", title);
            format_event_time(section + pos + 2u, now_start,
                              sizeof(now_start));
            format_event_end(section + pos + 2u, section + pos + 7u,
                             now_end, sizeof(now_end));
            have_now = 1;
        } else if (has_time && start_value > now &&
                   start_value < next_start_value) {
            snprintf(next_title, sizeof(next_title), "%s", title);
            format_event_time(section + pos + 2u, next_start,
                              sizeof(next_start));
            next_start_value = start_value;
            have_next = 1;
        }
        pos = desc_end;
    }
    dtv_mutex_lock(&g_lock);
    if (service_id == g_svc) {
        if (section_number == 0 && have_now) {
            snprintf(g_epg_now, sizeof(g_epg_now), "%s", now_title);
            snprintf(g_epg_now_start, sizeof(g_epg_now_start), "%s", now_start);
            snprintf(g_epg_now_end, sizeof(g_epg_now_end), "%s", now_end);
        }
        if (have_next) {
            snprintf(g_epg_next, sizeof(g_epg_next), "%s", next_title);
            snprintf(g_epg_next_start, sizeof(g_epg_next_start), "%s",
                     next_start);
        }
    }
    dtv_mutex_unlock(&g_lock);
}

static void process_metadata_section(uint16_t pid, const uint8_t *section,
                                     size_t size)
{
    if (pid == 0x0014u)
        parse_time_section(section, size);
    else if (pid == 0x0012u) {
        /* now/next feeds the info panel; the whole table goes to the
         * guide. */
        parse_eit_section(section, size);
        forward_epg_section(section, size);
    }
}

static void metadata_append(uint16_t pid, metadata_assembler *assembler,
                            const uint8_t *data, size_t size)
{
    while (size) {
        size_t copy;
        if (!assembler->length) {
            while (size && *data == 0xffu) { ++data; --size; }
            if (!size) return;
        }
        copy = size;
        if (copy > sizeof(assembler->data) - assembler->length)
            copy = sizeof(assembler->data) - assembler->length;
        memcpy(assembler->data + assembler->length, data, copy);
        assembler->length += copy;
        data += copy;
        size -= copy;
        if (!assembler->expected && assembler->length >= 3u)
            assembler->expected = 3u +
                (((size_t)assembler->data[1] & 0x0fu) << 8) +
                assembler->data[2];
        if (assembler->expected && assembler->expected <= assembler->length) {
            size_t extra = assembler->length - assembler->expected;
            uint8_t remainder[4096];
            process_metadata_section(pid, assembler->data,
                                     assembler->expected);
            if (extra) memcpy(remainder, assembler->data + assembler->expected,
                              extra);
            assembler->length = assembler->expected = 0;
            if (extra) metadata_append(pid, assembler, remainder, extra);
            return;
        }
        if (assembler->length == sizeof(assembler->data)) {
            assembler->length = assembler->expected = 0;
            return;
        }
    }
}

static void metadata_feed_packet(uint16_t pid, metadata_assembler *assembler,
                                 const uint8_t *packet)
{
    unsigned afc = (packet[3] >> 4) & 3u;
    size_t offset;
    if (afc == 1) offset = 4;
    else if (afc == 3) offset = 5u + packet[4];
    else return;
    if (offset >= 188u) return;
    if (packet[1] & 0x40u) {
        unsigned pointer = packet[offset++];
        if (offset + pointer > 188u) return;
        if (pointer && assembler->length)
            metadata_append(pid, assembler, packet + offset, pointer);
        offset += pointer;
        assembler->length = assembler->expected = 0;
    }
    if (offset < 188u)
        metadata_append(pid, assembler, packet + offset, 188u - offset);
}

/* Cold init: the most expensive steps, run only once.
 * NOTE: skipping the GPIO reset and the 48 KB firmware load when the demod
 * firmware is still in RAM makes init 0.7 s instead of 4.0 s, but acquire
 * then fails (rc=-3) and the full-start fallback costs 7.2 s in total. */
static const dtv_hardware_profile *select_hardware_profile(
    const char *key, unsigned device_index)
{
    const dtv_hardware_profile *profile = dtv_hardware_profile_by_key(key);
    size_t i;
    if (profile)
        return profile;
    if (key && strcmp(key, "auto") != 0)
        return NULL;
    for (i = 0; i < dtv_hardware_profile_count(); ++i) {
        size_t count = 0;
        profile = dtv_hardware_profile_at(i);
        if (profile &&
            dtv_usb_list(profile->vid, profile->pid, NULL, 0, &count) == 0 &&
            count > device_index)
            return profile;
    }
    return NULL;
}

static int daemon_cold_init(const dtv_hardware_profile *profile,
                            unsigned device_index)
{
    uint32_t chip_id = 0, core = 0, fec = 0, mpeg = 0;
    int rc;
    if (!profile || profile->bridge != DTV_BRIDGE_IT9303 ||
        profile->demod != DTV_DEMOD_AVL62X1 ||
        profile->tuner != DTV_TUNER_RDA5815M)
        return -1;
    g_profile = profile;
    log_stage(5, "Looking for USB tuner");
    if (dtv_usb_open_index(&g_usb, profile->vid, profile->pid,
                           device_index) != 0) {
        error_linef("USB device %lu could not be opened.\n", (unsigned long)device_index);
        return -1;
    }
    if (it9300_attach(&g_bridge, g_usb) != 0 || it9300_identify(&g_bridge) != 0)
        return -1;
    log_stage(15, "Loading USB bridge firmware");
    if (it9300_query_fw_version(&g_bridge) != 0 &&
        load_file_to_bridge(&g_bridge, profile->bridge_firmware, 0) != 0)
        return -1;
    if (it9300_bridge_init(&g_bridge) != 0)
        return -1;
    g_bridge.i2c_bus = profile->bridge_i2c_bus;
    /* The GPIO reset erases the demod firmware, so it is reloaded below. */
    it9300_gpio_set(&g_bridge, profile->demod_reset_gpio, 1); dtv_sleep_ms(100);
    it9300_gpio_set(&g_bridge, profile->demod_reset_gpio, 0); dtv_sleep_ms(200);
    it9300_gpio_set(&g_bridge, profile->demod_reset_gpio, 1); dtv_sleep_ms(100);
    if (avl62x1_read32(&g_bridge, DEMOD_ADDR, 0x040000, &chip_id) != 0 ||
        chip_id != AVL62X1_CHIP_ID) {
        error_linef("AVL6261 not found: 0x%08lx\n", (unsigned long)chip_id);
        return -1;
    }
    /* The patch upload is only the first dense burst. The short I2C writes
     * which immediately program clocks, tuner and DiSEqC otherwise form a
     * second uninterrupted USB burst just before "Hardware ready". Pace the
     * whole cold-init tail, never normal tuning or status reads. */
    avl62x1_set_cold_init_pacing(1);
    log_stage(35, "Loading demodulator firmware");
    if (load_file_to_bridge(&g_bridge, profile->demod_firmware, 1) != 0 ||
        avl62x1_wait_ready(&g_bridge, DEMOD_ADDR, 100, 50) != 0) {
        error_linef("AVL6261 firmware failed to boot.\n");
        avl62x1_set_cold_init_pacing(0);
        return -1;
    }
    log_stage(55, "Configuring demodulator clocks");
    rc = avl62x1_load_defaults(&g_bridge, DEMOD_ADDR, 1, 120000000u,
                               &core, &fec, &mpeg);
    if (rc != 0 || core < 10000000u || core > 500000000u) {
        avl62x1_set_cold_init_pacing(0);
        return -1;
    }
    g_avl_core = core;
    rc = avl62x1_init_tuner_i2c(&g_bridge, DEMOD_ADDR, core);
    if (rc == 0) rc = avl62x1_set_tuner_i2c(&g_bridge, DEMOD_ADDR, 1);
    if (rc != 0) {
        avl62x1_set_cold_init_pacing(0);
        return -1;
    }
    log_stage(70, "Starting tuner");
    rc = rda5815m_init(&g_bridge, TUNER_ADDR);
    avl62x1_set_tuner_i2c(&g_bridge, DEMOD_ADDR, 0);
    if (rc != 0) {
        avl62x1_set_cold_init_pacing(0);
        return -1;
    }
    if (avl62x1_init_demod_input(&g_bridge, DEMOD_ADDR, 0) != 0) {
        avl62x1_set_cold_init_pacing(0);
        return -1;
    }
    {
        const avl62x1_ts_config ts_config = {
            .mode = 1, .format = 0, .clock_rising = 1, .clock_phase = 0,
            .adaptive_clock = 1, .error_inverted = 0, .valid_inverted = 0,
            .serial_data_pin = 0, .serial_msb_first = 1
        };
        if (avl62x1_configure_ts(&g_bridge, DEMOD_ADDR, &ts_config, 1) != 0) {
            avl62x1_set_cold_init_pacing(0);
            return -1;
        }
    }
    log_stage(85, "Preparing LNB / DiSEqC");
    if (avl62x1_init_diseqc(&g_bridge, DEMOD_ADDR, core) != 0) {
        avl62x1_set_cold_init_pacing(0);
        return -1;
    }
    avl62x1_set_22khz_tone(&g_bridge, DEMOD_ADDR, 0);
    avl62x1_set_lnb_voltage(&g_bridge, DEMOD_ADDR, 0);
    avl62x1_set_cold_init_pacing(0);
    log_stage(90, "Hardware ready");
    log_line("Daemon cold-init complete.\n");
    return 0;
}

/* Cuts LNB power and the tone. Safe to call twice. Every path that ends
 * the daemon goes through here, including the control client vanishing:
 * nothing else would turn the dish off. */
static void lnb_power_down(void)
{
    if (!g_lnb_enabled)
        return;
    avl62x1_set_22khz_tone(&g_bridge, DEMOD_ADDR, 0);
    avl62x1_set_lnb_voltage(&g_bridge, DEMOD_ADDR, 0);
    /* Cleared so the next tune brings the LNB up from cold. */
    g_lnb_enabled = 0;
}

/* Change transponder; only a re-acquire when LNB/DiSEqC did not change.
 * DiSEqC order: 1.1 (uncommitted) first, then 1.0 (committed), tone burst
 * last -- the uncommitted switch sits upstream in cascaded setups. The
 * 50 ms gap is above the DiSEqC minimum. */
static int send_diseqc_sequence(uint32_t diseqc, uint32_t uncommitted,
                                uint32_t burst, uint32_t repeat,
                                uint32_t lnb_v, uint32_t tone)
{
    int rc;
    for (uint32_t pass = 0; pass <= repeat; ++pass) {
        int repeated = pass > 0;
        if (uncommitted != 0) {
            rc = avl62x1_select_diseqc_uncommitted(&g_bridge,
                                                   DEMOD_ADDR,
                                                   uncommitted, repeated);
            if (rc != 0) return rc;
            dtv_sleep_ms(50);
        }
        if (diseqc != 0) {
            rc = avl62x1_select_diseqc_committed(&g_bridge, DEMOD_ADDR,
                                                 diseqc, lnb_v == 18,
                                                 tone == 22, repeated);
            if (rc != 0) return rc;
            dtv_sleep_ms(50);
        }
    }
    if (burst != 0) {
        /* The burst comes after the DiSEqC commands: simple two-input
         * switches understand nothing else. */
        rc = avl62x1_send_tone_burst(&g_bridge, DEMOD_ADDR,
                                     burst == 2);
        if (rc != 0) return rc;
        dtv_sleep_ms(50);
    }
    return 0;
}

static int daemon_tune(uint32_t freq_khz, uint32_t sr_ksps, uint32_t lnb_v,
                       uint32_t tone, uint32_t diseqc, uint32_t uncommitted,
                       uint32_t burst, uint32_t repeat)
{
    int rc, locked = 0, stable_samples = 0;
    /* A symbol rate of zero asks for a blind acquisition: the tuner opens its
     * widest filter and the demodulator finds the symbol rate itself, as well
     * as the carrier offset it always searches. Slower to lock, so it is given
     * longer; what it found is kept, so a re-acquisition uses it. */
    int blind = sr_ksps == 0;
    uint32_t found_sr_ksps = 0;
    /* Set while there is no carrier: the supply is then power-cycled and the
     * whole LNB sequence sent again, which is what an unplugged cable needs. */
    int relight = dtv_atomic_set(&g_lnb_relight, 0) ||
                  !dtv_atomic_get(&g_tuned);
    int lnb_changed = (lnb_v != g_lnb_v) || (tone != g_tone) ||
                      (diseqc != g_diseqc) ||
                      (uncommitted != g_uncommitted) ||
                      (burst != g_burst) || (repeat != g_repeat) ||
                      !g_lnb_enabled || relight;

    dtv_atomic_set(&g_tuned, 0); /* pause the stream */
    dtv_atomic_set(&g_status_locked, 0);
    dtv_atomic_set(&g_status_fec, 0);
    dtv_atomic_set(&g_status_frame, 0);
    dtv_atomic_set(&g_status_snr_x100, -10000);
    dtv_atomic_set(&g_status_sr_hz, 0);

    log_stage(92, "Tuning transponder");
    rc = avl62x1_set_tuner_i2c(&g_bridge, DEMOD_ADDR, 1);
    if (rc == 0)
        rc = rda5815m_tune(&g_bridge, TUNER_ADDR, freq_khz,
                           blind ? BLIND_FILTER_KSPS : sr_ksps, 0);
    avl62x1_set_tuner_i2c(&g_bridge, DEMOD_ADDR, 0);
    if (rc != 0) { error_linef("tune rc=%d\n", rc); return -1; }

    if (lnb_changed && lnb_v == 0)
        lnb_power_down();
    if (lnb_changed && lnb_v != 0)
        log_stage(94, "Switching LNB / DiSEqC");
    if (lnb_changed && lnb_v != 0) {
        /* A board that latched its LNB supply off -- a shorted or unplugged
         * cable does that -- only comes back once the enable pin goes low. */
        if (relight) {
            avl62x1_set_lnb_voltage(&g_bridge, DEMOD_ADDR, 0);
            dtv_sleep_ms(120);
            g_lnb_enabled = 0;
        }
        rc = avl62x1_set_lnb_voltage(&g_bridge, DEMOD_ADDR, lnb_v);
        if (rc != 0) return -1;
        g_lnb_enabled = 1;
        /* The first start needs an LNB cold start; later changes are short. */
        dtv_sleep_ms(g_lnb_cold_done ? 150 : 500);
        g_lnb_cold_done = 1;
        if (send_diseqc_sequence(diseqc, uncommitted, burst, repeat,
                                 lnb_v, tone) != 0)
            return -1;
        avl62x1_set_22khz_tone(&g_bridge, DEMOD_ADDR, tone == 22);
        dtv_sleep_ms(100);
    }

    log_stage(96, "Searching for signal");
    rc = avl62x1_acquire(&g_bridge, DEMOD_ADDR,
                         (blind ? BLIND_NOMINAL_KSPS : sr_ksps) * 1000u, 0,
                         blind);
    if (rc != 0) { error_linef("acquire rc=%d\n", rc); return -1; }

    for (unsigned poll = 0; poll < (blind ? 45u : 25u) && !g_quit; ++poll) {
        avl62x1_signal_status st;
        dtv_sleep_ms(80);
        if (avl62x1_get_signal_status(&g_bridge, DEMOD_ADDR, &st) != 0)
            break;
        if (st.locked) {
            locked = 1;
            dtv_atomic_set(&g_status_locked, st.locked);
            dtv_atomic_set(&g_status_fec, st.fec_locked);
            dtv_atomic_set(&g_status_frame, st.frame_locked);
            dtv_atomic_set(&g_status_snr_x100, st.snr_db_x100);
            dtv_atomic_set(&g_status_sr_hz, (int32_t)st.symbol_rate_hz);
            /* LOCK can be raised before the FEC/SNR registers follow, so
             * wait for two complete samples to keep a transient
             * 0 dB / -100 dB value out of the OSD. */
            if (st.fec_locked && st.frame_locked &&
                st.snr_db_x100 > -1000 && st.symbol_rate_hz != 0) {
                found_sr_ksps = (st.symbol_rate_hz + 500u) / 1000u;
                if (++stable_samples >= 2) {
                    log_linef("Lock stable%s: SNR=%d.%02d dB SR=%lu (%u ms)\n",
                              blind ? " (blind)" : "",
                              st.snr_db_x100 / 100,
                              abs(st.snr_db_x100 % 100),
                              (unsigned long)st.symbol_rate_hz,
                              (poll + 1) * 80u);
                    break;
                }
            } else {
                stable_samples = 0;
            }
        } else {
            stable_samples = 0;
        }
    }
    g_freq_khz = freq_khz;
    g_sr_ksps = blind && locked && found_sr_ksps ? found_sr_ksps : sr_ksps;
    g_lnb_v = lnb_v; g_tone = tone; g_diseqc = diseqc;
    g_uncommitted = uncommitted; g_burst = burst; g_repeat = repeat;
    if (locked) { dtv_atomic_set(&g_tuned, 1); log_stage(100, "Broadcast ready"); }
    else log_line("No lock was achieved.\n");
    return locked ? 0 : 1;
}


/* When the stream thread last had bytes, for the monitor below. */
static dtv_atomic32 g_stream_data_tick;
/* One tune at a time: the monitor and a TUNE command must not interleave. */
static dtv_mutex g_tune_lock;

/* Re-acquires while there is no carrier: nothing else would notice a cable
 * plugged back in, since the demod is read only while tuning. */
static void daemon_signal_monitor(void *arg)
{
    (void)arg;
    while (!g_quit) {
        uint32_t idle;
        dtv_sleep_ms(1000);
        if (g_freq_khz == 0 || g_quit)
            continue;
        idle = dtv_tick32() -
               (uint32_t)dtv_atomic_get(&g_stream_data_tick);
        /* Never locked, or locked and then gone silent. */
        if (dtv_atomic_get(&g_tuned) ? idle > 8000u
                                                       : idle > 5000u) {
            avl62x1_signal_status st;
            int still_locked;
            dtv_mutex_lock(&g_tune_lock);
            /* A busy host can starve these USB reads without the carrier
             * itself moving; a cheap register read tells the two apart
             * before relighting the LNB over a carrier that never left. */
            still_locked = dtv_atomic_get(&g_tuned) &&
                          avl62x1_get_signal_status(&g_bridge, DEMOD_ADDR,
                                                    &st) == 0 && st.locked;
            if (still_locked) {
                log_line("Carrier still locked; USB stream stalled, "
                        "waiting it out.\n");
            } else {
                dtv_atomic_set(&g_lnb_relight, 1);
                log_line("Re-acquiring the carrier.\n");
                daemon_tune(g_freq_khz, g_sr_ksps, g_lnb_v, g_tone, g_diseqc,
                            g_uncommitted, g_burst, g_repeat);
            }
            dtv_atomic_set(&g_stream_data_tick, (int32_t)dtv_tick32());
            dtv_mutex_unlock(&g_tune_lock);
        }
    }
}

/* TS stream thread: filters the active PIDs, switched instantly by CHANNEL. */
static void daemon_stream_thread(void *arg)
{
    dtv_socket sock = DTV_INVALID_SOCKET;
    udp_ts_writer writer;
    uint8_t buffer[65424 + 5 * 188];
    size_t pending = 0;
    /* Transfers stay queued in the driver at all times, so descrambling,
     * filtering and the socket never leave the bridge FIFO to fill. About
     * a sixth of a second in flight at transponder rates, with a second
     * more in the ring behind it. dtv_usb_bulk_in() is the fallback where
     * the queue cannot be set up. */
    dtv_usb_stream *stream = NULL;
    unsigned long long ring_dropped = 0;
    int stream_rc;
    /* A transponder bursts tens of megabits a second and the player drains
     * on its own schedule; 4 MB is roughly a second of headroom. */
    int synced = 0, send_buf = 4 * 1024 * 1024;
    /* Two things are thrown away at the start of a channel:
     *
     * was_tuned/flush_until: the bridge FIFO still holds the tail of the
     * previous transponder and the noise from before the lock. Those bytes
     * are well-framed 188-byte packets, so sync cannot tell them apart.
     *
     * video_open: a channel is always joined mid-GOP, so video is held
     * back until a random access point rather than shown as blocks.
     *
     * Continuity counters are checked here, before UDP, so a gap can only
     * have come from the satellite or the bridge; the player counts its
     * own at the far end of the socket, and comparing the two says which
     * half of the path is losing packets. Send failures are counted
     * beside them, since a socket that cannot keep up otherwise looks
     * exactly like a signal problem. */
    struct { uint16_t pid; uint8_t next_cc; } cc_state[24];
    int cc_count = 0;
    unsigned long cc_errors = 0, send_failures = 0;
    unsigned long overflow_packets = 0;
    unsigned long reported_cc = 0, reported_failures = 0;
    unsigned long reported_overflow = 0;
    uint64_t next_loss_report = 0;
    int was_tuned = 0;
    uint32_t last_data_tick = dtv_tick32();
    int signal_reported = 1;   /* 0 once the silence was announced */
    uint32_t flush_until = 0;
    int video_open = 0;
    uint32_t video_wait_start = 0;
    /* Long enough to clear the FIFO, short enough to hide inside the lock
     * time already paid. */
#define TS_FIFO_FLUSH_MS 150
    /* How long to wait for an intra picture before opening anyway, so a
     * broadcast that carries none never hangs. Broadcasts run half a
     * second to two seconds between intra frames, so a shorter timeout
     * (1500 ms was tried) gives up before the picture was due. Still under
     * the 3000 ms vlc_wait_first_frame() allows for a first frame. */
#define TS_RAI_WAIT_MS  2000
    uint8_t pat_cc = 0, pmt_cc = 0;
    uint16_t pcr_pid = 0x1fff;
    uint16_t ttxpid = 0x1fff;
    /* Teletext pid from the PMT, kept until the channel changes: a channel
     * list scanned before teletext existed reports none. */
    uint16_t learned_ttx = 0x1fff;
    /* TKGS channels carry no PMT pid; it is recovered from the PAT and
     * cached until the channel changes. */
    uint16_t learned_pmt = 0x1fff;
    uint16_t learned_vpid = 0x1fff, learned_apid = 0x1fff;
    uint16_t stream_pids[UDP_TS_MAX_PROGRAM_PIDS];
    size_t stream_pid_count = 0;
    int reported_ca = -1;
    psi_section_assembler pat_asm, pmt_asm, ecm_asm;
    int32_t epoch = -1;
    /* Conditional access state, all of it per channel. */
    uint16_t ecm_pid = 0x1fff;
    unsigned key_generation = 0;
    int keys_installed = 0;
    dtv_camd_state reported_state = DTV_CAMD_STATE_OFF;
    char reported_message[128] = "";
    /* BISS asks with a synthetic ECM, one per elementary pid until the
     * server recognises one; which pid holds the key is not announced. */
    uint16_t biss_pids[UDP_TS_MAX_PROGRAM_PIDS + 1];
    size_t biss_pid_count = 0;
    size_t biss_pid_index = 0;
    uint64_t next_biss_try = 0;
    uint64_t next_camd_poll = 0;
    (void)arg;
    memset(&pat_asm, 0, sizeof(pat_asm));
    memset(&pmt_asm, 0, sizeof(pmt_asm));
    memset(&ecm_asm, 0, sizeof(ecm_asm));

    /* This thread carries the picture: when it is late, packets are lost
     * where nothing downstream can recover them. */
    void *boost = dtv_thread_realtime_begin();

    if (dtv_net_startup() != 0) {
        dtv_thread_realtime_end(boost);
        return;
    }
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == DTV_INVALID_SOCKET) {
        dtv_net_cleanup();
        dtv_thread_realtime_end(boost);
        return;
    }
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char *)&send_buf, sizeof(send_buf));
    memset(&writer, 0, sizeof(writer));
    writer.socket_handle = sock;
    writer.destination.sin_family = AF_INET;
    writer.destination.sin_port = htons((uint16_t)g_udp_port);
    writer.destination.sin_addr.s_addr = inet_addr("127.0.0.1");
    writer.destination_alt = writer.destination;
    writer.destination_alt.sin_port = htons((uint16_t)(g_udp_port + 1u));
    writer.has_alt = 1;

    stream_rc = dtv_usb_stream_open(g_usb, DTV_EP_TS_IN, 24, 32u * 1024u,
                                    4u * 1024u * 1024u, &stream);
    if (stream_rc != 0) {
        stream = NULL;
        log_linef("The queued USB reader could not be started (%s); falling "
                  "back to one transfer at a time.\n",
                  dtv_usb_strerror(stream_rc));
    }

    while (!g_quit) {
        uint16_t svc, pmt, vpid, apid, hint_caid;
        size_t available, pos = 0;
        int n;
        int filter;
        /* No demod/I2C telemetry here: it would stall this thread for
         * several USB command transfers, the IT9303 FIFO would overflow
         * and the picture freeze. The OSD uses the status from
         * daemon_tune(). */
        if (!dtv_atomic_get(&g_tuned)) {
            pending = 0; synced = 0;
            was_tuned = 0;
            /* Only the local clock: the monitor's must age here, or a tune
             * that never locked would never be retried. */
            last_data_tick = dtv_tick32();
            /* Transfers keep running through a retune -- stopping them
             * would reopen the gap they close -- so what they collect is
             * dropped here rather than left to age in the ring. */
            dtv_usb_stream_flush(stream);
            dtv_sleep_ms(15);
            continue;
        }
        if (!was_tuned) {
            /* Lock just reached; everything until the window closes
             * belongs to before it. */
            was_tuned = 1;
            flush_until = dtv_tick32() + TS_FIFO_FLUSH_MS;
            pending = 0;
            synced = 0;
            dtv_usb_stream_flush(stream);
        }
        dtv_mutex_lock(&g_lock);
        svc = g_svc; pmt = g_pmt; vpid = g_vpid; apid = g_apid;
        ttxpid = g_ttxpid;
        hint_caid = g_hint_ca_system_id;
        dtv_mutex_unlock(&g_lock);
        if (dtv_atomic_get(&g_pid_epoch) != epoch) {
            epoch = dtv_atomic_get(&g_pid_epoch);
            pat_cc = pmt_cc = 0; pcr_pid = vpid; synced = 0; pending = 0;
            /* A new service is joined mid-picture like a new channel, so
             * the wait for a random access point starts again. */
            video_open = 0;
            video_wait_start = dtv_tick32();
            /* Per channel, not per daemon run: the question is whether
             * THIS channel is breaking up. */
            cc_count = 0;
            cc_errors = send_failures = 0;
            overflow_packets = 0;
            reported_cc = reported_failures = reported_overflow = 0;
            /* Ring counts run for the life of the stream, so the per
             * channel figure is measured from here. */
            dtv_usb_stream_stats(stream, &ring_dropped, NULL);
            learned_pmt = 0x1fff;
            learned_vpid = learned_apid = 0x1fff;
            learned_ttx = 0x1fff;
            stream_pid_count = 0;
            reported_ca = -1;
            pat_asm.length = pat_asm.expected = 0;
            pmt_asm.length = pmt_asm.expected = 0;
            /* The new service has its own CA systems and control words;
             * nothing is descrambled until the PMT installs them. */
            ecm_asm.length = ecm_asm.expected = 0;
            ecm_pid = 0x1fff;
            keys_installed = 0;
            key_generation = 0;
            biss_pid_count = biss_pid_index = 0;
            next_biss_try = 0;
            if (g_csa)
                dtv_csa_clear_keys(g_csa);
        }
        if (pmt >= 0x1fffu)
            pmt = learned_pmt;
        /* After the epoch reset above, so a zap never keeps the previous
         * teletext pid in the filter. */
        if (ttxpid >= 0x1fffu)
            ttxpid = learned_ttx;
        n = stream ? dtv_usb_stream_read(stream, buffer + pending, 65424, 200)
                   : dtv_usb_bulk_in(g_usb, DTV_EP_TS_IN, buffer + pending,
                                     65424, 200);
        /* The stream only ends by itself when the receiver went away. */
        /* Lock is read only while tuning, so a dish or cable problem shows
         * up here: the carrier stops and no packet arrives. */
        if (n > 0) {
            last_data_tick = dtv_tick32();
            dtv_atomic_set(&g_stream_data_tick, (int32_t)last_data_tick);
            if (!signal_reported) {
                signal_reported = 1;
                if (g_host.signal_state)
                    g_host.signal_state(1);
            }
        } else if (signal_reported &&
                   /* A busy host can delay these reads by a couple of
                    * seconds on its own; the retune watchdog below already
                    * covers a real outage at 8 s. */
                   dtv_tick32() - last_data_tick > 4000u) {
            signal_reported = 0;
            if (g_host.signal_state)
                g_host.signal_state(0);
        }
        if (dtv_usb_stream_lost(stream)) {
            if (g_host.device_lost)
                g_host.device_lost("The receiver was disconnected");
            break;
        }
        if (n <= 0) continue;
        /* Still draining pre-lock FIFO data: the read above is what empties
         * it, so it has to be fetched and then dropped. */
        if (flush_until && (int32_t)(dtv_tick32() - flush_until) < 0) {
            pending = 0;
            synced = 0;
            continue;
        }
        if (flush_until) {
            /* First genuinely new data of the channel, so the wait for a
             * picture starts here. Doing it on the tune and not only on a
             * PID change covers the first channel after a cold start. */
            flush_until = 0;
            video_open = 0;
            video_wait_start = dtv_tick32();
        }
        /* A timer never started can never expire, which would hold video
         * for good on a broadcast that sets no random access indicator. */
        if (!video_open && !video_wait_start)
            video_wait_start = dtv_tick32();
        available = pending + (size_t)n;
        /* Filter whenever the service id is known: the PMT pid comes from
         * the PAT and the elementary pids from the PMT. Requiring them up
         * front let TKGS channels through unfiltered, and the player then
         * showed whichever service came first in the multiplex. */
        filter = svc != 0;

        /* Conditional access: refresh the control words, then decrypt every
         * scrambled packet of this read in one pass so libdvbcsa can batch.
         * A decrypted packet has its scrambling bits cleared, so a run
         * visited twice is never decrypted twice. */
        {
            dtv_camd *camd = camd_acquire();
            if (camd) {
                unsigned generation = 0;
                uint8_t even[8], odd[8];
                ecm_pid = dtv_camd_ecm_pid(camd);
                if (dtv_camd_get_keys(camd, even, odd, &generation) &&
                    (!keys_installed || generation != key_generation)) {
                    /* An all-zero word means not this parity. */
                    static const uint8_t none[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
                    dtv_csa_set_keys(g_csa, even,
                                     memcmp(even, none, 8) != 0, odd,
                                     memcmp(odd, none, 8) != 0);
                    key_generation = generation;
                    keys_installed = 1;
                }

                /* UI status, at most four times a second and only on a
                 * change. */
                if (dtv_tick_ms() >= next_camd_poll) {
                    dtv_camd_status status;
                    dtv_camd_get_status(camd, &status);
                    next_camd_poll = dtv_tick_ms() + 250u;
                    /* BISS: nothing is broadcast to ask with, so the ECM
                     * is made here, one per elementary pid, until the
                     * server answers. A BISS key does not roll, so one
                     * control word is the whole key. */
                    if (DTV_CA_IS_BISS(status.ca_system_id) &&
                        !keys_installed && biss_pid_count &&
                        dtv_tick_ms() >= next_biss_try) {
                        uint8_t ecm[DTV_CAMD_BISS_ECM_SIZE];
                        size_t size = dtv_camd_build_biss_ecm(
                            svc, biss_pids[biss_pid_index], ecm);
                        dtv_camd_submit_ecm(camd, ecm, size);
                        biss_pid_index = (biss_pid_index + 1u) %
                                         biss_pid_count;
                        next_biss_try = dtv_tick_ms() + 2000u;
                    }
                    if (status.state != reported_state ||
                        strcmp(status.message, reported_message) != 0) {
                        reported_state = status.state;
                        snprintf(reported_message, sizeof(reported_message),
                                 "%s", status.message);
                        report_camd((int)status.state, status.message);
                    }
                }
            } else {
                ecm_pid = 0x1fff;
                if (keys_installed) {
                    dtv_csa_clear_keys(g_csa);
                    keys_installed = 0;
                }
            }
        }
        if (keys_installed && dtv_csa_has_key(g_csa)) {
            size_t start = synced ? pos : find_ts_sync(buffer, available, pos);
            if (start != (size_t)-1 && available > start)
                dtv_csa_decrypt_run(g_csa, buffer + start,
                                    (available - start) / 188u);
        }

        while (pos + 188 <= available) {
            const uint8_t *pk;
            uint16_t pid;
            if (!synced) {
                size_t s = find_ts_sync(buffer, available, pos);
                if (s == (size_t)-1) break;
                pos = s; synced = 1;
            }
            if (buffer[pos] != 0x47) { synced = 0; ++pos; continue; }
            pk = buffer + pos;
            pid = (uint16_t)(((pk[1] & 0x1fu) << 8) | pk[2]);
            /* ECM packets are consumed here: the player has no use for
             * them and the CAM client wants whole sections. */
            if (pid == ecm_pid && ecm_pid < 0x1fffu) {
                const uint8_t *sec; size_t ss;
                if (psi_assemble(&ecm_asm, pk, &sec, &ss)) {
                    dtv_camd *camd = camd_acquire();
                    if (camd)
                        dtv_camd_submit_ecm(camd, sec, ss);
                }
                pos += 188;
                continue;
            }
            if (pid == 0x0012u)
                metadata_feed_packet(pid, &g_eit_assembler, pk);
            else if (pid == 0x0014u)
                metadata_feed_packet(pid, &g_time_assembler, pk);
            if (!filter) {
                udp_ts_send_packet(&writer, pk);
            } else {
                if (pid == 0) {
                    const uint8_t *sec; size_t ss;
                    if (psi_assemble(&pat_asm, pk, &sec, &ss)) {
                        if (pmt >= 0x1fffu) {
                            pmt = pat_lookup_pmt_pid(sec, ss, svc);
                            learned_pmt = pmt;
                        }
                        if (pmt < 0x1fffu)
                            udp_ts_send_single_pat(&writer, sec, ss, svc, pmt,
                                                   &pat_cc);
                    }
                } else if (pid == pmt && pmt < 0x1fffu) {
                    const uint8_t *sec; size_t ss;
                    if (psi_assemble(&pmt_asm, pk, &sec, &ss)) {
                        /* Learn the PIDs from the PMT when unknown. */
                        if (vpid >= 0x1fffu || apid >= 0x1fffu)
                            pmt_lookup_stream_pids(sec, ss, svc,
                                                   vpid >= 0x1fffu ? &vpid : NULL,
                                                   apid >= 0x1fffu ? &apid : NULL);
                        if (learned_vpid != vpid || learned_apid != apid) {
                            learned_vpid = vpid;
                            learned_apid = apid;
                            if (pcr_pid >= 0x1fffu)
                                pcr_pid = vpid;
                        }
                        /* Report the scrambling once per service with the
                         * system id and pid, and hand every CA system to
                         * the CAM client. */
                        if (reported_ca < 0) {
                            dtv_camd_candidate candidates[
                                DTV_CAMD_MAX_CANDIDATES];
                            size_t candidate_count = pmt_collect_ca(
                                sec, ss, svc, candidates,
                                DTV_CAMD_MAX_CANDIDATES);
                            /* Some fixed-key feeds are BISS but advertise
                             * no CA descriptor; fall back to the hint the
                             * channel scan stored. */
                            if (!candidate_count &&
                                DTV_CA_IS_BISS(hint_caid)) {
                                candidates[0].ca_system_id = hint_caid;
                                candidates[0].ecm_pid = 0x1fff;
                                candidates[0].provider_id = 0;
                                candidate_count = 1;
                            }
                            dtv_camd *camd = camd_acquire();
                            uint16_t sys = candidate_count
                                               ? candidates[0].ca_system_id
                                               : 0;
                            uint16_t capid = candidate_count
                                                 ? candidates[0].ecm_pid
                                                 : 0x1fff;
                            reported_ca = candidate_count != 0;
                            if (g_host.service_ca)
                                g_host.service_ca(svc, reported_ca, sys,
                                                  capid);
                            if (camd) {
                                dtv_camd_set_service(camd, svc, candidates,
                                                     candidate_count);
                                ecm_pid = dtv_camd_ecm_pid(camd);
                            }
                            /* Pids a BISS key may be stored against, video
                             * first because that is the usual one. */
                            biss_pid_count = biss_pid_index = 0;
                            next_biss_try = 0;
                            if (vpid < 0x1fffu)
                                biss_pids[biss_pid_count++] = vpid;
                            {
                                size_t p;
                                for (p = 0; p < stream_pid_count &&
                                     biss_pid_count < UDP_TS_MAX_PROGRAM_PIDS;
                                     ++p)
                                    if (stream_pids[p] != vpid)
                                        biss_pids[biss_pid_count++] =
                                            stream_pids[p];
                            }
                            /* Radio/data BISS keys are commonly indexed
                             * with 0x1fff rather than an elementary pid
                             * (F <sid>1FFF in SoftCam.Key). */
                            if (biss_pid_count <
                                sizeof(biss_pids) / sizeof(biss_pids[0]))
                                biss_pids[biss_pid_count++] = 0x1fff;
                        }
                        /* Strip the CA descriptors only while control words
                         * exist; without them the payload is still
                         * scrambled and the table should say so. */
                        udp_ts_send_selected_pmt(&writer, sec, ss, pmt, svc,
                                                 &ttxpid, &pcr_pid,
                                                 stream_pids,
                                                 UDP_TS_MAX_PROGRAM_PIDS,
                                                 &stream_pid_count, &pmt_cc,
                                                 keys_installed &&
                                                 dtv_csa_has_key(g_csa));
                        if (ttxpid != learned_ttx) {
                            learned_ttx = ttxpid;
                            if (g_host.service_teletext)
                                g_host.service_teletext(svc, ttxpid);
                        }
                    }
                } else if (udp_ts_pid_in_list(pid, stream_pids,
                                              stream_pid_count) ||
                           (pid == vpid && vpid < 0x1fffu) ||
                           (pid == apid && apid < 0x1fffu) ||
                           (pid == ttxpid && ttxpid < 0x1fffu) ||
                           (pid == pcr_pid && pcr_pid < 0x1fffu)) {
                    /* Video waits for a picture it can start from;
                     * audio, teletext and tables go straight through so the
                     * service is ready when the picture opens. The random
                     * access indicator sits in the adaptation field: byte 3
                     * bits 5-4 say whether there is one, byte 4 is its
                     * length, bit 6 of byte 5 is the flag. */
                    int is_video = (pid == vpid && vpid < 0x1fffu);
                    if (is_video && !video_open) {
                        unsigned afc = (pk[3] >> 4) & 0x3u;
                        size_t payload = 4u;
                        int rai = 0;
                        if (afc == 2u || afc == 3u) {
                            if (pk[4] >= 1u)
                                rai = (pk[5] >> 6) & 1;
                            payload = 5u + pk[4];
                        }
                        /* Both signals are worked out, not just the first
                         * to answer, and only at a payload unit start. A
                         * start code is the picture itself saying it can be
                         * decoded from here; the flag is only the
                         * multiplexer claiming a good join point, and a
                         * gradual-refresh stream sets it where no complete
                         * picture exists. */
                        int start_code =
                            (afc != 2u && payload < 188u)
                              ? video_access_unit_starts(pk + payload,
                                                         188u - payload)
                              : VIDEO_START_NONE;
                        /* An intra picture when the broadcast has one, the
                         * transport flag when it does not. Measured here:
                         * these channels carry no IDR slices at all, only
                         * open GOPs marked with the flag, so holding out
                         * for an IDR would only delay every zap. A
                         * parameter set alone is still refused. */
                        if ((pk[1] & 0x40u) &&
                            (start_code == VIDEO_START_CLEAN || rai)) {
                            video_open = 1;
                            /* Which signal found it and how long it took; a
                             * channel opening on the timeout below is one
                             * whose start was not recognised. */
                            log_linef("Video opened after %lu ms on %s.\n",
                                      (unsigned long)(dtv_tick32() -
                                                      video_wait_start),
                                      start_code == VIDEO_START_CLEAN
                                        ? "an intra picture"
                                        : "the access flag (open GOP: the "
                                          "decoder needs a moment to "
                                          "converge)");
                        } else if (video_wait_start &&
                                   dtv_tick32() - video_wait_start >
                                       TS_RAI_WAIT_MS) {
                            /* Neither signal turned up: open anyway and
                             * accept the blocks rather than stay black. */
                            video_open = 1;
                            log_line("No picture start found within the "
                                     "wait; opening video anyway (the "
                                     "first second may break up).\n");
                        }
                    }
                    if (!is_video || video_open) {
                        /* Counted on what is forwarded, so it means gaps
                         * that were already there when we received them. */
                        unsigned afc = (pk[3] >> 4) & 0x3u;
                        if (afc == 1u || afc == 3u) {
                            uint8_t cc = pk[3] & 0x0fu;
                            int discontinuity =
                                (afc == 3u && pk[4] >= 1u &&
                                 (pk[5] & 0x80u)) ? 1 : 0;
                            int slot;
                            for (slot = 0; slot < cc_count; ++slot)
                                if (cc_state[slot].pid == pid)
                                    break;
                            if (slot == cc_count) {
                                if (cc_count <
                                    (int)(sizeof(cc_state) /
                                          sizeof(cc_state[0]))) {
                                    cc_state[cc_count].pid = pid;
                                    cc_state[cc_count].next_cc =
                                        (uint8_t)((cc + 1u) & 0x0fu);
                                    ++cc_count;
                                }
                            } else {
                                /* A repeated counter is not a gap, nor is
                                 * anything after a declared discontinuity. */
                                if (!discontinuity &&
                                    cc != cc_state[slot].next_cc &&
                                    cc != (uint8_t)((cc_state[slot].next_cc
                                                     + 15u) & 0x0fu))
                                    ++cc_errors;
                                cc_state[slot].next_cc =
                                    (uint8_t)((cc + 1u) & 0x0fu);
                            }
                        }
                        if (udp_ts_send_packet(&writer, pk) != 0)
                            ++send_failures;
                    }
                }
            }
            pos += 188;
        }
        /* Reported rather than logged: the info page shows them beside the
         * player count. Only on a change, at most every other second. */
        {
            /* Packets the queued reader dropped because this thread fell
             * behind. Unlike the gaps above this is loss caused locally,
             * and stays at zero on a healthy system. */
            unsigned long long total = 0;
            dtv_usb_stream_stats(stream, &total, NULL);
            overflow_packets = total > ring_dropped
                                 ? (unsigned long)((total - ring_dropped) / 188u)
                                 : 0;
        }
        if ((cc_errors != reported_cc || send_failures != reported_failures ||
             overflow_packets != reported_overflow) &&
            dtv_tick_ms() >= next_loss_report) {
            reported_cc = cc_errors;
            reported_failures = send_failures;
            reported_overflow = overflow_packets;
            next_loss_report = dtv_tick_ms() + 2000u;
            if (g_host.loss)
                g_host.loss(cc_errors, send_failures, overflow_packets);
        }
        if (!synced && available - pos > 4u * 188u)
            pos = available - 4u * 188u;
        pending = available - pos;
        if (pending) memmove(buffer, buffer + pos, pending);
        if (pending > sizeof(buffer) - 65424u) { pending = 0; synced = 0; }
    }
    udp_ts_flush(&writer);
    dtv_usb_stream_close(stream);
    dtv_socket_close(sock);
    dtv_net_cleanup();
    dtv_thread_realtime_end(boost);
}

static void set_channel(uint16_t svc, uint16_t pmt, uint16_t vpid,
                        uint16_t apid, uint16_t ttxpid,
                        uint16_t hint_caid, uint16_t hint_capid)
{
    dtv_mutex_lock(&g_lock);
    g_svc = svc; g_pmt = pmt; g_vpid = vpid; g_apid = apid;
    g_ttxpid = ttxpid;
    g_hint_ca_system_id = hint_caid;
    g_hint_ca_pid = hint_capid;
    g_epg_now[0] = g_epg_next[0] = 0;
    g_epg_now_start[0] = g_epg_now_end[0] = g_epg_next_start[0] = 0;
    dtv_mutex_unlock(&g_lock);
    g_eit_assembler.length = g_eit_assembler.expected = 0;
    /* A new channel may be on a new carrier and the old events were sent
     * long ago; forgetting lets them be sent again on return. */
    memset(g_epg_seen, 0, sizeof(g_epg_seen));
    g_epg_seen_count = 0;
    dtv_atomic_inc(&g_pid_epoch);
    /* Park the CAM client until the new PMT names its CA systems, or it
     * would keep asking on the previous ECM pid and install the answer
     * against the new picture. */
    {
        dtv_camd *camd = camd_acquire();
        if (camd)
            dtv_camd_set_service(camd, svc, NULL, 0);
    }
}

/* ================= public interface ================= */

int usb_dvbs2_engine_open(const usb_dvbs2_engine_config *config,
                          const usb_dvbs2_engine_host *host)
{
    const char *profile_key =
        config && config->profile_key ? config->profile_key : "auto";
    unsigned device_index = config ? config->device_index : 0;
    const dtv_hardware_profile *profile;

    memset(&g_host, 0, sizeof(g_host));
    if (host)
        g_host = *host;
    g_udp_port = config && config->udp_port ? config->udp_port : 5560;
    if (config && config->camd) {
        g_camd_config = *config->camd;
        g_camd_configured = 1;
    }
    dtv_atomic_set(&g_quit, 0);
    dtv_mutex_init(&g_lock);
    dtv_mutex_init(&g_camd_lock);
    dtv_mutex_init(&g_tune_lock);
    dtv_atomic_set(&g_stream_data_tick, (int32_t)dtv_tick32());
    g_svc = g_pmt = g_vpid = g_apid = g_ttxpid = 0x1fff;
    /* The descrambler belongs to the stream thread for the whole run; only
     * the keys come and go. */
    g_csa = dtv_csa_create();
    if (!g_csa)
        error_linef("The CSA decoder could not be started; scrambled "
                    "broadcasts will not be decrypted.\n");

    profile = select_hardware_profile(profile_key, device_index);
    if (!profile) {
        error_linef("No hardware profile or compatible USB device was found: %s\n",
                    profile_key);
        dtv_mutex_destroy(&g_lock);
        return 2;
    }
    log_linef("Hardware profile: %s [%04x:%04x]\n", profile->name,
              profile->vid, profile->pid);
    if (daemon_cold_init(profile, device_index) != 0) {
        error_linef("Cold-init failed.\n");
        dtv_mutex_destroy(&g_lock);
        return 2;
    }
    /* Opened before the first tune, so the login is done by the time a
     * scrambled channel needs it. */
    if (g_camd_configured)
        camd_apply_config(1);
    /* The monitor runs for the life of the process. */
    dtv_thread_detach(dtv_thread_start(daemon_signal_monitor, NULL));
    g_stream_thread = dtv_thread_start(daemon_stream_thread, NULL);
    if (!g_stream_thread) {
        error_linef("no stream thread\n");
        return 3;
    }
    return 0;
}

void usb_dvbs2_engine_close(void)
{
    int stream_stopped;
    if (!g_stream_thread)
        return;
    dtv_atomic_set(&g_quit, 1);
    /* Long enough for the stream thread to retire its queued USB transfers:
     * the device may not be closed while any of them is outstanding. */
    stream_stopped = dtv_thread_join(g_stream_thread, 8000) == 0;
    lnb_power_down();
    if (!stream_stopped)
        dtv_thread_detach(g_stream_thread);
    g_stream_thread = NULL;
    /* After the stream thread has stopped: it is the only user of the
     * descrambler, and the CAM worker must be joined before its lock goes. */
    camd_apply_config(0);
    dtv_csa_destroy(g_csa);
    g_csa = NULL;
    dtv_mutex_destroy(&g_camd_lock);
    dtv_mutex_destroy(&g_lock);
    /* Only when nothing is still reading: a thread that never came back
     * has transfers a wedged driver never returned, and closing under it
     * would crash on the way out. The handle goes with the process. */
    if (stream_stopped)
        dtv_usb_close(g_usb);
}

int usb_dvbs2_engine_tune(uint32_t lband_khz, uint32_t symbol_rate_ksps,
                          uint32_t lnb_volts, uint32_t tone_khz,
                          uint32_t diseqc, uint32_t uncommitted,
                          uint32_t burst, uint32_t repeat)
{
    int rc;
    if (lband_khz == g_freq_khz && symbol_rate_ksps == g_sr_ksps &&
        lnb_volts == g_lnb_v && tone_khz == g_tone && diseqc == g_diseqc &&
        uncommitted == g_uncommitted && burst == g_burst &&
        repeat == g_repeat && dtv_atomic_get(&g_tuned) &&
        dtv_tick32() -
            (uint32_t)dtv_atomic_get(&g_stream_data_tick) <
            3000u) {
        log_line("TUNE skipped (same transponder).\n");
        return 0;
    }
    dtv_mutex_lock(&g_tune_lock);
    rc = daemon_tune(lband_khz, symbol_rate_ksps, lnb_volts, tone_khz,
                     diseqc, uncommitted, burst, repeat);
    dtv_atomic_set(&g_stream_data_tick, (int32_t)dtv_tick32());
    dtv_mutex_unlock(&g_tune_lock);
    return rc;
}

int usb_dvbs2_engine_tune_rf(uint32_t rf_mhz, uint32_t symbol_rate_ksps,
                             int horizontal, uint32_t diseqc)
{
    /* Ku-band universal LNB: RF MHz -> L-band, LNB voltage and tone. Through
     * usb_dvbs2_engine_tune, so it takes the tune lock and skips a carrier
     * that is already streaming like any other tune. */
    uint32_t tone = rf_mhz >= 11700u ? 22u : 0u;
    uint32_t lo = tone == 22u ? 10600u : 9750u;
    if (rf_mhz < 10700u || rf_mhz > 12750u) {
        error_linef("TUNE_RF: %lu MHz is outside a universal LNB's band.\n",
                    (unsigned long)rf_mhz);
        return -1;
    }
    return usb_dvbs2_engine_tune((rf_mhz - lo) * 1000u, symbol_rate_ksps,
                                 horizontal ? 18u : 13u, tone, diseqc, 0, 0,
                                 0);
}

void usb_dvbs2_engine_set_channel(uint16_t service_id, uint16_t pmt_pid,
                                  uint16_t video_pid, uint16_t audio_pid,
                                  uint16_t teletext_pid,
                                  uint16_t ca_system_id, uint16_t ca_pid)
{
    set_channel(service_id, pmt_pid, video_pid, audio_pid, teletext_pid,
                ca_system_id, ca_pid);
}

void usb_dvbs2_engine_set_camd(const dtv_camd_config *config)
{
    if (!config) {
        g_camd_configured = 0;
        camd_apply_config(0);
        return;
    }
    g_camd_config = *config;
    g_camd_configured = 1;
    camd_apply_config(1);
}

void usb_dvbs2_engine_lnb_off(void)
{
    lnb_power_down();
}

void usb_dvbs2_engine_get_status(usb_dvbs2_engine_status *status)
{
    dtv_camd_status camd_status;
    memset(status, 0, sizeof(*status));
    dtv_mutex_lock(&g_lock);
    if (g_sat_utc_filetime && g_sat_received_tick) {
        uint64_t ticks = g_sat_utc_filetime +
            (dtv_tick_ms() - g_sat_received_tick) * 10000u;
        dtv_utc_time clock;
        dtv_utc_split(ticks, &clock);
        snprintf(status->date, sizeof(status->date), "%04d-%02d-%02d",
                 clock.year, clock.month, clock.day);
        snprintf(status->time, sizeof(status->time), "%02d:%02d:%02d",
                 clock.hour, clock.minute, clock.second);
    } else {
        snprintf(status->date, sizeof(status->date), "%s", g_sat_date);
        snprintf(status->time, sizeof(status->time), "%s", g_sat_time);
    }
    snprintf(status->now_title, sizeof(status->now_title), "%s", g_epg_now);
    snprintf(status->next_title, sizeof(status->next_title), "%s",
             g_epg_next);
    snprintf(status->now_start, sizeof(status->now_start), "%s",
             g_epg_now_start);
    snprintf(status->now_end, sizeof(status->now_end), "%s", g_epg_now_end);
    snprintf(status->next_start, sizeof(status->next_start), "%s",
             g_epg_next_start);
    dtv_mutex_unlock(&g_lock);
    dtv_camd_get_status(camd_acquire(), &camd_status);
    status->locked = dtv_atomic_get(&g_status_locked);
    status->fec_locked = dtv_atomic_get(&g_status_fec);
    status->frame_locked = dtv_atomic_get(&g_status_frame);
    status->snr_x100 = dtv_atomic_get(&g_status_snr_x100);
    status->symbol_rate_hz = dtv_atomic_get(&g_status_sr_hz);
    status->cam_state = (int)camd_status.state;
    snprintf(status->cam_message, sizeof(status->cam_message), "%s",
             camd_status.message);
}
