# Trekker-NG usage

A minimal DJ player for **producer-made stems**: each track is 4 stems sharing
one playhead, mute-able live, with Technics-style varispeed pitch (tempo and
pitch change together, no time-stretching).

## Requirements

- Windows 10+ (64-bit). `SDL2.dll` ships next to the exe - no runtime install.
  A soundcard, obviously.

## Run

```
trekker-ng.exe [deck-A.zip | deck-A-folder] [deck-B.zip | deck-B-folder]
```

The UI starts empty. Load a track by:

- **drag & drop** - drop the track folder or `.zip` anywhere on the window,
- **path box** - paste a path into the box at the bottom and press `Load`,
- **command line** - pass the path as the first argument (as above).

A second argument pre-loads deck B; it becomes visible when you switch to
Mix mode with `M`.

The track must be a folder or `.zip` containing `meta.json` + 1..4 stereo stem
files (WAV/FLAC/MP3). See `docs/FORMAT.md` for the format and
`examples/magnat.zip` in the repository for a ready-made track.

Loading runs on a worker thread and hot-swaps into the running engine - no
click, no restart. If the new track has a different sample rate, the audio
device restarts once at that rate.

## UI

| Element | Action |
|---|---|
| waveforms | composite on top + 4 stem lanes below (in stem colors, dimmed while muted); click any lane to seek (audible seeks are crossfaded); red line = playhead on all lanes |
| `Play` / `Pause` | transport (at track end, Play restarts from 0) |
| `CUE` | the full main-cue behavior (SPEC §4.5): press **while playing** to jump to the main cue and pause; press **while stopped** to set the cue at the current position and preview it - hold to keep playing, release to return to the cue and stop. The main cue is session-only and defaults to the track start |
| hot cues `1`-`8` | 8 cue slots per deck: click an empty slot to store the playhead, click a filled one to jump (declicked), right-click to clear. Edits save immediately (folder tracks rewrite `meta.json`, zips get a `.cues.json` sidecar) |
| loop `In` `Out` `On` `Exit` + `1`-`16` | manual loop control: `In`/`Out` set points at the playhead (lit when set), `On` toggles the loop (needs a valid pair), `Exit` clears it. The numbered buttons make a quick loop of that many beats starting at the playhead (needs a BPM in `meta.json`; near the track end the out point clamps to the end) |
| `VU` meter | per-deck peak bar of that deck's own output: green, yellow above 0.7, red above 0.9 |
| `1` `2` `3` `4` checkboxes | toggle that stem (colored as in `meta.json`) |
| pitch fader | vertical, ±10 %, reversed like DJ gear: **up = slower, down = faster**; the button under the readout toggles the range **±10 % / ±16 %**. `Shift`+click on the fader is a jog-style **nudge**: pitch bends to the clicked position (below the handle = faster) while held, release snaps back |
| time / bar / pitch | position / length, `bar 2.3 | 128.0 BPM` (bar.beat aligned to the beat grid via `first_beat_offset_ms`, plus the effective BPM), pitch % - all render as cyan 7-segment VFD readouts on a dark inset (DSEG7 font); text like `bar`, `BPM` and `+`/`%` keeps the regular UI font |
| path box + `Load` | load from a typed path |

### Single and Mix mode

The app always starts in **Single** mode - one deck, full width. Press `M` to
switch to **Mix** and back; the toggle lasts for the session only (persisting
the choice waits for the M5 settings screen).

Mix mode shows:

- **deck A** (left) and **deck B** (right), each with its own transport,
  pitch fader, VU meter and stem toggles,
- a **mixer column** between them: A/B line faders, a crossfader (starts
  centered) and the master gain,
- an **active deck** - the panel under the mouse, marked by its highlighted
  title. `Space`, the pitch keys, the pitch reset and the cue/loop keys below
  act on it,
- `1`-`4` always toggle deck A's stems, `7`-`0` deck B's,
- dropping a file or pressing `Load` targets the deck under the pointer.

### Keys

| Key | Action |
|---|---|
| `Space` | play / pause (restarting at the end jumps back to 0); in Mix mode: the active deck |
| `1` `2` `3` `4` | toggle stem on/off (click-free 5 ms ramp); deck A in Mix mode |
| `7` `8` `9` `0` | deck B stems 1-4 in Mix mode (`7`=1, `8`=2, `9`=3, `0`=4) |
| `=` `+` `]` | pitch **up** 0.10 % (hold `Shift`: 0.01 %) |
| `-` `_` `[` | pitch **down** 0.10 % (hold `Shift`: 0.01 %) |
| `0` | reset pitch to 0.00 % (Single mode; in Mix it is deck B's stem 4) |
| `Shift+1` … `Shift+8` | hot cue of the active deck: jump to a stored slot, or store one at the playhead if the slot is empty |
| `I` / `O` | set loop in / loop out at the playhead (active deck) |
| `L` | toggle the loop on / off (a pair must be set; the guard rejects incomplete or inverted points) |
| `Shift+L` | exit and clear the loop |
| `Alt+1` … `Alt+5` | quick loop of 1 / 2 / 4 / 8 / 16 beats from the playhead (active deck; needs a BPM) |
| `M` | toggle Single / Mix mode (session only - always boots Single) |
| `Q` / `Esc` | quit |

Keyboard pitch steps move the fader, so both controls always show the same
value. Digit shortcuts ignore Alt/Ctrl so a modified digit never toggles a
stem by accident; `Shift`+click on the fader nudges (see the UI table).

## Console front-end

`trekker-console.exe` is the keyboard-driven console version (same track
argument) with extra keys and the offline renderer:

| Key | Action |
|---|---|
| `Space` | play / pause (restarting at the end jumps back to 0) |
| `1` `2` `3` `4` | toggle stem on/off (click-free 5 ms ramp) |
| `Shift+1..4` | solo that stem (toggles back to all-on) |
| `A` | all stems on |
| `=` `+` `]` | pitch **up** 0.10 % (hold `Shift`: 0.01 %) |
| `-` `_` `[` | pitch **down** 0.10 % (hold `Shift`: 0.01 %) |
| `0` | reset pitch to 0.00 % |
| `I` | toggle interpolator (cubic Hermite / linear - debug) |
| `Q` / `Esc` | quit |

Pitch range is ±10 %; the status line shows pitch, effective BPM, position and
the 4 stem states.

### Options (console only)

```
--render <out.wav>   render offline to a WAV instead of playing (no soundcard)
--rate <x>           playback rate for --render (e.g. 1.10 = +10%)
--seconds <n>        seconds to render (default 5)
--stems <bbbb>       4-bit stem mask kept on while rendering (default 1111)
```

Example - pitch-accuracy check (renders the 440 Hz test stem at +10%, expect
~484 Hz):

```
trekker-console.exe temp\testtrack --render temp\out.wav --rate 1.10 --stems 1000
python tools\make_test_track.py        # generate temp\testtrack first
```

## Build from source

MSYS2 MINGW64 shell, from the repository root:

```
./build.sh            # configure + build + tests -> dist/
./build.sh release    # additionally pack dist/trekker-ng-<VERSION>-win64.zip
```

`dist/` contains `trekker-ng.exe` (UI), `trekker-console.exe` and
`SDL2.dll`.
