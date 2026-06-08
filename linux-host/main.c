/* PSPdisp Linux host — entry point and frame pump.

   Replaces the original Windows host (Delphi app + XPDM mirror driver):
     capture (wlroots/X11) -> scale 480x272 -> JPEG -> transport (USB/TCP) -> PSP
     PSP buttons -> uinput gamepad ; optional PC audio -> PSP. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/file.h>
#include "pspdisp.h"

options g_opt;
static volatile sig_atomic_t run = 1;
static void on_sig(int s) { (void)s; run = 0; }

/* Single-instance lock: two hosts fighting over the same USB device cause
   claim failures and disconnect storms (a "black screen" we hit repeatedly).
   Refuse to start a second instance. Lock auto-releases when the process dies. */
static int lockfd = -1;
static bool acquire_lock(void)
{
  const char *rt = getenv("XDG_RUNTIME_DIR");
  char path[256];
  snprintf(path, sizeof path, "%s/pspdisp.lock", rt ? rt : "/tmp");
  lockfd = open(path, O_CREAT | O_RDWR, 0644);
  if (lockfd < 0) return true;                 /* can't lock -> don't block use */
  if (flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
    fprintf(stderr, "another pspdisp is already running (%s).\n"
                    "stop it cleanly first:  pkill -TERM -x pspdisp\n", path);
    return false;
  }
  return true;
}

/* Detach into the background: fork, the child keeps the lock + runs, the parent
   prints how to stop it and exits. Child's stdout/stderr go to a log so output
   isn't lost. Called after acquire_lock(), so the child inherits the held lock. */
static void daemonize(void)
{
  fflush(stdout);
  pid_t pid = fork();
  if (pid < 0) { perror("fork"); exit(1); }
  if (pid > 0) {
    printf("pspdisp is now running in the background (pid %d).\n"
           "Stop it with:  pspdisp --kill\n", pid);
    fflush(stdout);
    _exit(0);                       /* parent leaves; child holds the lock */
  }
  setsid();                         /* detach from the controlling terminal */
  const char *rt = getenv("XDG_RUNTIME_DIR");
  char lp[256];
  snprintf(lp, sizeof lp, "%s/pspdisp.log", rt && *rt ? rt : "/tmp");
  int fd = open(lp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); if (fd > 2) close(fd); }
  int z = open("/dev/null", O_RDONLY);
  if (z >= 0) { dup2(z, STDIN_FILENO); if (z > 2) close(z); }
}

static transport_backend *tp;
static capture_backend   *cap;

/* read the PSP response; drains the optional inline settings block. */
static bool read_response(ResponseHeader *out)
{
  if (tp->full_response) {                       /* USB: 14 + 76 inline */
    unsigned char buf[RESPONSE_FULL_SIZE];
    if (tp->read(buf, RESPONSE_FULL_SIZE, 1000) != 0) return false;
    memcpy(out, buf, sizeof *out);
    return true;
  }
  if (tp->read(out, sizeof *out, 1000) != 0) return false;   /* TCP: 14 */
  if (out->flags & COM_FLAGS_CONTAINS_SETTINGS_DATA) {
    unsigned char settings[76];
    if (tp->read(settings, sizeof settings, 1000) != 0) return false;
  }
  return true;
}

static bool force_next;   /* PSP asked (FORCE_UPDATE) for a full frame next */

/* one transmit/receive cycle. send_image=false => poll-only header. */
static bool pump(bool send_image)
{
  static unsigned char audio[8192];
  unsigned char *jpeg = NULL;
  unsigned long jsize = 0;
  uint32_t flags = 0;

  if (send_image) {
    jpeg = frame_encode(cap, &jsize, &flags);
    if (!jpeg) return false;
  }

  int asize = 0;
  if (g_opt.audio) {
    uint32_t aflags = 0;
    asize = audio_read_frame(audio, sizeof audio, &aflags);
    if (asize > 0) flags |= aflags;
  }

  /* Never overflow the PSP's 400 KB receive buffer. If an encoded frame is
     pathologically large, drop the image (keep audio) rather than corrupt
     PSP memory. Practically impossible at 480x272, but the PSP can't guard
     itself. */
  if (jsize + (unsigned)asize + 64 > COM_IMAGE_BUFFER_SIZE) {
    fprintf(stderr, "\nframe too large (%lu B), dropping image\n", jsize);
    free(jpeg); jpeg = NULL; jsize = 0;
    flags &= ~(COM_FLAGS_CONTAINS_IMAGE_DATA | COM_FLAGS_IMAGE_IS_JPEG |
               COM_FLAGS_IMAGE_IS_ROTATED_90 | COM_FLAGS_IMAGE_IS_ROTATED_180 |
               COM_FLAGS_IMAGE_IS_ROTATED_270);
  }

  FrameHeader hdr = { COM_HEADER_MAGIC, flags, (uint32_t)jsize, 0 };
  bool ok = (tp->write(&hdr, sizeof hdr) == 0);
  if (ok && jsize)  ok = (tp->write(jpeg, jsize) == 0);
  if (ok && asize)  ok = (tp->write(audio, asize) == 0);
  free(jpeg);
  if (!ok) return false;

  ResponseHeader resp;
  if (!read_response(&resp)) return false;
  if (resp.magic != COM_HEADER_MAGIC) return true;     /* tolerate one desync */

  /* PSP requests a forced full frame (e.g. after closing its menu/OSK). */
  if (resp.flags & COM_FLAGS_FORCE_UPDATE) force_next = true;

  if (g_opt.input) input_update(resp.buttons, resp.analogX, resp.analogY);
  VLOG("\r%6lu B  btn %08x  ax %3u ay %3u   ", jsize, resp.buttons, resp.analogX, resp.analogY);
  return true;
}

