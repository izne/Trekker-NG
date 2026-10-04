#!/usr/bin/env python3
"""Generate a StemDeck test track (SPEC §9) into the project's temp/ folder.

Stems (all stereo, 44.1 kHz, same length):
  stem 1: 440 Hz sine        (pitch-accuracy reference)
  stem 2: 880 Hz sine
  stem 3: click track at BPM (stem-lock / alignment reference)
  stem 4: 110 Hz sine

Output: <project>/temp/testtrack/ with meta.json + 4 WAV files.
Pure stdlib - no numpy/soundfile needed.
"""
import argparse
import array
import json
import math
import sys
import wave
from pathlib import Path

RATE = 44100
SINE_AMP = 0.4
CLICK_AMP = 0.8
CLICK_MS = 5.0


def write_wav(path: Path, samples: array.array) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(samples.tobytes())


def stereo_from_mono(mono: array.array) -> array.array:
    out = array.array("h", bytes(4 * len(mono)))  # L,R per frame = 4 bytes
    out[0::2] = mono
    out[1::2] = mono
    return out


def sine(freq: float, seconds: float, amp: float) -> array.array:
    n = int(seconds * RATE)
    step = 2.0 * math.pi * freq / RATE
    mono = array.array("h", (int(amp * math.sin(step * i) * 32767.0) for i in range(n)))
    return stereo_from_mono(mono)


def clicks(bpm: float, seconds: float) -> array.array:
    n = int(seconds * RATE)
    out = array.array("h", bytes(4 * n))
    period = 60.0 / bpm
    click_len = int(RATE * CLICK_MS / 1000.0)
    t = 0.0
    while t < seconds:
        start = int(t * RATE)
        for i in range(min(click_len, n - start)):
            v = int(CLICK_AMP * math.sin(2.0 * math.pi * 2000.0 * i / RATE) * 32767.0)
            out[2 * (start + i)] = v
            out[2 * (start + i) + 1] = v
        t += period
    return out


def main() -> int:
    project = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--bpm", type=float, default=128.0)
    ap.add_argument("--outdir", type=Path, default=project / "temp" / "testtrack")
    args = ap.parse_args()

    outdir: Path = args.outdir
    # SPEC §10.1: generated files never leave the project folder.
    if project not in outdir.resolve().parents and outdir.resolve() != project:
        print(f"refusing to write outside the project: {outdir}", file=sys.stderr)
        return 1
    outdir.mkdir(parents=True, exist_ok=True)

    print(f"generating {args.seconds:.0f}s test track -> {outdir}")
    write_wav(outdir / "sine440.wav", sine(440.0, args.seconds, SINE_AMP))
    print("  stem 1: sine440.wav")
    write_wav(outdir / "sine880.wav", sine(880.0, args.seconds, SINE_AMP))
    print("  stem 2: sine880.wav")
    write_wav(outdir / "click128.wav", clicks(args.bpm, args.seconds))
    print(f"  stem 3: click128.wav ({args.bpm:g} BPM)")
    write_wav(outdir / "sine110.wav", sine(110.0, args.seconds, SINE_AMP))
    print("  stem 4: sine110.wav")

    meta = {
        "format_version": 1,
        "title": "Test Track",
        "artist": "StemDeck tools",
        "bpm": args.bpm,
        "first_beat_offset_ms": 0.0,
        "stems": [
            {"name": "Sine 440", "file": "sine440.wav", "color": "#ff5555"},
            {"name": "Sine 880", "file": "sine880.wav", "color": "#ffaa00"},
            {"name": "Clicks", "file": "click128.wav", "color": "#55aaff"},
            {"name": "Sine 110", "file": "sine110.wav", "color": "#66dd88"},
        ],
        "cues": [],
        "loops": [],
    }
    (outdir / "meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print("  meta.json")
    print("done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
