/* usb-dvbs2-daemon: the tuner engine behind a command channel -- a named pipe
 * on Windows, a Unix domain socket elsewhere (dtv_ipc.h).
 *
 * Commands arrive on the channel, one line each, and STATUS is answered there;
 * everything else the engine reports goes to standard output as one line per
 * event, fields separated by '|'. tuner/README.md describes both. When the
 * control client goes, the LNB is powered down: nobody else is left to. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/resource.h>
#endif

#include "dtv_camd.h"
#include "dtv_ipc.h"
#include "dtv_thread.h"
#include "usb_dvbs2_engine.h"

static dtv_ipc *g_control;
static dtv_ipc_server *g_server;

/* --- engine events as protocol lines ------------------------------------ */

static void on_log(const char *line)
{
    fputs(line, stdout);
    fflush(stdout);
}

static void on_error(const char *line)
{
    fputs(line, stderr);
}

/* Boot screen progress. The format is fixed: STAGE|<percent>|<text> */
static void on_stage(int percent, const char *text)
{
    printf("STAGE|%d|%s\n", percent, text);
    fflush(stdout);
}

static void on_signal(int has_stream)
{
    printf("SIGNAL|%d|\n", has_stream ? 1 : 0);
    fflush(stdout);
}

static void on_device_lost(const char *text)
{
    printf("DEVICE|0|%s\n", text);
    fflush(stdout);
}

static void on_service_ca(unsigned service_id, int scrambled,
                          unsigned ca_system_id, unsigned ca_pid)
{
    printf("CA|%u|%d|%u|%u\n", service_id, scrambled, ca_system_id, ca_pid);
    fflush(stdout);
}

static void on_teletext(unsigned service_id, unsigned pid)
{
    printf("TTX|%u|%u\n", service_id, pid);
    fflush(stdout);
}

/* The engine has already replaced '|' in the texts. */
static void on_epg(const dtv_epg_event *event)
{
    printf("EPG|%u|%u|%lld|%u|%s|%s\n", (unsigned)event->service_id,
           (unsigned)event->event_id, (long long)event->start_utc,
           (unsigned)event->duration_seconds, event->title, event->text);
    fflush(stdout);
}

static void on_camd(int state, const char *message)
{
    printf("CAMD|%d|%s\n", state, message);
    fflush(stdout);
}

static void on_loss(unsigned long continuity_gaps, unsigned long send_failures,
                    unsigned long overflow_packets)
{
    printf("LOSS|%lu|%lu|%lu\n", continuity_gaps, send_failures,
           overflow_packets);
    fflush(stdout);
}

/* --- pipe commands ------------------------------------------------------- */

