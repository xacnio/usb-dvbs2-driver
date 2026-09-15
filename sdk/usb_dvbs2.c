#include "usb_dvbs2.h"
#include "hardware_profile.h"

#include "dtv_ipc.h"
#include "dtv_platform.h"
#include "dtv_process.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Talking to the tuner daemon: process, command channel, command formatting
 * and status parsing. Transport only -- which transponder to go to is the
 * caller's job. */

#define DAEMON_EXECUTABLE "usb-dvbs2-daemon" DTV_EXE_SUFFIX

static char g_exe_dir[DTV_PATH_MAX];
static unsigned g_udp_port = 5560;
static usb_dvbs2_host g_host;
static int g_hardware_profile = DTV_HARDWARE_AUTO;

static void tuner_log(const char *text)
{
    if (g_host.log)
        g_host.log(text);
}

static void tuner_logf(const char *format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    tuner_log(buffer);
}

/* The daemon writes UTF-8; the host API takes wide text, which is UTF-16 on
 * Windows and UTF-32 elsewhere. A byte that is not valid UTF-8 is taken as
 * Latin-1, as the old ANSI fallback did for the characters that matter. */
static void utf8_to_wide(const char *text, wchar_t *out, size_t capacity)
{
    const unsigned char *in = (const unsigned char *)(text ? text : "");
    size_t used = 0;
    if (!out || !capacity)
        return;
    while (*in && used + 1 < capacity) {
        unsigned long code = *in;
        size_t extra = code >= 0xf0 ? 3 : code >= 0xe0 ? 2 : code >= 0xc0 ? 1 : 0;
        size_t i;
        int valid = code < 0x80 || (code >= 0xc2 && code <= 0xf4);
        for (i = 1; valid && i <= extra; ++i)
            valid = (in[i] & 0xc0) == 0x80;
        if (!valid) {
            out[used++] = (wchar_t)*in++;
            continue;
        }
        if (extra) {
            code &= 0x3fu >> extra;
            for (i = 1; i <= extra; ++i)
                code = (code << 6) | (in[i] & 0x3fu);
        }
        in += 1 + extra;
        if (sizeof(wchar_t) == 2 && code > 0xffff) {
            if (used + 2 >= capacity)
                break;
            code -= 0x10000;
            out[used++] = (wchar_t)(0xd800 + (code >> 10));
            out[used++] = (wchar_t)(0xdc00 + (code & 0x3ff));
        } else {
            out[used++] = (wchar_t)code;
        }
    }
    out[used] = 0;
}

static void tuner_stage(int percent, const char *text)
{
    wchar_t wide[192];
    if (!g_host.stage)
        return;
    utf8_to_wide(text, wide, sizeof(wide) / sizeof(wide[0]));
    g_host.stage(percent, wide);
}

static dtv_process *g_daemon_proc;
static dtv_ipc *g_daemon_pipe;
static int g_daemon_running;
static dtv_atomic32 g_daemon_starting;   /* first cold init in progress */
static dtv_mutex g_pipe_lock;            /* one writer at a time */
static dtv_mutex g_status_lock;          /* guards the box */
static int g_locks_ready;
static dtv_thread *g_status_thread;
static dtv_atomic32 g_status_stopping;
static usb_dvbs2_status g_status_latest;
static dtv_atomic32 g_status_fresh;

/* Card server settings, kept here so a daemon started later gets them
 * without the settings page noticing. Guarded by g_status_lock. */
static int g_cam_enabled;
static int g_cam_proto;
static char g_cam_host[96];
static unsigned g_cam_port;
static char g_cam_user[64];
static char g_cam_password[64];
static char g_cam_des_key[64];
static int g_cam_state;
/* Loss counted by the daemon, before the local socket. */
static int g_daemon_cc_errors;
static int g_daemon_send_failures;
static int g_daemon_overflow;
static char g_cam_message[128];


