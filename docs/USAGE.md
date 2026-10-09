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
- **`Load...` button** - opens the native Windows zip file dialog (M5e);
  folders still load via drag & drop or the command line,
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
| `VU` meter | per-deck peak bar of that deck's own output: green, yellow above 0.7, red above 0.9. Where it stands follows the mode (M5g): in the deck's right column in Single mode; in the mixer column beside the deck's own A/B line fader in Mix mode (deck A's meter left of its fader, deck B's right of its own) |
| `1` `2` `3` `4` checkboxes | toggle that stem (colored as in `meta.json`) |
| pitch fader | vertical, ±10 % by default; direction is a setting (M5c): reversed like DJ gear (**up = slower, down = faster**, the default) or straight (up = faster); the button under the fader toggles the range **±10 % / ±16 %** (persisted) |
| nudge slider | (M5e, beside the pitch fader) a momentary **pitch bend**: click above or below the center handle and hold - pitch bends by an offset from the pitch playing at that moment, toward the clicked side (minimum 0.5 %; direction follows the fader setting, so reversed/down = faster), the time readout shows the bent value, and while held the handle shows the offset itself (center = the current pitch); release restores the pitch as it was when you pressed and the handle returns to center (M5f centered the handle, M5g made the bend relative). Replaced the M4d `Shift`+click nudge |
| time / bar / pitch | remaining time with a minus sign (`-02:33.75`, total - position) and pitch % next to it, `bar 2.3 | 128.0 BPM` below (bar.beat aligned to the beat grid via `first_beat_offset_ms`, plus the effective BPM) - all render as cyan 7-segment VFD readouts on a dark inset (DSEG7 font); text like `bar`, `BPM` and `+`/`%` keeps the regular UI font. M5e: the time+pitch row sits below Play/CUE at a configurable size (settings, 12-32 px, default 20); the bar row stays at 15 px |
| `Load...` | opens the native zip file dialog for this deck |

### Single and Mix mode

The app starts in the last used mode (M5c persists it; a fresh install boots
**Single** - one deck, full width). Press `M` to
switch to **Mix** and back; the choice is written to `trekker-ng.json`
immediately.

Mix mode shows:

- **deck A** (left) and **deck B** (right), each with its own transport,
  pitch fader, nudge slider and stem toggles (M5g: the VU meter moved out
  of the deck panel into the mixer column),
- a **mixer column** between them: A/B line faders with the per-deck **VU
  meters** beside them (M5g: deck A's meter stands left of its fader, deck
  B's right of its own), a per-deck **3-band EQ**
  (three thin vertical sliders labeled `lo / mid / hi` side by side: center
  = flat, top = +6 dB, bottom = kill; a square kill button under each band
  toggles kill and lights up while killed; clicking it again restores the
   level the band had before the kill. Right-click a slider resets
   to flat), a crossfader (starts centered; right-click it to snap it back
   to center) with a curve selector - `lin`
  (linear), `pw` (constant power, the default), `cut` (sharp cut: full
  volume until the last 5 % of the travel; persisted since M5c) - and the
  master gain plus a red `CLIP` light that flashes when
  the master soft-clip limiter engages (the output bends smoothly toward
  +-1 instead of hard-clipping),
- an **active deck** - the panel under the mouse, marked by its highlighted
  title. `Space`, the pitch keys, the pitch reset and the cue/loop keys below
  act on it,
- `1`-`4` always toggle deck A's stems, `7`-`0` deck B's,
- dropping a file or pressing `Load...` targets the deck under the pointer.

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
| `M` | toggle Single / Mix mode (persisted - next boot restores it) |
| `F11` | toggle fullscreen (borderless; persisted - next boot restores it) |
| `Q` / `Esc` | quit |

### Settings (`[settings]` next to the mode button)

A small window (M5c) with, top to bottom:

| Control | What it does |
|---|---|
| `audio device` | playback device dropdown from miniaudio's enumeration (`(default)` = system default); changing it restarts the audio device (brief dropout) |
| `buffer size` | callback size in frames: 64 / 128 / 256 (default) / 512 / 1024; smaller = lower latency, more load; changing it restarts the audio device |
| `pitch range` | ±10 % or ±16 % for both decks' faders (same value the per-deck button toggles) |
| `reversed pitch fader` | checked (default) = DJ-style up = slower; unchecked = up = faster |
| `start in Mix mode` | boot mode (same value `M` persists) |
| `time + pitch readout size` | (M5e) pixel size of the big VFD row with the remaining time and pitch %, 12..32 px (default 20); applies live to both decks, the `bar` row keeps its fixed 15 px |
| `crossfader curve` | `lin` / `pw` / `cut` - same selector as the mixer column |

Master gain has no row in the window: the mixer column slider persists its
value on release (same write-through file).

Every change applies immediately and saves to **`trekker-ng.json` next to
the exe** (portable-style, plain JSON via the same nlohmann stack as
`meta.json`). A missing or corrupt file falls back to the defaults; an
out-of-range value in the file is clamped on load. The same file also
remembers the last windowed size and the fullscreen state (M5f): resize
the window or press `F11`, quit, and the next boot restores both.

Keyboard pitch steps move the fader, so both controls always show the same
value. Digit shortcuts ignore Alt/Ctrl so a modified digit never toggles a
stem by accident; the nudge slider beside the fader bends pitch (see the UI
table).

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
