/* usb_backend.c - thin libusb wrapper, implementation */
#include "usb_backend.h"
#include "dtv_platform.h"
#include "dtv_thread.h"
#include <libusb.h>
#include <stdlib.h>
#include <string.h>

struct dtv_usb {
    libusb_context       *ctx;
    libusb_device_handle *h;
    uint8_t ep_cmd_out;
    uint8_t ep_cmd_in;
};

int dtv_usb_open_index(dtv_usb **out, uint16_t vid, uint16_t pid,
                       unsigned index)
{
    libusb_device **devices = NULL;
    ssize_t device_count;
    unsigned match = 0;
    int r = LIBUSB_ERROR_NO_DEVICE;
    *out = NULL;
    dtv_usb *u = calloc(1, sizeof(*u));
    if (!u)
        return LIBUSB_ERROR_NO_MEM;

    u->ep_cmd_out = DTV_EP_CMD_OUT;
    u->ep_cmd_in  = DTV_EP_CMD_IN;

    r = libusb_init(&u->ctx);
    if (r != 0) {
        free(u);
        return r;
    }

    device_count = libusb_get_device_list(u->ctx, &devices);
    if (device_count < 0) {
        r = (int)device_count;
        libusb_exit(u->ctx);
        free(u);
        return r;
    }
    for (ssize_t i = 0; i < device_count; ++i) {
        struct libusb_device_descriptor descriptor;
        if (libusb_get_device_descriptor(devices[i], &descriptor) != 0 ||
            descriptor.idVendor != vid || descriptor.idProduct != pid)
            continue;
        if (match++ == index) {
            r = libusb_open(devices[i], &u->h);
            break;
        }
    }
    libusb_free_device_list(devices, 1);
    if (r != 0 || !u->h) {
        libusb_exit(u->ctx);
        free(u);
        return r != 0 ? r : LIBUSB_ERROR_NO_DEVICE;
    }

    /* Detaching the kernel driver is not needed on Windows/WinUSB; it may
     * be on Linux. */
    libusb_set_auto_detach_kernel_driver(u->h, 1);

    r = libusb_claim_interface(u->h, 0);
    if (r != 0) {
        libusb_close(u->h);
        libusb_exit(u->ctx);
        free(u);
        return r;
    }

    *out = u;
    return 0;
}

int dtv_usb_open(dtv_usb **out, uint16_t vid, uint16_t pid)
{
    return dtv_usb_open_index(out, vid, pid, 0);
}

static void read_usb_string(libusb_device_handle *handle, uint8_t descriptor,
                            char *destination, size_t capacity)
{
    int length;
    if (!destination || capacity == 0)
        return;
    destination[0] = '\0';
    if (!handle || descriptor == 0)
        return;
    length = libusb_get_string_descriptor_ascii(
        handle, descriptor, (unsigned char *)destination, (int)capacity - 1);
    if (length > 0)
        destination[length] = '\0';
    else
        destination[0] = '\0';
}

