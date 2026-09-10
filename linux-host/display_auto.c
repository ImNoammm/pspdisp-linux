/* Auto-managed virtual display for wlroots compositors.

   When pspdisp runs with no explicit -o output, it creates a headless output on
   start and removes it on exit, so the PSP shows up as a real extra monitor
   that appears when you start pspdisp and disappears when you stop it.

   Supported compositors:
     - sway      : swaymsg create_output / output ... / unplug   (wlr-screencopy)
     - Hyprland  : hyprctl output create headless / monitor / remove (wlr-screencopy)
     - X11       : grow the framebuffer past the real screen and carve the extra
                   strip into a monitor with `xrandr --setmonitor` (XShm capture)

   The X11 path is driver-agnostic: it needs no VirtualHeads support (which
   amdgpu and the NVIDIA DDX lack, and many modesetting builds ship without), so
   it works on AMD/Intel/NVIDIA alike. It only needs the X screen's `Virtual`
   size to be larger than the desktop. The installer drops an xorg.conf snippet
   that sets `Virtual 5120 2160` so there's always headroom for the strip.

   On anything else display_auto_create() returns NULL and the caller falls
   back to mirroring an existing output. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pspdisp.h"

typedef enum { WM_NONE, WM_SWAY, WM_HYPR, WM_X11 } wm_kind;
static wm_kind   wm = WM_NONE;
static char      created_name[64];     /* the output we enabled, "" if none */
static int       x11_orig_w, x11_orig_h;  /* framebuffer size to restore on exit */

static wm_kind detect_wm(void)
{
  if (getenv("WAYLAND_DISPLAY")) {
    if (getenv("SWAYSOCK") && system("command -v swaymsg >/dev/null 2>&1") == 0)
      return WM_SWAY;
    if (getenv("HYPRLAND_INSTANCE_SIGNATURE") && system("command -v hyprctl >/dev/null 2>&1") == 0)
      return WM_HYPR;
    return WM_NONE;
  }
  /* X11: the framebuffer-strip method works on any driver (no VirtualHeads). */
  if (getenv("DISPLAY") && system("command -v xrandr >/dev/null 2>&1") == 0)
    return WM_X11;
  return WM_NONE;
}

bool display_sway_available(void) { return detect_wm() != WM_NONE; }

/* Create a headless output sized w x h, placed to the right of existing
   screens. Returns its name (e.g. "HEADLESS-1") or NULL on failure. */
