# Trekker-NG: Spec & Requirements

A minimal DJ player for **producer-made stems** with **Technics-style vinyl pitch**.
Each track is a set of 4 stereo stems (drums, bass, melody, "other", names are free-form) that play in perfect sync and can be switched on/off live while mixing two decks.

Origin: tracker / Buzz / Reason background. A song is a set of channels, and a DJ should be able to mute and swap them. This is a fun instrument first, a product never.

---

## 1. Goals

1. Play **4 stems per deck** sample-locked (one shared playhead, zero drift).
2. **Natural varispeed pitch** like a Technics SL-1210: tempo and pitch change together, with no time-stretch artifacts.
3. Per-stem **on/off (mute)** with click-free ramps. Optional per-stem gain.
4. **Two decks**, a crossfader and a master output.
5. **Cue points** and **loops**.
6. Open, simple, documented **track format** (folder or zip with a `meta.json`).
7. Small, readable codebase that one person can understand in an afternoon.

## 2. Non-goals (for now)

- No track library or database, no streaming services, no recording.
- No AI stem separation (stems come from the producer's DAW).
- No effects, no keylock in v1 (see Future).
- No MIDI controller support in v1 (see Future).
- No mobile or web targets.

## 3. Tech stack (decided, don't re-litigate)

| Concern | Choice |
|---|---|
| Language | C++17 |
| Build | CMake (single `CMakeLists.txt`, dependencies via `FetchContent` or vendored single headers) |
| Audio I/O and decoding | **miniaudio** (single header, public domain). Use `ma_decoder` for decoding and `ma_device` for output |
| UI | **Dear ImGui** + SDL2 (OpenGL3 backend). Immediate-mode, minimal code |
| JSON | nlohmann/json (or any single-header equivalent) |
| ZIP reading | **miniz** (single file, public domain), for `.zip` tracks |
| Tests | Catch2 or doctest, for the engine only |
| Toolchain | **MSYS2 MINGW64** (GCC, `mingw-w64-x86_64-*` packages), CMake + Ninja |
| Platforms | Windows first (primary, the only one tested for now). Keep the code portable so Linux/macOS can follow later |

### 3.1 Development environment (Windows)

- MSYS2 is installed at `D:\msys64`. The toolchain to use is in **`D:\msys64\mingw64\bin`** (the MINGW64 environment, not UCRT64 or MSVC). Make sure that directory is on `PATH` for all build commands.
- Use `mingw-w64-x86_64-*` packages (not `ucrt64`). If a package is missing, tell the user the exact `pacman -S` command instead of installing silently.
- Build with CMake + Ninja (`-G Ninja`), compiler `gcc`/`g++` from the path above. Don't use MSVC-style commands or flags.
- Use only the `MINGW64` shell consistently. Don't mix packages from other MSYS2 environments.
- Libraries that aren't in pacman (miniaudio, Dear ImGui, nlohmann/json, miniz, doctest/Catch2) are vendored or fetched with CMake `FetchContent`. SDL2 comes from pacman (`mingw-w64-x86_64-SDL2`).

**Architecture rule:** the engine (`src/engine/`) must have **no UI dependency** and compile as a static library. The UI (`src/ui/`) only talks to the engine through a small thread-safe API.

## 4. Audio engine requirements

### 4.1 Signal flow

```
Deck A: 4 stems ──(shared playhead, rate)──► stem gains ──► sum ─┐
                                                                 ├─► crossfader ─► master gain ─► soundcard
Deck B: 4 stems ──(shared playhead, rate)──► stem gains ──► sum ─┘
```

### 4.2 Deck / playhead

- Each deck owns 4 stereo float32 buffers of **equal length**, fully decoded to RAM on load (streaming is a later optimization).
- One `double playhead` (fractional sample position) per deck. All 4 stems are read at the same position, so they cannot drift.
- Per output frame: read all 4 stems at `playhead`, apply gains, sum, then `playhead += rate`.
- `rate` = playback speed ratio (1.0 = normal). It is set by the pitch fader.
- Reaching the end of the buffer stops the deck (or wraps, if a loop is active). Reaching the start while reversing is clamped.

### 4.3 Pitch (the critical part)

- **Varispeed only.** Rate change alters both tempo and pitch, with no time-stretching or pitch-preserving algorithm in v1.
- Interpolation: **cubic Hermite (Catmull-Rom)** by default. Provide linear as a debug option. Interpolator is a pure function, unit tested.
- Pitch range: **±10% / ±16%** in v1 (default ±10%, toggled under the fader, persisted via the settings screen); the ±8% option is deferred. Fader resolution at least 0.01%.
- **Rate smoothing:** rate changes are smoothed per sample or per block (one-pole or linear ramp, ~10 to 20 ms) to avoid zipper noise.
- Display: current pitch in % (e.g. `+3.42%`) and effective BPM (`bpm * rate`).
- **Reverse pitch fader direction** is a setting (M5c), not a hardcoded assumption: default DJ-style (up = slower), optionally up = faster; persisted with the rest of the settings and applied to the fader, the nudge math and the keyboard pitch.
- **Acceptance test:** a pure 440 Hz sine stem played at +10% must measure about 484 Hz (±0.5 Hz) with no audible artifacts, and all 4 stems must remain phase-aligned (see §9).

### 4.4 Stem on/off

- Each stem has a target gain (0 or 1, optionally continuous 0..1).
- The applied gain is **ramped over about 5 ms** toward the target to avoid clicks.
- Also provide **solo** (mutes the others) and "all on".
- Stem toggles must be triggerable from the UI thread without locks (atomics).

### 4.5 Cues and loops

- **Cue points:** at least 8 hot cues per deck. Set, trigger (jump the playhead), delete. Stored in the track's `meta.json` (or a sidecar if the container is read-only).
- **Main cue / "CUE" button:** standard behavior (set at the current position when stopped, hold to preview, release to return).
- **Loops:** in/out points, a loop on/off toggle, and quick loops of 1/2/4/8/16 beats (needs the beat grid).
- All jumps are sample-accurate. Add a tiny declick crossfade (about 2 ms) on playhead jumps.

### 4.6 Mixer

- Per-deck volume (line fader), crossfader (selectable curve: linear, constant-power, sharp-cut - the cut curve stays full until the last 5 % of the fader travel), master gain, soft limiter on the master to prevent clipping (bit-exact below a 0.9 knee, smooth asymptote to +-1 above it; a CLIP light in the mixer column flashes when it engages).
- 3-band EQ per deck (M5a): LR4 crossover band-split at 250 Hz / 4 kHz, Traktor-style thin vertical sliders side by side (center = 0 dB, top = +6 dB boost, bottom = kill) with a kill toggle square under each band (lit while killed; clicking again restores the previous level), one set per deck in the Mix-mode mixer column. The single-deck audio path (console, tests) bypasses it.

### 4.7 Realtime rules (non-negotiable)

The audio callback must **never**: allocate, lock a mutex, touch disk, log via `printf`/iostream, or throw.

- UI-to-audio communication uses `std::atomic` values or a lock-free SPSC queue.
- Buffer swaps (loading a new track) are done by preparing the new deck data off-thread, then publishing it with an atomic pointer swap. The old data is freed on a non-audio thread.
- Process in float32 internally. Output buffer size is configurable (default 256 frames, with target latency under 15 ms where the OS allows); M5c makes it a persisted settings choice (64/128/256/512/1024).

## 5. Track format ("Trekker-NG track", v1)

A **folder or a `.zip`** (the loader supports both) containing:

```
mytrack/
  meta.json
  drums.flac
  bass.flac
  melody.flac
  other.flac
```

**Audio files:** stereo, same sample rate, same length (±1 frame tolerance, padded to the longest). Supported: WAV, FLAC, MP3, OGG Vorbis (whatever miniaudio decodes). **Recommendation in docs:** WAV or FLAC for exports, because MP3 encoder delay/padding can misalign stems.

**`meta.json`:**

```json
{
  "format_version": 1,
  "title": "My Song",
  "artist": "Me",
  "bpm": 128.0,
  "first_beat_offset_ms": 42.5,
  "stems": [
    { "name": "Drums",  "file": "drums.flac",  "color": "#ff5555" },
    { "name": "Bass",   "file": "bass.flac",   "color": "#ffaa00" },
    { "name": "Melody", "file": "melody.flac", "color": "#55aaff" },
    { "name": "Volcas", "file": "other.flac",  "color": "#66dd88" }
  ],
  "cues": [
    { "name": "Intro", "position_ms": 0 },
    { "name": "Drop",  "position_ms": 61250 }
  ],
  "loops": []
}
```

- 1 to 4 stems are accepted in v1 (missing stems are silent). The number is fixed at 4 in the UI.
- Beat grid in v1 = constant BPM plus a first-beat offset (the producer knows both from the DAW).
- Unknown fields must be ignored (forward compatibility). Missing optional fields get defaults.
- Write `docs/FORMAT.md` describing the format so third parties can create tracks.

## 6. UI requirements (v1, functional over pretty)

- **Two modes:** Single (one deck, full width) is the boot default; `M` toggles
  Mix (two deck panels side by side, mixer column between them: A/B line
  faders flanked by the per-deck VU meters (M5g, each deck's meter beside its
  own fader), EQ, crossfader, master). M5c: the last used mode is persisted
  and restored at the next boot
  (the settings screen also has a "start in Mix mode" checkbox for it).
  v1 is exactly 2 decks.
- In Mix mode the **active deck** is the panel under the mouse (highlighted
  title); the global transport/pitch/cue keys target it.
- Per deck: track title, 4 stem toggle buttons (colored, lit when on),
  play/pause, CUE (set / hold-to-preview / return, §4.5), 8 hot cue buttons,
  loop controls (In/Out/On/Exit + 1/2/4/8/16-beat quick loops), vertical pitch
  fader with a range toggle (v1: ±10 / ±16 %) and a persisted direction
  (§4.3, M5c); M5e stacks the deck's bottom panel as a left column (transport,
  the remaining-time + pitch VFD row below Play/CUE, the `bar N.M | BPM`
  counter, hot cues, loops, stem toggles, `Load...`) and a right column (pitch
  fader and nudge slider; M5g: the per-deck VU meter only in Single mode - in
  Mix mode the meters stand in the mixer column, each deck's beside its own
  A/B line fader) whose sliders are sized to the left column's height. The
  time+pitch readout renders at a persisted size (M5e,
  12-32 px, default 20). M5e: the nudge is a dedicated slider beside the pitch
  fader - click above/below its center and hold for a momentary pitch bend
  (direction follows the fader setting), release snaps back; M5f: the handle
  jumps to the clicked value while held and snaps back to center on release;
  M5g: the bend is an offset added to the pitch playing at the press
  (minimum 0.5 %), so the handle shows that offset itself (center = the
  current pitch) and release restores the pitch as it was at the press.
  Shift+click on
  the pitch fader no longer nudges. `Load...` opens a native zip file dialog
  (folders still arrive via drag & drop or the CLI).
