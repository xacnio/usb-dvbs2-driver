/* AVL6261 clock/repeater/tuner bring-up and optional transponder lock probe. */
#include "dtv_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

#include "dtv_platform.h"

#include "usb_backend.h"
#include "it9300.h"
#include "avl62x1.h"
#include "rda5815m.h"
#include "ts_udp.h"
#include "tool_util.h"

#define UDTV_VID 0x048d
#define UDTV_PID 0xf036
#define BRIDGE_FW "firmware/hiremco-it9303.fw"
#define DEMOD_FW  "firmware/hiremco-avl62x1.fw"

static int parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;
    if (!text || !*text || !value) return -1;
    parsed = strtoul(text, &end, 10);
    if (*end != '\0' || parsed > 0xfffffffful) return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s --list-devices\n"
            "  %s [--device INDEX] --rf RF_MHz symbol_rate_kSym_s H|V [capture.ts]\n"
            "  %s [--device INDEX] --rf RF_MHz symbol_rate_kSym_s H|V --live-udp port stop_event [service_id pmt_pid video_pid audio_pid]\n"
            "  %s --fixed-capture frequency_kHz symbol_rate_kSym_s volt tone diseqc capture.ts duration_ms\n"
            "  %s\n"
            "  %s frequency_kHz symbol_rate_kSym_s [0|13|18 [0|22 [DiSEqC_port_0_4 [capture.ts [duration_ms]]]]]\n"
            "--rf tunes through a Ku-band universal LNB, on the DiSEqC 1.0 port given\n"
            "with --diseqc10 PORT (none by default).\n"
            , program, program, program, program, program, program);
}

static int print_devices(void)
{
    dtv_usb_device_info devices[32];
    size_t count = 0;
    int rc = dtv_usb_list(UDTV_VID, UDTV_PID, devices,
                          sizeof(devices) / sizeof(devices[0]), &count);
    if (rc != 0) {
        fprintf(stderr, "Could not get the USB device list: %s\n",
                dtv_usb_strerror(rc));
        return 2;
    }
    for (size_t i = 0; i < count && i < sizeof(devices) / sizeof(devices[0]); ++i) {
        const dtv_usb_device_info *device = &devices[i];
        printf("DEVICE\t%u\t%u\t%u\t%04x\t%04x\t%s\t%s\t%s\n",
               device->index, device->bus, device->address,
               device->vid, device->pid, device->manufacturer,
               device->product, device->serial);
    }
    return 0;
}

/* Watches a capture go by and says when there is nothing left to wait for.
 *
 * A scan needs only the PAT, one PMT per programme it names, and the SDT
 * with the service names. All repeat several times a second, so they are
 * normally complete within a second or two, while the capture used to run
 * its full five (or fourteen) seconds and write tens of megabytes for
 * nothing -- most of the waiting on a ninety-carrier search.
 *
 * The NIT is the exception: its sections are about a second apart, so when
 * it is wanted the capture keeps running until it arrives. */

/* A table is spread over numbered sections and only the whole set is the
 * whole truth. The SDT especially: each section names some services and
 * brings their names and scrambling flags. Stopping at the first left the
 * rest with the PAT placeholder name (Service 11303) and no encryption
 * flag -- which is how they reached the channel list. */
typedef struct psi_sections {
    int seen_any;
    unsigned last;          /* last_section_number from the sections seen */
    uint32_t mask;          /* one bit per section number, 0..31 */
} psi_sections;

static void psi_sections_note(psi_sections *table, unsigned number,
                              unsigned last)
{
    table->seen_any = 1;
    table->last = last;
    if (number < 32)
        table->mask |= 1u << number;
}

static int psi_sections_complete(const psi_sections *table)
{
    unsigned i;
    if (!table->seen_any)
        return 0;
    /* These carriers do not broadcast tables with more than 32 sections;
     * treating one as never complete would stall the capture. */
    if (table->last >= 32)
        return 1;
    for (i = 0; i <= table->last; ++i) {
        if (!(table->mask & (1u << i)))
            return 0;
    }
    return 1;
}

typedef struct psi_watch {
    int want_nit;
    psi_sections pat;
    psi_sections sdt;
    int have_nit;
    uint16_t pmt_pids[64];
    uint8_t pmt_seen[64];
    size_t pmt_count;
    psi_section_assembler pat_asm;
    psi_section_assembler sdt_asm;
    psi_section_assembler nit_asm;
    /* When something we were waiting for last arrived. A PAT can announce a
     * programme the multiplex does not carry -- Turksat has such entries --
     * and waiting for its PMT would always run the clock out. */
    uint32_t progress_tick;
} psi_watch;

#define PSI_IDLE_MS 1500

