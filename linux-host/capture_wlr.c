/* Wayland capture for wlroots compositors (sway, Hyprland, river, ...) via
   wlr-screencopy-unstable-v1. Captures a whole output into a wl_shm buffer.

   For a real second screen on sway:
     swaymsg create_output                       # makes HEADLESS-1
     swaymsg output HEADLESS-1 mode 480x272 pos 1920 0
   then run with  -o HEADLESS-1  and drag windows onto it. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "pspdisp.h"
#include "wlr-screencopy-client-protocol.h"

static struct wl_display  *dpy;
static struct wl_registry *reg;
static struct wl_shm      *shm;
static struct zwlr_screencopy_manager_v1 *scm;

#define MAX_OUTPUTS 16
static struct { struct wl_output *out; char name[64]; } outs[MAX_OUTPUTS];
static int n_outs;
static struct wl_output *target;          /* output we capture                */
static capture_backend backend;           /* forward decl; defined at bottom  */

/* shm buffer that the compositor copies frames into */
static struct wl_buffer *buf;
static void   *buf_data;
static size_t  buf_size;
static uint32_t f_format, f_width, f_height, f_stride;
static bool f_ready, f_failed, f_have_info, f_yinvert;
static int f_bpp = 4, f_ro = 2, f_go = 1, f_bo = 0;   /* set from f_format */

/* DRM/wl_shm fourcc codes (wl_shm uses fourcc for everything but A/XRGB8888) */
#define FCC(a,b,c,d) ((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))

/* Decode bytes-per-pixel + R/G/B byte offsets for the captured format. */
static void decode_format(uint32_t fmt)
{
  switch (fmt) {
    case WL_SHM_FORMAT_XRGB8888: case WL_SHM_FORMAT_ARGB8888:        /* B G R [A] */
      f_bpp = 4; f_bo = 0; f_go = 1; f_ro = 2; break;
    case WL_SHM_FORMAT_XBGR8888: case WL_SHM_FORMAT_ABGR8888:        /* R G B [A] */
      f_bpp = 4; f_ro = 0; f_go = 1; f_bo = 2; break;
    case FCC('B','G','2','4'):   /* DRM_FORMAT_BGR888  -> memory R G B */
      f_bpp = 3; f_ro = 0; f_go = 1; f_bo = 2; break;
    case FCC('R','G','2','4'):   /* DRM_FORMAT_RGB888  -> memory B G R */
      f_bpp = 3; f_bo = 0; f_go = 1; f_ro = 2; break;
    default:                     /* assume 32-bit XRGB */
      f_bpp = 4; f_bo = 0; f_go = 1; f_ro = 2; break;
  }
}

/* output name plumbing (wl_output v4) */
static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
  int32_t pw, int32_t ph, int32_t sp, const char *mk, const char *md, int32_t tr)
{ (void)d;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)sp;(void)mk;(void)md;(void)tr; }
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r)
{ (void)d;(void)o;(void)f;(void)w;(void)h;(void)r; }
static void out_done(void *d, struct wl_output *o) { (void)d;(void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t s) { (void)d;(void)o;(void)s; }
static void out_name(void *d, struct wl_output *o, const char *name)
{ int i = (int)(intptr_t)d; if (i < MAX_OUTPUTS) strncpy(outs[i].name, name, sizeof(outs[i].name)-1); (void)o; }
static void out_desc(void *d, struct wl_output *o, const char *desc) { (void)d;(void)o;(void)desc; }
static const struct wl_output_listener out_listener = {
  out_geometry, out_mode, out_done, out_scale, out_name, out_desc
};

/* registry */
static void reg_global(void *d, struct wl_registry *r, uint32_t id,
                       const char *iface, uint32_t ver)
{
  (void)d;
  if (!strcmp(iface, wl_shm_interface.name)) {
    shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
  } else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name)) {
    scm = wl_registry_bind(r, id, &zwlr_screencopy_manager_v1_interface, ver < 3 ? ver : 3);
  } else if (!strcmp(iface, wl_output_interface.name) && n_outs < MAX_OUTPUTS) {
    uint32_t bind_ver = ver < 4 ? ver : 4;
    outs[n_outs].out = wl_registry_bind(r, id, &wl_output_interface, bind_ver);
    if (bind_ver >= 4)
      wl_output_add_listener(outs[n_outs].out, &out_listener, (void *)(intptr_t)n_outs);
    n_outs++;
  }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t id) { (void)d;(void)r;(void)id; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* shm pool buffer */
static bool make_buffer(uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride)
{
  if (buf) { wl_buffer_destroy(buf); buf = NULL; }
  if (buf_data) { munmap(buf_data, buf_size); buf_data = NULL; }

  buf_size = (size_t)stride * h;
  char name[] = "/pspdisp-XXXXXX";
  int fd = -1;
  for (int i = 0; i < 100; i++) {
    name[8 + (i % 6)] = 'a' + (rand() % 26);
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) { shm_unlink(name); break; }
  }
  if (fd < 0) return false;
  if (ftruncate(fd, buf_size) != 0) { close(fd); return false; }
  buf_data = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (buf_data == MAP_FAILED) { buf_data = NULL; close(fd); return false; }

  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, buf_size);
  buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, fmt);
  wl_shm_pool_destroy(pool);
  close(fd);
  return buf != NULL;
}

