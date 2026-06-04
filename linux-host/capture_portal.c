/* PipeWire / xdg-desktop-portal ScreenCast capture backend.

   For Wayland compositors that do NOT support wlr-screencopy — notably
   KDE/KWin and GNOME/Mutter. Uses the org.freedesktop.portal.ScreenCast portal
   (over D-Bus / GDBus) to pick a monitor, then reads frames from the PipeWire
   stream the portal hands back.

   Built only when libpipewire-0.3 + gio-2.0 are available (see Makefile;
   -DHAVE_PORTAL). On first run the desktop shows a "share your screen?" dialog;
   we request a restore token so later runs don't re-prompt.

   NOTE: this captures an EXISTING monitor (mirror). KDE/GNOME have no portable
   "create a virtual output" path, so the auto virtual-display feature stays
   sway/Hyprland-only; on KDE/GNOME pspdisp mirrors a chosen screen.

   Capture path: full D-Bus handshake (CreateSession/SelectSources/Start/
   OpenPipeWireRemote) -> PipeWire stream -> shm OR DMA-BUF buffers. DMA-BUF
   (what modern compositors hand back) is negotiated with SPA modifiers
   (DONT_FIXATE), imported as an EGLImage, sampled through a tiny GLES2 shader
   into an RGBA FBO and read back with glReadPixels (the GPU detiles).

   Status: EXPERIMENTAL. Verified to reach a streaming PipeWire frame on sway via
   xdg-desktop-portal-wlr. DMA-BUF readback works on Mesa (Intel/AMD). On the
   NVIDIA proprietary driver the producer only offers the implicit modifier and
   NVIDIA's EGL can't detile implicit-modifier dmabufs, so frames come out
   garbled — a known NVIDIA/wlroots limitation, not specific to pspdisp. The
   well-supported backends are wlroots (wlr-screencopy), X11 (XShm), USB and
   Wi-Fi; use those unless you're on KDE/GNOME Wayland with a Mesa GPU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>
#include <gbm.h>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "pspdisp.h"

static capture_backend backend;        /* defined at bottom; filled in p_init */

/* ---- portal handshake (GDBus) ----------------------------------------- */
static GDBusConnection *bus;
static char *session_handle;
static int   pw_fd = -1;
static uint32_t pw_node = 0;
static GMainLoop *handshake_loop;
static bool  handshake_ok;

#define PORTAL_BUS  "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_SC   "org.freedesktop.portal.ScreenCast"

/* unique token + the request object path the portal will use for the reply */
static char *new_request(char **out_path)
{
  static unsigned ctr;
  char *token = g_strdup_printf("pspdisp%u", ctr++);
  char *sender = g_strdup(g_dbus_connection_get_unique_name(bus) + 1);
  for (char *p = sender; *p; p++) if (*p == '.') *p = '_';
  *out_path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
  g_free(sender);
  return token;
}

typedef void (*resp_fn)(guint response, GVariant *results);
struct pending { resp_fn cb; guint sub; };

static void on_response(GDBusConnection *c, const char *sender, const char *path,
                        const char *iface, const char *sig, GVariant *params, gpointer ud)
{
  (void)c;(void)sender;(void)path;(void)iface;(void)sig;
  struct pending *pend = ud;
  guint response; GVariant *results;
  g_variant_get(params, "(u@a{sv})", &response, &results);
  g_dbus_connection_signal_unsubscribe(bus, pend->sub);
  resp_fn cb = pend->cb;
  g_free(pend);
  cb(response, results);
  g_variant_unref(results);
}

static void subscribe_response(const char *req_path, resp_fn cb)
{
  struct pending *pend = g_new0(struct pending, 1);
  pend->cb = cb;
  pend->sub = g_dbus_connection_signal_subscribe(bus, PORTAL_BUS,
      "org.freedesktop.portal.Request", "Response", req_path, NULL,
      G_DBUS_SIGNAL_FLAGS_NONE, on_response, pend, NULL);
}

/* forward decls of the handshake steps */
static void step_select_sources(void);
static void step_start(void);
static void step_open_pw(void);

static void fail(const char *why) { fprintf(stderr, "portal: %s\n", why); handshake_ok = false; g_main_loop_quit(handshake_loop); }