static void psi_watch_packet(psi_watch *watch, const uint8_t *packet)
{
    uint16_t pid = (uint16_t)(((packet[1] & 0x1f) << 8) | packet[2]);
    const uint8_t *section;
    size_t size;
    if (pid == 0x0000) {
        if (!psi_sections_complete(&watch->pat) &&
            psi_assemble(&watch->pat_asm, packet, &section, &size) &&
            size >= 12 && section[0] == 0x00) {
            size_t pos;
            for (pos = 8; pos + 4 <= size - 4; pos += 4) {
                uint16_t service = (uint16_t)((section[pos] << 8) |
                                              section[pos + 1]);
                uint16_t pmt = (uint16_t)(((section[pos + 2] & 0x1f) << 8) |
                                          section[pos + 3]);
                size_t i;
                int known = 0;
                if (!service || watch->pmt_count >= 64)
                    continue;
                for (i = 0; i < watch->pmt_count; ++i)
                    known |= watch->pmt_pids[i] == pmt;
                if (known)
                    continue;
                watch->pmt_pids[watch->pmt_count] = pmt;
                watch->pmt_seen[watch->pmt_count] = 0;
                ++watch->pmt_count;
            }
            psi_sections_note(&watch->pat, section[6], section[7]);
            watch->progress_tick = dtv_tick32();
        }
        return;
    }
    if (pid == 0x0011) {
        if (!psi_sections_complete(&watch->sdt) &&
            psi_assemble(&watch->sdt_asm, packet, &section, &size) &&
            size >= 12 && section[0] == 0x42) {
            uint32_t before = watch->sdt.mask;
            psi_sections_note(&watch->sdt, section[6], section[7]);
            if (watch->sdt.mask != before)
                watch->progress_tick = dtv_tick32();
        }
        return;
    }
    if (pid == 0x0010) {
        if (!watch->have_nit &&
            psi_assemble(&watch->nit_asm, packet, &section, &size) &&
            size >= 16 && section[0] == 0x40)
            watch->have_nit = 1;
        return;
    }
    /* A PMT only has to be seen, not parsed: the scan reads it back out of
     * the file afterwards. */
    if (watch->pat.seen_any && (packet[1] & 0x40)) {
        size_t i;
        for (i = 0; i < watch->pmt_count; ++i) {
            if (watch->pmt_pids[i] == pid && !watch->pmt_seen[i]) {
                watch->pmt_seen[i] = 1;
                watch->progress_tick = dtv_tick32();
            }
        }
    }
}

static int psi_watch_complete(const psi_watch *watch)
{
    size_t i;
    int missing = 0;
    if (!psi_sections_complete(&watch->pat) ||
        !psi_sections_complete(&watch->sdt))
        return 0;
    /* The network table has no such shortcut: it either arrives or the
     * timeout ends the capture. */
    if (watch->want_nit && !watch->have_nit)
        return 0;
    for (i = 0; i < watch->pmt_count; ++i) {
        if (!watch->pmt_seen[i])
            missing = 1;
    }
    /* A transport stream with no programmes is empty, not complete; let
     * such a carrier use its full time. */
    if (!watch->pmt_count)
        return 0;
    if (missing)
        return (uint32_t)(dtv_tick32() - watch->progress_tick) > PSI_IDLE_MS;
    return 1;
}