/* Defined with the status thread below, which comes later in this file. */
static void status_thread_start(void);
static void status_thread_stop(void);
/* The settings page can hand over the card server configuration before the
 * tuner has ever started, so the locks it writes under must be creatable
 * from there too. */
static void locks_init(void);

static int daemon_send(const char *cmd)
{
    char line[256];
    int len, result;
    if (!g_daemon_pipe)
        return -1;
    len = snprintf(line, sizeof(line), "%s\n", cmd);
    if (len <= 0 || (size_t)len >= sizeof(line))
        return -1;
    if (g_locks_ready)
        dtv_mutex_lock(&g_pipe_lock);
    result = dtv_ipc_write(g_daemon_pipe, line, (size_t)len);
    if (g_locks_ready)
        dtv_mutex_unlock(&g_pipe_lock);
    /* No flush waits for the daemon to read: it does not read its channel
     * while it is retuning, and the status query goes out on a timer that
     * would then stall for as long as the tune lasted. A command is a
     * handful of bytes against a buffer the daemon drains between tunes. */
    return result;
}


/* Kills daemon processes that never opened their channel but are still
 * alive: such a process holds the USB device, so a new daemon cannot start. */
static void terminate_stale_daemons(void)
{
    int killed = dtv_process_kill_named(DAEMON_EXECUTABLE);
    if (killed)
        tuner_logf("Stuck daemon terminated (%d).\r\n", killed);
    /* Wait only if something was really killed; otherwise every normal
     * start would pay 300 ms for nothing. */
    if (killed)
        dtv_sleep_ms(300); /* let the USB handle go */
}

/* Reads the daemon output line by line: STAGE|<percent>|<text> updates the
 * progress display, everything else goes to the log. */