- **Settings screen** (M5c, `[settings]` next to the mode button): audio
  device (miniaudio enumeration, default = system), buffer size, pitch
  range, reversed-pitch-fader checkbox, boot mode, crossfader curve, master
  gain, and (M5e) the time+pitch readout size (12-32 px slider). Immediate
  apply + write-through save to `trekker-ng.json` next to
  the exe (nlohmann JSON, portable-style, same stack as meta.json); device
  and buffer changes restart the audio device (brief dropout).
- **Window state** (M5f): default 1150 x 660 (wide enough that the
  Mix-mode VU meters are never clipped), resizable. The windowed size and
  the fullscreen flag persist in the same `trekker-ng.json` and restore at
  boot; `F11` toggles borderless fullscreen. The size is written at exit
  and on toggle only - maximized/fullscreen sizes never overwrite the
  windowed restore dimensions. Tooltips teach state, never click actions
  (M5f/M5g): right-click resets are documented instead - EQ sliders reset to
  flat (M5a), the crossfader centers (M5f) - and the EQ kill buttons carry no
  tooltip at all (M5g; the band slider already names band and dB).
- **Waveform:** overview (summed stems plus per-stem lanes) with a playhead;
  pre-computed min/max peaks on load, click-to-seek. A stretch goal is a
  zoomed scrolling waveform.
