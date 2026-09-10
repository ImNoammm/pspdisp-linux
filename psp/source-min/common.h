/*
  pspdisp-min: common types.

  Minimal USB-only rewrite of the PSPdisp homebrew. Same wire protocol as the
  original (see shared.h), so it talks to the unchanged linux host.
*/
#ifndef COMMON_H
#define COMMON_H

#include <pspsdk.h>
#include <psptypes.h>

#ifndef __cplusplus
#define true (1)
#define false (0)
typedef int bool;
#endif

#endif
