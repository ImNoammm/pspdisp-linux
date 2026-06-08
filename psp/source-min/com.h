/*
  pspdisp-min — frame transport loop (subset of the original com.c).

  Speaks the unchanged PSPdisp wire protocol (shared.h) over the USBHostFS bulk
  channel: read frame header + JPEG, reply with buttons/analog, decode, blit.
*/
#ifndef COM_H
#define COM_H

#include "common.h"

#define COM_IMAGE_BUFFER_SIZE (400 * 1024)   /* must match the host's cap */

/* comRun result codes. */
#define COM_LINK_LOST 0    /* was connected, host went away                  */
#define COM_TIMEOUT   1    /* no host appeared within the connect window      */
#define COM_QUIT      2    /* user chose Quit in the menu                     */

/* Run the receive/draw loop. firstForce asks the host (via FORCE_UPDATE) for an
   immediate full frame. If no valid frame arrives within connectTimeoutUs of
   starting (before the first frame), returns COM_TIMEOUT; once connected the
   timeout no longer applies. */
int comRun(bool firstForce, int connectTimeoutUs);

#endif
