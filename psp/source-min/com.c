/*
  pspdisp-min: frame transport loop.
*/
#include <pspkernel.h>
#include <pspctrl.h>
#include <psppower.h>
#include <pspdebug.h>
#include <string.h>
#include "com.h"
#include "shared.h"
#include "jpeg.h"
#include "display.h"
#include "usbhostfs/usbasync.h"

extern volatile int g_quit;      /* set here when "Quit" is chosen in the menu */

/* 64-byte aligned so usbWriteRawBulkData (which wants 64-bit-aligned data) and
   the bulk DMA are happy. */
static unsigned char  __attribute__((aligned(64))) l_recv[COM_IMAGE_BUFFER_SIZE];
static comSettingsResponse __attribute__((aligned(64))) l_response;

/* ---- overlay / menu state -------------------------------------------------*/
static bool         l_menu  = false;     /* menu open?  toggled by HOME+VOL+  */
static bool         l_debug = false;     /* debug overlay shown?              */
static int          l_sel   = 0;         /* menu cursor: 0=Debug 1=Quit       */
static unsigned int l_prevBtn = 0;       /* for edge detection                */
#define MENU_ITEMS 2

/* ---- debug stats ----------------------------------------------------------*/
static unsigned int l_lastBytes = 0;
static unsigned int l_fps = 0, l_fpsCount = 0;
static u64          l_fpsT0 = 0;

/* One bulk read; back off briefly on error so a dropped link doesn't spin. */
static int readData(void *buf, int len)
{
  int r = usbReadRawBulkData(buf, len);
  if (r < 0)
    sceKernelDelayThread(200 * 1000);
  return r;
}

/* Read exactly len bytes (bulk reads can be short). false on link error. */
static bool readExact(void *buf, int len)
{
  unsigned char *p = buf;
  int got = 0;
  while (got < len)
  {
    int r = readData(p + got, len - got);
    if (r < 0)
      return false;
    got += r;
  }
  return true;
}

/* Read the 16-byte frame header and validate its magic. */
static bool readHeader(comFrameHeader *h)
{
  memset(h, 0, sizeof *h);
  if (readData(h, sizeof *h) != (int)sizeof *h)
    return false;
  return h->magic == COM_HEADER_MAGIC;
}

/* Send the 90-byte USB response: 14B header (buttons + analog) + 76B settings.
   The host reads all 90 but only uses buttons/analog; settings stay zeroed. */
static bool sendResponse(unsigned int buttons, unsigned char ax, unsigned char ay, bool force)
{
  l_response.response.magic   = COM_HEADER_MAGIC;
  l_response.response.flags   = force ? COM_FLAGS_FORCE_UPDATE : 0;
  l_response.response.buttons = buttons;
  l_response.response.analogX = ax;
  l_response.response.analogY = ay;

  sceKernelDcacheWritebackInvalidateAll();
  return usbWriteRawBulkData(&l_response, sizeof l_response) == (int)sizeof l_response;
}

/* Double-press HOME (PS) toggles the menu (the PSP's VOL buttons don't register
   in user mode, so a chord isn't usable). While open: D-pad moves the cursor,
   X selects, O closes. Edge-triggered off the previous button state. */
#define DOUBLE_TAP_US 500000
static u64 l_lastHome = 0;

static void handleInput(unsigned int btn)
{
  unsigned int pressed = btn & ~l_prevBtn;        /* newly-pressed this frame */

  if (pressed & PSP_CTRL_HOME)
  {
    u64 now = sceKernelGetSystemTimeWide();
    if (l_lastHome && (now - l_lastHome) <= DOUBLE_TAP_US)
    {
      l_menu = !l_menu;
      l_lastHome = 0;                              /* consume the pair */
    }
    else
      l_lastHome = now;
  }

  if (l_menu)
  {
    if (pressed & PSP_CTRL_UP)     l_sel = (l_sel + MENU_ITEMS - 1) % MENU_ITEMS;
    if (pressed & PSP_CTRL_DOWN)   l_sel = (l_sel + 1) % MENU_ITEMS;
    if (pressed & PSP_CTRL_CIRCLE) l_menu = false;          /* O = close */
    if (pressed & PSP_CTRL_CROSS)                            /* X = select */
    {
      if (l_sel == 0) { l_debug = !l_debug; l_menu = false; }
      else            { g_quit = 1; }
    }
  }

  l_prevBtn = btn;
}

/* Draw text straight into the just-rendered frame (before swap). */
static void drawDebug(const comFrameHeader *h, const SceCtrlData *pad)
{
  unsigned int rot = (h->flags & COM_FLAGS_IMAGE_ROTATION_MASK);
  int cpu = scePowerGetCpuClockFrequencyInt();
  int bus = scePowerGetBusClockFrequencyInt();

  pspDebugScreenSetBase((u32 *)displayDrawAddr());
  pspDebugScreenSetTextColor(0xFF00FF00);
  pspDebugScreenSetXY(0, 0);  pspDebugScreenPrintf("pspdisp-min DEBUG");
  pspDebugScreenSetXY(0, 2);  pspDebugScreenPrintf("link  : USB connected");
  pspDebugScreenSetXY(0, 3);  pspDebugScreenPrintf("frame : %u B  rot %lu",
                                  l_lastBytes, (unsigned long)(rot >> 8));
  pspDebugScreenSetXY(0, 4);  pspDebugScreenPrintf("fps   : %-3u", l_fps);
  pspDebugScreenSetXY(0, 5);  pspDebugScreenPrintf("clock : %d/%d MHz", cpu, bus);
  pspDebugScreenSetXY(0, 6);  pspDebugScreenPrintf("btn   : 0x%08lX",
                                  (unsigned long)pad->Buttons);
  pspDebugScreenSetXY(0, 7);  pspDebugScreenPrintf("analog: %3u,%3u", pad->Lx, pad->Ly);
}