static void daemon_log_reader(void *parameter)
{
    dtv_pipe *pipe = (dtv_pipe *)parameter;
    char buffer[1024];
    char line[512];
    size_t used = 0;
    int read_size;
    while ((read_size = dtv_pipe_read(pipe, buffer, sizeof(buffer))) > 0) {
        int i;
        for (i = 0; i < read_size; ++i) {
            char c = buffer[i];
            if (c != '\n' && used + 1 < sizeof(line)) {
                if (c != '\r')
                    line[used++] = c;
                continue;
            }
            line[used] = '\0';
            if (used) {
                if (strncmp(line, "STAGE|", 6) == 0) {
                    char *percent_text = line + 6;
                    char *text = strchr(percent_text, '|');
                    if (text) {
                        int percent = atoi(percent_text);
                        *text++ = '\0';
                        if (percent < 0) percent = 0;
                        if (percent > 100) percent = 100;
                        tuner_stage(percent, text);
                    }
                } else if (strncmp(line, "SIGNAL|", 7) == 0) {
                    if (g_host.signal_state)
                        g_host.signal_state(line[7] != '0');
                } else if (strncmp(line, "DEVICE|", 7) == 0) {
                    /* DEVICE|<state>|<text>, state 0 = gone. */
                    char *text = strchr(line + 7, '|');
                    if (text) {
                        wchar_t wide[192];
                        *text++ = '\0';
                        utf8_to_wide(text, wide, sizeof(wide) / sizeof(wide[0]));
                        if (g_host.device_lost)
                            g_host.device_lost(wide);
                    }
                } else if (strncmp(line, "TTX|", 4) == 0) {
                    char *pid = strchr(line + 4, '|');
                    if (pid) {
                        *pid++ = '\0';
                        if (g_host.service_teletext)
                            g_host.service_teletext(
                                (unsigned)atoi(line + 4),
                                (unsigned)atoi(pid));
                    }
                } else if (strncmp(line, "EPG|", 4) == 0) {
                    /* EPG|<sid>|<event>|<start>|<duration>|<title>|<text>.
                     * The two texts come last as the only long fields; the
                     * daemon has already removed the separator. */
                    char *field[6];
                    int found = 0;
                    char *cursor = line + 4;
                    while (found < 6) {
                        field[found++] = cursor;
                        cursor = strchr(cursor, '|');
                        if (!cursor)
                            break;
                        *cursor++ = '\0';
                    }
                    if (found >= 5 && g_host.epg_event) {
                        dtv_epg_event event;
                        memset(&event, 0, sizeof(event));
                        event.service_id = (uint16_t)atoi(field[0]);
                        event.event_id = (uint16_t)atoi(field[1]);
                        event.start_utc = strtoll(field[2], NULL, 10);
                        event.duration_seconds = (uint32_t)atoi(field[3]);
                        snprintf(event.title, sizeof(event.title), "%s",
                                 field[4]);
                        if (found >= 6)
                            snprintf(event.text, sizeof(event.text), "%s",
                                     field[5]);
                        g_host.epg_event(&event);
                    }
                } else if (strncmp(line, "CA|", 3) == 0) {
                    /* CA|<sid>|<scrambled>[|<system>|<capid>]. The last two
                     * are newer; a daemon without them still reports the
                     * lock state. */
                    char *flag = strchr(line + 3, '|');
                    if (flag) {
                        char *sys = strchr(flag + 1, '|');
                        unsigned system_id = 0, ca_pid = 0x1fff;
                        *flag++ = '\0';
                        if (sys) {
                            char *capid = strchr(sys + 1, '|');
                            *sys++ = '\0';
                            system_id = (unsigned)atoi(sys);
                            if (capid) {
                                *capid++ = '\0';
                                ca_pid = (unsigned)atoi(capid);
                            }
                        }
                        if (g_host.service_ca)
                            g_host.service_ca((unsigned)atoi(line + 3),
                                              atoi(flag) ? 1 : 0,
                                              system_id, ca_pid);
                    }
                } else if (strncmp(line, "LOSS|", 5) == 0) {
                    /* LOSS|<continuity gaps>|<send failures>|<overflow>,
                     * counted by the daemon BEFORE the stream reaches the
                     * local socket. Read with the player's own gap count
                     * they say which half of the path loses packets: both
                     * moving means the loss arrived with the stream, only
                     * the player's moving puts it between the processes.
                     * The third is packets the daemon's USB reader had to
                     * discard because this machine was too busy -- loss
                     * caused locally. Older daemons send two fields. */
                    char *failures = strchr(line + 5, '|');
                    if (failures) {
                        char *overflow;
                        *failures++ = '\0';
                        overflow = strchr(failures, '|');
                        if (overflow)
                            *overflow++ = '\0';
                        locks_init();
                        dtv_mutex_lock(&g_status_lock);
                        g_daemon_cc_errors = atoi(line + 5);
                        g_daemon_send_failures = atoi(failures);
                        g_daemon_overflow = overflow ? atoi(overflow) : 0;
                        dtv_mutex_unlock(&g_status_lock);
                    }
                } else if (strncmp(line, "CAMD|", 5) == 0) {
                    /* CAMD|<state>|<message>: the daemon reports every
                     * change of the card server connection. The message is
                     * English and is localised where it is drawn. */
                    char *text = strchr(line + 5, '|');
                    if (text) {
                        *text++ = '\0';
                        locks_init();
                        dtv_mutex_lock(&g_status_lock);
                        g_cam_state = atoi(line + 5);
                        snprintf(g_cam_message, sizeof(g_cam_message), "%s",
                                 text);
                        dtv_mutex_unlock(&g_status_lock);
                        tuner_logf("CAM: %s - %s\r\n", line + 5, text);
                    } else {
                        tuner_logf("CAM: %s\r\n", line + 5);
                    }
                } else {
                    tuner_logf("%s\r\n", line);
                }
            }
            used = 0;
        }
    }
    dtv_pipe_close(pipe);
}