/* 1) CreateSession -> 2) SelectSources -> 3) Start -> 4) OpenPipeWireRemote */
static void on_create(guint response, GVariant *results)
{
  VLOG("portal: CreateSession response=%u\n", response);
  if (response != 0) { fail("CreateSession refused"); return; }
  g_variant_lookup(results, "session_handle", "s", &session_handle);
  VLOG("portal: session_handle=%s\n", session_handle ? session_handle : "(null)");
  if (!session_handle) { fail("no session_handle"); return; }
  step_select_sources();
}

static void on_select(guint response, GVariant *results)
{
  (void)results;
  VLOG("portal: SelectSources response=%u\n", response);
  if (response != 0) { fail("SelectSources refused"); return; }
  step_start();
}

static void on_start(guint response, GVariant *results)
{
  VLOG("portal: Start response=%u\n", response);
  if (response != 0) { fail("Start refused (dialog cancelled?)"); return; }
  GVariant *streams = g_variant_lookup_value(results, "streams", NULL);
  if (streams) {
    GVariantIter it; g_variant_iter_init(&it, streams);
    GVariant *props; guint32 node;
    if (g_variant_iter_next(&it, "(u@a{sv})", &node, &props)) {
      pw_node = node;
      g_variant_unref(props);
    }
    g_variant_unref(streams);
  }
  VLOG("portal: stream node id=%u\n", pw_node);
  if (!pw_node) { fail("no stream node id"); return; }
  step_open_pw();
}

static void step_create_session(void)
{
  char *rpath, *rtok = new_request(&rpath);
  subscribe_response(rpath, on_create);
  char *stok = g_strdup_printf("pspdisp_sess%u", (unsigned)0);
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(rtok));
  g_variant_builder_add(&b, "{sv}", "session_handle_token", g_variant_new_string(stok));
  g_dbus_connection_call(bus, PORTAL_BUS, PORTAL_PATH, PORTAL_SC, "CreateSession",
      g_variant_new("(a{sv})", &b), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  g_free(rpath); g_free(rtok); g_free(stok);
}

static void step_select_sources(void)
{
  char *rpath, *rtok = new_request(&rpath);
  subscribe_response(rpath, on_select);
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(rtok));
  g_variant_builder_add(&b, "{sv}", "types", g_variant_new_uint32(1));        /* MONITOR */
  g_variant_builder_add(&b, "{sv}", "multiple", g_variant_new_boolean(FALSE));
  g_variant_builder_add(&b, "{sv}", "cursor_mode", g_variant_new_uint32(2));  /* embedded */
  g_dbus_connection_call(bus, PORTAL_BUS, PORTAL_PATH, PORTAL_SC, "SelectSources",
      g_variant_new("(oa{sv})", session_handle, &b), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  g_free(rpath); g_free(rtok);
}

static void step_start(void)
{
  char *rpath, *rtok = new_request(&rpath);
  subscribe_response(rpath, on_start);
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(rtok));
  g_dbus_connection_call(bus, PORTAL_BUS, PORTAL_PATH, PORTAL_SC, "Start",
      g_variant_new("(osa{sv})", session_handle, "", &b), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  g_free(rpath); g_free(rtok);
}

static void on_open_pw(GObject *src, GAsyncResult *res, gpointer ud)
{
  (void)ud;
  GError *err = NULL;
  GUnixFDList *fds = NULL;
  GVariant *ret = g_dbus_connection_call_with_unix_fd_list_finish(
      G_DBUS_CONNECTION(src), &fds, res, &err);
  if (!ret) { fail(err ? err->message : "OpenPipeWireRemote failed"); if (err) g_error_free(err); return; }
  gint32 idx; g_variant_get(ret, "(h)", &idx);
  pw_fd = g_unix_fd_list_get(fds, idx, NULL);
  g_variant_unref(ret);
  if (fds) g_object_unref(fds);
  handshake_ok = (pw_fd >= 0);
  g_main_loop_quit(handshake_loop);
}

static void step_open_pw(void)
{
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call_with_unix_fd_list(bus, PORTAL_BUS, PORTAL_PATH, PORTAL_SC,
      "OpenPipeWireRemote", g_variant_new("(oa{sv})", session_handle, &b),
      G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, on_open_pw, NULL);
}

/* ---- PipeWire stream --------------------------------------------------- */
static struct pw_thread_loop *pw_loop;
static struct pw_context *pw_ctx;
static struct pw_core *pw_core;
static struct pw_stream *pw_stream;
static struct spa_hook stream_hook;

