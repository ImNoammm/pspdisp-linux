/* USB transport: libusb-1.0 bulk to the PSP running usbhostfs. */
#include <stdio.h>
#include <string.h>
#include <libusb-1.0/libusb.h>
#include "pspdisp.h"

#define PSP_VID 0x054C
#define PSP_PID_B 0x1C9
#define PSP_PID_C 0x1CA
#define EP_OUT 0x02   /* PC -> PSP */
#define EP_IN  0x81   /* PSP -> PC */

static libusb_device_handle *dev;

static int u_write(const void *data, int len)
{
  int sent = 0;
  int r = libusb_bulk_transfer(dev, EP_OUT, (unsigned char *)data, len, &sent, 10000);
  if (r != 0) return r;
  return (sent == len) ? 0 : LIBUSB_ERROR_IO;
}

static int u_read(void *data, int len, unsigned tmo)
{
  int got = 0;
  int r = libusb_bulk_transfer(dev, EP_IN, (unsigned char *)data, len, &got, tmo);
  if (r != 0) return r;
  return (got == len) ? 0 : LIBUSB_ERROR_IO;
}

/* usbhostfs hello handshake (see app/source/usb.pas:UsbInitializeAsync). */
static bool handshake(void)
{
  unsigned char magic[4] = { 0x12, 0x08, 0x2F, 0x78 };   /* 0x782F0812 LE */
  unsigned char buf[12];
  int sent = 0;

  if (u_write(magic, 4) != 0) return false;

  /* Probe: if it completes, PSP was already connected -> done. On a fresh
     connect the PSP is busy writing its hello on EP_IN first, so this 12-byte
     OUT write times out and we fall through to read+echo. */
  memset(buf, 0, sizeof buf);
  if (libusb_bulk_transfer(dev, EP_OUT, buf, 12, &sent, 500) == 0 && sent == 12) {
    VLOG("usb: already connected\n");
    return true;
  }
  if (u_read(buf, 12, 1000) != 0) { fprintf(stderr, "usb: hello read failed\n"); return false; }
  if (u_write(buf, 12) != 0)      { fprintf(stderr, "usb: hello echo failed\n"); return false; }
  return true;
}

static bool u_open(void)
{
  static bool inited;
  if (!inited) { if (libusb_init(NULL) != 0) return false; inited = true; }

  dev = libusb_open_device_with_vid_pid(NULL, PSP_VID, PSP_PID_B);
  if (!dev) dev = libusb_open_device_with_vid_pid(NULL, PSP_VID, PSP_PID_C);
  if (!dev) return false;

  libusb_set_auto_detach_kernel_driver(dev, 1);
  if (libusb_claim_interface(dev, 0) != 0) {
    fprintf(stderr, "usb: claim interface 0 failed (udev rule / root?)\n");
    libusb_close(dev); dev = NULL; return false;
  }
  if (!handshake()) {
    libusb_release_interface(dev, 0); libusb_close(dev); dev = NULL; return false;
  }
  return true;
}

static void u_close(void)
{
  if (dev) { libusb_release_interface(dev, 0); libusb_close(dev); dev = NULL; }
}

static transport_backend backend = {
  .name = "usb", .open = u_open, .write = u_write, .read = u_read,
  .close = u_close, .full_response = true,
};
transport_backend *transport_usb(void) { return &backend; }
