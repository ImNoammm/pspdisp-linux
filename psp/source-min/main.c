/*
  pspdisp-min — minimal USB-only PSPdisp homebrew.

  Boots straight into USB display: max clock, GU + hardware JPEG up, load the
  USBHostFS bulk driver, then run the frame loop and auto-reconnect when the
  cable drops. No on-PSP menu / WLAN / OSK / audio — the linux host drives
  resolution, fps and quality. Wire-compatible with the unchanged host.
*/
#include <pspkernel.h>
#include <pspctrl.h>
#include <psppower.h>
#include <pspimpose_driver.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include "common.h"
#include "display.h"
#include "jpeg.h"
#include "usb.h"
#include "com.h"

PSP_MODULE_INFO("pspdisp", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(2048);

static volatile bool l_exit = false;
volatile int g_quit = 0;          /* set by the menu Quit / prompt quit combo */

/* Centered message on a black screen (pspDebugScreen is ~60 cols). l2 optional
   (dimmer, drawn below) — pass NULL for a single line. */
static void screenMsg2(const char *l1, const char *l2)
{
  int n1 = 0; while (l1[n1]) n1++;
  int c1 = (60 - n1) / 2; if (c1 < 0) c1 = 0;
  displayClear(0xFF000000);
  pspDebugScreenSetBase((u32 *)displayDrawAddr());
  pspDebugScreenSetTextColor(0xFFFFFFFF);
  pspDebugScreenSetXY(c1, 15);
  pspDebugScreenPrintf("%s", l1);
  if (l2)
  {
    int n2 = 0; while (l2[n2]) n2++;
    int c2 = (60 - n2) / 2; if (c2 < 0) c2 = 0;
    pspDebugScreenSetTextColor(0xFF888888);
    pspDebugScreenSetXY(c2, 17);
    pspDebugScreenPrintf("%s", l2);
  }
  displaySwap();
}

static void screenMsg(const char *msg) { screenMsg2(msg, NULL); }

/* Wait for X. Returns true on X; false if the user asked to quit (double HOME). */
static bool waitForCross(void)
{
  unsigned int prev = 0;
  u64 lastHome = 0;
  for (;;)
  {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    unsigned int b = pad.Buttons;
    unsigned int pressed = b & ~prev;
    if (pressed & PSP_CTRL_HOME)
    {
      u64 now = sceKernelGetSystemTimeWide();
      if (lastHome && (now - lastHome) <= 500000) { g_quit = 1; return false; }
      lastHome = now;
    }
    if (pressed & PSP_CTRL_CROSS) return true;
    prev = b;
    scePowerTick(0);
    sceKernelDelayThread(30 * 1000);
  }
}

static int exit_callback(int arg1, int arg2, void *common)
{
  (void)arg1; (void)arg2; (void)common;
  l_exit = true;
  return 0;
}

static int callback_thread(SceSize args, void *argp)
{
  (void)args; (void)argp;
  int cbid = sceKernelCreateCallback("exit", exit_callback, NULL);
  sceKernelRegisterExitCallback(cbid);
  sceKernelSleepThreadCB();
  return 0;
}

static void setup_callbacks(void)
{
  SceUID thid = sceKernelCreateThread("cb", callback_thread, 0x11, 0xFA0, PSP_THREAD_ATTR_USER, 0);
  if (thid >= 0)
    sceKernelStartThread(thid, 0, 0);
}

int main(int argc, char *argv[])
{
  (void)argc; (void)argv;

  setup_callbacks();
  sceImposeSetHomePopup(0);

  /* Max it out: 333 MHz CPU / 166 MHz bus for the fastest decode + USB and the
     lowest latency. USB supplies power, so there's no battery reason not to. */
  scePowerSetClockFrequency(333, 333, 166);

  sceCtrlSetSamplingCycle(0);
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

  displayInit();
  /* Debug overlay uses pspDebugScreen; setup=0 so it does NOT take over the
     display (GU owns it) — we just point its text base at our draw buffer. */
  pspDebugScreenInitEx((void *)displayDrawAddr(), PSP_DISPLAY_PIXEL_FORMAT_8888, 0);
  displayClear(0xFF000000);
  displaySwap();

  if (!jpegInit())
  {
    /* No hardware JPEG -> nothing can render. Flag it red and idle. */
    while (!l_exit)
    {
      displayClear(0xFF0000A0);
      displaySwap();
      sceKernelDelayThread(500 * 1000);
    }
    sceKernelExitGame();
    return 0;
  }

  /* USB driver stays active for the whole run; the host connects to it whenever
     it's reachable. */
  if (usbInit() != 0)
  {
    while (!l_exit && !g_quit)
    {
      screenMsg("USB driver failed to start");
      sceKernelDelayThread(500 * 1000);
    }
  }

  while (!l_exit && !g_quit)
  {
    /* Prompt -> wait for X -> try to connect for 10s -> report -> repeat. */
    screenMsg2("Connect a USB device and press X",
               "Double-press PS button to quit");
    if (!waitForCross())
      break;                                 /* HOME+VOL- = quit */

    screenMsg("Connecting...");
    int r = comRun(true, 10 * 1000 * 1000);  /* 10s connect window */

    if (r == COM_QUIT)
      break;                                 /* menu Quit */
    if (r == COM_TIMEOUT)
    {
      screenMsg("USB connection was not found");
      sceKernelDelayThread(2 * 1000 * 1000);
    }
    /* COM_LINK_LOST (host disconnected) falls through to the prompt again. */
  }

  usbTerm();
  jpegTerm();
  sceImposeSetHomePopup(1);
  sceKernelExitGame();
  return 0;
}
