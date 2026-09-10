TARGET = pspdisp

# App objects + the USBHostFS import stub (USBHostFS.S supplies the NID stubs
# for usbReadRawBulkData/usbWriteRawBulkData/usbAsyncRegister; the real code
# lives in usbhostfs.prx, built separately).
OBJS = main.o display.o jpeg.o usb.o com.o utils.o usbhostfs/USBHostFS.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -std=gnu11 -fcommon
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

# Only the libs build.mak does NOT add by default. It already appends
# -lpspdebug -lpspdisplay -lpspge -lpspctrl, and the specs add -lpspuser/-lpspsdk
# /-lpspkernel, listing those again makes duplicate import stubs ("stubs out of
# order"). pspDebugScreen (overlay) rides on the auto-added -lpspdebug.
LIBS = -lpspgum -lpspgu -lpspjpeg -lpsputility -lpsppower -lpspusb -lpspkubridge -lm

LDFLAGS =

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = pspdisp-min
PSP_EBOOT_ICON = ICON0.PNG

PSP_FW_VERSION = 371
BUILD_PRX = 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