static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *frame_buf;             /* latest frame (written by PW thread) */
static uint8_t *read_buf;              /* stable snapshot for the encoder      */
static int read_stride, read_bpp = 4, read_ro = 0, read_go = 1, read_bo = 2;
static int f_w, f_h, f_stride, f_bpp = 4, f_ro = 2, f_go = 1, f_bo = 0;
static bool have_frame;
static uint32_t f_spa_format = SPA_VIDEO_FORMAT_BGRx;
static uint64_t f_modifier = DRM_FORMAT_MOD_INVALID;

/* GBM device for importing DMA-BUF buffers and mapping them to CPU memory. */
static struct gbm_device *gbm;
static int drm_fd = -1;

static bool gbm_open(void)
{
  drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
  if (drm_fd < 0) return false;
  gbm = gbm_create_device(drm_fd);
  return gbm != NULL;
}

/* SPA video format -> DRM fourcc (for gbm import) */
static uint32_t spa_to_drm(uint32_t spa)
{
  switch (spa) {
    case SPA_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
    case SPA_VIDEO_FORMAT_RGBx: return DRM_FORMAT_XBGR8888;
    case SPA_VIDEO_FORMAT_BGRA: return DRM_FORMAT_ARGB8888;
    case SPA_VIDEO_FORMAT_RGBA: return DRM_FORMAT_ABGR8888;
    case SPA_VIDEO_FORMAT_xRGB: return DRM_FORMAT_BGRX8888;
    case SPA_VIDEO_FORMAT_xBGR: return DRM_FORMAT_RGBX8888;
    default:                    return DRM_FORMAT_XRGB8888;
  }
}

/* EGL/GLES used to read back tiled GPU DMA-BUF buffers: import the dmabuf as an
   EGLImage, attach to an FBO, glReadPixels (the GPU detiles into linear RGBA).
   gbm_bo_map only works for linear buffers, which compositors rarely produce. */
static EGLDisplay egl_dpy = EGL_NO_DISPLAY;
static EGLContext egl_ctx = EGL_NO_CONTEXT;
static bool egl_current, gl_ready;
static PFNEGLCREATEIMAGEKHRPROC  pCreateImage;
static PFNEGLDESTROYIMAGEKHRPROC pDestroyImage;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImgTargetTex;
static GLuint gl_prog, gl_vbo, gl_dst_tex, gl_dst_fbo;
static int gl_dst_w, gl_dst_h;

static bool egl_setup(void)   /* display + context only (no GL yet) */
{
  PFNEGLGETPLATFORMDISPLAYEXTPROC getdisp =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
  egl_dpy = getdisp ? getdisp(EGL_PLATFORM_GBM_KHR, gbm, NULL)
                    : eglGetDisplay((EGLNativeDisplayType)gbm);
  if (egl_dpy == EGL_NO_DISPLAY || !eglInitialize(egl_dpy, NULL, NULL)) return false;
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  egl_ctx = eglCreateContext(egl_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxattr);
  if (egl_ctx == EGL_NO_CONTEXT) return false;
  pCreateImage  = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  pDestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  pImgTargetTex = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  return pCreateImage && pImgTargetTex;
}

static GLuint gl_shader(GLenum type, const char *src)
{
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL); glCompileShader(s);
  return s;
}

/* one-time GL setup (must run on the thread the context is current on) */
static bool gl_init(void)
{
  static const char *vs =
      "attribute vec2 p; varying vec2 uv;"
      "void main(){ uv = p*0.5+0.5; gl_Position = vec4(p,0.0,1.0); }";
  static const char *fs =
      "#extension GL_OES_EGL_image_external : require\n"
      "precision mediump float; varying vec2 uv; uniform samplerExternalOES t;"
      "void main(){ gl_FragColor = texture2D(t, uv); }";
  GLuint v = gl_shader(GL_VERTEX_SHADER, vs), f = gl_shader(GL_FRAGMENT_SHADER, fs);
  gl_prog = glCreateProgram();
  glAttachShader(gl_prog, v); glAttachShader(gl_prog, f);
  glBindAttribLocation(gl_prog, 0, "p");
  glLinkProgram(gl_prog);
  GLint ok = 0; glGetProgramiv(gl_prog, GL_LINK_STATUS, &ok);
  if (!ok) return false;
  static const float quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
  glGenBuffers(1, &gl_vbo); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  glGenTextures(1, &gl_dst_tex);
  glGenFramebuffers(1, &gl_dst_fbo);
  return true;
}