int dtv_usb_list(uint16_t vid, uint16_t pid, dtv_usb_device_info *items,
                 size_t capacity, size_t *count)
{
    libusb_context *context = NULL;
    libusb_device **devices = NULL;
    ssize_t device_count;
    size_t matches = 0;
    int r;
    if (!count || (capacity != 0 && !items))
        return LIBUSB_ERROR_INVALID_PARAM;
    *count = 0;
    r = libusb_init(&context);
    if (r != 0)
        return r;
    device_count = libusb_get_device_list(context, &devices);
    if (device_count < 0) {
        libusb_exit(context);
        return (int)device_count;
    }
    for (ssize_t i = 0; i < device_count; ++i) {
        struct libusb_device_descriptor descriptor;
        libusb_device_handle *handle = NULL;
        if (libusb_get_device_descriptor(devices[i], &descriptor) != 0 ||
            descriptor.idVendor != vid || descriptor.idProduct != pid)
            continue;
        if (matches < capacity) {
            dtv_usb_device_info *item = &items[matches];
            memset(item, 0, sizeof(*item));
            item->index = (unsigned)matches;
            item->bus = libusb_get_bus_number(devices[i]);
            item->address = libusb_get_device_address(devices[i]);
            item->vid = descriptor.idVendor;
            item->pid = descriptor.idProduct;
            if (libusb_open(devices[i], &handle) == 0) {
                read_usb_string(handle, descriptor.iManufacturer,
                                item->manufacturer, sizeof(item->manufacturer));
                read_usb_string(handle, descriptor.iProduct,
                                item->product, sizeof(item->product));
                read_usb_string(handle, descriptor.iSerialNumber,
                                item->serial, sizeof(item->serial));
                libusb_close(handle);
            }
        }
        ++matches;
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(context);
    *count = matches;
    return 0;
}

void dtv_usb_close(dtv_usb *u)
{
    if (!u)
        return;
    if (u->h) {
        libusb_release_interface(u->h, 0);
        libusb_close(u->h);
    }
    if (u->ctx)
        libusb_exit(u->ctx);
    free(u);
}

void dtv_usb_set_cmd_eps(dtv_usb *u, uint8_t ep_out, uint8_t ep_in)
{
    u->ep_cmd_out = ep_out;
    u->ep_cmd_in  = ep_in;
}

int dtv_usb_bulk_out(dtv_usb *u, uint8_t ep, const uint8_t *buf, int len, unsigned timeout_ms)
{
    int transferred = 0;
    int r = libusb_bulk_transfer(u->h, ep, (unsigned char *)buf, len,
                                 &transferred, timeout_ms);
    if (r != 0)
        return r;
    return transferred;
}

int dtv_usb_bulk_in(dtv_usb *u, uint8_t ep, uint8_t *buf, int len, unsigned timeout_ms)
{
    int transferred = 0;
    int r = libusb_bulk_transfer(u->h, ep, buf, len, &transferred, timeout_ms);
    if (r != 0)
        return r;
    return transferred;
}

const char *dtv_usb_strerror(int code)
{
    return libusb_error_name(code);
}

/* --- continuous capture (see usb_backend.h) --- */

/* Long enough that an idle endpoint costs almost nothing, short enough that
 * a stopped stream does not hold its transfers for ever. A timeout is not
 * an error: a partly filled transfer is delivered and resubmitted. */
#define STREAM_TRANSFER_TIMEOUT_MS 1000

struct dtv_usb_stream {
    dtv_usb *usb;
    struct libusb_transfer **transfers;
    unsigned transfer_count;
    unsigned chunk_bytes;

    dtv_mutex lock;
    dtv_cond data_ready;
    uint8_t *ring;
    size_t ring_size;
    size_t head, tail, used, peak;
    unsigned long long dropped;

    dtv_atomic32 stopping;
    dtv_atomic32 in_flight;
    dtv_atomic32 lost;      /* transfers retired without being stopped */
    dtv_thread *event_thread;
};

/* Runs on the event thread. No room means the consumer is behind; the
 * newest data is dropped, so what is queued stays in order and the
 * consumer resynchronises once. */
static void stream_store(dtv_usb_stream *s, const uint8_t *data, size_t size)
{
    dtv_mutex_lock(&s->lock);
    if (size > s->ring_size - s->used) {
        s->dropped += size;
    } else {
        size_t first = s->ring_size - s->head;
        if (first > size)
            first = size;
        memcpy(s->ring + s->head, data, first);
        if (size > first)
            memcpy(s->ring, data + first, size - first);
        s->head = (s->head + size) % s->ring_size;
        s->used += size;
        if (s->used > s->peak)
            s->peak = s->used;
    }
    dtv_mutex_unlock(&s->lock);
    dtv_cond_signal(&s->data_ready);
}

static void LIBUSB_CALL stream_complete(struct libusb_transfer *transfer)
{
    dtv_usb_stream *s = (dtv_usb_stream *)transfer->user_data;
    int resubmit = !dtv_atomic_get(&s->stopping);
    switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED:
    case LIBUSB_TRANSFER_TIMED_OUT:
        if (transfer->actual_length > 0)
            stream_store(s, transfer->buffer, (size_t)transfer->actual_length);
        break;
    case LIBUSB_TRANSFER_STALL:
        /* A halted endpoint often clears itself on the next transfer; if
         * not, the submit below fails and this transfer retires. */
        break;
    default:
        /* Cancelled, or the device has gone. Nothing to resubmit to. */
        resubmit = 0;
        break;
    }
    if (resubmit && libusb_submit_transfer(transfer) == 0)
        return;
    /* Lost only when the LAST transfer retires unasked: one that fails to go
     * back is normal, the others keep the stream running. */
    if (dtv_atomic_dec(&s->in_flight) == 0 &&
        !dtv_atomic_get(&s->stopping))
        dtv_atomic_set(&s->lost, 1);
    /* A reader blocked on an empty ring would otherwise wait out its
     * timeout after the stream has already ended. */
    dtv_cond_broadcast(&s->data_ready);
}

static void stream_event_thread(void *argument)
{
    dtv_usb_stream *s = (dtv_usb_stream *)argument;
    /* This thread only hands completed transfers back to the kernel. It is
     * the one thread that must never wait, so it asks for the real-time
     * scheduling class. */
    void *boost = dtv_thread_realtime_begin();
    while (dtv_atomic_get(&s->in_flight) > 0) {
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        libusb_handle_events_timeout_completed(s->usb->ctx, &timeout, NULL);
    }
    dtv_thread_realtime_end(boost);
}