/* frame events */
static void fr_buffer(void *d, struct zwlr_screencopy_frame_v1 *f,
                      uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride)
{
  (void)d;(void)f;
  f_format = fmt; f_width = w; f_height = h; f_stride = stride; f_have_info = true;
  decode_format(fmt);
}
static void fr_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags)
{ (void)d;(void)f; f_yinvert = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0; }
static void fr_ready(void *d, struct zwlr_screencopy_frame_v1 *f,
                     uint32_t sh, uint32_t sl, uint32_t ns)
{ (void)d;(void)f;(void)sh;(void)sl;(void)ns; f_ready = true; }
static void fr_failed(void *d, struct zwlr_screencopy_frame_v1 *f) { (void)d;(void)f; f_failed = true; }
static void fr_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)d;(void)f;(void)x;(void)y;(void)w;(void)h; }
static void fr_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h)
{ (void)d;(void)f;(void)fmt;(void)w;(void)h; }
static void fr_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) { (void)d;(void)f; }
static const struct zwlr_screencopy_frame_v1_listener fr_listener = {
  fr_buffer, fr_flags, fr_ready, fr_failed, fr_damage, fr_dmabuf, fr_buffer_done
};

/* backend */
extern const char *g_wlr_output_name;     /* set from main via option        */

static bool w_init(void)
{
  dpy = wl_display_connect(NULL);
  if (!dpy) { fprintf(stderr, "wlr: cannot connect to Wayland display\n"); return false; }
  reg = wl_display_get_registry(dpy);
  wl_registry_add_listener(reg, &reg_listener, NULL);
  wl_display_roundtrip(dpy);   /* globals */
  wl_display_roundtrip(dpy);   /* output names */

  if (!shm || !scm) { fprintf(stderr, "wlr: compositor lacks wl_shm or wlr-screencopy\n"); return false; }
  if (n_outs == 0)  { fprintf(stderr, "wlr: no outputs found\n"); return false; }

  target = outs[0].out;
  if (g_wlr_output_name) {
    target = NULL;
    for (int i = 0; i < n_outs; i++)
      if (!strcmp(outs[i].name, g_wlr_output_name)) { target = outs[i].out; break; }
    if (!target) {
      fprintf(stderr, "wlr: output '%s' not found. Available:\n", g_wlr_output_name);
      for (int i = 0; i < n_outs; i++) fprintf(stderr, "  %s\n", outs[i].name);
      return false;
    }
  }
  VLOG("wlr: capturing output %s\n", g_wlr_output_name ? g_wlr_output_name : outs[0].name);
  return true;
}

static bool w_grab(void)
{
  f_ready = f_failed = f_have_info = false;

  struct zwlr_screencopy_frame_v1 *frame =
    zwlr_screencopy_manager_v1_capture_output(scm, 0, target);
  zwlr_screencopy_frame_v1_add_listener(frame, &fr_listener, NULL);

  /* wait for the buffer info (+ buffer_done on v3) */
  while (!f_have_info && !f_failed)
    if (wl_display_dispatch(dpy) < 0) { zwlr_screencopy_frame_v1_destroy(frame); return false; }
  if (f_failed) { zwlr_screencopy_frame_v1_destroy(frame); return false; }

  /* (re)allocate the shm buffer if the output size changed */
  if (!buf || (uint32_t)backend.width != f_width || (uint32_t)backend.height != f_height) {
    if (!make_buffer(f_format, f_width, f_height, f_stride)) {
      zwlr_screencopy_frame_v1_destroy(frame); return false;
    }
    backend.width = f_width;
    backend.height = f_height;
  }

  zwlr_screencopy_frame_v1_copy(frame, buf);
  while (!f_ready && !f_failed)
    if (wl_display_dispatch(dpy) < 0) { zwlr_screencopy_frame_v1_destroy(frame); return false; }

  zwlr_screencopy_frame_v1_destroy(frame);
  return f_ready;
}

/* decode one captured pixel as RGB, honoring format bpp/order + y-invert */
static void w_pixel(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b)
{
  int yy = f_yinvert ? (int)f_height - 1 - y : y;
  uint8_t *p = (uint8_t *)buf_data + (size_t)yy * f_stride + (size_t)x * f_bpp;
  *r = p[f_ro]; *g = p[f_go]; *b = p[f_bo];
}

static void w_term(void)
{
  if (buf) wl_buffer_destroy(buf);
  if (buf_data) munmap(buf_data, buf_size);
  if (dpy) wl_display_disconnect(dpy);
}

/* backend.width/height are set after first grab (output size). */
static capture_backend backend = {
  .name = "wlr", .init = w_init, .grab = w_grab, .pixel = w_pixel, .term = w_term,
};
const char *g_wlr_output_name;

capture_backend *capture_wlr(void)
{
  backend.width = g_opt.w; backend.height = g_opt.h;
  return &backend;
}
