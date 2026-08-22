# alsa-pcm-keepalive

Keeps HDMI, SPDIF, and USB outputs locked by writing a continuous PCM
stream. A small daemon owns the hardware. Players write into a thin
virtual ALSA PCM that is mixed with inaudible (-100 dB) Gaussian noise.

There is no volume control and no silence/passthrough switch.


## The problem

Digital receivers detect signal presence from incoming audio frames.
When a player stops, ALSA closes the PCM and frames cease. The receiver
drops the link and re-negotiates on the next play — often 1–5 seconds
of silence, worst on HDMI.

An inline ALSA plugin that *switches* between a silence thread and the
player cannot do this reliably on Volumio: the plugin is loaded into
whatever process opened the device, persist state dies with that
process, and two writers sharing one IEC958/USB handle race.


## How it works

    Players --> pcm.volumio --> other contributions
            --> keepalive virtual PCM --> daemon
                                        ^
                         -100 dB noise -+
                                        v
                               keepaliveProxyOut --> hw

The daemon is the only writer on the output device. It always emits
noise. When a player is active, music is mixed in. Mute and volume sit
upstream, so they never drop the hardware lock.

Format follows the last client (bit-perfect while playing). Idle keeps
that format. The hardware is reopened only when rate, bit depth, or
channel count changes.


## Configuration

Volumio's modular ALSA pipeline wires the contribution automatically.
Standalone /etc/asound.conf equivalent:

    pcm.keepaliveProxy {
        type keepalive
        socket "/run/audio-keepalive/ctl.sock"
    }

    pcm.keepaliveProxyOut {
        type plug
        slave.pcm "hw:0,0"
    }

Start the daemon before any player:

    audio-keepalive-daemon --pcm keepaliveProxyOut --fallback-pcm hw:0,0

Then point the player at `keepaliveProxy`.


## Building

### Dependencies

Build:
- libasound2-dev
- gcc
- make

Runtime:
- libasound2

### Native build

    make

Produces `libasound_module_pcm_keepalive.so` and `audio-keepalive-daemon`.

### Cross-compile

    make CROSS_COMPILE=arm-linux-gnueabihf-
    make CROSS_COMPILE=aarch64-linux-gnu-

### Docker multi-arch

    chmod +x build.sh
    ./build.sh

Writes `dist/armhf/`, `dist/arm64/`, and `dist/amd64/`.


## Installation

    ALSA_PLUGIN_DIR=$(pkg-config --variable=libdir alsa)/alsa-lib
    sudo install -m 0644 libasound_module_pcm_keepalive.so \
        ${ALSA_PLUGIN_DIR}/libasound_module_pcm_keepalive.so
    sudo install -m 0755 audio-keepalive-daemon /usr/bin/audio-keepalive-daemon

On Volumio the plugin package installs both binaries, the systemd unit,
and the ALSA contribution.


## Verification

    # daemon must be running
    aplay -D keepaliveProxy /usr/share/sounds/alsa/Front_Center.wav

    # after playback ends the hardware PCM should stay RUNNING
    cat /proc/asound/card0/pcm0p/sub0/status

On any device, including analogue speakers, create `/data/keepalive`
to raise the mix from -100 dB to an audible test hiss (-30 dB by
default). Optional file contents: a dB value (`-24`, or `24`).
Clamped to -80…-12. Remove the file to return to silent keepalive.
The daemon picks the change up without a restart.


## License

GPL-2.0-or-later
