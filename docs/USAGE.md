# Trekker-NG usage

A minimal DJ player for **producer-made stems**: each track is 4 stems sharing
one playhead, mute-able live, with Technics-style varispeed pitch (tempo and
pitch change together, no time-stretching).

## Requirements

- Windows 10+ (64-bit). `SDL2.dll` ships next to the exe - no runtime install.
  A soundcard, obviously.

## Run

```
trekker-ng.exe [track.zip | track-folder]
```

The UI starts empty. Load a track by:

- **drag & drop** - drop the track folder or `.zip` anywhere on the window,
- **path box** - paste a path into the box at the bottom and press `Load`,
- **command line** - pass the path as the first argument (as above).

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
| `1` `2` `3` `4` checkboxes | toggle that stem (colored as in `meta.json`) |
| pitch fader | vertical, ±10 %, reversed like DJ gear: **up = slower, down = faster** |
| time / beat | position / length, beat within the bar, effective BPM |
| path box + `Load` | load from a typed path |

### Keys

| Key | Action |
|---|---|
| `Space` | play / pause (restarting at the end jumps back to 0) |
| `1` `2` `3` `4` | toggle stem on/off (click-free 5 ms ramp) |
| `=` `+` `]` | pitch **up** 0.10 % (hold `Shift`: 0.01 %) |
| `-` `_` `[` | pitch **down** 0.10 % (hold `Shift`: 0.01 %) |
| `0` | reset pitch to 0.00 % |
| `Q` / `Esc` | quit |

Keyboard pitch steps move the fader, so both controls always show the same
value.

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