- Keyboard shortcuts for everything (a laptop is the controller for v1):
  `Space` play/pause, `1`-`4`/`7`-`0` stems, pitch keys `=+-_[]` + `0` reset,
  `Shift+1`-`8` hot cues, `I`/`O`/`L`/`Shift+L` loop keys, `Alt+1`-`5` quick
  loops, `M` mode, `F11` fullscreen (M5f), `Q`/`Esc` quit. A configurable
  map is nice but not required.
- Drag and drop a track folder or zip anywhere on the window (routes to the
  deck under the pointer); or the `Load...` native zip dialog per deck (M5e;
  folders = drop/CLI only); or CLI arguments.

## 7. Project structure

```
trekker-ng/
  CMakeLists.txt
  build.sh             (one-command rebuild in the MINGW64 environment)
  AGENTS.md            (copy of this file or a pointer to it)
  temp/                (agent scratch space, git-ignored, the ONLY place for scratch writes)
  build/               (CMake build tree, git-ignored)
  dist/                (latest runnable build for the user to test, self-contained)
  docs/FORMAT.md
  src/
    engine/            (no UI deps, builds as libtrekker_engine)
      deck.{h,cpp}
      interpolate.{h,cpp}
      eq.{h,cpp}
      mixer.{h,cpp}
      settings.{h,cpp}    (M5c: trekker-ng.json load/save, clamped fields)
      track_loader.{h,cpp}
      audio_device.{h,cpp}
    ui/
      main.cpp
      deck_view.{h,cpp}
  tests/
  tools/
    make_test_track.py   (generates sine/click test stems, see §9)
  examples/
```

