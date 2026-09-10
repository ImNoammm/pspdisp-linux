/*
  pspdisp-min: GU display (subset of the original graphic.c).

  Hardware-accelerated blit of a decoded 480x272 RGBA frame to the screen via
  sceGu, with optional 90/180/270 rotation. Double-buffered.
*/
#ifndef DISPLAY_H
#define DISPLAY_H

#include "common.h"

/* Set up sceGu, the draw/display buffers and texture state. Call once at boot. */
void displayInit(void);

/* Fill the back buffer with a solid 0xAABBGGRR color (does not swap). */
void displayClear(unsigned int color);

/* Upload the decoded frame as a texture and blit it (rotation = the
   COM_FLAGS_IMAGE_ROTATION_MASK bits from the frame header). Does not swap. */
void displayDrawFrame(unsigned int *pixels, unsigned int rotation);

/* Present the back buffer. */
void displaySwap(void);

/* Absolute (uncached) address of the buffer currently being drawn to. Used by
   the debug overlay to write text straight into the frame before it is shown. */
unsigned int displayDrawAddr(void);

#endif
