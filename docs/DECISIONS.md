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
