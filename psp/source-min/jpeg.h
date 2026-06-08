/*
  pspdisp-min — hardware MJPEG decode (subset of the original compress.c).

  Uses the PSP Media Engine JPEG decoder (sceJpeg*). The host only ever sends
  baseline JPEG, so the libjpeg/PNG software fallbacks of the original are gone.
*/
#ifndef JPEG_H
#define JPEG_H

#include "common.h"

/* Load the AV codec module and create the MJPEG decoder. Returns true on
   success; false means hardware JPEG is unavailable (nothing will render). */
bool jpegInit(void);

void jpegTerm(void);

/* Decode a JPEG frame into the internal RGBA pixel buffer and return it.
   `rotation` is the COM_FLAGS_IMAGE_ROTATION_MASK bits (portrait frames are
   272x480). Returns NULL on decode error. */
unsigned int *jpegDecode(unsigned char *data, int size, unsigned int rotation);

#endif