/* Handles one command line; returns 1 for QUIT. */
static int handle_line(char *line)
{
    if (strncmp(line, "TUNE_RF ", 8) == 0) {
        /* TUNE_RF <rf_mhz> <sr_ksps> <H|V> [<diseqc>] */
        unsigned long mhz = 0, sr = 0;
        unsigned long dq = 0;
        char pol = 'V';
        if (sscanf(line + 8, "%lu %lu %c %lu", &mhz, &sr, &pol, &dq) >= 3)
            usb_dvbs2_engine_tune_rf((uint32_t)mhz, (uint32_t)sr,
                                     pol == 'H' || pol == 'h', (uint32_t)dq);
    } else if (strncmp(line, "TUNE ", 5) == 0) {
        unsigned long f = 0, sr = 0, lnb = 0, tone = 0, dq = 0;
        unsigned long unc = 0, burst = 0, repeat = 0;
        /* The last three fields are optional; old-format TUNE lines leave
         * them zero. */
        if (sscanf(line + 5, "%lu %lu %lu %lu %lu %lu %lu %lu", &f, &sr,
                   &lnb, &tone, &dq, &unc, &burst, &repeat) >= 2)
            usb_dvbs2_engine_tune((uint32_t)f, (uint32_t)sr, (uint32_t)lnb,
                                  (uint32_t)tone, (uint32_t)dq,
                                  (uint32_t)unc, (uint32_t)burst,
                                  (uint32_t)repeat);
    } else if (strncmp(line, "CHANNEL ", 8) == 0) {
        unsigned long sid = 0, pmt = 0, vp = 0, ap = 0, tp = 0x1fff;
        unsigned long caid = 0, capid = 0x1fff;
        /* The teletext pid is optional: without it the channel simply gets
         * no pages. */
        int fields = sscanf(line + 8, "%lu %lu %lu %lu %lu %lu %lu",
                            &sid, &pmt, &vp, &ap, &tp, &caid, &capid);
        if (fields >= 4) {
            usb_dvbs2_engine_set_channel(
                (uint16_t)sid, (uint16_t)pmt, (uint16_t)vp, (uint16_t)ap,
                fields >= 5 ? (uint16_t)tp : (uint16_t)0x1fff,
                fields >= 6 ? (uint16_t)caid : 0,
                fields >= 7 ? (uint16_t)capid : (uint16_t)0x1fff);
            on_log("CHANNEL applied.\n");
        }
    } else if (strncmp(line, "CAMD ", 5) == 0) {
        /* CAMD OFF
         * CAMD <newcamd|cs378x> <host> <port> <user> <pass> <des-key>
         * The card server can be changed while the daemon runs. */
        char proto[16] = "", host[96] = "", user[64] = "", pass[64] = "";
        char des_key[64] = "";
        unsigned port = 0;
        if (strncmp(line + 5, "OFF", 3) == 0) {
            usb_dvbs2_engine_set_camd(NULL);
        } else if (sscanf(line + 5, "%15s %95s %u %63s %63s %63s", proto,
                          host, &port, user, pass, des_key) >= 5) {
            dtv_camd_config config;
            memset(&config, 0, sizeof(config));
            if (dtv_camd_parse_proto(proto, &config.proto) != 0) {
                printf("CAMD|-1|Unknown protocol: %s\n", proto);
                fflush(stdout);
            } else if (config.proto == DTV_CAMD_PROTO_NEWCAMD &&
                       dtv_camd_parse_des_key(des_key, config.des_key) != 0) {
                printf("CAMD|-1|The DES key must be 28 hexadecimal digits\n");
                fflush(stdout);
            } else {
                snprintf(config.host, sizeof(config.host), "%s", host);
                config.port = port;
                snprintf(config.user, sizeof(config.user), "%s", user);
                snprintf(config.password, sizeof(config.password), "%s",
                         pass);
                usb_dvbs2_engine_set_camd(&config);
            }
        }
    } else if (strncmp(line, "STATUS", 6) == 0) {
        /* The CAM state is appended after the programme fields, so a reader
         * that stops at the seventh field is unaffected. */
        usb_dvbs2_engine_status status;
        char reply[768];
        int length;
        usb_dvbs2_engine_get_status(&status);
        length = snprintf(
            reply, sizeof(reply),
            "STATUS %ld %ld %ld %ld %ld"
            "\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%s\n",
            status.locked, status.fec_locked, status.frame_locked,
            status.snr_x100, status.symbol_rate_hz,
            status.date, status.time, status.now_start, status.now_end,
            status.now_title, status.next_start, status.next_title,
            status.cam_state, status.cam_message);
        if (g_control && length > 0)
            dtv_ipc_write(g_control, reply,
                          (size_t)length < sizeof(reply) ? (size_t)length
                                                         : sizeof(reply) - 1);
    } else if (strncmp(line, "STOP", 4) == 0) {
        /* Stop receiving but stay alive. Sent just before QUIT so the dish
         * is powered down even if this process is killed first. */
        usb_dvbs2_engine_lnb_off();
    } else if (strncmp(line, "QUIT", 4) == 0) {
        return 1;
    }
    return 0;
}

#ifndef _WIN32
/* SIGTERM, SIGINT and SIGHUP end the daemon the way QUIT does, so the LNB
 * is powered down on the way out rather than left on. */
static void on_stop_signal(int signal_number)
{
    (void)signal_number;
    dtv_ipc_server_interrupt(g_server);
}
#endif

