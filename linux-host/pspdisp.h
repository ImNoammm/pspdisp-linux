/* PSPdisp Linux host — shared interfaces between modules. */
#ifndef PSPDISP_H
#define PSPDISP_H

#include <stdbool.h>
#include <stdint.h>
#include "proto.h"

/* ---- runtime options (defined in main.c) ------------------------------- */
typedef enum { TRANSPORT_USB, TRANSPORT_TCP } transport_kind;
typedef enum { CAPTURE_WLR, CAPTURE_X11, CAPTURE_PORTAL } capture_kind;

typedef struct {
  transport_kind transport;
  capture_kind   capture;
  int   x, y, w, h;          /* X11 region (w/h also = evdi virtual mode)  */
  int   rotation;            /* 0/90/180/270                                */
  int   quality;             /* JPEG 1..100                                 */
  int   fps;
  bool  input;               /* uinput gamepad                              */
  bool  audio;               /* PulseAudio capture (best-effort, untested)  */
  bool  verbose;
  int   tcp_port;            /* NET_PORT + offset                           */
  char  password[NET_PASSWORD_LEN];
  bool  no_display;          /* don't auto-create a virtual output (mirror) */
  int   disp_w, disp_h;      /* virtual output size (downscaled to PSP)     */
} options;

extern options g_opt;
#define VLOG(...) do { if (g_opt.verbose) fprintf(stderr, __VA_ARGS__); } while (0)

/* ---- auto virtual display (display_sway.c) ----------------------------- */
bool        display_sway_available(void);
const char *display_auto_create(int w, int h);   /* NULL if unavailable     */
void        display_auto_destroy(void);

/* ---- transport backend (transport_usb.c / transport_tcp.c) ------------- */
typedef struct {
  const char *name;
  bool (*open)(void);                       /* connect/accept + handshake   */
  int  (*write)(const void *buf, int len);  /* 0 ok, <0 error               */
  int  (*read)(void *buf, int len, unsigned timeout_ms);
  void (*close)(void);
  bool full_response;                        /* true => read 90B, else 14B  */
} transport_backend;

transport_backend *transport_usb(void);
transport_backend *transport_tcp(void);

/* ---- capture backend (capture_x11.c / capture_evdi.c) ------------------ */
typedef struct {
  const char *name;
  int width, height;                         /* native capture dimensions   */
  bool (*init)(void);
  bool (*grab)(void);                        /* refresh internal frame       */
  /* read one pixel of the captured frame as RGB */
  void (*pixel)(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b);
  void (*term)(void);
} capture_backend;

capture_backend *capture_x11(void);
capture_backend *capture_wlr(void);
extern const char *g_wlr_output_name;   /* -o <output>, NULL = first output */
#ifdef HAVE_PORTAL
capture_backend *capture_portal(void);  /* KDE/GNOME via PipeWire portal */
#endif

/* ---- frame encode (frame.c) -------------------------------------------- */
/* Encode the current capture (via cap->pixel) to a malloc'd JPEG buffer.
   Returns NULL on error; sets *size and the rotation/format *flags. */
unsigned char *frame_encode(capture_backend *cap, unsigned long *size, uint32_t *flags);
/* Cheap content hash for skip-unchanged-frame detection. */
uint64_t frame_hash(capture_backend *cap);

/* ---- input injection (input.c) ----------------------------------------- */
bool input_init(void);
void input_update(uint32_t buttons, uint8_t analogX, uint8_t analogY);
void input_term(void);

/* ---- audio capture (audio_pulse.c) ------------------------------------- */
bool audio_init(void);
/* Fill up to one PSP audio frame; returns bytes produced (0 if none ready). */
int  audio_read_frame(uint8_t *dst, int max, uint32_t *audio_flags);
void audio_term(void);

#endif