/* Import one DMA-BUF plane, sample it through a shader into a normal RGBA
   destination FBO, then read that back linear. Runs on the PipeWire thread. */
static bool dmabuf_copy(int fd, uint32_t stride, uint32_t offset, uint8_t *dst)
{
  if (egl_dpy == EGL_NO_DISPLAY) return false;
  if (!egl_current) {
    if (!eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx)) return false;
    egl_current = true;
  }
  if (!gl_ready) { if (!gl_init()) return false; gl_ready = true; }

  EGLint attr[32]; int a = 0;
  attr[a++] = EGL_WIDTH;  attr[a++] = f_w;
  attr[a++] = EGL_HEIGHT; attr[a++] = f_h;
  attr[a++] = EGL_LINUX_DRM_FOURCC_EXT;      attr[a++] = (EGLint)spa_to_drm(f_spa_format);
  attr[a++] = EGL_DMA_BUF_PLANE0_FD_EXT;     attr[a++] = fd;
  attr[a++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attr[a++] = (EGLint)offset;
  attr[a++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  attr[a++] = (EGLint)stride;
  if (f_modifier != DRM_FORMAT_MOD_INVALID) {
    attr[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; attr[a++] = (EGLint)(f_modifier & 0xffffffff);
    attr[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; attr[a++] = (EGLint)(f_modifier >> 32);
  }
  attr[a++] = EGL_NONE;

  EGLImageKHR img = pCreateImage(egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
  if (img == EGL_NO_IMAGE_KHR) { static int o; if(!o){o=1;VLOG("portal: eglCreateImage failed\n");} return false; }

  /* (re)size the destination texture/FBO */
  if (gl_dst_w != f_w || gl_dst_h != f_h) {
    glBindTexture(GL_TEXTURE_2D, gl_dst_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f_w, f_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, gl_dst_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl_dst_tex, 0);
    gl_dst_w = f_w; gl_dst_h = f_h;
  }
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    static int o; if(!o){o=1;VLOG("portal: dest FBO incomplete\n");} pDestroyImage(egl_dpy, img); return false;
  }

  /* source: imported dmabuf bound to an EXTERNAL texture (required for dmabuf) */
  GLuint src = 0; glGenTextures(1, &src);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, src);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  pImgTargetTex(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)img);

  glBindFramebuffer(GL_FRAMEBUFFER, gl_dst_fbo);
  glViewport(0, 0, f_w, f_h);
  glUseProgram(gl_prog);
  glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, f_w, f_h, GL_RGBA, GL_UNSIGNED_BYTE, dst);   /* -> R,G,B,A, top-down */

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteTextures(1, &src);
  pDestroyImage(egl_dpy, img);
  return true;
}

static void set_offsets_from_spa(uint32_t fmt)
{
  /* common SPA formats from the portal */
  switch (fmt) {
    case SPA_VIDEO_FORMAT_BGRx: case SPA_VIDEO_FORMAT_BGRA:  /* mem: B G R x */
      f_bpp = 4; f_bo = 0; f_go = 1; f_ro = 2; break;
    case SPA_VIDEO_FORMAT_RGBx: case SPA_VIDEO_FORMAT_RGBA:  /* mem: R G B x */
      f_bpp = 4; f_ro = 0; f_go = 1; f_bo = 2; break;
    case SPA_VIDEO_FORMAT_BGR:                               /* mem: B G R   */
      f_bpp = 3; f_bo = 0; f_go = 1; f_ro = 2; break;
    case SPA_VIDEO_FORMAT_RGB:                               /* mem: R G B   */
      f_bpp = 3; f_ro = 0; f_go = 1; f_bo = 2; break;
    default: f_bpp = 4; f_bo = 0; f_go = 1; f_ro = 2; break;
  }
}

