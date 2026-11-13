# Changelog

All notable changes to Audio Keepalive. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the plugin uses
[semantic versioning](https://semver.org/).

## [2.1.1] - 2026-11-13

### Fixed

- The release zip carried `node_modules`. The plugin now uses Node's own
  modules and Volumio's `kew` and `v-conf`, and packaging refuses a zip with
  node modules in it.

### Changed

- The ALSA client is in the plugin's `asound/<arch>/`, linked as
  `asound/libasound_module_pcm_keepalive.so` beside the contribution that
  loads it; the daemon stays in `bin/<arch>/`.
- The daemon's unit is shipped in `systemd/`.

## [2.1.0] - 2026-11-13

### Fixed

- Music lost with the keepalive on: the client is paced to the output, so a
  player no longer runs ahead of what the daemon can play. Reported on HDMI to
  an AV receiver; the fix is tested against a clocked fake device and still
  awaits confirmation on the reporter's receiver.
- Music no longer stutters while a stream plays: idle noise was written into
  the device whenever the queue was momentarily empty, splicing a period of
  silence between music periods. Noise is now added during a stream only when
  the device is genuinely running low.
- Idle noise is generated at the level asked for, in every sample format.
- Queued music plays out on pause, stop and close instead of being dropped; the
  drain no longer blocks the daemon, and noise keeps flowing while it waits.
- A short write to the device (common on HDMI and IEC958) is resumed where it
  stopped instead of losing the rest of the period.
- A client message that arrives in pieces no longer stalls the idle keepalive.

### Changed

- Music is passed through unchanged; inaudible noise is written only while
  nothing plays.
- The daemon and the ALSA client stay in the plugin folder. The ALSA
  contribution names the client by path (`pcm_type.keepalive`), so nothing is
  installed under `/usr/bin` or the system's `alsa-lib` directory, and the
  install removes what 2.0.x put there.
- Built, tested and signed from this repository: warnings are errors, GCC's
  analyzer runs on every build, and the release zip carries a signed manifest.
- `audio-keepalive-daemon --version`.
- Licence named correctly as GPL-2.0-or-later.

## [2.0.1] - 2026-08-22

### Fixed

- Daemon reopen on `EBUSY`; the client waits for `OPEN_OK` before writing.

## [2.0.0] - 2026-08-22

### Changed

- A single-writer daemon owns the output and the thin ALSA client forwards
  music to it, replacing the in-process switch between silence and the player.
