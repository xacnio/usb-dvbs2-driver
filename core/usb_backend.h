/* usb_backend.h - thin libusb wrapper
 *
 * All USB IO of the IT9300 bridge goes through this layer: command/control
 * bulk transfers and the TS stream. In an Android port this file is
 * replaced by an equivalent based on UsbDeviceConnection (fd); the layers
 * above it (it9300, cxd2878) stay unchanged. */
#ifndef USB_BACKEND_H
#define USB_BACKEND_H

#include <stddef.h>
#include <stdint.h>

typedef struct dtv_usb dtv_usb;

#define DTV_USB_TEXT_MAX 128

typedef struct dtv_usb_device_info {
    unsigned index;
    uint8_t bus;
    uint8_t address;
    uint16_t vid;
    uint16_t pid;
    char manufacturer[DTV_USB_TEXT_MAX];
    char product[DTV_USB_TEXT_MAX];
    char serial[DTV_USB_TEXT_MAX];
} dtv_usb_device_info;

/* Default bulk endpoints for the command protocol (verified by probing). */
#define DTV_EP_CMD_OUT 0x02
#define DTV_EP_CMD_IN  0x81
#define DTV_EP_TS_IN   0x84

/* Open the device (VID:PID) and claim interface 0. 0 on success, < 0 on
 * error. */
int  dtv_usb_open(dtv_usb **out, uint16_t vid, uint16_t pid);
int  dtv_usb_open_index(dtv_usb **out, uint16_t vid, uint16_t pid,
                        unsigned index);
int  dtv_usb_list(uint16_t vid, uint16_t pid, dtv_usb_device_info *items,
                  size_t capacity, size_t *count);
void dtv_usb_close(dtv_usb *u);

/* Set the command endpoints (defaults above). */
void dtv_usb_set_cmd_eps(dtv_usb *u, uint8_t ep_out, uint8_t ep_in);

/* Bulk OUT/IN. Returns the number of bytes transferred (>= 0), or < 0 on
 * error. timeout_ms = 0 means unlimited. */
int  dtv_usb_bulk_out(dtv_usb *u, uint8_t ep, const uint8_t *buf, int len, unsigned timeout_ms);
int  dtv_usb_bulk_in (dtv_usb *u, uint8_t ep, uint8_t *buf, int len, unsigned timeout_ms);

/* --- continuous capture of the transport stream ---
 *
 * dtv_usb_bulk_in() submits one transfer at a time, so everything the
 * caller does between reads -- descrambling, filtering, the socket -- is
 * time with nothing queued for the endpoint, and the bytes the tuner
 * produces meanwhile pile up in the small bridge FIFO. On a busy machine
 * it overflows, and the packets it drops look exactly like packets the
 * dish never received.
 *
 * A stream keeps several transfers submitted at all times and hands what
 * arrives to a ring the consumer reads at its own pace, which is what
 * every DVB driver does. */
typedef struct dtv_usb_stream dtv_usb_stream;

/* transfers   how many stay submitted at once (8..64 is sensible)
 * chunk_bytes bytes per transfer; a multiple of the 512-byte bulk packet
 * ring_bytes  how far the consumer may fall behind before data is lost
 *
 * 0 on success. The device must stay open for the life of the stream. */
int  dtv_usb_stream_open(dtv_usb *u, uint8_t ep, unsigned transfers,
                         unsigned chunk_bytes, size_t ring_bytes,
                         dtv_usb_stream **out);
void dtv_usb_stream_close(dtv_usb_stream *s);

/* Copies out whatever the ring holds, up to capacity, waiting up to
 * timeout_ms for the first byte. Returns bytes copied, 0 on timeout. */
int  dtv_usb_stream_read(dtv_usb_stream *s, uint8_t *destination,
                         size_t capacity, unsigned timeout_ms);

/* Nonzero once the transfers retired without anyone stopping them: the
 * receiver was unplugged, or its driver let go of it. */
int  dtv_usb_stream_lost(dtv_usb_stream *s);

/* Throws away what is buffered. Used when the tuner has just changed
 * transponder: everything read before the lock belongs to the old one. */
void dtv_usb_stream_flush(dtv_usb_stream *s);

/* dropped_bytes: what arrived while the ring was full, i.e. loss caused on
 * THIS machine. peak_bytes: how close the ring came to filling, so the size
 * can be judged. Either pointer may be NULL. */
void dtv_usb_stream_stats(dtv_usb_stream *s, unsigned long long *dropped_bytes,
                          size_t *peak_bytes);

/* Human-readable last error (libusb_error_name). */
const char *dtv_usb_strerror(int code);

#endif /* USB_BACKEND_H */