static void on_param_changed(void *ud, uint32_t id, const struct spa_pod *param)
{
  (void)ud;
  if (!param || id != SPA_PARAM_Format) return;
  struct spa_video_info info; memset(&info, 0, sizeof info);
  if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) return;
  if (info.media_type != SPA_MEDIA_TYPE_video) return;
  if (spa_format_video_raw_parse(param, &info.info.raw) < 0) return;

  uint8_t pod[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod, sizeof pod);

  /* DMA-BUF modifier negotiation: if the modifier is still a DONT_FIXATE choice,
     pick one and re-send the format fixated (single format + single modifier).
     The producer then replies with the settled format and we proceed. */
  const struct spa_pod_prop *mp = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier);
  if (mp && (mp->flags & SPA_POD_PROP_FLAG_DONT_FIXATE)) {
    uint32_t fmt = info.info.raw.format;
    uint32_t drm = spa_to_drm(fmt);
    /* The modifier prop is an enum choice [default, m0, m1, ...]. Pick a REAL
       (non-INVALID) modifier the GPU supports — importing the actual tiled
       layout. Falling back to INVALID makes EGL read tiled memory as linear
       (garbled bands). Only use INVALID if no explicit modifier is supported. */
    uint64_t mod = DRM_FORMAT_MOD_INVALID;
    bool got = false;
    uint32_t nv = SPA_POD_CHOICE_N_VALUES(&mp->value);
    int64_t *mv = SPA_POD_CHOICE_VALUES(&mp->value);
    for (uint32_t i = 1; i < nv && !got; i++) {
      uint64_t cand = (uint64_t)mv[i];
      int planes = gbm_device_get_format_modifier_plane_count(gbm, drm, cand);
      VLOG("portal:   offered modifier[%u]=0x%llx gbm_planes=%d\n", i,
           (unsigned long long)cand, planes);
      if (cand != DRM_FORMAT_MOD_INVALID && planes > 0) { mod = cand; got = true; }
    }
    struct spa_pod_frame fo;
    spa_pod_builder_push_object(&b, &fo, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&b, SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(&b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(fmt), 0);
    spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
    spa_pod_builder_long(&b, (int64_t)mod);
    spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_size,
        SPA_POD_Rectangle(&SPA_RECTANGLE(info.info.raw.size.width,
                                         info.info.raw.size.height)), 0);
    const struct spa_pod *fixed = spa_pod_builder_pop(&b, &fo);
    pw_stream_update_params(pw_stream, &fixed, 1);
    VLOG("portal: fixating modifier=0x%llx\n", (unsigned long long)mod);
    return;
  }

  pthread_mutex_lock(&frame_lock);
  f_w = info.info.raw.size.width;
  f_h = info.info.raw.size.height;
  f_spa_format = info.info.raw.format;
  f_modifier = info.info.raw.modifier;     /* 0/INVALID if shm */
  set_offsets_from_spa(f_spa_format);
  f_stride = f_w * f_bpp;
  free(frame_buf); frame_buf = malloc((size_t)f_stride * f_h);
  have_frame = false;
  pthread_mutex_unlock(&frame_lock);
  VLOG("portal: format settled %dx%d spa=%u modifier=0x%llx\n",
       f_w, f_h, f_spa_format, (unsigned long long)f_modifier);

  /* Accept DMA-BUF and/or MemPtr buffers. */
  const struct spa_pod *params[1];
  params[0] = spa_pod_builder_add_object(&b,
      SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
          (1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
  pw_stream_update_params(pw_stream, params, 1);
}

static void on_process(void *ud)
{
  (void)ud;
  struct pw_buffer *b = pw_stream_dequeue_buffer(pw_stream);
  if (!b) return;
  struct spa_buffer *buf = b->buffer;
  struct spa_data *d0 = &buf->datas[0];
  uint32_t stride = d0->chunk->stride ? (uint32_t)d0->chunk->stride : (uint32_t)f_stride;

  if (frame_buf) {
    pthread_mutex_lock(&frame_lock);
    if (d0->type == SPA_DATA_DmaBuf) {
      /* GPU buffer: import + map to read pixels back */
      bool okc = dmabuf_copy((int)d0->fd, stride, d0->chunk->offset, frame_buf);
      static int once; if (!once) { once = 1;
        VLOG("portal: on_process dmabuf fd=%d stride=%u copy=%d\n", (int)d0->fd, stride, okc); }
      if (okc) {
        /* glReadPixels(GL_RGBA) gives R,G,B,A regardless of source fourcc */
        f_ro = 0; f_go = 1; f_bo = 2; f_bpp = 4;
        have_frame = true;
        if (getenv("PSPDISP_DUMP")) { static int dz; if (!dz) { dz = 1;
          FILE *fp = fopen("/tmp/rawframe.ppm", "wb");
          if (fp) { fprintf(fp, "P6\n%d %d\n255\n", f_w, f_h);
            for (int yy = 0; yy < f_h; yy++) for (int xx = 0; xx < f_w; xx++) {
              uint8_t *p = frame_buf + (size_t)yy*f_stride + (size_t)xx*4;
              fputc(p[0], fp); fputc(p[1], fp); fputc(p[2], fp); }
            fclose(fp); VLOG("portal: dumped /tmp/rawframe.ppm\n"); } } }
      }
    } else if (d0->data) {
      /* shm / mapped memory */
      for (int y = 0; y < f_h; y++)
        memcpy(frame_buf + (size_t)y * f_stride,
               (uint8_t *)d0->data + d0->chunk->offset + (size_t)y * stride, f_stride);
      have_frame = true;
    }
    pthread_mutex_unlock(&frame_lock);
  }
  pw_stream_queue_buffer(pw_stream, b);
}

static void on_state_changed(void *ud, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error)
{
  (void)ud; (void)old;
  if (state == PW_STREAM_STATE_ERROR)
    fprintf(stderr, "portal: pipewire stream error: %s\n", error ? error : "?");
  else
    VLOG("portal: pw stream state=%s\n", pw_stream_state_as_string(state));
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_state_changed,
  .param_changed = on_param_changed,
  .process = on_process,
};

static bool start_pipewire(void)
{
  pw_init(NULL, NULL);
  pw_loop = pw_thread_loop_new("pspdisp-pw", NULL);
  if (!pw_loop) return false;
  pw_thread_loop_lock(pw_loop);
  pw_ctx = pw_context_new(pw_thread_loop_get_loop(pw_loop), NULL, 0);
  pw_core = pw_context_connect_fd(pw_ctx, fcntl(pw_fd, F_DUPFD_CLOEXEC, 5), NULL, 0);
  if (!pw_core) { pw_thread_loop_unlock(pw_loop); return false; }

  pw_stream = pw_stream_new(pw_core, "pspdisp-capture",
      pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                        PW_KEY_MEDIA_CATEGORY, "Capture",
                        PW_KEY_MEDIA_ROLE, "Screen", NULL));
  pw_stream_add_listener(pw_stream, &stream_hook, &stream_events, NULL);

  /* Offer two EnumFormat params: a DMA-BUF one (with a modifier property, so
     GPU-buffer producers like KWin/Mutter/wlroots accept it) and a plain shm
     one. The producer picks whichever it can; on_process handles both. */
  uint8_t pod[2048];
  struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(pod, sizeof pod);
  const struct spa_pod *params[2];
  static const uint32_t fmts[] = {
      SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_BGRA,
      SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_xRGB, SPA_VIDEO_FORMAT_xBGR };
  for (int dmabuf = 0; dmabuf < 2; dmabuf++) {
    struct spa_pod_frame fo, fc;
    spa_pod_builder_push_object(&pb, &fo, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&pb, SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(&pb, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    /* format = enum choice */
    spa_pod_builder_prop(&pb, SPA_FORMAT_VIDEO_format, 0);
    spa_pod_builder_push_choice(&pb, &fc, SPA_CHOICE_Enum, 0);
    spa_pod_builder_id(&pb, fmts[0]);                     /* default */
    for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++)
      spa_pod_builder_id(&pb, fmts[i]);
    spa_pod_builder_pop(&pb, &fc);
    if (dmabuf) {
      /* modifier present + DONT_FIXATE => producer fills in its real modifier */
      spa_pod_builder_prop(&pb, SPA_FORMAT_VIDEO_modifier,
          SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
      spa_pod_builder_push_choice(&pb, &fc, SPA_CHOICE_Enum, 0);
      spa_pod_builder_long(&pb, DRM_FORMAT_MOD_INVALID);  /* default */
      spa_pod_builder_long(&pb, DRM_FORMAT_MOD_INVALID);
      spa_pod_builder_pop(&pb, &fc);
    }
    spa_pod_builder_add(&pb, SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(64, 64),
            &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)), 0);
    params[dmabuf] = spa_pod_builder_pop(&pb, &fo);
  }

  pw_stream_connect(pw_stream, PW_DIRECTION_INPUT, pw_node,
      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
      params, 2);
  pw_thread_loop_unlock(pw_loop);
  pw_thread_loop_start(pw_loop);
  return true;
}

