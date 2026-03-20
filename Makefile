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
#   Native: libasound2-dev
#   Cross (Debian multiarch): libasound2-dev:<target-arch>
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
LDFLAGS  = -shared -Wl,-soname,$(PLUGIN)
LIBS     = -lasound -lpthread

ifdef CROSS_COMPILE
  # Cross-compile: do NOT use host pkg-config (leaks host include paths).
  # Derive target triple from CROSS_COMPILE (strip trailing hyphen).
  #
  # ALSA headers are symlinked into the cross sysroot by Dockerfile.
  # The cross-gcc finds them there alongside target libc headers.
  # We only need the library path for linking.
  TARGET_TRIPLE := $(patsubst %-,%,$(CROSS_COMPILE))
  LDFLAGS += -L/usr/lib/$(TARGET_TRIPLE)
else
  # Native build: use pkg-config if available
  CFLAGS  += $(shell pkg-config --cflags alsa 2>/dev/null)
  _PKGLIBS := $(shell pkg-config --libs alsa 2>/dev/null)
  ifneq ($(_PKGLIBS),)
    LIBS = $(_PKGLIBS) -lpthread
  endif
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