static int capture_ts(it9300 *bridge, const char *path, unsigned duration_ms,
                      int want_nit, int fixed_duration)
{
    dtv_usb *usb = bridge->usb;
    uint32_t last_status = 0;
    /* Extra space retains a partial packet and enough bytes to reacquire
     * five-packet sync across USB transfer boundaries. */
    uint8_t buffer[65424 + 5 * 188];
    FILE *file = fopen(path, "wb");
    uint32_t started = dtv_tick32();
    size_t total = 0;
    size_t pending = 0;
    unsigned sync_acquisitions = 0;
    int synced = 0;
    psi_watch watch;
    int complete = 0;

    memset(&watch, 0, sizeof(watch));
    watch.want_nit = want_nit;
    /* The idle rule measures from the start, not from tick zero. */
    watch.progress_tick = started;
    if (!file) return -1;
    while ((fixed_duration || !complete) &&
           (uint32_t)(dtv_tick32() - started) < duration_ms) {
        size_t available, pos = 0;
        int n = dtv_usb_bulk_in(usb, DTV_EP_TS_IN, buffer + pending,
                                65424, 500);
        if (n < 0) continue; /* timeout while the TS FIFO is temporarily idle */
        if (n == 0) continue;
        available = pending + (size_t)n;
        while (pos + 188 <= available) {
            if (!synced) {
                size_t sync = find_ts_sync(buffer, available, pos);
                if (sync == (size_t)-1)
                    break;
                pos = sync;
                synced = 1;
                ++sync_acquisitions;
            }
            if (buffer[pos] != 0x47) {
                synced = 0;
                ++pos;
                continue;
            }
            if (fwrite(buffer + pos, 1, 188, file) != 188) {
                fclose(file); return -2;
            }
            psi_watch_packet(&watch, buffer + pos);
            total += 188;
            pos += 188;
        }
        if (!synced && available - pos > 4u * 188u)
            pos = available - 4u * 188u;
        pending = available - pos;
        if (pending)
            memmove(buffer, buffer + pos, pending);
        if (pending > sizeof(buffer) - 65424u) {
            /* Defensive bound; normal synced operation retains <188 bytes. */
            pending = 0;
            synced = 0;
        }
        complete = psi_watch_complete(&watch);
        /* One reading a second: enough for the gauge to follow, cheap enough
         * not to compete with the transport stream for the USB bus. */
        if ((uint32_t)(dtv_tick32() - last_status) >= 1000u) {
            avl62x1_signal_status status;
            last_status = dtv_tick32();
            if (avl62x1_get_signal_status(bridge, AVL62X1_I2C_ADDR,
                                          &status) == 0)
                printf("  %4lu ms: lock=%u fec=%u frame=%u SNR=%d.%02d dB "
                       "SR=%lu\n",
                       (unsigned long)(dtv_tick32() - started),
                       status.locked, status.fec_locked, status.frame_locked,
                       status.snr_db_x100 / 100,
                       abs(status.snr_db_x100 % 100),
                       (unsigned long)status.symbol_rate_hz);
        }
    }
    fclose(file);
    printf("TS capture: %s, %lu bytes, %lu ms, sync acquisitions=%u%s\n", path,
           (unsigned long)total,
           (unsigned long)(dtv_tick32() - started), sync_acquisitions,
           complete ? " (tables complete)" : "");
    return total != 0 ? 0 : -3;
}

/* What ends --live-udp. On Windows the caller names an event and sets it; on
 * the others the name is ignored and SIGINT or SIGTERM end the stream. */
#ifdef _WIN32
typedef HANDLE stop_signal;

static int stop_open(const char *name, stop_signal *out)
{
    *out = OpenEventA(SYNCHRONIZE, FALSE, name);
    return *out ? 0 : -1;
}

static int stop_requested(stop_signal stop)
{
    return WaitForSingleObject(stop, 0) == WAIT_OBJECT_0;
}

static void stop_close(stop_signal stop)
{
    if (stop)
        CloseHandle(stop);
}
#else
typedef int stop_signal;

static volatile sig_atomic_t g_stop_requested;

static void on_stop_signal(int signal_number)
{
    (void)signal_number;
    g_stop_requested = 1;
}

static int stop_open(const char *name, stop_signal *out)
{
    struct sigaction action;
    (void)name;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    *out = 1;
    return 0;
}

static int stop_requested(stop_signal stop)
{
    (void)stop;
    return g_stop_requested != 0;
}

static void stop_close(stop_signal stop)
{
    (void)stop;
}
#endif

