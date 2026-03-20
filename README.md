# alsa-pcm-keepalive

ALSA external plugin that prevents audio signal loss on HDMI, SPDIF,
and USB outputs by maintaining continuous PCM output during idle.

When no audio is playing, the plugin feeds silence to the downstream
device. The hardware never sees the stream stop. Receivers, soundbars,
and DACs maintain signal lock permanently.


## The problem

Digital audio receivers (AVRs, soundbars, HDMI TVs, SPDIF decoders)
detect signal presence by monitoring incoming audio frames. When a
music player stops or pauses, the ALSA PCM stream closes and audio
frames cease. The receiver detects the loss and drops its audio link.

When playback resumes, the receiver must re-negotiate the connection.
This produces 1-5 seconds of silence at the start of every track,
after every pause, and on every stop/play cycle. The delay varies
by receiver model and is especially severe on HDMI, where audio
info frame negotiation adds overhead.

Kodi solved this years ago with a dedicated silence injection layer
in their audio engine (ActiveAESink). This plugin provides the same
functionality as a reusable ALSA component that works with any
application - MPD, GStreamer, PipeWire's ALSA backend, or anything
else that outputs through ALSA.


## How it works

The plugin is an ALSA ioplug that sits inline in the PCM chain
between the application and the output device:

    Application --> keepalive --> downstream device (hw, iec958, plug, etc.)

When the application writes audio, the plugin passes it through
unchanged. Zero processing, zero resampling, zero quality impact.

When the application stops or closes, the plugin feeds silence
frames to the downstream device. The PCM stream stays in RUNNING
state. Audio info frames continue. The receiver maintains lock.

When the application reopens and plays again, the plugin switches
back to passthrough. The transition is seamless within the hardware
buffer window (typically 50-500ms depending on configuration).

The downstream device connection persists across application
close/reopen cycles. The hardware never sees a gap unless the
audio format (sample rate, bit depth, channels) changes between
tracks.


## Use cases

- HDMI output to AVR/soundbar: eliminates re-negotiation delay on
  every play/stop cycle
- SPDIF output to external DAC: prevents receiver unlock/relock
- USB DAC: prevents DAC standby mode during brief pauses
- Any digital output where signal continuity matters


## Configuration

Add to /etc/asound.conf or ~/.asoundrc:

    pcm.keepalive_out {
        type keepalive
        slave.pcm "hw:0,0"
    }

Then point your application at "keepalive_out" instead of the
hardware device directly.

For more complex chains (e.g. with softvol or iec958 wrapping),
insert the keepalive plugin at the appropriate point:

    # example: keepalive before iec958 wrapper for HDMI
    pcm.keepalive_hdmi {
        type keepalive
        slave.pcm "hdmi_iec958"
    }

    pcm.hdmi_iec958 {
        type iec958
        slave.pcm "hw:vc4hdmi0"
        slave.format IEC958_SUBFRAME_LE
    }


## Building

### Dependencies

Build:
- libasound2-dev (ALSA library headers)
- gcc
- make

Runtime:
- libasound2 (present on any Linux system with ALSA)

No additional runtime dependencies.

### Native build

    make

### Cross-compile (e.g. for Raspberry Pi armhf)

    make CROSS_COMPILE=arm-linux-gnueabihf-

### Cross-compile for aarch64

    make CROSS_COMPILE=aarch64-linux-gnu-

### Strip for deployment

    make strip

### Docker multi-arch build

    chmod +x build.sh
    ./build.sh

Produces binaries for armhf, arm64, and amd64 in the dist/ directory.


## Installation

Copy the shared object to the ALSA external plugin directory:

    # determine the correct path
    ALSA_PLUGIN_DIR=$(pkg-config --variable=libdir alsa)/alsa-lib

    # install
    sudo install -m 0644 libasound_module_pcm_keepalive.so \
        ${ALSA_PLUGIN_DIR}/libasound_module_pcm_keepalive.so

Common paths:
- armhf: /usr/lib/arm-linux-gnueabihf/alsa-lib/
- arm64: /usr/lib/aarch64-linux-gnu/alsa-lib/
- amd64: /usr/lib/x86_64-linux-gnu/alsa-lib/


## Verification

After installation and asound.conf configuration:

    # play something, then stop it
    aplay -D keepalive_out /usr/share/sounds/alsa/Front_Center.wav

    # check PCM state after playback ends
    cat /proc/asound/card0/pcm0p/sub0/status

Should show "state: RUNNING" even after playback stops. Without
the plugin, it would show "closed".


## Technical detail

### Architecture

The plugin uses the ALSA ioplug (external I/O plugin) API. It
implements the mandatory callbacks (start, stop, pointer, transfer)
plus hw_params, prepare, drain, close, and poll handling.

A dedicated pthread feeds silence frames to the downstream device
when no audio is flowing from the upstream application. The thread
coordinates with the transfer callback via mutex and condition
variable to ensure silence writes and audio writes never overlap
on the slave PCM handle.

The downstream (slave) PCM connection is held in persistent static
state that survives application close/reopen cycles. When the
application closes the plugin, the slave stays open. When the next
application instance opens the plugin with matching format
parameters, it reclaims the existing slave connection without any
gap.

If the new instance requests different format parameters (sample
rate, bit depth, channels), the slave is closed and reopened with
the new configuration. This is the only scenario where a brief
gap occurs, and it is unavoidable because the hardware must
reconfigure for the new format.

### Thread safety

- Mutex (kd->mtx) protects the active/in_write flags
- Condition variable (kd->cond) synchronises start/stop transitions
- Persistent state mutex (persist.lock) protects slave PCM access
- The start callback waits for any in-progress silence write to
  complete before allowing audio passthrough
- No overlap between silence writes and audio writes is possible

### Error recovery

- EPIPE (buffer underrun): automatic snd_pcm_prepare() and retry
- ESTRPIPE (suspended): wait for resume, then prepare
- Slave open failure: error propagated to application
- Thread creation failure: error propagated to application

### Poll mechanism

Uses Linux eventfd for signaling. The ioplug framework polls on
the eventfd and calls transfer when data is available. The plugin
signals the eventfd after each successful write to indicate
readiness for more data.


## Prior art

- Kodi ActiveAESink (xbmc/cores/AudioEngine/Engines/ActiveAE/ActiveAESink.cpp):
  Application-level silence injection with state machine
  (S_TOP_CONFIGURED_SILENCE state). GenerateNoise() produces
  -100dB Gaussian white noise or zeros. Integrated into Kodi's
  audio engine, not reusable by other applications.

- MPD always_on parameter: Keeps ALSA handle open but calls
  snd_pcm_drop() which stops the PCM stream. No audio frames
  flow. Ineffective for HDMI/SPDIF where continuous frames
  are required.

- PulseAudio module-suspend-on-idle: Controls when sinks are
  suspended. Does not inject silence into the ALSA stream.

This plugin provides the Kodi approach as a standalone ALSA
component usable by any application.


## License

GPL-2.0-or-later

See LICENSE file for full text.