/* Draw the popup menu. */
static void drawMenu(void)
{
  pspDebugScreenSetBase((u32 *)displayDrawAddr());
  pspDebugScreenSetTextColor(0xFFFFFFFF);
  pspDebugScreenSetXY(18, 9);  pspDebugScreenPrintf("== pspdisp-min ==");
  pspDebugScreenSetTextColor(l_sel == 0 ? 0xFF00FF00 : 0xFFAAAAAA);
  pspDebugScreenSetXY(18, 11); pspDebugScreenPrintf("%s Debug overlay: %s",
                                  l_sel == 0 ? ">" : " ", l_debug ? "ON " : "OFF");
  pspDebugScreenSetTextColor(l_sel == 1 ? 0xFF00FF00 : 0xFFAAAAAA);
  pspDebugScreenSetXY(18, 12); pspDebugScreenPrintf("%s Quit app",
                                  l_sel == 1 ? ">" : " ");
  pspDebugScreenSetTextColor(0xFF888888);
  pspDebugScreenSetXY(18, 14); pspDebugScreenPrintf("UP/DOWN  X=select  O=close");
}

int comRun(bool firstForce, int connectTimeoutUs)
{
  comFrameHeader h;
  bool first = firstForce;
  bool connected = false;
  SceCtrlData pad;

  memset(&l_response, 0, sizeof l_response);
  l_fpsT0 = sceKernelGetSystemTimeWide();
  l_fpsCount = 0;
  u64 connStart = l_fpsT0;

  for (;;)
  {
    if (!readHeader(&h))
    {
      /* Nothing yet: give up if the host never showed up in the window. */
      if (!connected &&
          (sceKernelGetSystemTimeWide() - connStart) >= (u64)connectTimeoutUs)
        return COM_TIMEOUT;
      sceKernelDelayThread(200 * 1000);
      continue;                              /* desync / waiting for host */
    }
    connected = true;

    /* Payload = JPEG image, plus (if enabled) an audio chunk and a settings
       block. We decode only the image; audio/settings are drained to stay in
       sync. Audio chunk size comes from the flags, as in the original. */
    unsigned int audioBytes = 0;
    if (h.flags & COM_FLAGS_CONTAINS_AUDIO_DATA)
      audioBytes = (h.flags & COM_FLAGS_AUDIO_CHUNK_2240) ? (2240 * 2) : (2688 * 2);

    unsigned int total = h.imageSize + audioBytes + h.settingsSize;
    if (total > COM_IMAGE_BUFFER_SIZE)
      return COM_LINK_LOST;                  /* malformed: bail, reconnect */

    if (!readExact(l_recv, total))
      return COM_LINK_LOST;

    /* Controls + menu. While the menu is open we (a) FORCE_UPDATE so the host
       keeps streaming frames -> the menu stays responsive, and (b) send neutral
       buttons so menu navigation doesn't leak to the host as gamepad input. */
    sceCtrlPeekBufferPositive(&pad, 1);
    handleInput(pad.Buttons);
    if (g_quit)
      return COM_QUIT;

    bool force = first || l_menu;
    first = false;
    if (l_menu)
    {
      if (!sendResponse(0, 128, 128, force)) return COM_LINK_LOST;
    }
    else
    {
      if (!sendResponse(pad.Buttons, pad.Lx, pad.Ly, force)) return COM_LINK_LOST;
    }

    unsigned int rot = h.flags & COM_FLAGS_IMAGE_ROTATION_MASK;
    bool drew = false;
    if (h.flags & COM_FLAGS_IMAGE_CLEAR_SCREEN)
    {
      displayClear(0xFF000000);
      drew = true;
    }
    else if (h.flags & COM_FLAGS_CONTAINS_IMAGE_DATA)
    {
      l_lastBytes = h.imageSize;
      unsigned int *px = jpegDecode(l_recv, h.imageSize, rot);
      if (px)
      {
        displayDrawFrame(px, rot);
        drew = true;
      }
    }

    if (drew)
    {
      u64 now = sceKernelGetSystemTimeWide();
      l_fpsCount++;
      if (now - l_fpsT0 >= 1000000)
      {
        l_fps = l_fpsCount;
        l_fpsCount = 0;
        l_fpsT0 = now;
      }
      if (l_debug)
        drawDebug(&h, &pad);
      if (l_menu)
        drawMenu();
      displaySwap();
    }

    scePowerTick(0);                         /* keep the PSP awake */
  }
}