static void shutdown_daemon(void)
{
    /* Not is it connected but is there a process. The connection is made at
     * the END of a start that waits up to fifteen seconds for the channel,
     * so closing the program during the opening screen came through here
     * with g_daemon_running still 0 and did nothing -- leaving the daemon
     * running and the LNB powered. */
    if (!g_daemon_running && !g_daemon_proc)
        return;
    /* Before the channel is closed: the status thread uses it. */
    status_thread_stop();
    if (g_daemon_pipe) {
        /* STOP before QUIT: STOP takes the power off the dish and is acted
         * on the moment it is read, QUIT ends the process. Sending both
         * stops the receiver even if the daemon has to be killed first. */
        daemon_send("STOP");
        daemon_send("QUIT");
        dtv_ipc_close(g_daemon_pipe);
        g_daemon_pipe = NULL;
    }
    if (g_daemon_proc) {
        /* Long enough to cover a cold init: the daemon does not read its
         * channel while uploading firmware, so both commands sit in the
         * buffer until that finishes, and killing it first is what left the
         * dish powered. Terminating is the last resort. */
        if (!dtv_process_wait(g_daemon_proc, 9000)) {
            tuner_log("The daemon did not exit; terminating it.\r\n");
            dtv_process_kill(g_daemon_proc);
            dtv_process_wait(g_daemon_proc, 1000);
        }
        dtv_process_free(g_daemon_proc);
        g_daemon_proc = NULL;
    }
    g_daemon_running = 0;
}

/* Sends the stored card server settings to the daemon. Safe at any time:
 * with no daemon it does nothing, and they are sent again on connect. The
 * password travels over a local channel to a process this application
 * started, the same trust boundary as the rest of the tuner commands. */
static void send_camd_config(void)
{
    char command[512];
    int enabled;
    if (!g_daemon_running || !g_daemon_pipe)
        return;
    dtv_mutex_lock(&g_status_lock);
    enabled = g_cam_enabled && g_cam_host[0] && g_cam_port;
    if (enabled)
        snprintf(command, sizeof(command), "CAMD %s %s %u %s %s %s",
                 g_cam_proto == 1 ? "cs378x" : "newcamd", g_cam_host,
                 g_cam_port, g_cam_user[0] ? g_cam_user : "-",
                 g_cam_password[0] ? g_cam_password : "-",
                 g_cam_des_key[0] ? g_cam_des_key
                                  : "0102030405060708091011121314");
    else
        snprintf(command, sizeof(command), "CAMD OFF");
    dtv_mutex_unlock(&g_status_lock);
    daemon_send(command);
}

void usb_dvbs2_set_camd(int enabled, int proto, const char *host,
                        unsigned port, const char *user,
                        const char *password, const char *des_key)
{
    locks_init();
    dtv_mutex_lock(&g_status_lock);
    g_cam_enabled = enabled ? 1 : 0;
    g_cam_proto = proto == 1 ? 1 : 0;
    snprintf(g_cam_host, sizeof(g_cam_host), "%s", host ? host : "");
    g_cam_port = port;
    snprintf(g_cam_user, sizeof(g_cam_user), "%s", user ? user : "");
    snprintf(g_cam_password, sizeof(g_cam_password), "%s",
             password ? password : "");
    snprintf(g_cam_des_key, sizeof(g_cam_des_key), "%s",
             des_key ? des_key : "");
    if (!g_cam_enabled) {
        g_cam_state = 0;
        snprintf(g_cam_message, sizeof(g_cam_message), "%s", "");
    }
    dtv_mutex_unlock(&g_status_lock);
    send_camd_config();
}

void usb_dvbs2_loss_counters(int *continuity_gaps, int *send_failures,
                             int *overflow_packets)
{
    locks_init();
    dtv_mutex_lock(&g_status_lock);
    if (continuity_gaps)
        *continuity_gaps = g_daemon_cc_errors;
    if (send_failures)
        *send_failures = g_daemon_send_failures;
    if (overflow_packets)
        *overflow_packets = g_daemon_overflow;
    dtv_mutex_unlock(&g_status_lock);
}

