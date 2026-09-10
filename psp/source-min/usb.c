/*
  pspdisp-min: USB driver bring-up.
*/
#include <pspkernel.h>
#include <pspusb.h>
#include "usb.h"
#include "utils.h"
#include "usbhostfs/usbasync.h"

static SceUID l_moduleId = 0;
/* File-static (not a stack local) so the async provider's buffer outlives
   usbInit() for the lifetime of the connection. */
static struct AsyncEndpoint l_endp;

int usbInit(void)
{
  if (l_moduleId == 0)
    l_moduleId = utilsLoadStartModule("usbhostfs.prx");
  if (l_moduleId < 0)
    return -1;

  if (sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0) != 0)
    return -2;
  if (sceUsbStart(USB_HOSTFSDRIVER_NAME, 0, 0) != 0)
    return -3;
  if (sceUsbActivate(USB_HOSTFSDRIVER_PID_TYPE_B) != 0)
    return -4;

  usbAsyncRegister(ASYNC_USER, &l_endp);
  return 0;
}

void usbTerm(void)
{
  usbAsyncUnregister(ASYNC_USER);
  sceUsbDeactivate(USB_HOSTFSDRIVER_PID_TYPE_B);
  sceUsbStop(USB_HOSTFSDRIVER_NAME, 0, 0);
  sceUsbStop(PSP_USBBUS_DRIVERNAME, 0, 0);

  int result = utilsStopUnloadModule(l_moduleId);
  /* Unloading is broken on some CFW (6.20 TN-C returns 0x80020136); only forget
     the handle when it actually unloaded so we don't reload a second copy. */
  if (result != 0x80020136)
    l_moduleId = 0;
}
