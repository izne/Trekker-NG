# StemDeck

StemDeck is a stem-based DJ player: it plays a track as four separate stems
(drums, bass, melody, vocals) through one sample-locked playhead, with
vinyl-style pitch control, stem muting, and click-free transport - like the
stem features of Traktor / djay, but for your own exported stems.

Written in C++17. Windows first (MSYS2/MinGW64); the engine is kept portable
so Linux/macOS can follow.

**Current status: M2 of 6** - the engine is proven by an automated test suite;
the front-end is still a console (the ImGui UI arrives in M3).
The project is governed by [`SPEC.md`](SPEC.md).

| # | Milestone | Status |
|---|-----------|--------|
| **M1** | Pitch PoC (console): load stems, one playhead, per-stem mute, pitch ±10% with cubic Hermite | done |
| **M2** | Engine cleanup: rate smoothing, declick ramps, mixer extraction, unit tests | done |
| **M3** | ImGui UI, one deck, track-format loader, waveform overview | planned |
| **M4** | Second deck, crossfader, master section, hot cues, loops, beat display | planned |
| **M5** | Polish: 3-band EQ, limiter, crossfader curves, settings | planned |
| **M6** | Packaging: Windows build, Linux AppImage, docs, example tracks | planned |

## Features

- **Four stems, one playhead** - all stems share a single fractional playhead,
  so they stay sample-locked forever (no phase drift between stems).
- **Pitch ±10 %** - cubic Hermite (Catmull-Rom) interpolation, with a per-sample
  rate smoothing (one-pole, 15 ms) so pitch moves glide instead of stepping.
- **Click-free by construction** - 5 ms transport fade (play/pause), 2 ms
  crossfade on cue jumps while audible, 5 ms gain ramps on stem mutes.
- **Master mixer stage** - atomic master gain + hard clamp, applied identically
  to live output and offline renders (limiter arrives in M5).
- **Input formats** - WAV, FLAC, MP3, OGG Vorbis (whatever miniaudio decodes);
  recommended exports are WAV or FLAC (MP3 delay/padding can misalign stems).
- **Load from a folder or a zip** - loose files, or a zip with an enclosing
  folder; `meta.json` supplies title/artist/BPM/stem colors.
- **Offline render mode** - render to WAV without a soundcard (`--render`),
  used for tests and automation.
- **Self-contained executable** - the release binary is one static `.exe`
  (only Windows system DLLs).

## Quick start

### Run the prebuilt binary

```
dist\stemdeck.exe examples\magnat.zip
```

(`examples/magnat.zip` is a 4-stem example track: `drums/bass/melody/vocals`
at 128 BPM.)

### Build from source

