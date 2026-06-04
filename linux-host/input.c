/* PSP buttons/analog -> Linux uinput virtual gamepad. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include "pspdisp.h"

static int fd = -1;

static const struct { uint32_t psp; int key; } map[] = {
  { PSP_CROSS, BTN_SOUTH }, { PSP_CIRCLE, BTN_EAST },
  { PSP_SQUARE, BTN_WEST }, { PSP_TRIANGLE, BTN_NORTH },
  { PSP_LTRIG, BTN_TL }, { PSP_RTRIG, BTN_TR },
  { PSP_START, BTN_START }, { PSP_SELECT, BTN_SELECT },
  { PSP_UP, BTN_DPAD_UP }, { PSP_DOWN, BTN_DPAD_DOWN },
  { PSP_LEFT, BTN_DPAD_LEFT }, { PSP_RIGHT, BTN_DPAD_RIGHT },
};
#define NMAP (int)(sizeof(map)/sizeof(map[0]))

bool input_init(void)
{
  fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) { fprintf(stderr, "input: open /dev/uinput failed (udev rule / root)\n"); return false; }

  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  for (int i = 0; i < NMAP; i++) ioctl(fd, UI_SET_KEYBIT, map[i].key);
  ioctl(fd, UI_SET_ABSBIT, ABS_X);
  ioctl(fd, UI_SET_ABSBIT, ABS_Y);

  struct uinput_setup us; memset(&us, 0, sizeof us);
  us.id.bustype = BUS_USB; us.id.vendor = 0x054C; us.id.product = 0x01C9;
  strcpy(us.name, "PSPdisp Gamepad");
  ioctl(fd, UI_DEV_SETUP, &us);

  struct uinput_abs_setup abs; memset(&abs, 0, sizeof abs);
  abs.absinfo.minimum = 0; abs.absinfo.maximum = 255; abs.absinfo.flat = 16;
  abs.code = ABS_X; ioctl(fd, UI_ABS_SETUP, &abs);
  abs.code = ABS_Y; ioctl(fd, UI_ABS_SETUP, &abs);

  ioctl(fd, UI_DEV_CREATE);
  return true;
}

static void emit(int type, int code, int val)
{
  struct input_event ev; memset(&ev, 0, sizeof ev);
  ev.type = type; ev.code = code; ev.value = val;
  if (write(fd, &ev, sizeof ev) < 0) { /* ignore */ }
}

void input_update(uint32_t buttons, uint8_t ax, uint8_t ay)
{
  static uint32_t last;
  if (fd < 0) return;
  for (int i = 0; i < NMAP; i++)
    if ((buttons & map[i].psp) != (last & map[i].psp))
      emit(EV_KEY, map[i].key, (buttons & map[i].psp) ? 1 : 0);
  last = buttons;
  emit(EV_ABS, ABS_X, ax);
  emit(EV_ABS, ABS_Y, ay);
  emit(EV_SYN, SYN_REPORT, 0);
}

void input_term(void)
{
  if (fd >= 0) { ioctl(fd, UI_DEV_DESTROY); close(fd); fd = -1; }
}