static void loop(void)
{
  long frame_us = 1000000L / (g_opt.fps > 0 ? g_opt.fps : 20);
  uint64_t last_hash = 0;
  int idle = 0;

  while (run) {
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);

    if (!cap->grab()) { fprintf(stderr, "\ncapture failed\n"); break; }
    uint64_t h = frame_hash(cap);
    bool changed = (h != last_hash);
    last_hash = h;

    /* send the image on change; force a refresh at least every ~2s even if
       static (keeps the PSP from timing out and recovers dropped frames). */
    bool send_image = changed || force_next || (++idle >= g_opt.fps * 2);
    if (send_image) { idle = 0; force_next = false; }

    if (!pump(send_image)) { fprintf(stderr, "\nlink lost\n"); break; }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long used = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
    if (used < frame_us) usleep(frame_us - used);
  }
}

static void usage(const char *p)
{
  printf(
   "PSPdisp Linux host — use a PSP as a second display + gamepad.\n"
   "\n"
   "usage: %s [options]\n"
   "\n"
   "Connection:\n"
   "  -t usb|tcp        how to reach the PSP (default: usb)\n"
   "                      usb = cable; tcp = Wi-Fi (PSP connects to this PC)\n"
   "  -P PORT           TCP/Wi-Fi port (default: %d)\n"
   "  -k PASSWORD       Wi-Fi password, must match the PSP (default: none)\n"
   "\n"
   "What to show on the PSP:\n"
   "  (default on sway/Hyprland: create a NEW virtual screen, removed when pspdisp stops)\n"
   "  --no-display      mirror an existing screen instead of a new one\n"
   "  -o NAME           mirror this monitor by name, e.g. HDMI-A-1 / DisplayPort-0\n"
   "                    (Wayland: by name; X11: looks up its xrandr geometry)\n"
   "  -b wlr|x11|portal capture backend (default: auto picks the right one)\n"
   "                    wlr=sway/Hyprland, x11=Xorg, portal=KDE/GNOME Wayland\n"
   "                    (portal needs the PipeWire-portal build; experimental)\n"
   "  -x N -y N -w N -h N  capture an explicit screen region (x11 backend only)\n"
   "  -r 0|90|180|270   rotate the image (default: 0)\n"
   "\n"
   "Quality / speed:\n"
   "  -q 1..100         JPEG quality (default: 100; lower = faster)\n"
   "  -f N              max frames per second (default: 60; PSP caps at 60)\n"
   "\n"
   "Extras:\n"
   "  -i                expose PSP buttons as a uinput gamepad\n"
   "  -a                stream PC audio to the PSP (experimental)\n"
   "  -v                verbose (show fps / button data)\n"
   "  --background, -D  run detached (logs to $XDG_RUNTIME_DIR/pspdisp.log)\n"
   "  --help, -H        show this help\n"
   "  --kill            stop a running pspdisp cleanly\n"
   "\n"
   "Examples:\n"
   "  %s                      # new virtual screen on the PSP (sway/Hyprland)\n"
   "  %s -i                   # ...plus PSP buttons as a gamepad\n"
   "  %s --no-display -t tcp  # mirror your screen over Wi-Fi instead\n",
   p, NET_PORT, p, p, p);
}