int dtv_usb_stream_open(dtv_usb *u, uint8_t ep, unsigned transfers,
                        unsigned chunk_bytes, size_t ring_bytes,
                        dtv_usb_stream **out)
{
    dtv_usb_stream *s;
    unsigned i;
    if (!out)
        return LIBUSB_ERROR_INVALID_PARAM;
    *out = NULL;
    if (!u || !u->h || !transfers || !chunk_bytes ||
        ring_bytes < (size_t)chunk_bytes * transfers)
        return LIBUSB_ERROR_INVALID_PARAM;

    s = calloc(1, sizeof(*s));
    if (!s)
        return LIBUSB_ERROR_NO_MEM;
    s->usb = u;
    s->transfer_count = transfers;
    s->chunk_bytes = chunk_bytes;
    s->ring_size = ring_bytes;
    s->ring = malloc(ring_bytes);
    s->transfers = calloc(transfers, sizeof(*s->transfers));
    if (!s->ring || !s->transfers) {
        free(s->ring);
        free(s->transfers);
        free(s);
        return LIBUSB_ERROR_NO_MEM;
    }
    dtv_mutex_init(&s->lock);
    dtv_cond_init(&s->data_ready);

    for (i = 0; i < transfers; ++i) {
        uint8_t *buffer = malloc(chunk_bytes);
        struct libusb_transfer *transfer = libusb_alloc_transfer(0);
        if (!buffer || !transfer) {
            free(buffer);
            if (transfer)
                libusb_free_transfer(transfer);
            break;
        }
        libusb_fill_bulk_transfer(transfer, u->h, ep, buffer,
                                  (int)chunk_bytes, stream_complete, s,
                                  STREAM_TRANSFER_TIMEOUT_MS);
        /* The buffer belongs to the transfer from here on, so teardown has
         * one thing to track instead of two. */
        transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER;
        s->transfers[i] = transfer;
        if (libusb_submit_transfer(transfer) != 0)
            break;
        dtv_atomic_inc(&s->in_flight);
    }
    /* Some transfers submitting is enough to stream; none is not. */
    if (dtv_atomic_get(&s->in_flight) == 0) {
        dtv_usb_stream_close(s);
        return LIBUSB_ERROR_IO;
    }
    s->event_thread = dtv_thread_start(stream_event_thread, s);
    if (!s->event_thread) {
        dtv_usb_stream_close(s);
        return LIBUSB_ERROR_OTHER;
    }
    *out = s;
    return 0;
}

int dtv_usb_stream_read(dtv_usb_stream *s, uint8_t *destination,
                        size_t capacity, unsigned timeout_ms)
{
    size_t copied = 0, first;
    if (!s || !destination || !capacity)
        return 0;
    dtv_mutex_lock(&s->lock);
    if (!s->used && timeout_ms)
        dtv_cond_wait_ms(&s->data_ready, &s->lock, timeout_ms);
    copied = s->used < capacity ? s->used : capacity;
    if (copied) {
        first = s->ring_size - s->tail;
        if (first > copied)
            first = copied;
        memcpy(destination, s->ring + s->tail, first);
        if (copied > first)
            memcpy(destination + first, s->ring, copied - first);
        s->tail = (s->tail + copied) % s->ring_size;
        s->used -= copied;
    }
    dtv_mutex_unlock(&s->lock);
    return (int)copied;
}

int dtv_usb_stream_lost(dtv_usb_stream *s)
{
    return s ? (int)dtv_atomic_get(&s->lost) : 0;
}

void dtv_usb_stream_flush(dtv_usb_stream *s)
{
    if (!s)
        return;
    dtv_mutex_lock(&s->lock);
    s->head = s->tail = s->used = 0;
    dtv_mutex_unlock(&s->lock);
}

void dtv_usb_stream_stats(dtv_usb_stream *s, unsigned long long *dropped_bytes,
                          size_t *peak_bytes)
{
    if (!s) {
        if (dropped_bytes) *dropped_bytes = 0;
        if (peak_bytes) *peak_bytes = 0;
        return;
    }
    dtv_mutex_lock(&s->lock);
    if (dropped_bytes) *dropped_bytes = s->dropped;
    if (peak_bytes) *peak_bytes = s->peak;
    dtv_mutex_unlock(&s->lock);
}

void dtv_usb_stream_close(dtv_usb_stream *s)
{
    unsigned i;
    if (!s)
        return;
    dtv_atomic_set(&s->stopping, 1);
    for (i = 0; i < s->transfer_count; ++i)
        if (s->transfers[i])
            libusb_cancel_transfer(s->transfers[i]);
    if (s->event_thread) {
        /* Wake the handler so the cancellations are seen now rather than at
         * the end of its current wait. */
        libusb_interrupt_event_handler(s->usb->ctx);
        if (dtv_thread_join(s->event_thread, 5000) != 0) {
            /* A transfer the driver never completed still owns its buffer
             * and its callback, so nothing may be freed. Leaking a few
             * hundred kilobytes on a dead device is the safe end. */
            dtv_thread_detach(s->event_thread);
            return;
        }
    } else {
        /* No handler thread was started, so the cancellations are collected
         * here or a transfer would be freed while the driver owns it. */
        while (dtv_atomic_get(&s->in_flight) > 0) {
            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000;
            libusb_handle_events_timeout_completed(s->usb->ctx, &timeout,
                                                   NULL);
        }
    }
    for (i = 0; i < s->transfer_count; ++i)
        if (s->transfers[i])
            libusb_free_transfer(s->transfers[i]);
    dtv_cond_destroy(&s->data_ready);
    dtv_mutex_destroy(&s->lock);
    free(s->transfers);
    free(s->ring);
    free(s);
}