static int stream_ts_udp(dtv_usb *usb, unsigned port,
                         const char *stop_event_name, uint16_t service_id,
                         uint16_t pmt_pid, uint16_t video_pid,
                         uint16_t audio_pid)
{
    dtv_socket socket_handle = DTV_INVALID_SOCKET;
    udp_ts_writer writer;
    stop_signal stop_event = 0;
    int net_started = 0;
    uint8_t buffer[65424 + 5 * 188];
    size_t pending = 0;
    unsigned long packets = 0;
    int send_buffer_size = 1024 * 1024;
    int synced = 0, result = -1;
    /* A known PMT pid is not required: the elementary PIDs are enough and
     * the PMT pid comes from the PAT below. Without this, TKGS channels
     * disabled filtering and the player showed the first service. */
    int filter_service = service_id != 0 &&
                         (pmt_pid < 0x1fffu || video_pid < 0x1fffu ||
                          audio_pid < 0x1fffu);
    uint16_t pcr_pid = video_pid;
    /* Filled in from the table when the service carries pages; the probe
     * only passes them through. */
    uint16_t teletext_pid = 0x1fff;
    uint16_t stream_pids[UDP_TS_MAX_PROGRAM_PIDS];
    size_t stream_pid_count = 0;
    uint8_t pat_cc = 0, pmt_cc = 0;
    psi_section_assembler pat_asm, pmt_asm;

    memset(&pat_asm, 0, sizeof(pat_asm));
    memset(&pmt_asm, 0, sizeof(pmt_asm));

    if (port == 0 || port > 65535 || !stop_event_name)
        return -1;
    if (stop_open(stop_event_name, &stop_event) != 0)
        return -2;
    if (dtv_net_startup() != 0)
        goto done;
    net_started = 1;
    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == DTV_INVALID_SOCKET)
        goto done;
    /* USB bulk reads arrive in bursts. Leave enough localhost UDP headroom
       for short receiver scheduling delays. */
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDBUF,
               (const char *)&send_buffer_size,
               (int)sizeof(send_buffer_size));
    memset(&writer, 0, sizeof(writer));
    writer.socket_handle = socket_handle;
    writer.destination.sin_family = AF_INET;
    writer.destination.sin_port = htons((uint16_t)port);
    writer.destination.sin_addr.s_addr = inet_addr("127.0.0.1");
    printf("Live TS: udp://127.0.0.1:%u (stop event=%s)\n",
           port, stop_event_name);
    if (filter_service)
        printf("Single-service TS: SID=%u PMT=0x%04x video=0x%04x audio=0x%04x\n",
               service_id, pmt_pid, video_pid, audio_pid);
    fflush(stdout);

    while (!stop_requested(stop_event)) {
        size_t available, pos = 0;
        int n = dtv_usb_bulk_in(usb, DTV_EP_TS_IN, buffer + pending,
                                65424, 500);
        if (n < 0)
            continue;
        if (n == 0)
            continue;
        available = pending + (size_t)n;
        while (pos + 188 <= available) {
            if (!synced) {
                size_t sync = find_ts_sync(buffer, available, pos);
                if (sync == (size_t)-1)
                    break;
                pos = sync;
                synced = 1;
            }
            if (buffer[pos] != 0x47) {
                synced = 0;
                ++pos;
                continue;
            }
            ++packets;
            if (!filter_service) {
                if (udp_ts_send_packet(&writer, buffer + pos) != 0) {
                    result = -3;
                    goto done;
                }
            } else {
                const uint8_t *packet = buffer + pos;
                uint16_t pid = (uint16_t)(((packet[1] & 0x1fu) << 8) |
                                          packet[2]);
                if (pid == 0) {
                    const uint8_t *section;
                    size_t section_size;
                    if (psi_assemble(&pat_asm, packet, &section,
                                     &section_size)) {
                        if (pmt_pid >= 0x1fffu)
                            pmt_pid = pat_lookup_pmt_pid(section, section_size,
                                                         service_id);
                        if (pmt_pid < 0x1fffu &&
                            udp_ts_send_single_pat(&writer, section,
                                                   section_size, service_id,
                                                   pmt_pid, &pat_cc) != 0) {
                            result = -3;
                            goto done;
                        }
                    }
                } else if (pid == pmt_pid && pmt_pid < 0x1fffu) {
                    const uint8_t *section;
                    size_t section_size;
                    if (psi_assemble(&pmt_asm, packet, &section,
                                     &section_size) &&
                        udp_ts_send_selected_pmt(
                            &writer, section, section_size, pmt_pid,
                            service_id, &teletext_pid, &pcr_pid,
                            stream_pids, UDP_TS_MAX_PROGRAM_PIDS,
                            &stream_pid_count, &pmt_cc, 0) != 0) {
                        result = -3;
                        goto done;
                    }
                } else if (udp_ts_pid_in_list(pid, stream_pids,
                                              stream_pid_count) ||
                           (pid == video_pid && video_pid < 0x1fffu) ||
                           (pid == audio_pid && audio_pid < 0x1fffu) ||
                           (pid == teletext_pid && teletext_pid < 0x1fffu) ||
                           (pid == pcr_pid && pcr_pid < 0x1fffu)) {
                    if (udp_ts_send_packet(&writer, packet) != 0) {
                        result = -3;
                        goto done;
                    }
                }
            }
            pos += 188;
        }
        if (!synced && available - pos > 4u * 188u)
            pos = available - 4u * 188u;
        pending = available - pos;
        if (pending)
            memmove(buffer, buffer + pos, pending);
        if (pending > sizeof(buffer) - 65424u) {
            pending = 0;
            synced = 0;
        }
    }
    result = udp_ts_flush(&writer) == 0 ? 0 : -3;
    printf("Live TS stopped: source=%lu output=%lu packets\n",
           packets, writer.packets);

done:
    if (socket_handle != DTV_INVALID_SOCKET)
        dtv_socket_close(socket_handle);
    if (net_started)
        dtv_net_cleanup();
    stop_close(stop_event);

    return result;
}

