/* probe.c - phase 0 USB descriptor dump
 *
 * Opens the Hiremco "UDTV" USB satellite tuner (ITE IT9300 bridge) with
 * libusb and dumps every descriptor, interface and endpoint. This is what
 * determines which bulk endpoints carry command/control and which carry the
 * TS stream, before any driver is written.
 *
 * Prerequisite: the device must be bound to the WinUSB driver with Zadig.
 *   VID:PID = 048D:F036
 *
 * Building: see CMakeLists.txt (target: usb-dvbs2-usb-probe) */
#include <stdio.h>
#include <libusb.h>

#define UDTV_VID 0x048D
#define UDTV_PID 0xF036

static const char *xfer_type(int t)
{
    switch (t & 0x03) {
    case LIBUSB_TRANSFER_TYPE_CONTROL:     return "CONTROL";
    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: return "ISOC";
    case LIBUSB_TRANSFER_TYPE_BULK:        return "BULK";
    case LIBUSB_TRANSFER_TYPE_INTERRUPT:   return "INTERRUPT";
    default:                               return "?";
    }
}

static void dump_config(libusb_device *dev, struct libusb_device_descriptor *dd)
{
    struct libusb_config_descriptor *cfg;
    int r = libusb_get_active_config_descriptor(dev, &cfg);
    if (r != 0) {
        r = libusb_get_config_descriptor(dev, 0, &cfg);
        if (r != 0) {
            printf("  (config descriptor could not be read: %s)\n", libusb_error_name(r));
            return;
        }
    }

    printf("  Configuration: %d interface\n", cfg->bNumInterfaces);
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        for (int a = 0; a < itf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *id = &itf->altsetting[a];
            printf("  Interface %d (alt %d): class=0x%02x subclass=0x%02x proto=0x%02x, %d endpoint\n",
                   id->bInterfaceNumber, id->bAlternateSetting,
                   id->bInterfaceClass, id->bInterfaceSubClass,
                   id->bInterfaceProtocol, id->bNumEndpoints);
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                int in = (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0;
                printf("    EP 0x%02x  %-3s  %-9s  maxpkt=%d\n",
                       ep->bEndpointAddress,
                       in ? "IN" : "OUT",
                       xfer_type(ep->bmAttributes),
                       ep->wMaxPacketSize);
            }
        }
    }
    libusb_free_config_descriptor(cfg);
}

int main(void)
{
    libusb_context *ctx = NULL;
    int r = libusb_init(&ctx);
    if (r != 0) {
        fprintf(stderr, "libusb_init error: %s\n", libusb_error_name(r));
        return 1;
    }

    libusb_device_handle *h =
        libusb_open_device_with_vid_pid(ctx, UDTV_VID, UDTV_PID);
    if (!h) {
        fprintf(stderr,
                "Device could not be opened (VID=%04x PID=%04x).\n"
                "  - Is the device plugged in?\n"
                "  - Was the WinUSB driver assigned with Zadig?\n",
                UDTV_VID, UDTV_PID);
        libusb_exit(ctx);
        return 2;
    }

    libusb_device *dev = libusb_get_device(h);
    struct libusb_device_descriptor dd;
    libusb_get_device_descriptor(dev, &dd);

    printf("=== Device found: %04x:%04x ===\n", dd.idVendor, dd.idProduct);
    printf("  USB %x.%02x  class=0x%02x  bcdDevice=%x.%02x  #config=%d\n",
           dd.bcdUSB >> 8, dd.bcdUSB & 0xff,
           dd.bDeviceClass,
           dd.bcdDevice >> 8, dd.bcdDevice & 0xff,
           dd.bNumConfigurations);

    unsigned char str[256];
    if (dd.iManufacturer &&
        libusb_get_string_descriptor_ascii(h, dd.iManufacturer, str, sizeof(str)) > 0)
        printf("  Manufacturer: %s\n", str);
    if (dd.iProduct &&
        libusb_get_string_descriptor_ascii(h, dd.iProduct, str, sizeof(str)) > 0)
        printf("  Product:      %s\n", str);
    if (dd.iSerialNumber &&
        libusb_get_string_descriptor_ascii(h, dd.iSerialNumber, str, sizeof(str)) > 0)
        printf("  Serial:       %s\n", str);

    dump_config(dev, &dd);

    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