int usb_dvbs2_cam_state(void)
{
    int state;
    locks_init();
    dtv_mutex_lock(&g_status_lock);
    state = g_cam_state;
    dtv_mutex_unlock(&g_status_lock);
    return state;
}

const char *usb_dvbs2_cam_message(void)
{
    /* Returned to the drawing code, which copies it immediately; the buffer
     * only ever grows a complete replacement string. */
    return g_cam_message;
}

static int ensure_daemon(void)
{
    char daemon_path[DTV_PATH_MAX];
    char port_text[16];
    const char *pipe_name = dtv_ipc_default_name();
    dtv_pipe *output = NULL;
    int i;
    if (g_daemon_running)
        return 0;
    locks_init();

    /* If a daemon from an earlier session is still running, attach to it:
     * otherwise the new one cannot open the USB device, dies quietly, and
     * the wait below ends after 15 s. */
    g_daemon_pipe = dtv_ipc_connect(pipe_name);
    if (g_daemon_pipe) {
        tuner_log("Found a running daemon; reusing it.\r\n");
        tuner_stage(90, "Found a running tuner");
        goto daemon_connected;
    }
    /* No channel but a live process means it is stuck: kill it so it
     * releases the USB device. */
    tuner_stage(3, "Starting tuner service...");
    terminate_stale_daemons();
    {
        size_t length = strlen(g_exe_dir);
        snprintf(daemon_path, sizeof(daemon_path), "%s%s%s", g_exe_dir,
                 length && (g_exe_dir[length - 1] == '\\' ||
                            g_exe_dir[length - 1] == '/')
                     ? "" : (DTV_PATH_SEPARATOR == '/' ? "/" : "\\"),
                 DAEMON_EXECUTABLE);
    }
    snprintf(port_text, sizeof(port_text), "%u", g_udp_port);
    {
        const char *argv[] = {
            daemon_path, "--udp", port_text, "--pipe", pipe_name,
            "--profile", dtv_hardware_selection_key(g_hardware_profile), NULL
        };
        /* The daemon's output is captured: the host learns the current
         * stage from it. */
        g_daemon_proc = dtv_process_spawn(daemon_path, argv, g_exe_dir,
                                          &output);
    }
    if (!g_daemon_proc) {
        tuner_log("Tuner daemon could not be started.\r\n");
        return -1;
    }
    if (output) {
        dtv_thread *reader = dtv_thread_start(daemon_log_reader, output);
        if (reader)
            dtv_thread_detach(reader);
        else
            dtv_pipe_close(output);
    }
    /* While cold init runs, wait for the channel to appear (at most ~15 s). */
    g_daemon_pipe = NULL;
    for (i = 0; i < 150; ++i) {
        g_daemon_pipe = dtv_ipc_connect(pipe_name);
        if (g_daemon_pipe)
            break;
        /* Invalid profile, a missing driver and firmware errors make the
         * daemon exit at once; do not turn those into a 15-second wait. */
        if (dtv_process_wait(g_daemon_proc, 0))
            break;
        dtv_sleep_ms(100);
    }
    if (!g_daemon_pipe) {
        tuner_log("Could not connect to the daemon channel.\r\n");
        dtv_process_kill(g_daemon_proc);
        dtv_process_wait(g_daemon_proc, 1000);
        dtv_process_free(g_daemon_proc);
        g_daemon_proc = NULL;
        return -1;
    }
daemon_connected:
    g_daemon_running = 1;
    status_thread_start();
    /* A daemon reused from an earlier session may know nothing about the
     * card server, so the settings are sent on every connect. */
    send_camd_config();
    return 0;
}

void usb_dvbs2_init(const char *exe_dir, unsigned udp_port,
                    const usb_dvbs2_host *host)
{
    if (exe_dir)
        snprintf(g_exe_dir, sizeof(g_exe_dir), "%s", exe_dir);
    if (udp_port)
        g_udp_port = udp_port;
    if (host)
        g_host = *host;
}

