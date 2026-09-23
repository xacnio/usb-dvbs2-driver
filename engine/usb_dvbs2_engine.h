#ifndef USB_DVBS2_ENGINE_H
#define USB_DVBS2_ENGINE_H

#include <stdint.h>

#include "dtv_camd.h"
#include "ts_epg.h"

/* The receiver in-process: cold init, tuning, service selection, the
 * filtered MPEG-TS on UDP, and what the stream reveals -- signal, CA and
 * teletext of the service, programme events, card server state, loss.
 *
 * usb-dvbs2-daemon is this engine behind a named pipe. Use it directly only
 * where no daemon runs: the engine owns the USB device, and there is one per
 * process. */

/* Every callback is optional. They run on the engine's threads (the stream
 * thread, the signal monitor) or on the caller's, and must return quickly:
 * the stream thread carries the picture. */
typedef struct usb_dvbs2_engine_host {
    /* Diagnostic text; one line, newline included. */
    void (*log)(const char *line);
    /* A failure; one line, newline included. Falls back to log. */
    void (*error)(const char *line);
    /* Progress of cold init and of a tune. */
    void (*stage)(int percent, const char *text);
    /* The stream started arriving, or stopped for a few seconds. */
    void (*signal_state)(int has_stream);
    /* The receiver went away; the stream has ended. */
    void (*device_lost)(const char *text);
    /* From the PMT of the selected service. */
    void (*service_ca)(unsigned service_id, int scrambled,
                       unsigned ca_system_id, unsigned ca_pid);
    void (*service_teletext)(unsigned service_id, unsigned pid);
    /* A programme from the event table, once per revision. */
    void (*epg_event)(const dtv_epg_event *event);
    /* A dtv_camd_state, or -1 for settings that could not be used. */
    void (*camd_state)(int state, const char *message);
    /* The carrier is streaming but its table of programmes does not name
     * the selected service: the channel list sent us to the wrong one, or
     * the service has left the carrier. Once per channel. */
    void (*service_missing)(unsigned service_id,
                            unsigned long continuity_gaps);
    /* Packets lost before the UDP socket, per service; on a change. */
    void (*loss)(unsigned long continuity_gaps, unsigned long send_failures,
                 unsigned long overflow_packets);
} usb_dvbs2_engine_host;

/* Services carried beside the one the viewer is watching, for a window
 * showing several channels at once. They all come off the carrier the tuner
 * is locked to -- one tuner, one carrier -- and each goes to a UDP port of
 * its own, the watched channel's port plus two for the first, four for the
 * second and so on. They are filtered and nothing more: no descrambling, no
 * teletext, no programme events, and no waiting for a clean picture, which
 * a small tile can do without. */
#define USB_DVBS2_MAX_EXTRA_SERVICES 9

typedef struct usb_dvbs2_engine_config {
    /* "auto" or a hardware profile key. */
    const char *profile_key;
    /* Which of several matching receivers. */
    unsigned device_index;
    /* The stream goes to 127.0.0.1 on this port and the one above. */
    unsigned udp_port;
    /* Card server to start with; NULL for none. */
    const dtv_camd_config *camd;
} usb_dvbs2_engine_config;

/* Cold init, then the stream and signal monitor threads. Firmware is read
 * by relative path, so the working directory must hold firmware\.
 * 0 ready; 2 no profile, no receiver or cold init failed; 3 no stream
 * thread. */
int usb_dvbs2_engine_open(const usb_dvbs2_engine_config *config,
                          const usb_dvbs2_engine_host *host);
/* Stops the stream, powers the LNB down and releases the receiver. */
void usb_dvbs2_engine_close(void);

/* Locks a carrier: L-band frequency, symbol rate, LNB volts (13, 18, 0),
 * tone (0, 22), DiSEqC 1.0 port (0 off, 1-4), 1.1 port (0 off, 1-16),
 * tone burst (0 none, 1 A, 2 B) and repeats. Skipped when that carrier is
 * already streaming. 0 locked or skipped, 1 no lock, -1 failure.
 * A symbol rate of 0 acquires blind: the widest tuner filter, and the
 * demodulator finds the symbol rate itself; STATUS then reports what it
 * found. */
int usb_dvbs2_engine_tune(uint32_t lband_khz, uint32_t symbol_rate_ksps,
                          uint32_t lnb_volts, uint32_t tone_khz,
                          uint32_t diseqc, uint32_t uncommitted,
                          uint32_t burst, uint32_t repeat);
/* The same from the RF frequency, through a Ku-band universal LNB: LO 9750
 * MHz, or 10600 MHz and the 22 kHz tone from 11700 MHz; 13 V vertical, 18 V
 * horizontal. diseqc is the DiSEqC 1.0 port, 0 for none. -1 outside
 * 10700-12750 MHz. */
int usb_dvbs2_engine_tune_rf(uint32_t rf_mhz, uint32_t symbol_rate_ksps,
                             int horizontal, uint32_t diseqc);

/* Selects a service on the locked carrier. 0x1fff for PIDs not known: the
 * PMT and the elementary PIDs are then learned from the stream. The CA pair
 * is a hint for BISS services whose PMT names none. */
/* One of the extra services, or none for that slot when service_id is 0.
 * slot counts from 0 and is below USB_DVBS2_MAX_EXTRA_SERVICES. The pids may
 * be 0x1fff: the tables on the carrier are read for whatever is missing. */
void usb_dvbs2_engine_set_extra(unsigned slot, uint16_t service_id,
                                uint16_t pmt_pid, uint16_t video_pid,
                                uint16_t audio_pid);

void usb_dvbs2_engine_set_channel(uint16_t service_id, uint16_t pmt_pid,
                                  uint16_t video_pid, uint16_t audio_pid,
                                  uint16_t teletext_pid,
                                  uint16_t ca_system_id, uint16_t ca_pid);

/* Starts, replaces or (NULL) stops the card server client. */
void usb_dvbs2_engine_set_camd(const dtv_camd_config *config);

/* Powers the LNB down; the next tune brings it up again. */
void usb_dvbs2_engine_lnb_off(void);

typedef struct usb_dvbs2_engine_status {
    long locked;
    long fec_locked;
    long frame_locked;
    long snr_x100;
    long symbol_rate_hz;
    /* Satellite clock (TDT/TOT) and the service's now and next (EIT);
     * empty until the stream has carried them. */
    char date[16];
    char time[16];
    char now_start[16];
    char now_end[16];
    char now_title[192];
    char next_start[16];
    char next_title[192];
    int cam_state;
    char cam_message[128];
} usb_dvbs2_engine_status;

void usb_dvbs2_engine_get_status(usb_dvbs2_engine_status *status);

#endif