const char *display_auto_create(int w, int h)
{
  wm = detect_wm();
  if (wm == WM_NONE) return NULL;

  char cmd[1500];
  if (wm == WM_SWAY) {
    /* snapshot HEADLESS-* before, create one, diff to find its name, place it
       right of the current outputs, set its mode, print the name. */
    snprintf(cmd, sizeof cmd,
      "before=$(swaymsg -t get_outputs -r 2>/dev/null | grep -oE 'HEADLESS-[0-9]+' | sort -u);"
      "ncount=$(swaymsg -t get_outputs -r 2>/dev/null | grep -c '\"name\"');"
      "swaymsg create_output >/dev/null 2>&1 || exit 1;"
      "new=;"
      "for i in 1 2 3 4 5; do"
      "  after=$(swaymsg -t get_outputs -r 2>/dev/null | grep -oE 'HEADLESS-[0-9]+' | sort -u);"
      "  new=$(printf '%%s\\n' \"$after\" | grep -vxF \"$before\" 2>/dev/null | head -1);"
      "  [ -n \"$new\" ] && break; sleep 0.2;"
      "done;"
      "[ -z \"$new\" ] && exit 1;"
      "xoff=$((ncount * 1920));"
      "swaymsg \"output $new mode --custom %dx%d\" >/dev/null 2>&1;"
      "swaymsg \"output $new position $xoff 0\" >/dev/null 2>&1;"
      "printf '%%s' \"$new\"",
      w, h);
  } else if (wm == WM_HYPR) {
    /* Hyprland: create a headless output, find the new HEADLESS-* name, then
       set its mode/position with a monitor keyword. */
    snprintf(cmd, sizeof cmd,
      "before=$(hyprctl monitors -j 2>/dev/null | grep -oE 'HEADLESS-[0-9]+' | sort -u);"
      "ncount=$(hyprctl monitors -j 2>/dev/null | grep -c '\"name\"');"
      "hyprctl output create headless >/dev/null 2>&1 || exit 1;"
      "new=;"
      "for i in 1 2 3 4 5; do"
      "  after=$(hyprctl monitors -j 2>/dev/null | grep -oE 'HEADLESS-[0-9]+' | sort -u);"
      "  new=$(printf '%%s\\n' \"$after\" | grep -vxF \"$before\" 2>/dev/null | head -1);"
      "  [ -n \"$new\" ] && break; sleep 0.2;"
      "done;"
      "[ -z \"$new\" ] && exit 1;"
      "xoff=$((ncount * 1920));"
      "hyprctl keyword monitor \"$new,%dx%d,${xoff}x0,1\" >/dev/null 2>&1;"
      "printf '%%s' \"$new\"",
      w, h);
  } else { /* WM_X11: grow the framebuffer and carve the off-screen strip */
    /* Place a w x h region just right of the current desktop. The framebuffer is
       grown to (curW + w) so the strip lives in video memory but on no physical
       output; `--setmonitor` makes the WM treat it as a real head. We print
       "xoff curW curH" so the caller sets the capture region and we can restore
       the framebuffer on exit. Falls back (exit 1) if the screen can't grow
       (Virtual too small), caller then mirrors instead.
       NOTE: every side-effect xrandr must suppress BOTH stdout and stderr
       (`--delmonitor` prints "No monitor named ..." to stdout), so the only
       thing on stdout is the final printf the caller parses. */
    snprintf(cmd, sizeof cmd,
      "cur=$(xrandr 2>/dev/null | awk '/^Screen 0/{w=$8;h=$10;sub(/,/,\"\",w);sub(/,/,\"\",h);print w\" \"h}');"
      "set -- $cur; cw=$1; ch=$2;"
      "[ -z \"$cw\" ] && exit 1;"
      "xoff=$cw; nw=$((cw + %d)); nh=$ch; [ %d -gt $nh ] && nh=%d;"
      "xrandr --fb ${nw}x${nh} >/dev/null 2>&1 || exit 1;"
      "gw=$(xrandr 2>/dev/null | awk '/^Screen 0/{w=$8;sub(/,/,\"\",w);print w}');"
      "if [ \"${gw:-0}\" -lt \"$nw\" ] 2>/dev/null; then xrandr --fb ${cw}x${ch} >/dev/null 2>&1; exit 1; fi;"
      "xrandr --delmonitor PSP-1 >/dev/null 2>&1;"
      "xrandr --setmonitor PSP-1 %d/100x%d/60+${xoff}+0 none >/dev/null 2>&1"
      "  || { xrandr --fb ${cw}x${ch} >/dev/null 2>&1; exit 1; };"
      "printf '%%s %%s %%s' \"$xoff\" \"$cw\" \"$ch\"",
      w, h, h, w, h);
  }

  FILE *p = popen(cmd, "r");
  if (!p) return NULL;
  char line[128] = "";
  if (fgets(line, sizeof line, p) == NULL) line[0] = '\0';
  pclose(p);

  if (wm == WM_X11) {
    /* parse "xoff origW origH": set the capture region to the off-screen strip
       and remember the framebuffer size to restore on exit. */
    int xoff = -1;
    if (sscanf(line, "%d %d %d", &xoff, &x11_orig_w, &x11_orig_h) != 3 || xoff < 0)
      return NULL;
    snprintf(created_name, sizeof created_name, "PSP-1");
    g_opt.x = xoff; g_opt.y = 0; g_opt.w = w; g_opt.h = h;
    VLOG("display: X11 virtual head PSP-1 (%dx%d @ x=%d), fb %dx%d->%dx%d\n",
         w, h, xoff, x11_orig_w, x11_orig_h, xoff + w, x11_orig_h);
    return created_name;
  }

  snprintf(created_name, sizeof created_name, "%s", line);
  size_t n = strlen(created_name);
  while (n && (created_name[n-1] == '\n' || created_name[n-1] == ' ' || created_name[n-1] == '\r'))
    created_name[--n] = '\0';
  if (strncmp(created_name, "HEADLESS-", 9) != 0) { created_name[0] = '\0'; return NULL; }
  VLOG("display: created virtual output %s (%dx%d) on %s\n",
       created_name, w, h, wm == WM_SWAY ? "sway" : "Hyprland");
  return created_name;
}

/* Remove the output we created (no-op if we didn't create one). */
void display_auto_destroy(void)
{
  if (created_name[0] == '\0') return;
  char cmd[200];
  if (wm == WM_SWAY)
    snprintf(cmd, sizeof cmd, "swaymsg 'output %s unplug' >/dev/null 2>&1", created_name);
  else if (wm == WM_HYPR)
    snprintf(cmd, sizeof cmd, "hyprctl output remove %s >/dev/null 2>&1", created_name);
  else /* WM_X11: drop the carved monitor and shrink the framebuffer back */
    snprintf(cmd, sizeof cmd,
      "xrandr --delmonitor PSP-1 >/dev/null 2>&1; xrandr --fb %dx%d >/dev/null 2>&1",
      x11_orig_w, x11_orig_h);
  if (system(cmd) != 0) { /* best effort */ }
  VLOG("display: removed virtual output %s\n", created_name);
  created_name[0] = '\0';
}