void usb_dvbs2_set_hardware_profile(int profile)
{
    if (profile < DTV_HARDWARE_AUTO || profile >= DTV_HARDWARE_PROFILE_COUNT)
        profile = DTV_HARDWARE_AUTO;
    g_hardware_profile = profile;
}

int usb_dvbs2_hardware_profile(void)
{
    return g_hardware_profile;
}

int usb_dvbs2_start(void)     { return ensure_daemon(); }
void usb_dvbs2_stop(void)     { shutdown_daemon(); }
int usb_dvbs2_running(void)   { return g_daemon_running; }
int usb_dvbs2_send(const char *line) { return daemon_send(line); }

int usb_dvbs2_starting(void)
{
    return (int)dtv_atomic_get(&g_daemon_starting);
}

void usb_dvbs2_set_starting(int starting)
{
    dtv_atomic_set(&g_daemon_starting, starting ? 1 : 0);
}

int usb_dvbs2_claim_start(void)
{
    return dtv_atomic_cas(&g_daemon_starting, 0, 1) == 0;
}

int usb_dvbs2_tune(unsigned lband_khz, unsigned symbol_rate_ksps,
                   unsigned lnb_voltage, unsigned tone_khz,
                   const char *diseqc_args)
{
    char command[192];
    snprintf(command, sizeof(command), "TUNE %u %u %u %u %s", lband_khz,
             symbol_rate_ksps, lnb_voltage, tone_khz,
             diseqc_args ? diseqc_args : "0 0 0 0");
    return daemon_send(command);
}

int usb_dvbs2_tune_rf(unsigned rf_mhz, unsigned symbol_rate_ksps,
                      int horizontal, unsigned diseqc)
{
    char command[96];
    snprintf(command, sizeof(command), "TUNE_RF %u %u %c %u", rf_mhz,
             symbol_rate_ksps, horizontal ? 'H' : 'V', diseqc);
    return daemon_send(command);
}

int usb_dvbs2_channel(unsigned service_id, unsigned pmt_pid,
                      unsigned video_pid, unsigned audio_pid,
                      unsigned teletext_pid)
{
    char command[128];
    snprintf(command, sizeof(command), "CHANNEL %u %u %u %u %u", service_id,
             pmt_pid, video_pid, audio_pid, teletext_pid);
    return daemon_send(command);
}

/* Reads and parses one STATUS reply if the daemon has sent it. Called only
 * from the status thread below. */
static int parse_status_reply(usb_dvbs2_status *out)
{
    size_t available;
    int filled = 0;
    if (!g_daemon_running || !g_daemon_pipe)
        return 0;
    available = dtv_ipc_available(g_daemon_pipe);
    if (available) {
        char reply[1024];
        int read_size;
        if (available >= sizeof(reply))
            available = sizeof(reply) - 1;
        read_size = dtv_ipc_read(g_daemon_pipe, reply, available);
        if (read_size > 0) {
            long locked, fec, frame, snr, sr;
            reply[read_size] = 0;
            if (sscanf(reply, "STATUS %ld %ld %ld %ld %ld",
                       &locked, &fec, &frame, &snr, &sr) == 5) {
                char *metadata = strchr(reply, 9);   /* tab */
                memset(out, 0, sizeof(*out));
                out->locked = locked != 0;
                out->fec_locked = fec != 0;
                out->frame_locked = frame != 0;
                out->snr_x100 = (int)snr;
                out->symbol_rate_hz = sr > 0 ? (unsigned)sr : 0;
                if (metadata) {
                    /* Seven programme fields, then the CAM state and its
                     * message. An older daemon sends only the seven. */
                    char *fields[9] = { 0 };
                    int i;
                    ++metadata;
                    for (i = 0; i < 9; ++i) {
                        char *separator;
                        fields[i] = metadata;
                        separator = strpbrk(metadata, "\t\r\n");
                        if (!separator) {
                            metadata += strlen(metadata);
                            break;
                        }
                        *separator = 0;
                        metadata = separator + 1;
                    }
#define COPY_META(target, index) do {     snprintf((target), sizeof(target), "%s",              fields[index] ? fields[index] : ""); } while (0)
                    COPY_META(out->date, 0);
                    COPY_META(out->time, 1);
                    COPY_META(out->now_start, 2);
                    COPY_META(out->now_end, 3);
                    COPY_META(out->now_title, 4);
                    COPY_META(out->next_start, 5);
                    COPY_META(out->next_title, 6);
                    COPY_META(out->cam_message, 8);
#undef COPY_META
                    out->cam_state = fields[7] ? atoi(fields[7]) : 0;
                }
                filled = 1;
            }
        }
    }
    return filled;
}

