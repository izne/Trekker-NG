# DECISIONS.md

Ambiguous requirements (per SPEC §10.3) resolved here instead of stopping to ask.

## 2026-10-04 — M1 (Pitch PoC, console)

- **Console front-end lives in `src/console/`.** SPEC §7 lists `src/ui/`, but that is
  the ImGui UI starting at M3. M1 is explicitly a console PoC.
- **Pitch keys (user-confirmed):** `=`/`+`/`]`/`}` raise pitch, `-`/`_`/`[`/`{` lower it.
  Step is **0.10 %** per press, **0.01 %** while Shift is held. `0` resets to 0.00 %.
  Pitch range is fixed at **±10 %** for M1 (SPEC §4.3 default); the ±8/10/16 % range
  selector is a later milestone.
- **Track loading happens only while the audio device is stopped** (at startup in M1).
  The lock-free "publish pointer + free old data elsewhere" swap from SPEC §4.7 is
  needed for *hot* loading and arrives with M3; M1 needs no locks because it never
  loads concurrently with playback.
- **`mixer.{h,cpp}` deferred to M4.** In M1 the master gain lives in `audio_device`
  (one deck, no crossfader yet).
- **Master gain is 0.7 with a hard clamp** until the real limiter lands in M5
  (SPEC §4.6/§8). Cheapest thing that keeps the DAC from wrapping.
- **miniz is vendored as its 10 upstream source files** (miniz 3.1.2 is a split source
  tree, not a 2-file amalgamation) plus a hand-written `miniz_export.h` stub. Upstream
  generates that header with CMake; for a static build the export macros are empty.
- **Cues/loops in `meta.json` are not parsed yet** (M4). Unknown fields are ignored by
  design (SPEC §5), so this is forward-compatible.
- **Solo keys:** `Shift+1..4` sends `!@#$` on a US layout, so the console maps
  `!@#$` to solo of stem 1..4; plain `1..4` toggles, `A` = all on.
- **Test track is written as WAV**, not FLAC: miniaudio decodes both, and Python's
  stdlib writes WAV without extra packages (SPEC §5 allows WAV).
- **Rate smoothing and declick ramps are intentionally absent** — they are M2 scope
  (SPEC §8). M1 steps pitch in ≥0.01 % increments, which is inaudible as zipper noise
  at keyboard repeat rates.

## 2026-10-04 - M2 (declick, mixer, tests)

- **`mixer.{h,cpp}` extracted now (supersedes the M1 deferral to M4):** SPEC �7 puts it
  in `src/engine/`, and M2 is the "engine library cleanup" milestone. `Mixer::process()`
  does master gain (atomic, 0.7) + hard clamp only; `AudioDevice::init()` now takes the
  mixer, and `--render` runs the same mixer after `Deck::render()` - so offline renders
  and live output are gain-identical (M1's render path was unity-gain and could clip).
- **Rate smoothing is a per-sample one-pole, t = 15 ms** (SPEC �4.3): the UI writes a
  target, `render()` glides `rateSmoothed_` toward it. Per-block stepping would still
  zipper on big fader moves.
- **Declick constants:** transport fade 5 ms (`kRampSeconds`), seek crossfade 2 ms
  (`kDeclickSeconds`). Seek while audible crossfades old->new position; seek while
  silent moves instantly. Pause ramps `outputGain_` to 0 instead of cutting.
- **Tests synthesize everything in memory** - no Python, no files, no soundcard at test
  time (the `temp/`-based tooling stays for manual acceptance). The stem-lock test
  renders 10 simulated minutes at 8 kHz in 8-frame blocks and matches detected click
  peaks against playhead-crossing windows.
- **Thresholds were derived, not guessed:** sample-step checks allow the waveform's own
  derivative (440 Hz sine max step ~0.031) plus ramp contribution => 0.05, seek => 0.08
  (crossfade blend adds ~0.012); the click-peak threshold 0.9 comes from the worst-case
  Hermite phase at the �10 % rate extremes (~0.486/stem summed).
- **The stem-lock test compares against the final playhead position**, not the click
  count: the random rate schedule is a random walk, so the track end is approached from
  slightly above or below ~10 clicks' worth of frames either way.
- **doctest v2.4.12 vendored as `third_party/doctest.h`** (SPEC �3 allows doctest or
  Catch2); one ctest entry `engine` runs the whole suite; failing tests block `dist/`.
- **The �9.6 allocation counter replaces only `operator new`:** counting needs `new`,
  and libstdc++'s default `operator delete` (free) pairs with our malloc anyway;
  replacing `delete` too only earns a -Wmismatched-new-delete false positive.

## 2026-10-04 - Rename to Trekker-NG

- **Working name changed from StemDeck to Trekker-NG** (user decision; matches
  the GitHub repo `izne/Trekker-NG`). Renamed everywhere user-visible: exe and
  release zips (`trekker-ng.exe`, `trekker-ng-<version>-win64.zip`), CMake
  project/targets (`trekker-ng`, `trekker_engine`, `trekker_tests`, option
  `TREKKER_BUILD_TESTS`), banner/usage text, all docs, and the engine namespace
  `sde::` -> `tng::`. Vendored `third_party/` names were left alone.
- **Release zips now include `README.md`** at the zip root alongside `docs/`
  (FORMAT.md, USAGE.md), so a standalone download documents itself - including
  the new "Running the example track" chapter for `examples/magnat.zip`.
