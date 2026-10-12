#pragma once

// Minimal 16-bit PCM stereo WAV writer for filesystem fixtures (M8 flat
// tests, M9 BPM tests): PCM + interleaved s16 is what decodeStem's
// ma_decoder reads back exactly. Fixtures go into the test binary's
// working directory (the build dir - inside the project).

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace tutil {

inline void wavWriteU16(std::ostream& o, uint16_t v) {
    o.put(static_cast<char>(v & 0xFF));
    o.put(static_cast<char>((v >> 8) & 0xFF));
}

inline void wavWriteU32(std::ostream& o, uint32_t v) {
    wavWriteU16(o, static_cast<uint16_t>(v & 0xFFFF));
    wavWriteU16(o, static_cast<uint16_t>(v >> 16));
}

inline void writeWavStereo(const std::string& path, uint32_t fs,
                           const std::vector<int16_t>& pcm) {
    std::ofstream f(path, std::ios::binary);
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * 2);
    f.write("RIFF", 4);
    wavWriteU32(f, 36 + dataBytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    wavWriteU32(f, 16);
    wavWriteU16(f, 1);                     // PCM
    wavWriteU16(f, 2);                     // stereo
    wavWriteU32(f, fs);
    wavWriteU32(f, fs * 2 * 2);            // byte rate
    wavWriteU16(f, 2 * 2);                 // block align
    wavWriteU16(f, 16);                    // bits per sample
    f.write("data", 4);
    wavWriteU32(f, dataBytes);
    for (int16_t s : pcm) wavWriteU16(f, static_cast<uint16_t>(s));
}

} // namespace tutil