/* ---- capture_backend interface ---------------------------------------- */
static bool p_init(void)
{
  bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  if (!bus) { fprintf(stderr, "portal: no session D-Bus\n"); return false; }

  handshake_loop = g_main_loop_new(NULL, FALSE);
  handshake_ok = false;
  step_create_session();
  /* portal dialog may need the user; allow plenty of time via the loop. */
  g_main_loop_run(handshake_loop);
  g_main_loop_unref(handshake_loop);
  VLOG("portal: handshake_ok=%d pw_fd=%d pw_node=%u\n", handshake_ok, pw_fd, pw_node);
  if (!handshake_ok || pw_fd < 0) { fprintf(stderr, "portal: handshake failed\n"); return false; }

  if (!gbm_open() || !egl_setup())
    fprintf(stderr, "portal: GPU/EGL init failed — DMA-BUF frames unavailable\n");

  if (!start_pipewire()) { fprintf(stderr, "portal: PipeWire connect failed\n"); return false; }
  VLOG("portal: pipewire stream connecting, waiting for first frame...\n");

  /* wait briefly for the first frame so width/height are known */
  for (int i = 0; i < 100; i++) {
    pthread_mutex_lock(&frame_lock);
    bool ok = have_frame && f_w > 0;
    pthread_mutex_unlock(&frame_lock);
    if (ok) break;
    g_usleep(50 * 1000);
  }
  pthread_mutex_lock(&frame_lock);
  backend.width = f_w; backend.height = f_h;
  pthread_mutex_unlock(&frame_lock);
  if (f_w <= 0) {
    fprintf(stderr,
      "portal: connected to the screencast but no frame arrived.\n"
      "        The compositor is likely producing DMA-BUF buffers, which this\n"
      "        backend can't yet read (shm/MemPtr only). KDE/GNOME capture is\n"
      "        experimental — see capture_portal.c. Other backends work.\n");
    return false;
  }
  VLOG("portal: first frame %dx%d\n", f_w, f_h);
  return f_w > 0;
}

