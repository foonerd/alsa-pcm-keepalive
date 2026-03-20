# libasound_module_pcm_keepalive - Makefile
#
# Native build:
#   make
#
# Cross-compile for Volumio 4 armhf (Raspberry Pi):
#   make CROSS_COMPILE=arm-linux-gnueabihf-
#
# Cross-compile for Volumio 4 arm64:
#   make CROSS_COMPILE=aarch64-linux-gnu-
#
# Dependencies (build host):
#   libasound2-dev (or target-arch equivalent for cross-compile)
#
# Dependencies (target runtime):
#   libasound2 (always present on Volumio)
#
# Install (target):
#   make install DESTDIR=/usr/lib/arm-linux-gnueabihf
#

PLUGIN   = libasound_module_pcm_keepalive.so
SRC      = libasound_module_pcm_keepalive.c

CC       = $(CROSS_COMPILE)gcc
STRIP    = $(CROSS_COMPILE)strip

CFLAGS   = -Wall -Wextra -O2 -fPIC
CFLAGS  += $(shell pkg-config --cflags alsa 2>/dev/null)

LDFLAGS  = -shared -Wl,-soname,$(PLUGIN)
LIBS     = $(shell pkg-config --libs alsa 2>/dev/null)
LIBS    += -lpthread

# Fallback if pkg-config unavailable (cross-compile with manual sysroot)
ifeq ($(filter -lasound,$(LIBS)),)
LIBS    += -lasound
endif

.PHONY: all clean strip install

all: $(PLUGIN)

$(PLUGIN): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

strip: $(PLUGIN)
	$(STRIP) $(PLUGIN)

clean:
	rm -f $(PLUGIN)

install: $(PLUGIN)
	install -d $(DESTDIR)/alsa-lib
	install -m 0644 $(PLUGIN) $(DESTDIR)/alsa-lib/$(PLUGIN)