Requirements: [MSYS2](https://www.msys2.org/) with the **MINGW64** toolchain.
In a MINGW64 shell:

```
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja
```

Then, inside the project directory (also MINGW64 shell):

```
./build.sh          # configure + build + run tests + copy to dist/
./build.sh release  # the same, plus dist/stemdeck-<version>-win64.zip
```

The runnable result is always `dist\stemdeck.exe`. Failing tests block the
build, so a `dist/` build is known-good.

## Command line

```
stemdeck <track.zip | track-folder> [options]

  --render <out.wav>  render offline to a WAV instead of playing (no soundcard)
  --rate <x>          playback rate for --render (e.g. 1.10 = +10%)
  --seconds <n>       seconds to render (default 5)
  --stems <bbbb>      4-bit stem mask to keep on while rendering (default 1111)
  --help              show usage
```

Example - render 6 seconds of the example track at -7 %, only drums and bass:

```
dist\stemdeck.exe examples\magnat.zip --render out.wav --rate 0.93 --seconds 6 --stems 1100
```

## Keyboard control (live mode)

| Key | Action |
|-----|--------|
| `Space` | play / pause (at track end: restart) |
| `1` `2` `3` `4` | toggle stem on/off |
| `Shift+1..4` (or `!@#$` on US layout) | solo stem (press again to unsolo) |
| `A` | all stems on |
| `=` `+` `]` | pitch up 0.10 % per press (hold `Shift`: 0.01 %) |
| `-` `_` `[` | pitch down 0.10 % per press (hold `Shift`: 0.01 %) |
| `0` | reset pitch to 0.00 % |
| `I` | toggle interpolator: cubic Hermite / linear (debug) |
| `Q` / `Esc` | quit |

Track loading happens at startup (only while the audio device is stopped);
drag & drop and hot loading arrive with M3.

## Track format

A track is a folder (or zip) containing up to 4 stereo audio files plus a
`meta.json`:

```
mytrack/
  meta.json
  drums.flac
  bass.flac
  melody.flac
  other.flac
```

```json
{
  "title": "Magnat",
  "artist": "izne",
  "bpm": 128.0,
  "stems": [
    { "name": "Drums",  "file": "drums.flac", "color": "#ff5555" },
    { "name": "Bass",   "file": "bass.flac",  "color": "#55ff55" },
    { "name": "Melody", "file": "melody.flac","color": "#5555ff" },
    { "name": "Vocals", "file": "other.flac", "color": "#ffff55" }
  ]
}
```

All files must be stereo and share one sample rate/length (missing stems are
simply silent). Full details: [`docs/FORMAT.md`](docs/FORMAT.md).

## Architecture

```
src/
  engine/            no UI dependencies; builds as static libstemdeck_engine
    deck.{h,cpp}             4 stems, one playhead, smoothing + declick
    interpolate.{h,cpp}      pure Hermite / linear interpolator
    mixer.{h,cpp}            master gain + clamp (crossfader in M4/M5)
    track_loader.{h,cpp}     folder/zip -> DeckData, meta.json
    audio_device.{h,cpp}     miniaudio device, realtime callback
  console/           M1/M2 console front-end (replaced by src/ui/ in M3)
tests/               doctest suite (SPEC §9), run automatically by build.sh
tools/               manual acceptance helpers (test-track generator)
examples/            example track
docs/                FORMAT.md, USAGE.md, DECISIONS.md
third_party/         vendored: miniaudio, miniz, nlohmann/json, doctest
```

Realtime rules (SPEC §4.7): the audio callback may not allocate, lock, do IO,
log, or throw. UI -> engine communication is atomics only (targets, stem
mutes, seek requests); the `tests/` suite enforces the no-allocation rule
with a global `operator new` counter.

Libraries (SPEC §3): **miniaudio** (device, decode, WAV encode), **miniz**
(zip), **nlohmann/json** (`meta.json`), **doctest** (tests). *Planned:*
Dear ImGui + SDL2 (M3 UI).

## Testing

```
./build.sh        # builds and runs the whole suite via ctest
```

The suite (SPEC §9) runs headless - no soundcard, no Python, everything is
synthesized in memory:

1. **Pitch accuracy** - 440 Hz sine at rate 1.10 measures 484 Hz ±0.5
2. **Stem lock** - 10 simulated minutes at 8 kHz under a random rate schedule;
   click peaks must stay inside their playhead-predicted windows (no drift)
3. **Mute ramp** - stem toggles never jump the waveform above the threshold
4. **Jump declick** - seek while playing is discontinuity-free
5. **Interpolator** - integer positions are bit-identical; rate 1.0 output
   equals the input byte-for-byte
6. **Realtime safety** - the render path performs zero heap allocations
   (plus transport pause/resume and mixer checks)

Manual acceptance (SPEC §9): listen at ±10 % on real music - it should sound
like pitching a record, with no warble, phasing, or digital smear.

## Documentation

- [`SPEC.md`](SPEC.md) - the governing specification (features, rules, milestones)
- [`docs/USAGE.md`](docs/USAGE.md) - end-user usage guide
- [`docs/FORMAT.md`](docs/FORMAT.md) - track folder/zip and `meta.json` reference
- [`docs/DECISIONS.md`](docs/DECISIONS.md) - why the project works the way it does
