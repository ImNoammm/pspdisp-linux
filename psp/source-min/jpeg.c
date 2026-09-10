/*
  pspdisp-min: hardware MJPEG decode.
*/
#include <pspkernel.h>
#include <pspjpeg.h>
#include <psputility.h>
#include "jpeg.h"
#include "shared.h"

static unsigned char __attribute__((aligned(16))) l_pixels[480 * 272 * 4];
static bool l_available = false;
static bool l_rotated = false;   /* current decoder geometry: portrait? */

bool jpegInit(void)
{
  if (sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC) < 0)
    return false;
  if (sceJpegInitMJpeg() != 0)
    return false;
  if (sceJpegCreateMJpeg(480, 272) != 0)
    return false;

  l_rotated = false;
  l_available = true;
  return true;
}

void jpegTerm(void)
{
  if (!l_available)
    return;
  sceJpegDeleteMJpeg();
  sceJpegFinishMJpeg();
  sceUtilityUnloadModule(PSP_MODULE_AV_AVCODEC);
  l_available = false;
}

unsigned int *jpegDecode(unsigned char *data, int size, unsigned int rotation)
{
  if (!l_available)
    return NULL;

  bool rotated = (rotation == COM_FLAGS_IMAGE_IS_ROTATED_90_DEG) ||
                 (rotation == COM_FLAGS_IMAGE_IS_ROTATED_270_DEG);

  /* The MJPEG decoder is created for a fixed geometry; portrait frames need a
     272x480 decoder. Only recreate when the orientation actually flips. */
  if (rotated != l_rotated)
  {
    sceJpegDeleteMJpeg();
    if (rotated)
      sceJpegCreateMJpeg(272, 480);
    else
      sceJpegCreateMJpeg(480, 272);
    l_rotated = rotated;
  }

  if (sceJpegDecodeMJpeg(data, size, l_pixels, 0) < 0)
    return NULL;

  /* Flush so the GU can texture the freshly decoded buffer. */
  sceKernelDcacheWritebackInvalidateAll();
  return (unsigned int *)l_pixels;
}