static bool p_grab(void)
{
  /* Snapshot the latest frame into read_buf under the lock, so the encoder
     reads a stable image while the PipeWire thread writes the next one. */
  pthread_mutex_lock(&frame_lock);
  bool ok = have_frame && frame_buf;
  if (ok) {
    size_t sz = (size_t)f_stride * f_h;
    read_buf = realloc(read_buf, sz);
    memcpy(read_buf, frame_buf, sz);
    read_stride = f_stride; read_bpp = f_bpp;
    read_ro = f_ro; read_go = f_go; read_bo = f_bo;
  }
  pthread_mutex_unlock(&frame_lock);
  return ok;
}

static void p_pixel(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b)
{
  uint8_t *px = read_buf + (size_t)y * read_stride + (size_t)x * read_bpp;
  *r = px[read_ro]; *g = px[read_go]; *b = px[read_bo];
}

static void p_term(void)
{
  if (pw_loop) { pw_thread_loop_stop(pw_loop); }
  if (pw_stream) pw_stream_destroy(pw_stream);
  if (pw_core) pw_core_disconnect(pw_core);
  if (pw_ctx) pw_context_destroy(pw_ctx);
  if (pw_loop) pw_thread_loop_destroy(pw_loop);
  if (gbm) { gbm_device_destroy(gbm); gbm = NULL; }
  if (drm_fd >= 0) { close(drm_fd); drm_fd = -1; }
  free(frame_buf); frame_buf = NULL;
  if (session_handle) {
    /* close the portal session */
    g_dbus_connection_call(bus, PORTAL_BUS, session_handle,
        "org.freedesktop.portal.Session", "Close", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_free(session_handle); session_handle = NULL;
  }
  if (bus) g_object_unref(bus);
}

static capture_backend backend = {
  .name = "portal", .init = p_init, .grab = p_grab, .pixel = p_pixel, .term = p_term,
};
capture_backend *capture_portal(void) { return &backend; }   /* dims set in p_init */
