#pragma once

// M9 (SPEC §5): offline BPM estimation for tracks that carry no tempo -
// flat single files (no meta.json) and stem tracks whose meta.json omits
// "bpm". Wraps SoundTouch's BPMDetect (LGPL 2.1, MSYS2 package + shipped
// DLL): decimate to ~500 Hz, amplitude envelope, short-term
// autocorrelation, peak pick (valid range ~45-190 BPM).

#include <cstdint>

namespace tng {

// Estimates the tempo of a decoded interleaved buffer (the loader's stem
// data). Returns the detected BPM rounded by the analyzer itself, or 0.0f
// when detection fails (silence, too short, nothing rhythmic). Not
// realtime-safe (allocates) - call it on the loader worker thread only,
// with the track fully decoded in RAM (SPEC §4.7).
float detectBpm(const float* interleaved, int64_t frames, int channels,
                uint32_t sampleRate);

} // namespace tng