/* --- daemon status, off the message thread -------------------------- */

/* The signal reading comes from the daemon over the channel, and asking for
 * it used to happen on the message thread from a timer.
 *
 * That is the freeze: a write blocks while the far end is not reading, and
 * the daemon does not read while retuning. Ask in that moment and the window
 * stops answering until the tune finishes (watchdog: stuck in WM_TIMER, nine
 * seconds and counting), worst of all with the blur on.
 *
 * So the conversation runs on its own thread: it asks, waits, parses and
 * puts the result in a box the message thread only copies out of. */
static void locks_init(void)
{
    if (g_locks_ready)
        return;
    dtv_mutex_init(&g_pipe_lock);
    dtv_mutex_init(&g_status_lock);
    g_locks_ready = 1;
}

/* One round trip: ask, then give the daemon a moment to answer. Runs on the
 * status thread, so a slow answer costs nothing anyone waits on. */
static void status_round_trip(void)
{
    usb_dvbs2_status sample;
    unsigned waited = 0;
    if (daemon_send("STATUS") != 0)
        return;
    while (waited < 1500) {
        if (!g_daemon_pipe)
            return;
        if (dtv_ipc_available(g_daemon_pipe)) {
            if (parse_status_reply(&sample)) {
                dtv_mutex_lock(&g_status_lock);
                g_status_latest = sample;
                dtv_mutex_unlock(&g_status_lock);
                dtv_atomic_set(&g_status_fresh, 1);
            }
            return;
        }
        if (dtv_atomic_get(&g_status_stopping))
            return;
        dtv_sleep_ms(25);
        waited += 25;
    }
}

static void status_worker(void *parameter)
{
    (void)parameter;
    for (;;) {
        unsigned slept;
        for (slept = 0; slept < 250; slept += 25) {
            if (dtv_atomic_get(&g_status_stopping))
                return;
            dtv_sleep_ms(25);
        }
        if (!g_daemon_running || !g_daemon_pipe)
            continue;
        status_round_trip();
    }
}

static void status_thread_start(void)
{
    locks_init();
    if (g_status_thread)
        return;
    dtv_atomic_set(&g_status_stopping, 0);
    g_status_thread = dtv_thread_start(status_worker, NULL);
}

/* Stopped BEFORE the channel is closed: the thread reads it. */
static void status_thread_stop(void)
{
    if (!g_status_thread)
        return;
    dtv_atomic_set(&g_status_stopping, 1);
    if (dtv_thread_join(g_status_thread, 3000) != 0)
        dtv_thread_detach(g_status_thread);
    g_status_thread = NULL;
    dtv_atomic_set(&g_status_fresh, 0);
}

/* What the message thread calls: copies the last reading and says whether
 * it has not been handed out yet. */
int usb_dvbs2_poll(usb_dvbs2_status *out)
{
    if (!out || !g_locks_ready || !g_daemon_running)
        return 0;
    if (!dtv_atomic_set(&g_status_fresh, 0))
        return 0;
    dtv_mutex_lock(&g_status_lock);
    *out = g_status_latest;
    dtv_mutex_unlock(&g_status_lock);
    return 1;
}
