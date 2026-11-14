# alsa-pcm-keepalive

Keeps HDMI, SPDIF, and USB outputs locked by writing a continuous PCM
stream. A small daemon owns the hardware. Players write through a thin
virtual ALSA PCM; the daemon forwards their samples unchanged and sends
inaudible Gaussian noise only while idle.

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

The daemon is the only writer on the output device. It forwards music
without altering the samples, waits for queued music to play before
acknowledging completion, and keeps writing noise while it waits. Mute
and volume sit upstream, so they never drop the hardware lock.

Format follows the last client (bit-perfect while playing). Idle keeps
that format. The hardware is reopened only when rate, bit depth, or
channel count changes.

This keeps a PCM stream active; it does not generate IEC61937 pause
bursts and is not an encoded Dolby/DTS passthrough keepalive.


## The Volumio plugin

`plugin/` is the Audio Keepalive plugin for Volumio 4 (bookworm). Its
one switch writes the ALSA contribution, rebuilds Volumio's ALSA
configuration and starts the daemon. Everything stays in the plugin
folder: the daemon for each architecture is in `bin/<arch>/`, the ALSA
client in `asound/<arch>/` beside the contribution that names it by
path, and the daemon's unit in `systemd/`. Nothing is installed under
the system's `alsa-lib` directory, and the install removes what 2.0.x
put in `/usr/bin` and `alsa-lib`. The zip carries no `node_modules`:
the plugin uses Node's own modules and Volumio's `kew` and `v-conf`.

Released plugins are on the
[releases page](https://github.com/foonerd/alsa-pcm-keepalive/releases):
`audio_keepalive-<version>.zip`, installed from Volumio's plugin page
(Upload Plugin) or with `volumio plugin install` from the unpacked zip.


## Configuration

Volumio's modular ALSA pipeline wires the contribution automatically.
Standalone /etc/asound.conf equivalent:

    pcm_type.keepalive {
        lib "/path/to/libasound_module_pcm_keepalive.so"
    }

    pcm.keepaliveProxy {
        type keepalive
        socket "/run/audio-keepalive/ctl.sock"
    }

    pcm.keepaliveProxyOut {
        type plug
        slave.pcm "hw:0,0"
    }

Without `pcm_type.keepalive`, ALSA looks for the client in its plugin
directory (`$(pkg-config --variable=libdir alsa)/alsa-lib`).

Start the daemon before any player:

    audio-keepalive-daemon --pcm keepaliveProxyOut --fallback-pcm hw:0,0

`--rate HZ` sets the sample rate the output idles at before a player asks
for one, 48000 unless given. A player whose first stream is 16-bit stereo at
that rate is taken on without the output being opened again, so the device's
clocks do not stop and start for it. When `asound.conf` changes, the output
is opened again once the file has been left alone for two seconds, not at
once: whatever rewrote it is often still at work on the device.

Then point the player at `keepaliveProxy`.


## Building

### Native build

Needs gcc, make and libasound2-dev; at runtime, libasound2.

    make
    make test

Produces `libasound_module_pcm_keepalive.so` and
`audio-keepalive-daemon`. Warnings are errors. `make test` runs the
daemon and the client against a clocked fake device (needs python3 and
aplay).

### The plugin

Needs Docker. The builder image (`scripts/builder/Dockerfile`) is
Debian bookworm, Volumio 4's glibc and ALSA, with the armhf and arm64
cross compilers.

    scripts/check.sh     # build, tests, analyzer, shell, JSON, i18n, workflows
    scripts/ship.sh      # bin/{arm,armv7,armv8,x64}/
    scripts/package.sh   # dist/audio_keepalive-<version>.zip

The version is `plugin/package.json`'s; the daemon reports the same one
(`audio-keepalive-daemon --version`).

### Releases

A `v<version>` tag that matches `plugin/package.json` and a
`CHANGELOG.md` section builds the release in GitHub Actions: the check,
the cross builds and the zip, signed with Ed25519 (`MANIFEST` and
`MANIFEST.sig` inside the zip, `SHA256SUMS` and `SHA256SUMS.sig` beside
it, verifiable with `keys/release-signing.pub`), published as a
pre-release with the changelog section as its notes.

    openssl pkeyutl -verify -pubin -inkey keys/release-signing.pub \
        -rawin -in SHA256SUMS -sigfile SHA256SUMS.sig
    sha256sum -c SHA256SUMS


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


## Credits

- [Kodi](https://kodi.tv)'s ActiveAE audio engine, whose single sink
  owner, rate-paced writes and explicit drain shaped the daemon.
- The [ALSA project](https://www.alsa-project.org) for the external
  PCM plugin (ioplug) interface.
- The Volumio team for the modular ALSA pipeline and the plugin
  framework.
- PluggedIn and the Volumio community, for reports and logs from
  receivers this was never tested on.


## Support

- Volumio Community forum:
  [Audio Keepalive thread](https://community.volumio.com/t/plugin-audio-keepalive-volumio-4-seven-years-one-toggle-zero-lost-seconds/75760)
- Bugs: [GitHub issues](https://github.com/foonerd/alsa-pcm-keepalive/issues),
  with a Volumio log link, the output device and the receiver or DAC.

Audio Keepalive is free and open source, designed, built and maintained
by one developer in his own time. If it has earned a place on your
player, you can support the work:

- GitHub Sponsors: https://github.com/sponsors/foonerd
- Ko-fi: https://ko-fi.com/justanerd

Sponsorship is a thank-you, not a purchase: it buys no priority, no
features and no support agreement.


## License

GPL-2.0-or-later. `LICENSE` carries the text of version 3, which the
"or later" permits.
