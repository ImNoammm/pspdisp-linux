/*
  pspdisp-min — module load helpers.
*/
#include "utils.h"
#include <kubridge.h>

SceUID utilsLoadStartModule(const char *path)
{
  SceUID moduleId = pspSdkLoadStartModule(path, PSP_MEMORY_PARTITION_KERNEL);
  if (moduleId >= 0)
    return moduleId;

  /* Loader refused (e.g. SCE_KERNEL_ERROR_PROHIBIT_LOADMODULE_DEVICE on some
     CFW) — retry through kubridge. */
  moduleId = kuKernelLoadModule((char *)path, 0, NULL);
  if (moduleId < 0)
    return moduleId;

  int status;
  SceUID result = sceKernelStartModule(moduleId, 0, NULL, &status, NULL);
  return (result >= 0) ? moduleId : result;
}

int utilsStopUnloadModule(SceUID modID)
{
  int status;
  int result = sceKernelStopModule(modID, 0, NULL, &status, NULL);
  if (result < 0)
    return result;
  return sceKernelUnloadModule(modID);
}