## 8. Milestones (each ends with something runnable)

| # | Milestone | Done when |
|---|---|---|
| **M1** | **Pitch PoC (console).** Load 4 audio files, one playhead, per-stem mute via keys, pitch ±10% with cubic Hermite, output via miniaudio. | You can load your old exported song, toggle stems, and move pitch, and it sounds like vinyl. **This milestone answers the original question.** |
| **M2** | Engine library cleanup, rate smoothing, declick ramps, unit tests for the interpolator and deck. | The tests in §9 pass. |
| **M3** | ImGui UI with one deck, the track format loader (`meta.json`, folder and zip), waveform overview. | A track loads by drag and drop. |
| **M4** | Second deck, crossfader, master section, hot cues, loops, beat display. | You can do a full mix of two tracks. |
| **M5** | Polish: 3-band EQ, limiter, crossfader curves, settings (audio device, buffer size, pitch range, readout size), pitch-fader direction, nudge slider, native load dialog, window state (size + F11 fullscreen), mixer-column VU meters. | A friend can use it without you explaining. |
| **M6** | Packaging: Windows build and Linux AppImage, docs, example tracks. | Ready for jamming with friends. |

Work on **one milestone at a time**. Do not start the next one until the current one builds, runs, and is committed. Every milestone ends with a fresh runnable build in `dist/` for the user to test (see §10.2).

## 9. Testing & acceptance

`tools/make_test_track.py` generates a test track (output goes to `temp/`, never outside the project): stem 1 is a 440 Hz sine, stem 2 an 880 Hz sine, stem 3 a click track at the track BPM, stem 4 a 110 Hz sine (all stereo, 44.1 kHz, same length, with a `meta.json`).

