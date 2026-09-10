/*
  pspdisp-min: module load helpers (subset of the original utils.c).
*/
#ifndef UTILS_H
#define UTILS_H

#include <pspsdk.h>
#include <pspkernel.h>
#include "common.h"

/* Load + start a kernel-partition module (used for usbhostfs.prx). Falls back
   to kuKernelLoadModule when the loader refuses (some CFW). Returns the module
   UID, or a negative kernel error. */
SceUID utilsLoadStartModule(const char *path);

/* Stop + unload a module. Returns 0 on success, negative on error. */
int utilsStopUnloadModule(SceUID modID);

#endif
