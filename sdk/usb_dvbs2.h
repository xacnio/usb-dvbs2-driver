#ifndef USB_DVBS2_H
#define USB_DVBS2_H

#include "../core/ts_epg.h"

#include <stddef.h>
#include <wchar.h>
#ifdef _WIN32
/* Kept for programs that were written against it coming along. */
#include <windows.h>
#endif

/* Talking to the tuner daemon; the tuner SDK.
 *
 * A separate process (usb-dvbs2-daemon) drives the hardware: USB, firmware,
 * demodulator and LNB. Programs talk to it over a local channel (a named
 * pipe on Windows, a Unix domain socket elsewhere) with text
 * commands (see tuner/README.md), and this library is that transport -- it
 * starts the process, formats commands and parses events and status. Which
 * transponder to go to and when is the caller's job. */

/* The daemon broadcasts the TS to this port and the player reads it. */
#define USB_DVBS2_UDP_PORT 5560u

typedef struct usb_dvbs2_status {
    int locked;
    int fec_locked;
    int frame_locked;
    int snr_x100;
    unsigned symbol_rate_hz;
    /* Clock from the satellite (TDT); may be empty. */
    char date[16];
    char time[16];
    /* Current and next programme (EIT); may be empty. */
    char now_title[192];
    char now_start[16];
    char now_end[16];
    char next_title[192];
    char next_start[16];
    /* Card server state, mirroring dtv_camd_state in the daemon: 0 off,
     * 1 connecting, 2 connected, 3 decrypting, 4 rejected, 5 error. */
    int cam_state;
    char cam_message[128];
} usb_dvbs2_status;

typedef struct usb_dvbs2_host {
    void (*log)(const char *text);
    /* Cold init takes a while; stage information goes to a progress display. */
    void (*stage)(int percent, const wchar_t *text);
    /* On a channel switch the daemon reports from the PMT whether the
     * service is scrambled; the TKGS table has no such flag. */
    void (*service_ca)(unsigned service_id, int scrambled,
                       unsigned ca_system_id, unsigned ca_pid);
    /* Likewise teletext: the daemon reads the live PMT, so it knows which
     * stream carries the pages even when the channel list says none. */
    void (*service_teletext)(unsigned service_id, unsigned pid);
    /* One programme from the event table of whatever carrier is tuned. */
    void (*epg_event)(const dtv_epg_event *event);
    /* The receiver went away while it was streaming. Called on the reader
     * thread, so the handler has to marshal. */
    void (*device_lost)(const wchar_t *text);
    /* Transport stream running or silent, i.e. the carrier is there or it is
     * not. Also called on the reader thread. */
    void (*signal_state)(int has_stream);
} usb_dvbs2_host;

void usb_dvbs2_init(const char *exe_dir, unsigned udp_port,
                    const usb_dvbs2_host *host);

/* Card server (CAM) settings for descrambling. The daemon does the ECM
 * traffic and the descrambling; the caller only stores them and shows
 * what came back. Applying them while the daemon runs takes effect at once,
 * and they are remembered for a later start. enabled = 0 shuts it down. */
void usb_dvbs2_set_camd(int enabled, int proto, const char *host,
                        unsigned port, const char *user,
                        const char *password, const char *des_key);
/* Last state the daemon reported; see usb_dvbs2_status.cam_state. */
/* Packets the daemon saw missing BEFORE handing the stream to the local
 * socket, datagrams it could not send, and packets its USB reader had to
 * discard because this machine was too busy. Beside the player's own gap
 * count they separate a reception problem from a local one. Any pointer
 * may be NULL. */
void usb_dvbs2_loss_counters(int *continuity_gaps, int *send_failures,
                             int *overflow_packets);
int usb_dvbs2_cam_state(void);
const char *usb_dvbs2_cam_message(void);
/* Selects the hardware chain used on the next daemon start. Changing it
 * while running requires stopping the daemon first. */
void usb_dvbs2_set_hardware_profile(int profile);
int usb_dvbs2_hardware_profile(void);

/* Is the daemon running; starts it if not. 0 = ready. */
int usb_dvbs2_start(void);
void usb_dvbs2_stop(void);
int usb_dvbs2_running(void);
/* Is the first cold init still running (a start screen can wait for this)? */
int usb_dvbs2_starting(void);
void usb_dvbs2_set_starting(int starting);
/* Tries to claim the start: 1 = you own it, 0 = someone else already did.
 * Test and mark are atomic. */
int usb_dvbs2_claim_start(void);

/* Sends a raw command line. 0 = written. */
int usb_dvbs2_send(const char *line);
/* TUNE command; diseqc_args is "<1.0> <1.1> <tone burst> <repeat>". */
int usb_dvbs2_tune(unsigned lband_khz, unsigned symbol_rate_ksps,
                   unsigned lnb_voltage, unsigned tone_khz,
                   const char *diseqc_args);
/* TUNE_RF: the same from the RF frequency, through a Ku-band universal LNB.
 * diseqc is the DiSEqC 1.0 port, 0 for none. */
int usb_dvbs2_tune_rf(unsigned rf_mhz, unsigned symbol_rate_ksps,
                      int horizontal, unsigned diseqc);
/* teletext_pid is 0x1fff when the service carries no pages. */
int usb_dvbs2_channel(unsigned service_id, unsigned pmt_pid,
                      unsigned video_pid, unsigned audio_pid,
                      unsigned teletext_pid);

/* Reads a pending status reply and sends a new query if needed. 1 = out was
 * filled, 0 = no reply yet. */
int usb_dvbs2_poll(usb_dvbs2_status *out);

#endif