int main(int argc, char **argv)
{
    dtv_usb *usb = NULL;
    it9300 bridge;
    int result = 1;
    uint32_t chip_id = 0, core = 0, fec = 0, mpeg = 0;
    uint32_t frequency_khz = 0, symbol_rate_ksps = 0;
    uint32_t lnb_voltage = 0, tone_khz = 0;
    uint32_t diseqc_port = 0;
    /* DiSEqC 1.1 (uncommitted) port, tone burst and command repeats, taken
     * as options stripped off the front since the positional arguments
     * already branch on argc. */
    uint32_t diseqc_uncommitted = 0, tone_burst = 0, diseqc_repeat = 0;
    uint32_t live_udp_port = 0;
    uint32_t live_service_id = 0, live_pmt_pid = 0x1fffu;
    /* The caller reads this through a pipe, which is fully buffered:
     * without this the readings arrive in one lump at exit. */
    setvbuf(stdout, NULL, _IONBF, 0);
    uint32_t live_video_pid = 0x1fffu, live_audio_pid = 0x1fffu;
    uint32_t device_index = 0;
    int lnb_enabled = 0;
    int rf_profile = 0;
    int diseqc10_given = 0;
    const char *capture_path = NULL;
    const char *stop_event_name = NULL;
    uint32_t capture_duration_ms = 5000;
    /* --need-nit keeps the capture running until the network table has been
     * seen; without it the tables a scan needs are enough to stop early. */
    int want_nit = 0;
    /* Private/operator tables such as TKGS repeat more slowly than
     * PAT/PMT/SDT, so the generic ones must not end the capture early. */
    int fixed_capture = 0;

    if (argc == 2 && strcmp(argv[1], "--list-devices") == 0)
        return print_devices();
    if (argc >= 3 && strcmp(argv[1], "--device") == 0) {
        if (parse_u32(argv[2], &device_index) != 0 || device_index > 31u) {
            fprintf(stderr, "Invalid USB device index.\n");
            return 2;
        }
        argc -= 2;
        argv += 2;
    }
    while (argc >= 2 && strcmp(argv[1], "--need-nit") == 0) {
        want_nit = 1;
        argc -= 1;
        argv += 1;
    }
    while (argc >= 2 && strcmp(argv[1], "--fixed-capture") == 0) {
        fixed_capture = 1;
        argc -= 1;
        argv += 1;
    }
    while (argc >= 3 && argv[1][0] == '-' && argv[1][1] == '-') {
        uint32_t *target = NULL;
        uint32_t limit = 0;
        if (strcmp(argv[1], "--diseqc10") == 0) {
            target = &diseqc_port;
            limit = 4;
            diseqc10_given = 1;
        } else if (strcmp(argv[1], "--diseqc11") == 0) {
            target = &diseqc_uncommitted;
            limit = 16;
        } else if (strcmp(argv[1], "--burst") == 0) {
            target = &tone_burst;
            limit = 2;
        } else if (strcmp(argv[1], "--repeat") == 0) {
            target = &diseqc_repeat;
            limit = 2;
        } else {
            break;
        }
        if (parse_u32(argv[2], target) != 0 || *target > limit) {
            fprintf(stderr, "Invalid %s value (0..%lu).\n", argv[1],
                    (unsigned long)limit);
            return 2;
        }
        argc -= 2;
        argv += 2;
    }

    if (argc >= 2 && strcmp(argv[1], "--rf") == 0) {
        uint32_t rf_mhz = 0;
        int horizontal;
        uint32_t local_oscillator_mhz;

        if ((argc != 5 && argc != 6 && argc != 7 && argc != 8 && argc != 12) ||
            parse_u32(argv[2], &rf_mhz) != 0 ||
            parse_u32(argv[3], &symbol_rate_ksps) != 0 ||
            rf_mhz < 10700u || rf_mhz > 12750u ||
            symbol_rate_ksps == 0 || symbol_rate_ksps > 70000u ||
            argv[4][1] != '\0' ||
            (argv[4][0] != 'H' && argv[4][0] != 'h' &&
             argv[4][0] != 'V' && argv[4][0] != 'v')) {
            fprintf(stderr, "Invalid RF frequency, symbol rate or polarization.\n");
            print_usage(argv[0]);
            return 2;
        }
        horizontal = argv[4][0] == 'H' || argv[4][0] == 'h';
        tone_khz = rf_mhz >= 11700u ? 22u : 0u;
        local_oscillator_mhz = tone_khz == 22 ? 10600u : 9750u;
        frequency_khz = (rf_mhz - local_oscillator_mhz) * 1000u;
        lnb_voltage = horizontal ? 18u : 13u;
        if (!diseqc10_given)
            diseqc_port = 0u;
        rf_profile = 1;
        if (argc == 6 || argc == 7) {
            capture_path = argv[5];
            if (argc == 7) {
                if (parse_u32(argv[6], &capture_duration_ms) != 0 ||
                    capture_duration_ms == 0 || capture_duration_ms > 60000u) {
                    fprintf(stderr,
                            "The capture duration must be in the range 1..60000 ms.\n");
                    return 2;
                }
            }
        } else if (argc == 8 || argc == 12) {
            if (strcmp(argv[5], "--live-udp") != 0 ||
                parse_u32(argv[6], &live_udp_port) != 0 ||
                live_udp_port == 0 || live_udp_port > 65535u ||
                !argv[7][0]) {
                fprintf(stderr, "Invalid live UDP port or stop event.\n");
                return 2;
            }
            stop_event_name = argv[7];
            if (argc == 12 &&
                (parse_u32(argv[8], &live_service_id) != 0 ||
                 parse_u32(argv[9], &live_pmt_pid) != 0 ||
                 parse_u32(argv[10], &live_video_pid) != 0 ||
                 parse_u32(argv[11], &live_audio_pid) != 0 ||
                 live_service_id == 0 || live_service_id > 0xffffu ||
                 live_pmt_pid >= 0x1fffu ||
                 live_video_pid > 0x1fffu || live_audio_pid > 0x1fffu)) {
                fprintf(stderr, "Invalid live service/PID information.\n");
                return 2;
            }
        }
        printf("Universal LNB: RF=%lu MHz, LO=%lu MHz, L-band=%lu kHz, "
               "%c, %lu V, tone=%lu kHz, DiSEqC 1.0 port %lu\n",
               (unsigned long)rf_mhz,
               (unsigned long)local_oscillator_mhz,
               (unsigned long)frequency_khz, horizontal ? 'H' : 'V',
               (unsigned long)lnb_voltage, (unsigned long)tone_khz,
               (unsigned long)diseqc_port);
    } else {
        if (argc != 1 && (argc < 3 || argc > 8)) {
            print_usage(argv[0]);
            return 2;
        }
        if (argc >= 3 &&
            (parse_u32(argv[1], &frequency_khz) != 0 ||
             parse_u32(argv[2], &symbol_rate_ksps) != 0 ||
             frequency_khz < 950000u || frequency_khz > 2150000u ||
             symbol_rate_ksps == 0 || symbol_rate_ksps > 70000u)) {
            fprintf(stderr, "Invalid L-band frequency or symbol rate.\n");
            return 2;
        }
        if (argc >= 4 &&
            (parse_u32(argv[3], &lnb_voltage) != 0 ||
             (lnb_voltage != 0 && lnb_voltage != 13 && lnb_voltage != 18))) {
            fprintf(stderr, "The LNB voltage must be 0, 13 or 18.\n");
            return 2;
        }
        if (argc >= 5 &&
            (parse_u32(argv[4], &tone_khz) != 0 ||
             (tone_khz != 0 && tone_khz != 22))) {
            fprintf(stderr, "The tone value must be 0 or 22.\n");
            return 2;
        }
        if (argc == 6) {
            uint32_t parsed_port = 0;
            if (parse_u32(argv[5], &parsed_port) == 0) {
                if (parsed_port > 4) {
                    fprintf(stderr, "The DiSEqC port must be 0..4 (0=off).\n");
                    return 2;
                }
                diseqc_port = parsed_port;
            } else {
                capture_path = argv[5]; /* compatibility with the old CLI */
            }
        } else if (argc == 7 || argc == 8) {
            if (parse_u32(argv[5], &diseqc_port) != 0 || diseqc_port > 4) {
                fprintf(stderr, "The DiSEqC port must be 0..4 (0=off).\n");
                return 2;
            }
            capture_path = argv[6];
            if (argc == 8) {
                if (parse_u32(argv[7], &capture_duration_ms) != 0 ||
                    capture_duration_ms == 0 || capture_duration_ms > 60000u) {
                    fprintf(stderr,
                            "The capture duration must be in the range 1..60000 ms.\n");
                    return 2;
                }
            }
        }
    }
    if (tone_khz == 22 && lnb_voltage == 0) {
        fprintf(stderr, "An LNB voltage must be selected as well for the 22 kHz tone.\n");
        return 2;
    }
    if (diseqc_port != 0 && lnb_voltage == 0) {
        fprintf(stderr, "A DiSEqC selection requires a 13 or 18 V LNB voltage.\n");
        return 2;
    }

    printf("USB device index: %lu\n", (unsigned long)device_index);
    if (dtv_usb_open_index(&usb, UDTV_VID, UDTV_PID, device_index) != 0) {
        fprintf(stderr, "USB device %lu could not be opened. Refresh the device list.\n",
                (unsigned long)device_index); return 2;
    }
    if (it9300_attach(&bridge, usb) != 0 || it9300_identify(&bridge) != 0)
        goto done;
    if (it9300_query_fw_version(&bridge) != 0 &&
        load_file_to_bridge(&bridge, BRIDGE_FW, 0) != 0)
        goto done;
    if (it9300_bridge_init(&bridge) != 0) goto done;

    it9300_gpio_set(&bridge, 1, 1); dtv_sleep_ms(100);
    it9300_gpio_set(&bridge, 1, 0); dtv_sleep_ms(200);
    it9300_gpio_set(&bridge, 1, 1); dtv_sleep_ms(100);
    bridge.i2c_bus = 3;

    if (avl62x1_read32(&bridge, AVL62X1_I2C_ADDR, 0x040000, &chip_id) != 0 ||
        chip_id != AVL62X1_CHIP_ID) {
        fprintf(stderr, "AVL6261 not found: 0x%08lx\n", (unsigned long)chip_id);
        goto done;
    }
    if (load_file_to_bridge(&bridge, DEMOD_FW, 1) != 0 ||
        avl62x1_wait_ready(&bridge, AVL62X1_I2C_ADDR, 100, 50) != 0) {
        fprintf(stderr, "AVL6261 firmware failed to boot.\n"); goto done;
    }

    /* Hiremco family and the public AVL6261 reference design both use a
     * 27 MHz crystal. Serial TS below runs in adaptive-clock mode. */
    int rc = avl62x1_load_defaults(&bridge, AVL62X1_I2C_ADDR, 1, 120000000u,
                                   &core, &fec, &mpeg);
    printf("Load-default rc=%d core=%lu Hz fec=%lu Hz mpeg=%lu Hz\n", rc,
           (unsigned long)core, (unsigned long)fec, (unsigned long)mpeg);
    if (rc != 0 || core < 10000000u || core > 500000000u) goto done;

    rc = avl62x1_init_tuner_i2c(&bridge, AVL62X1_I2C_ADDR, core);
    if (rc == 0) rc = avl62x1_set_tuner_i2c(&bridge, AVL62X1_I2C_ADDR, 1);
    printf("Tuner I2C repeater rc=%d\n", rc);
    if (rc != 0) goto done;

    /* An address sweep used to sit here. It is a bring-up diagnostic
     * nothing reads, and it costs a USB round trip for each of 112
     * addresses on every carrier of a search; usb-dvbs2-i2c-scan does it. */
    rc = rda5815m_init(&bridge, RDA5815M_I2C_ADDR);
    printf("RDA5815M init rc=%d\n", rc);
    if (rc == 0 && frequency_khz != 0) {
        rc = rda5815m_tune(&bridge, RDA5815M_I2C_ADDR,
                           frequency_khz, symbol_rate_ksps, 0);
        printf("RDA5815M tune %lu kHz / %lu kSym/s rc=%d\n",
               (unsigned long)frequency_khz,
               (unsigned long)symbol_rate_ksps, rc);
    }
    avl62x1_set_tuner_i2c(&bridge, AVL62X1_I2C_ADDR, 0);
    if (rc != 0) goto done;

    rc = avl62x1_init_demod_input(&bridge, AVL62X1_I2C_ADDR, 0);
    printf("Demod ADC/AGC init rc=%d\n", rc);
    if (rc != 0) goto done;

    /* IT9303 register 0xda58 selects serial TS0 input. Data0/MSB-first and
     * rising edge are the AVL reference defaults, kept explicit so a live
     * polarity/pin test can change them independently. */
    const avl62x1_ts_config ts_config = {
        .mode = 1, .format = 0, .clock_rising = 1, .clock_phase = 0,
        .adaptive_clock = 1, .error_inverted = 0, .valid_inverted = 0,
        .serial_data_pin = 0, .serial_msb_first = 1
    };
    rc = avl62x1_configure_ts(&bridge, AVL62X1_I2C_ADDR, &ts_config, 1);
    printf("AVL6261 serial TS init rc=%d\n", rc);
    if (rc != 0) goto done;

    rc = avl62x1_init_diseqc(&bridge, AVL62X1_I2C_ADDR, core);
    printf("AVL6261 DiSEqC init rc=%d\n", rc);
    if (rc != 0) goto done;

    /* Establish a known-safe state before honoring an explicit CLI voltage. */
    rc = avl62x1_set_22khz_tone(&bridge, AVL62X1_I2C_ADDR, 0);
    if (rc == 0)
        rc = avl62x1_set_lnb_voltage(&bridge, AVL62X1_I2C_ADDR, 0);
    printf("LNB safe-off rc=%d\n", rc);
    if (rc != 0) goto done;

    if (lnb_voltage != 0) {
        rc = avl62x1_set_lnb_voltage(&bridge, AVL62X1_I2C_ADDR,
                                     lnb_voltage);
        if (rc == 0) lnb_enabled = 1;
        printf("LNB voltage %lu V rc=%d\n", (unsigned long)lnb_voltage, rc);
        if (rc != 0) goto done;
        /* USB reconnect cuts LNB power. Give the LNB and DiSEqC switch time
         * to cold-start before sending the committed-switch command. */
        dtv_sleep_ms(500);
        /* 1.1 (uncommitted) first, then 1.0 (committed), and the tone burst
         * last: the established order for cascaded setups. */
        for (uint32_t pass = 0; pass <= diseqc_repeat; ++pass) {
            int repeated = pass > 0;
            if (diseqc_uncommitted != 0) {
                rc = avl62x1_select_diseqc_uncommitted(
                    &bridge, AVL62X1_I2C_ADDR, diseqc_uncommitted,
                    repeated);
                printf("DiSEqC 1.1 port %lu select rc=%d\n",
                       (unsigned long)diseqc_uncommitted, rc);
                if (rc != 0) goto done;
                dtv_sleep_ms(50);
            }
            if (diseqc_port != 0) {
                rc = avl62x1_select_diseqc_committed(
                    &bridge, AVL62X1_I2C_ADDR, diseqc_port,
                    lnb_voltage == 18, tone_khz == 22, repeated);
                printf("DiSEqC 1.0 LNB %lu select rc=%d\n",
                       (unsigned long)diseqc_port, rc);
                if (rc != 0) goto done;
                dtv_sleep_ms(50);
            }
        }
        if (tone_burst != 0) {
            rc = avl62x1_send_tone_burst(&bridge, AVL62X1_I2C_ADDR,
                                         tone_burst == 2);
            printf("Tone burst %c rc=%d\n",
                   tone_burst == 2 ? (char)66 : (char)65, rc);
            if (rc != 0) goto done;
            dtv_sleep_ms(50);
        }
        rc = avl62x1_set_22khz_tone(&bridge, AVL62X1_I2C_ADDR,
                                    tone_khz == 22);
        printf("LNB tone %lu kHz rc=%d\n", (unsigned long)tone_khz, rc);
        if (rc != 0) goto done;
        dtv_sleep_ms(100);
    }

    if (frequency_khz != 0) {
        rc = avl62x1_acquire(&bridge, AVL62X1_I2C_ADDR,
                             symbol_rate_ksps * 1000u, 0, 0);
        printf("AVL6261 acquire rc=%d\n", rc);
        if (rc != 0) goto done;

        avl62x1_signal_status status;
        int locked = 0;
        for (unsigned poll = 0; poll < 30; ++poll) {
            dtv_sleep_ms(100);
            rc = avl62x1_get_signal_status(&bridge, AVL62X1_I2C_ADDR,
                                           &status);
            if (rc != 0) break;
            printf("  %4u ms: lock=%u fec=%u frame=%u SNR=%d.%02d dB SR=%lu\n",
                   (poll + 1) * 100u, status.locked, status.fec_locked,
                   status.frame_locked, status.snr_db_x100 / 100,
                   abs(status.snr_db_x100 % 100),
                   (unsigned long)status.symbol_rate_hz);
            if (status.locked) { locked = 1; break; }
        }
        if (rc != 0) goto done;
        if (live_udp_port != 0) {
            if (locked) {
                rc = stream_ts_udp(bridge.usb, live_udp_port,
                                   stop_event_name,
                                   (uint16_t)live_service_id,
                                   (uint16_t)live_pmt_pid,
                                   (uint16_t)live_video_pid,
                                   (uint16_t)live_audio_pid);
                if (rc != 0) goto done;
            } else {
                puts("Live view skipped: the demod did not lock.");
                result = 3;
                goto done;
            }
        } else if (capture_path) {
            if (locked) {
                rc = capture_ts(&bridge, capture_path,
                                capture_duration_ms, want_nit,
                                fixed_capture);
                if (rc != 0) goto done;
            } else {
                puts("TS capture skipped: the demod did not lock.");
                result = 3;
                goto done;
            }
        }
    } else {
        puts("Lock test skipped; no frequency and symbol rate were given.");
    }
    if (rf_profile)
        printf("Universal LNB profile applied (DiSEqC 1.0 port %lu).\n",
               (unsigned long)diseqc_port);
    result = 0;

done:
    if (lnb_enabled) {
        int tone_off_rc = avl62x1_set_22khz_tone(
            &bridge, AVL62X1_I2C_ADDR, 0);
        int voltage_off_rc = avl62x1_set_lnb_voltage(
            &bridge, AVL62X1_I2C_ADDR, 0);
        printf("LNB cleanup: tone_off=%d voltage_off=%d\n",
               tone_off_rc, voltage_off_rc);
        if (result == 0 && (tone_off_rc != 0 || voltage_off_rc != 0))
            result = 1;
    }
    dtv_usb_close(usb);
    return result;
}
