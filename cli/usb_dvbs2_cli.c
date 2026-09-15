/* usb-dvbs2: drives the receiver from a terminal through the tuner SDK.
 *
 * Starts usb-dvbs2-daemon (or reuses a running one), prints what it reports
 * and reads commands one per line, from the console or a pipe. The session
 * is the connection: when this tool ends, the daemon powers the LNB down. */
#include "usb_dvbs2.h"
#include "hardware_profile.h"
#include "dtv_platform.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static dtv_mutex g_print_lock;
static dtv_atomic32 g_stopping;
static dtv_atomic32 g_show_epg;
static dtv_atomic32 g_epg_events;

/* SDK callbacks arrive on its reader thread; lines must not interleave. */
static void print_line(const char *format, ...)
{
    va_list args;
    dtv_mutex_lock(&g_print_lock);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
    dtv_mutex_unlock(&g_print_lock);
}

/* Wide text from the SDK back to UTF-8: UTF-16 on Windows, UTF-32 on the
 * others. */
static void print_wide(const char *prefix, const wchar_t *text)
{
    char utf8[512];
    size_t used = 0;
    const wchar_t *in = text ? text : L"";
    while (*in && used + 5 < sizeof(utf8)) {
        unsigned long code = (unsigned long)*in++;
        if (sizeof(wchar_t) == 2 && code >= 0xd800 && code < 0xdc00 &&
            *in >= 0xdc00 && *in < 0xe000)
            code = 0x10000 + ((code - 0xd800) << 10) +
                   ((unsigned long)*in++ - 0xdc00);
        if (code < 0x80) {
            utf8[used++] = (char)code;
        } else if (code < 0x800) {
            utf8[used++] = (char)(0xc0 | (code >> 6));
            utf8[used++] = (char)(0x80 | (code & 0x3f));
        } else if (code < 0x10000) {
            utf8[used++] = (char)(0xe0 | (code >> 12));
            utf8[used++] = (char)(0x80 | ((code >> 6) & 0x3f));
            utf8[used++] = (char)(0x80 | (code & 0x3f));
        } else {
            utf8[used++] = (char)(0xf0 | (code >> 18));
            utf8[used++] = (char)(0x80 | ((code >> 12) & 0x3f));
            utf8[used++] = (char)(0x80 | ((code >> 6) & 0x3f));
            utf8[used++] = (char)(0x80 | (code & 0x3f));
        }
    }
    utf8[used] = '\0';
    print_line("%s%s\n", prefix, utf8);
}

static void on_log(const char *text)
{
    size_t length = strlen(text);
    while (length && (text[length - 1] == '\n' || text[length - 1] == '\r'))
        --length;
    print_line("log: %.*s\n", (int)length, text);
}

static void on_stage(int percent, const wchar_t *text)
{
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "stage: %3d%% ", percent);
    print_wide(prefix, text);
}

static void on_service_ca(unsigned service_id, int scrambled,
                          unsigned ca_system_id, unsigned ca_pid)
{
    print_line("ca: service %u %s, system 0x%04x, pid %u\n", service_id,
               scrambled ? "scrambled" : "clear", ca_system_id, ca_pid);
}

static void on_teletext(unsigned service_id, unsigned pid)
{
    print_line("teletext: service %u, pid %u\n", service_id, pid);
}

static void on_epg(const dtv_epg_event *event)
{
    dtv_atomic_inc(&g_epg_events);
    if (dtv_atomic_get(&g_show_epg))
        print_line("epg: service %u, event %u, start %lld, %u s: %s\n",
                   (unsigned)event->service_id, (unsigned)event->event_id,
                   (long long)event->start_utc,
                   (unsigned)event->duration_seconds, event->title);
}

static void on_device_lost(const wchar_t *text)
{
    print_wide("device: ", text);
}

static void on_signal(int has_stream)
{
    print_line("signal: %s\n", has_stream ? "stream" : "no stream");
}