Automated (engine, headless, no soundcard needed, render offline to a buffer):

1. **Pitch accuracy:** render 5 s at rate 1.10, measure the frequency (zero-crossing or FFT), expect 484 Hz ±0.5.
2. **Stem lock:** render with rate varying randomly over time, then verify that the click stem's peak positions stay aligned with the playhead (no cumulative drift between stems over 10 minutes of simulated time).
3. **Mute ramp:** toggling a stem produces no sample-to-sample jump above a defined threshold (no clicks).
4. **Jump declick:** cue jumps produce no discontinuity above the threshold.
5. **Interpolator:** at rate 1.0 the output equals the input exactly (bit-identical for integer positions).
6. **Realtime safety:** a debug build option that asserts there are no allocations in the callback (or review by inspection with a checklist).

Manual: listen test at ±10% on real music (the original exported song). It should sound like pitching a record, with no warble, phasing, or "digital" smear.

## 10. Rules for the agent

### 10.1 File system safety (hard rules)

- **Write only inside the project directory.** Scratch files, generated test tracks, downloads, build experiments, and logs go in the project's **`temp/`** folder. Add `temp/` to `.gitignore`.
- **Never write, move, or delete anything outside the project folder without asking the user first**, and wait for an explicit yes. This includes system folders, the MSYS2 installation, the user profile, the registry, and global config files.
- Do not run `pacman -S`, `pip install --user`, or any global install on your own. Tell the user the exact command and let them run it.
- Never run destructive commands (`rm -rf`, `del /s`, `git clean -fdx`, `git reset --hard`) on anything except `temp/` and the build directory. Double check the path before running them.
- Don't read or touch the user's music files except the ones explicitly pointed to for testing. Work on copies in `temp/` when in doubt.

### 10.2 Deliverable: always leave a runnable build

- At the **end of every milestone** (and after any significant change), the agent must leave a **freshly compiled application** that the user can run and test right away. Build it in `build/` and, when it succeeds, copy the runnable files (exe plus any needed DLLs) into **`dist/`**.
- `dist/` must be self-contained: double-clicking the exe works without MSYS2 on `PATH` (copy the required runtime DLLs from `D:\msys64\mingw64\bin` into `dist/`, or link statically with `-static`). Check this by listing the DLL dependencies (`ldd` / `objdump -p`).
- Provide a `build.sh` (or `build.bat` that calls the MINGW64 shell) so the user can rebuild with one command.
- Finish each milestone by telling the user: the exact path of the exe, how to run it (including command-line arguments, e.g. a track path), what to test, and what to listen for. If the build fails, say so clearly and don't pretend it works.
- Never leave `main` or `dist/` in a broken state. If a change breaks the build, fix it or revert it before stopping.

### 10.3 General conduct

- Read this file fully before writing code. Follow §3: don't swap libraries.
- Keep dependencies minimal and the code simple. Prefer clear code over clever code. Comment the *why* in DSP code.
- Commit after each working step with a clear message. Keep `main` always buildable.
- When a requirement is ambiguous, pick the simplest reasonable option, note it in `docs/DECISIONS.md`, and continue instead of stopping to ask.
- Never put UI code in the engine, and never break the realtime rules in §4.7.
- Show how to build and run at the end of each milestone (exact commands).

## 11. Future ideas (explicitly out of scope until v1 is fun)

- **Keylock / master tempo** (Rubber Band or SoundTouch) as an optional toggle, with varispeed staying the default.
- Beat sync / tempo matching between decks (using the BPM from `meta.json`).
- MIDI controller mapping (RtMidi) and a jog wheel with scratching and nudge.
- Per-stem filters and effects, and per-stem volume faders.
- A DAW-side export helper: a script or Reaper action that renders stems and writes `meta.json`.
- Streaming decode and large-library support.
- Opus stem support (needs libopus).
