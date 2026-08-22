# audio-keepalive — daemon + thin ALSA PCM client
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

PLUGIN      = libasound_module_pcm_keepalive.so
DAEMON      = audio-keepalive-daemon
COMMON_SRC  = keepalive_common.c
PLUGIN_SRC  = libasound_module_pcm_keepalive.c $(COMMON_SRC)
DAEMON_SRC  = keepalive_daemon.c $(COMMON_SRC)

CC          = $(CROSS_COMPILE)gcc
STRIP       = $(CROSS_COMPILE)strip

# -DPIC selects the dynamic-build path in ALSA's global.h for
# SND_PCM_PLUGIN_SYMBOL(). Without it, the macro generates a
# static-build linked-list entry referencing snd_dlsym_start,
# which is absent from Volumio's RPi-patched libasound2.
CFLAGS      = -Wall -Wextra -O2 -fPIC -DPIC
PLUGIN_LDFLAGS = -shared -Wl,-soname,$(PLUGIN)
LIBS        = -lasound -lpthread -lm

ifdef CROSS_COMPILE
  TARGET_TRIPLE := $(patsubst %-,%,$(CROSS_COMPILE))
  PLUGIN_LDFLAGS += -L/usr/lib/$(TARGET_TRIPLE)
  DAEMON_LDFLAGS += -L/usr/lib/$(TARGET_TRIPLE)
else
  CFLAGS  += $(shell pkg-config --cflags alsa 2>/dev/null)
  _PKGLIBS := $(shell pkg-config --libs alsa 2>/dev/null)
  ifneq ($(_PKGLIBS),)
    LIBS = $(_PKGLIBS) -lpthread -lm
  endif
endif

.PHONY: all clean strip install

all: $(PLUGIN) $(DAEMON)

$(PLUGIN): $(PLUGIN_SRC) keepalive.h
	$(CC) $(CFLAGS) $(PLUGIN_LDFLAGS) -o $@ $(PLUGIN_SRC) $(LIBS)

$(DAEMON): $(DAEMON_SRC) keepalive.h
	$(CC) $(CFLAGS) $(DAEMON_LDFLAGS) -o $@ $(DAEMON_SRC) $(LIBS)

strip: $(PLUGIN) $(DAEMON)
	$(STRIP) $(PLUGIN) $(DAEMON)

clean:
	rm -f $(PLUGIN) $(DAEMON)

install: $(PLUGIN) $(DAEMON)
	install -d $(DESTDIR)/alsa-lib
	install -m 0644 $(PLUGIN) $(DESTDIR)/alsa-lib/$(PLUGIN)
	install -d $(DESTDIR)/bin
	install -m 0755 $(DAEMON) $(DESTDIR)/bin/$(DAEMON)