static void print_help(void)
{
    print_line(
        "Commands:\n"
        "  tune <lband_khz> <sr_ksps> <lnb_v> <tone_khz> [<1.0> <1.1> <burst> <repeat>]\n"
        "                   lock a carrier; lnb_v 13, 18 or 0, tone 0 or 22\n"
        "  tune_rf <rf_mhz> <sr_ksps> <H|V> [<diseqc>]\n"
        "                   the same from the RF frequency, universal LNB\n"
        "  channel <service> <pmt> <video> <audio> [<teletext>]\n"
        "                   select a service on the locked carrier\n"
        "  status           signal, satellite clock, programme, card server\n"
        "  camd off | camd <newcamd|cs378x> <host> <port> [<user> <pass> <des_key>]\n"
        "  epg on|off       print programme events as they arrive\n"
        "  raw <line>       send a daemon command as it is\n"
        "  wait <seconds>   pause, for scripts fed on standard input\n"
        "  help, quit\n"
        "Numbers may be decimal or 0x hexadecimal. The stream goes to\n"
        "udp://@127.0.0.1:<port>.\n");
}

static void print_usage(void)
{
    size_t i;
    printf("Usage: usb-dvbs2 [--dir <folder>] [--profile <key>] [--udp <port>]\n"
           "\n"
           "  --dir      folder holding usb-dvbs2-daemon, libusb and firmware/\n"
           "             (default: this program's folder)\n"
           "  --profile  auto or a hardware profile:");
    for (i = 0; i < dtv_hardware_profile_count(); ++i) {
        const dtv_hardware_profile *profile = dtv_hardware_profile_at(i);
        if (profile)
            printf(" %s", profile->key);
    }
    printf("\n  --udp      stream port (default %u)\n\n", USB_DVBS2_UDP_PORT);
    print_help();
}

static int parse_number(const char *text, unsigned *out)
{
    char *end = NULL;
    unsigned long value;
    if (!text || !*text)
        return 0;
    value = strtoul(text, &end, 0);
    if (!end || *end)
        return 0;
    *out = (unsigned)value;
    return 1;
}

static void print_status(const usb_dvbs2_status *status)
{
    int gaps = 0, failures = 0, overflow = 0;
    int snr = status->snr_x100;
    usb_dvbs2_loss_counters(&gaps, &failures, &overflow);
    print_line("status: carrier %s, fec %s, frame %s, snr %s%d.%02d dB, %u Bd\n",
               status->locked ? "locked" : "unlocked",
               status->fec_locked ? "locked" : "unlocked",
               status->frame_locked ? "locked" : "unlocked",
               snr < 0 ? "-" : "", abs(snr) / 100, abs(snr) % 100,
               status->symbol_rate_hz);
    if (status->date[0] || status->time[0])
        print_line("status: satellite clock %s %s\n", status->date,
                   status->time);
    if (status->now_title[0])
        print_line("status: now %s-%s %s\n", status->now_start,
                   status->now_end, status->now_title);
    if (status->next_title[0])
        print_line("status: next %s %s\n", status->next_start,
                   status->next_title);
    print_line("status: card server state %d %s\n", status->cam_state,
               status->cam_message);
    print_line("status: %d continuity gaps, %d send failures, %d overflow, "
               "%ld programme events\n", gaps, failures, overflow,
               (long)dtv_atomic_get(&g_epg_events));
}

/* The SDK asks four times a second; the next fresh reading is shown. */
static void command_status(void)
{
    usb_dvbs2_status status;
    int waited;
    for (waited = 0; waited < 3000; waited += 50) {
        if (usb_dvbs2_poll(&status)) {
            print_status(&status);
            return;
        }
        dtv_sleep_ms(50);
    }
    print_line("error: the daemon did not answer\n");
}

static void report_send(int result)
{
    if (result != 0)
        print_line("error: the command could not be sent\n");
}

