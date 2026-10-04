# Trekker-NG usage

A minimal DJ player for **producer-made stems**: each track is 4 stems sharing
one playhead, mute-able live, with Technics-style varispeed pitch (tempo and
pitch change together, no time-stretching).

## Requirements

- Windows 10+ (64-bit). No runtime install needed - the exe is statically
  linked. A soundcard, obviously.

## Run

```
trekker-ng.exe <track.zip | track-folder>
```

The track must be a folder or `.zip` containing `meta.json` + 1..4 stereo stem
files (WAV/FLAC/MP3). See `docs/FORMAT.md` for the format and
`examples/magnat.zip` in the repository for a ready-made track.

## Keys

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

## Options

```
--render <out.wav>   render offline to a WAV instead of playing (no soundcard)
--rate <x>           playback rate for --render (e.g. 1.10 = +10%)
--seconds <n>        seconds to render (default 5)
--stems <bbbb>       4-bit stem mask kept on while rendering (default 1111)
```

Example - pitch-accuracy check (renders the 440 Hz test stem at +10%, expect
~484 Hz):

```
trekker-ng.exe temp\testtrack --render temp\out.wav --rate 1.10 --stems 1000
python tools\make_test_track.py        # generate temp\testtrack first
```

## Build from source

MSYS2 MINGW64 shell, from the repository root:

```
./build.sh            # configure + build + tests -> dist/trekker-ng.exe
./build.sh release    # additionally pack dist/trekker-ng-<VERSION>-win64.zip
```
