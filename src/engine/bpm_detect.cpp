#include "bpm_detect.h"

#include <soundtouch/BPMDetect.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace tng {

float detectBpm(const float* interleaved, int64_t frames, int channels,
                uint32_t sampleRate) {
    // One second of audio is the floor: the autocorrelation window needs a
    // handful of beats to show a peak. SoundTouch handles mono and stereo.
    if (interleaved == nullptr || frames < static_cast<int64_t>(sampleRate) ||
        sampleRate < 8000 || (channels != 1 && channels != 2)) {
        return 0.0f;
    }

    soundtouch::BPMDetect det(channels, static_cast<int>(sampleRate));

    // inputSamples may disrupt the samples it is given (SoundTouch docs) -
    // the decoded stem belongs to the playing track, so feed copies in
    // chunks. 4096 frames is a few kB of scratch.
    constexpr int kChunk = 4096;
    std::vector<soundtouch::SAMPLETYPE> scratch(
        static_cast<size_t>(kChunk) * static_cast<size_t>(channels));

    int64_t done = 0;
    while (done < frames) {
        const int n = static_cast<int>(std::min<int64_t>(kChunk, frames - done));
        const size_t samples = static_cast<size_t>(n) * static_cast<size_t>(channels);
        std::copy_n(interleaved + done * channels, samples, scratch.begin());
        det.inputSamples(scratch.data(), n);
        done += n;
    }

    const float bpm = det.getBpm();
    // 0 = failed; anything outside the analyzer's own valid window is noise.
    if (!(bpm >= 45.0f && bpm <= 190.0f)) return 0.0f;
    return bpm;
}

} // namespace tng