/* Runs one line; 1 when the session should end. */
static int run_command(char *line)
{
    char original[512];
    char *argv[12];
    int argc = 0;
    char *token;
    unsigned values[8];
    int i;

    snprintf(original, sizeof(original), "%s", line);
    for (token = strtok(line, " \t"); token && argc < 12;
         token = strtok(NULL, " \t"))
        argv[argc++] = token;
    if (argc == 0 || argv[0][0] == '#')
        return 0;

    if (strcmp(argv[0], "quit") == 0 || strcmp(argv[0], "exit") == 0)
        return 1;
    if (strcmp(argv[0], "help") == 0) {
        print_help();
    } else if (strcmp(argv[0], "tune") == 0) {
        char diseqc[64] = "0 0 0 0";
        if (argc != 5 && argc != 9) {
            print_line("error: tune takes 4 or 8 numbers\n");
            return 0;
        }
        for (i = 1; i < argc; ++i) {
            if (!parse_number(argv[i], &values[i - 1])) {
                print_line("error: not a number: %s\n", argv[i]);
                return 0;
            }
        }
        if (argc == 9)
            snprintf(diseqc, sizeof(diseqc), "%u %u %u %u", values[4],
                     values[5], values[6], values[7]);
        report_send(usb_dvbs2_tune(values[0], values[1], values[2], values[3],
                                   diseqc));
    } else if (strcmp(argv[0], "tune_rf") == 0) {
        values[2] = 0;
        if ((argc != 4 && argc != 5) || !parse_number(argv[1], &values[0]) ||
            !parse_number(argv[2], &values[1]) ||
            (argc == 5 && !parse_number(argv[4], &values[2])) ||
            strlen(argv[3]) != 1 || !strchr("HhVv", argv[3][0])) {
            print_line("error: tune_rf <rf_mhz> <sr_ksps> <H|V> [<diseqc>]\n");
            return 0;
        }
        report_send(usb_dvbs2_tune_rf(values[0], values[1],
                                      argv[3][0] == 'H' || argv[3][0] == 'h',
                                      values[2]));
    } else if (strcmp(argv[0], "channel") == 0) {
        if (argc != 5 && argc != 6) {
            print_line("error: channel takes 4 or 5 numbers\n");
            return 0;
        }
        values[4] = 0x1fff;
        for (i = 1; i < argc; ++i) {
            if (!parse_number(argv[i], &values[i - 1])) {
                print_line("error: not a number: %s\n", argv[i]);
                return 0;
            }
        }
        report_send(usb_dvbs2_channel(values[0], values[1], values[2],
                                      values[3], values[4]));
    } else if (strcmp(argv[0], "status") == 0) {
        command_status();
    } else if (strcmp(argv[0], "camd") == 0) {
        if (argc == 2 && dtv_stricmp(argv[1], "off") == 0) {
            usb_dvbs2_set_camd(0, 0, "", 0, "", "", "");
        } else if (argc >= 4 && (strcmp(argv[1], "newcamd") == 0 ||
                                 strcmp(argv[1], "cs378x") == 0) &&
                   parse_number(argv[3], &values[0])) {
            usb_dvbs2_set_camd(1, strcmp(argv[1], "cs378x") == 0, argv[2],
                               values[0], argc > 4 ? argv[4] : "",
                               argc > 5 ? argv[5] : "",
                               argc > 6 ? argv[6] : "");
        } else {
            print_line("error: camd off | camd <newcamd|cs378x> <host> <port> "
                       "[<user> <pass> <des_key>]\n");
        }
    } else if (strcmp(argv[0], "epg") == 0 && argc == 2) {
        dtv_atomic_set(&g_show_epg, strcmp(argv[1], "on") == 0);
    } else if (strcmp(argv[0], "raw") == 0 && argc > 1) {
        char *rest = strstr(original, "raw") + 3;
        while (*rest == ' ' || *rest == '\t')
            ++rest;
        report_send(usb_dvbs2_send(rest));
    } else if (strcmp(argv[0], "wait") == 0 && argc == 2 &&
               parse_number(argv[1], &values[0])) {
        uint32_t waited;
        for (waited = 0; waited < values[0] * 1000u &&
                         !dtv_atomic_get(&g_stopping);
             waited += 100)
            dtv_sleep_ms(100);
    } else {
        print_line("error: unknown command; type help\n");
    }
    return 0;
}

