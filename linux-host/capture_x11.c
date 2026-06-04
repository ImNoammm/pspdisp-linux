/* X11 region capture via MIT-SHM (mirror a rectangle of the desktop). */
#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/shm.h>
#include "pspdisp.h"

static Display *dpy;
static Window   root;
static XShmSegmentInfo shm;
static XImage  *img;

static bool x_init(void)
{
  dpy = XOpenDisplay(NULL);
  if (!dpy) { fprintf(stderr, "x11: cannot open display (Wayland? use Xwayland)\n"); return false; }
  root = DefaultRootWindow(dpy);
  if (!XShmQueryExtension(dpy)) { fprintf(stderr, "x11: no MIT-SHM\n"); return false; }

  int scr = DefaultScreen(dpy);
  img = XShmCreateImage(dpy, DefaultVisual(dpy, scr), DefaultDepth(dpy, scr),
                        ZPixmap, NULL, &shm, g_opt.w, g_opt.h);
  if (!img) { fprintf(stderr, "x11: XShmCreateImage failed\n"); return false; }

  shm.shmid = shmget(IPC_PRIVATE, (size_t)img->bytes_per_line * img->height, IPC_CREAT | 0600);
  if (shm.shmid < 0) { fprintf(stderr, "x11: shmget failed\n"); return false; }
  shm.shmaddr = img->data = shmat(shm.shmid, NULL, 0);
  shm.readOnly = False;
  if (!XShmAttach(dpy, &shm)) { fprintf(stderr, "x11: XShmAttach failed\n"); return false; }
  XSync(dpy, False);
  shmctl(shm.shmid, IPC_RMID, NULL);
  return true;
}

static bool x_grab(void)
{
  return XShmGetImage(dpy, root, img, g_opt.x, g_opt.y, AllPlanes);
}

static void x_pixel(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b)
{
  if (img->bits_per_pixel == 32 && img->byte_order == LSBFirst) {
    uint8_t *p = (uint8_t *)img->data + y * img->bytes_per_line + x * 4;
    *b = p[0]; *g = p[1]; *r = p[2];
  } else {
    unsigned long pix = XGetPixel(img, x, y);
    *r = (pix & img->red_mask)   >> 16;
    *g = (pix & img->green_mask) >> 8;
    *b = (pix & img->blue_mask);
  }
}

static void x_term(void)
{
  if (img) { XShmDetach(dpy, &shm); if (shm.shmaddr) shmdt(shm.shmaddr); XDestroyImage(img); img = NULL; }
  if (dpy) { XCloseDisplay(dpy); dpy = NULL; }
}

static capture_backend backend = {
  .name = "x11", .init = x_init, .grab = x_grab, .pixel = x_pixel, .term = x_term,
};
capture_backend *capture_x11(void)
{
  backend.width = g_opt.w; backend.height = g_opt.h;
  return &backend;
}