int main(int argc, char **argv)
{
  /* defaults: on Wayland use wlr-screencopy on wlroots compositors, else the
     PipeWire portal (KDE/GNOME); on X11 use XShm. */
  g_opt.transport = TRANSPORT_USB;
  if (getenv("WAYLAND_DISPLAY")) {
    const char *xdg = getenv("XDG_CURRENT_DESKTOP");
    bool wlroots = getenv("SWAYSOCK") || getenv("HYPRLAND_INSTANCE_SIGNATURE") ||
                   (xdg && (strstr(xdg, "sway") || strstr(xdg, "Hyprland") ||
                            strstr(xdg, "wlroots") || strstr(xdg, "river") ||
                            strstr(xdg, "Wayfire") || strstr(xdg, "labwc")));
#ifdef HAVE_PORTAL
    g_opt.capture = wlroots ? CAPTURE_WLR : CAPTURE_PORTAL;
#else
    g_opt.capture = CAPTURE_WLR;   /* no portal built; KDE/GNOME need -b x11 */
#endif
  } else {
    g_opt.capture = CAPTURE_X11;
  }
  g_opt.x = g_opt.y = 0; g_opt.w = PSP_W; g_opt.h = PSP_H;
  g_opt.rotation = 0; g_opt.quality = 100; g_opt.fps = 60;
  g_opt.tcp_port = NET_PORT;
  g_opt.disp_w = 1920; g_opt.disp_h = 1080;   /* virtual output, downscaled */

  /* getopt() doesn't handle long options. Handle the long ones here, then
     compact argv so getopt only sees short options. */
  bool background = false;
  int w = 1;
  for (int a = 1; a < argc; a++) {
    if (!strcmp(argv[a], "--help")) { usage(argv[0]); return 0; }
    if (!strcmp(argv[a], "--background") || !strcmp(argv[a], "--daemon")) { background = true; continue; }
    if (!strcmp(argv[a], "--kill") || !strcmp(argv[a], "--stop")) {
      /* Stop our own running host(s) cleanly: SIGTERM, never kill -9 (that
         wedges the USB device). Restrict to this user's processes so we don't
         touch a root-owned instance and leak permission errors. */
      int r = system("pkill -TERM -u \"$(id -u)\" -x pspdisp 2>/dev/null");
      printf(r == 0 ? "stopped pspdisp.\n" : "no running pspdisp found.\n");
      return 0;
    }
    if (!strcmp(argv[a], "--no-display")) { g_opt.no_display = true; continue; }
    argv[w++] = argv[a];          /* keep everything else for getopt */
  }
  argc = w;

  int c;
  while ((c = getopt(argc, argv, "t:b:o:x:y:w:h:r:q:f:P:k:iavHD")) != -1) {
    switch (c) {
      case 't': g_opt.transport = strcmp(optarg, "tcp") ? TRANSPORT_USB : TRANSPORT_TCP; break;
      case 'b':
        if      (!strcmp(optarg, "x11"))    g_opt.capture = CAPTURE_X11;
        else if (!strcmp(optarg, "portal")) g_opt.capture = CAPTURE_PORTAL;
        else                                g_opt.capture = CAPTURE_WLR;
        break;
      case 'o': g_wlr_output_name = optarg; break;
      case 'x': g_opt.x = atoi(optarg); break;
      case 'y': g_opt.y = atoi(optarg); break;
      case 'w': g_opt.w = atoi(optarg); break;
      case 'h': g_opt.h = atoi(optarg); break;
      case 'r': g_opt.rotation = atoi(optarg); break;
      case 'q': g_opt.quality = atoi(optarg); break;
      case 'f': g_opt.fps = atoi(optarg); break;
      case 'P': g_opt.tcp_port = atoi(optarg); break;
      case 'k': strncpy(g_opt.password, optarg, NET_PASSWORD_LEN - 1); break;
      case 'i': g_opt.input = true; break;
      case 'a': g_opt.audio = true; break;
      case 'v': g_opt.verbose = true; break;
      case 'D': background = true; break;
      case 'H': usage(argv[0]); return 0;
      default:  usage(argv[0]); return 1;
    }
  }
  if (g_opt.w <= 0 || g_opt.h <= 0) { fprintf(stderr, "bad region size\n"); return 1; }
  if (g_opt.rotation && g_opt.rotation != 90 && g_opt.rotation != 180 && g_opt.rotation != 270) {
    fprintf(stderr, "bad rotation\n"); return 1; }

  if (!acquire_lock()) return 1;     /* refuse a second, device-fighting instance */
  if (background) daemonize();       /* detach; parent prints how to --kill it */

  /* Install handlers WITHOUT SA_RESTART so SIGTERM/SIGINT interrupt blocking
     syscalls (accept, recv, usb transfers) with EINTR — the loops then see
     run==0 and exit cleanly. With signal()'s default SA_RESTART the host could
     hang in accept()/recv() and only die to kill -9 (which wedges USB). */
  {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;            /* sa_flags = 0 -> no SA_RESTART */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
  }

  tp  = (g_opt.transport == TRANSPORT_TCP) ? transport_tcp() : transport_usb();
  switch (g_opt.capture) {
    case CAPTURE_X11:    cap = capture_x11(); break;
#ifdef HAVE_PORTAL
    case CAPTURE_PORTAL: cap = capture_portal(); break;
#else
    case CAPTURE_PORTAL:
      fprintf(stderr, "this build has no PipeWire portal backend; install "
                      "libpipewire-0.3-dev + libglib2.0-dev and rebuild, or use -b x11\n");
      return 1;
#endif
    default:             cap = capture_wlr(); break;
  }

  /* By default, create a dedicated virtual output (a real extra monitor) and
     capture that, instead of mirroring an existing screen. Works on sway/Hyprland
     (wlr) and on X11 (an off-screen framebuffer strip; installer adds the
     `Virtual` headroom). Skipped if the
     user picked an output (-o) / a region (-x..-h) or opted out (--no-display).
     Torn down on exit (atexit + clean SIGTERM shutdown), so the extra screen
     appears with pspdisp and vanishes when it stops. */
  /* X11: -o NAME means "mirror this monitor" — look up its geometry from xrandr
     and turn it into a capture region, so users pick a screen by name instead of
     computing -x/-y/-w/-h. (On Wayland the wlr backend uses the name directly.) */
  if (g_opt.capture == CAPTURE_X11 && g_wlr_output_name) {
    char qc[256];
    snprintf(qc, sizeof qc,
      "xrandr 2>/dev/null | awk -v o=\"%s\" '$1==o && / connected/{"
      "for(i=1;i<=NF;i++) if($i ~ /^[0-9]+x[0-9]+\\+[0-9]+\\+[0-9]+$/){print $i; exit}}'",
      g_wlr_output_name);
    FILE *qp = popen(qc, "r");
    int oW = 0, oH = 0, oX = 0, oY = 0;
    if (qp) { char ql[64] = ""; if (fgets(ql, sizeof ql, qp)) sscanf(ql, "%dx%d+%d+%d", &oW, &oH, &oX, &oY); pclose(qp); }
    if (oW > 0 && oH > 0) {
      g_opt.x = oX; g_opt.y = oY; g_opt.w = oW; g_opt.h = oH;
      printf("Mirroring X11 output %s (%dx%d @ %d,%d).\n", g_wlr_output_name, oW, oH, oX, oY);
    } else {
      fprintf(stderr, "output '%s' not found (check: xrandr | grep ' connected')\n",
              g_wlr_output_name);
      return 1;
    }
  }

  bool region_is_default = (g_opt.x == 0 && g_opt.y == 0 &&
                            g_opt.w == PSP_W && g_opt.h == PSP_H);
  bool may_auto = !g_opt.no_display && g_wlr_output_name == NULL &&
                  ((g_opt.capture == CAPTURE_WLR) ||
                   (g_opt.capture == CAPTURE_X11 && region_is_default));
  if (may_auto) {
    const char *name = display_auto_create(g_opt.disp_w, g_opt.disp_h);
    if (name) {
      if (g_opt.capture == CAPTURE_WLR) g_wlr_output_name = name;
      /* X11: display_auto_create already set g_opt.x/y/w/h to the virtual monitor */
      atexit(display_auto_destroy);
      printf("Created virtual display %s (%dx%d) — removed when pspdisp stops.\n",
             name, g_opt.disp_w, g_opt.disp_h);
    } else {
      VLOG("display: no virtual output available, mirroring instead\n");
    }
  }

  if (!cap->init()) { fprintf(stderr, "capture init failed\n"); display_auto_destroy(); return 1; }
  if (g_opt.input && !input_init()) g_opt.input = false;
  if (g_opt.audio && !audio_init()) g_opt.audio = false;

  printf("PSPdisp Linux host: capture=%s transport=%s rot=%d q=%d %dfps%s%s\n",
         cap->name, tp->name, g_opt.rotation, g_opt.quality, g_opt.fps,
         g_opt.input ? " +input" : "", g_opt.audio ? " +audio" : "");
  if (g_opt.transport == TRANSPORT_USB)
    printf("Put the PSP into USB mode in the PSPdisp homebrew.\n");

  while (run) {
    if (!tp->open()) { if (run) sleep(2); continue; }
    printf("connected (%s).\n", tp->name);
    loop();
    tp->close();
    if (run) sleep(1);
  }

  audio_term(); input_term(); cap->term();
  printf("\nbye.\n");
  return 0;
}