int main(int argc, char **argv)
{
    unsigned device_index = 0;
    unsigned udp_port = 5560;
    const char *profile_key = "auto";
    char pipe_name[128];
    dtv_camd_config camd;
    int camd_configured = 0;
    usb_dvbs2_engine_config config;
    usb_dvbs2_engine_host host;
    int i, rc;

    snprintf(pipe_name, sizeof(pipe_name), "%s", dtv_ipc_default_name());
    memset(&camd, 0, sizeof(camd));
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc)
            device_index = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--udp") == 0 && i + 1 < argc)
            udp_port = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--pipe") == 0 && i + 1 < argc)
            snprintf(pipe_name, sizeof(pipe_name), "%s", argv[++i]);
        else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc)
            profile_key = argv[++i];
    /* Card server options, named after tsdecrypt so a working command line
     * carries over. Optional; the CAMD pipe command can also change them
     * while running. */
        else if (strcmp(argv[i], "--camd-proto") == 0 && i + 1 < argc) {
            if (dtv_camd_parse_proto(argv[++i], &camd.proto) != 0) {
                fprintf(stderr, "Unknown CAM protocol: %s\n", argv[i]);
                return 2;
            }
            camd_configured = 1;
        } else if (strcmp(argv[i], "--camd-server") == 0 && i + 1 < argc) {
            char *colon;
            snprintf(camd.host, sizeof(camd.host), "%s", argv[++i]);
            colon = strrchr(camd.host, ':');
            if (colon) {
                *colon = 0;
                camd.port = (unsigned)strtoul(colon + 1, NULL, 10);
            }
            camd_configured = 1;
        } else if (strcmp(argv[i], "--camd-user") == 0 && i + 1 < argc) {
            snprintf(camd.user, sizeof(camd.user), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--camd-pass") == 0 && i + 1 < argc) {
            snprintf(camd.password, sizeof(camd.password), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--camd-des-key") == 0 && i + 1 < argc) {
            if (dtv_camd_parse_des_key(argv[++i], camd.des_key) != 0) {
                fprintf(stderr, "The DES key must be 28 hexadecimal digits.\n");
                return 2;
            }
        }
    }
    /* A deadline job that spends nearly all its time blocked: raising it
     * above ordinary desktop work costs the foreground nothing and stops a
     * heavy application from taking the picture with it. */
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
#else
    /* Only granted with the right to; otherwise it runs at normal priority. */
    setpriority(PRIO_PROCESS, 0, -5);
    {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = on_stop_signal;
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGHUP, &action, NULL);
        signal(SIGPIPE, SIG_IGN);
    }
#endif
    /* And out of the reduced-clock class given to windowless processes. */
    dtv_thread_process_no_throttle();

    memset(&host, 0, sizeof(host));
    host.log = on_log;
    host.error = on_error;
    host.stage = on_stage;
    host.signal_state = on_signal;
    host.device_lost = on_device_lost;
    host.service_ca = on_service_ca;
    host.service_teletext = on_teletext;
    host.epg_event = on_epg;
    host.camd_state = on_camd;
    host.loss = on_loss;
    memset(&config, 0, sizeof(config));
    config.profile_key = profile_key;
    config.device_index = device_index;
    config.udp_port = udp_port;
    config.camd = camd_configured ? &camd : NULL;
    rc = usb_dvbs2_engine_open(&config, &host);
    if (rc != 0)
        return rc;

    g_server = dtv_ipc_listen(pipe_name);
    if (!g_server)
        fprintf(stderr, "The command channel %s could not be opened.\n",
                pipe_name);
    printf("Daemon ready. pipe=%s udp=%u\n", pipe_name, udp_port);
    fflush(stdout);

    if (g_server) {
        int done = 0;
        while (!done) {
            char buf[512];
            int got;
            g_control = dtv_ipc_accept(g_server);
            if (!g_control)
                break;
            while ((got = dtv_ipc_read(g_control, buf, sizeof(buf) - 1)) > 0) {
                char *p = buf;
                buf[got] = 0;
                while (*p) {
                    char *line = p;
                    char *nl = strchr(p, '\n');
                    if (nl) { *nl = 0; p = nl + 1; } else { p += strlen(p); }
                    { char *cr = strchr(line, '\r'); if (cr) *cr = 0; }
                    if (handle_line(line)) { done = 1; break; }
                }
                if (done) break;
            }
            dtv_ipc_disconnect(g_control);
            g_control = NULL;
            /* Read returned nothing: the control client has gone, so
             * nobody is watching and nobody else can power the dish down. */
            usb_dvbs2_engine_lnb_off();
        }
    }
    usb_dvbs2_engine_close();
    dtv_ipc_server_close(g_server);
    g_server = NULL;
    return 0;
}

