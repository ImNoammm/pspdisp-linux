# PSPdisp Linux host
CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra
CFLAGS   += -D_GNU_SOURCE        # clock_gettime, timeval, ftruncate, MSG_NOSIGNAL
PREFIX   ?= /usr/local

# pkg-config sets (libjpeg has no .pc on some distros -> linked manually)
PKGS      = libusb-1.0 x11 xext wayland-client libpulse-simple libpulse
CFLAGS   += $(shell pkg-config --cflags $(PKGS)) -Igen
LDLIBS    = $(shell pkg-config --libs $(PKGS)) -ljpeg -lrt

SCANNER  ?= wayland-scanner
WLR_XML   = protocol/wlr-screencopy-unstable-v1.xml

GEN       = gen/wlr-screencopy-client-protocol.h gen/wlr-screencopy-protocol.c
SRC       = main.c frame.c input.c audio_pulse.c \
            transport_usb.c transport_tcp.c \
            capture_wlr.c capture_x11.c display_auto.c \
            gen/wlr-screencopy-protocol.c

# Optional PipeWire-portal capture backend (KDE/GNOME Wayland). Built only when
# libpipewire-0.3 + gio-2.0 dev libs are present.
PORTAL_PKGS = libpipewire-0.3 gio-2.0 gio-unix-2.0 gbm libdrm egl glesv2
ifeq ($(shell pkg-config --exists $(PORTAL_PKGS) && echo yes),yes)
  CFLAGS  += -DHAVE_PORTAL $(shell pkg-config --cflags $(PORTAL_PKGS))
  LDLIBS  += $(shell pkg-config --libs $(PORTAL_PKGS)) -lpthread
  SRC     += capture_portal.c
endif

pspdisp: $(GEN) $(SRC) pspdisp.h proto.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

gen/wlr-screencopy-client-protocol.h: $(WLR_XML)
	@mkdir -p gen
	$(SCANNER) client-header $< $@

gen/wlr-screencopy-protocol.c: $(WLR_XML)
	@mkdir -p gen
	$(SCANNER) private-code $< $@

# system-wide install (needs root): /usr/local/bin + udev rule
install: pspdisp
	install -Dm755 pspdisp $(DESTDIR)$(PREFIX)/bin/pspdisp
	install -Dm644 99-pspdisp.rules $(DESTDIR)/etc/udev/rules.d/99-pspdisp.rules
	@echo "now: sudo udevadm control --reload && sudo udevadm trigger"

# per-user install (no root): ~/.local/bin. The udev rule still needs root for
# USB/uinput access without sudo — install it separately with 'make install-udev'.
install-user: pspdisp
	install -Dm755 pspdisp $(HOME)/.local/bin/pspdisp
	@echo "installed: $(HOME)/.local/bin/pspdisp"
	@case ":$$PATH:" in *":$(HOME)/.local/bin:"*) ;; \
	  *) echo "note: add ~/.local/bin to PATH, e.g.:"; \
	     echo "  echo 'export PATH=\"$$HOME/.local/bin:$$PATH\"' >> ~/.profile" ;; esac
	@echo "for USB without sudo, also: sudo make install-udev"

install-udev:
	install -Dm644 99-pspdisp.rules $(DESTDIR)/etc/udev/rules.d/99-pspdisp.rules
	udevadm control --reload && udevadm trigger

clean:
	rm -f pspdisp
	rm -rf gen

.PHONY: install install-user install-udev clean
