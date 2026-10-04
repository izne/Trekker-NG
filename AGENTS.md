# AGENTS.md

This project is governed by **`SPEC.md`**. Read it fully before writing code.

Quick reminders:

- Milestone order is M1..M6, one at a time. A milestone is done only when it builds,
  runs, and the user has a fresh runnable build in `dist/`.
- Build toolchain: MSYS2 MINGW64 (`D:\msys64\mingw64\bin` on PATH), CMake + Ninja.
  Rebuild with `./build.sh` inside the MINGW64 shell.
- Engine (`src/engine/`) has no UI dependencies and must obey the realtime rules in
  SPEC §4.7 (no alloc/lock/IO/throw in the audio callback).
- Scratch files go in `temp/` only. Never write outside the project folder.
- Don't install packages (`pacman`, `pip`) yourself — tell the user the command.
- Don't swap the libraries chosen in SPEC §3.
