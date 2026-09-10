/*
  pspdisp-min: USB driver bring-up (subset of the original usb.c).

  Loads usbhostfs.prx and the USBHostFS bulk driver, the same transport the
  linux host's transport_usb.c speaks. Channel 4 (ASYNC_USER) carries frames.
*/
#ifndef USB_H
#define USB_H

#include "common.h"

#define USB_HOSTFSDRIVER_NAME       "USBHostFSDriver"
#define USB_HOSTFSDRIVER_PID_TYPE_B (0x1C9)   /* the original's default mode */

/* Load modules and activate the bulk driver. Returns 0 on success, negative on
   error. */
int usbInit(void);

/* Deactivate + unload the USB driver. */
void usbTerm(void);

#endif