#ifdef _WIN32
/* Ctrl+C and closing the console: stop the daemon so the LNB goes down, then
 * end here, since the main thread may be blocked reading the console. */
static BOOL WINAPI on_console_event(DWORD type)
{
    (void)type;
    if (dtv_atomic_set(&g_stopping, 1))
        return TRUE;
    usb_dvbs2_stop();
    ExitProcess(0);
    return TRUE;
}
#else
/* Ctrl+C, a closed terminal or a kill: only the flag is set here. The read
 * of the next command is interrupted, the loop in main() ends, and the
 * daemon is stopped from there, where it is safe to. */
static void on_stop_signal(int signal_number)
{
    (void)signal_number;
    dtv_atomic_set(&g_stopping, 1);
}
#endif

int main(int argc, char **argv)
{
    char dir[DTV_PATH_MAX] = "";
    char line[512];
    const char *profile_key = "auto";
    const dtv_hardware_profile *profile = NULL;
    unsigned port = USB_DVBS2_UDP_PORT;
    usb_dvbs2_host host;
    int i;

    /* Before anything is printed: print_line takes this lock. */
    dtv_mutex_init(&g_print_lock);
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            snprintf(dir, sizeof(dir), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            profile_key = argv[++i];
        } else if (strcmp(argv[i], "--udp") == 0 && i + 1 < argc &&
                   parse_number(argv[i + 1], &port) && port && port < 65535) {
            ++i;
        } else {
            print_usage();
            return strcmp(argv[i], "--help") == 0 ? 0 : 2;
        }
    }

    if (dtv_stricmp(profile_key, "auto") != 0) {
        profile = dtv_hardware_profile_by_key(profile_key);
        if (!profile) {
            fprintf(stderr, "Unknown hardware profile: %s\n", profile_key);
            return 2;
        }
    }
    if (!dir[0])
        dtv_executable_dir(dir, sizeof(dir));

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    memset(&host, 0, sizeof(host));
    host.log = on_log;
    host.stage = on_stage;
    host.service_ca = on_service_ca;
    host.service_teletext = on_teletext;
    host.epg_event = on_epg;
    host.device_lost = on_device_lost;
    host.signal_state = on_signal;
    usb_dvbs2_init(dir, port, &host);
    usb_dvbs2_set_hardware_profile(profile ? profile->id : DTV_HARDWARE_AUTO);
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_event, TRUE);
#else
    {
        /* No SA_RESTART, so a blocked fgets() returns when one arrives. */
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = on_stop_signal;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGHUP, &action, NULL);
    }
#endif

    print_line("Starting the tuner daemon in %s\n", dir);
    if (usb_dvbs2_start() != 0) {
        fprintf(stderr, "The tuner daemon could not be started. The folder "
                        "must hold usb-dvbs2-daemon and firmware/ (and "
                        "libusb-1.0.dll on Windows), and the receiver must be "
                        "plugged in.\n");
        return 1;
    }
    print_line("Ready; the stream is udp://@127.0.0.1:%u. Type help for "
               "commands.\n", port);

    while (!dtv_atomic_get(&g_stopping) &&
           fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (run_command(line))
            break;
    }

#ifdef _WIN32
    /* The console handler is already stopping the daemon and ends the
     * process itself. */
    if (dtv_atomic_set(&g_stopping, 1))
        Sleep(INFINITE);
#endif
    usb_dvbs2_stop();
    return 0;

}
