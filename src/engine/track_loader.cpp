#include "track_loader.h"

#include "json.hpp"
#include "miniaudio.h"

// miniz.h defines several static inline helpers that most translation units
// never call; keep -Wall/-Wextra quiet about them.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "miniz.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// C++17: path::u8string() returns std::string (UTF-8) - safe for error text
// with non-ASCII paths (the console is switched to UTF-8 in main).
std::string pathText(const fs::path& p) {
    return std::string(p.u8string());
}

bool readFileBytes(const fs::path& p, std::vector<uint8_t>& out, std::string& err) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        err = "cannot open file: " + pathText(p);
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

uint32_t parseColor(const std::string& s, uint32_t fallback) {
    // "#rrggbb" -> 0xFFrrggbb
    if (s.size() >= 7 && s[0] == '#') {
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int v[6];
        bool valid = true;
        for (int i = 0; i < 6; ++i) {
            v[i] = hex(s[1 + i]);
            if (v[i] < 0) valid = false;
        }
        if (valid) {
            uint32_t rgb = 0;
            for (int i = 0; i < 6; ++i) rgb = (rgb << 4) | static_cast<uint32_t>(v[i]);
            return 0xFF000000u | rgb;
        }
    }
    return fallback;
}

struct DecodedStem {
    std::vector<float> data; // interleaved stereo f32
    int64_t frames = 0;
    uint32_t sampleRate = 0;
};

// Decode one audio file (already in memory) to stereo float32 at the source
// sample rate (SPEC §5).
bool decodeStem(const std::vector<uint8_t>& bytes,
                const std::string& label,
                DecodedStem& out,
                std::string& err) {
    if (bytes.empty()) {
        err = label + ": file is empty";
        return false;
    }

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 0);
    ma_decoder decoder;
    ma_result result = ma_decoder_init_memory(bytes.data(), bytes.size(), &cfg, &decoder);
    if (result != MA_SUCCESS) {
        err = label + ": decode failed (" + ma_result_description(result) + ")";
        return false;
    }

    ma_uint32 rate = 0;
    ma_decoder_get_data_format(&decoder, nullptr, nullptr, &rate, nullptr, 0);

    ma_uint64 hint = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &hint) == MA_SUCCESS && hint > 0) {
        out.data.reserve(static_cast<size_t>(hint) * 2);
    }

    constexpr ma_uint64 kChunkFrames = 4096;
    float buf[kChunkFrames * 2];
    for (;;) {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&decoder, buf, kChunkFrames, &got);
        if (got > 0) {
            out.data.insert(out.data.end(), buf, buf + got * 2);
        }
        if (got < kChunkFrames || (r != MA_SUCCESS && r != MA_AT_END)) break;
    }
    ma_decoder_uninit(&decoder);

    out.frames = static_cast<int64_t>(out.data.size() / 2);
    out.sampleRate = rate;
    if (out.frames <= 0) {
        err = label + ": decoded 0 frames";
        return false;
    }
    return true;
}

// Owns a miniz archive opened on a caller-owned memory buffer (the buffer
// must outlive this object - miniz does not copy it).
class ZipSource {
public:
    ~ZipSource() {
        if (open_) mz_zip_reader_end(&zip_);
    }

    bool open(const std::vector<uint8_t>& bytes, std::string& err) {
        std::memset(&zip_, 0, sizeof(zip_));
        if (!mz_zip_reader_init_mem(&zip_, bytes.data(), bytes.size(), 0)) {
            err = "not a valid zip archive";
            return false;
        }
        open_ = true;
        return true;
    }

    // Prefers an exact path match, falls back to a basename match so tracks
    // zipped with an enclosing folder ("mytrack/meta.json") also load.
    int findEntry(const std::string& want) const {
        const size_t slash = want.find_last_of('/');
        const std::string base = (slash == std::string::npos) ? want : want.substr(slash + 1);
        int basenameMatch = -1;
        const mz_uint count = mz_zip_reader_get_num_files(const_cast<mz_zip_archive*>(&zip_));
        for (mz_uint i = 0; i < count; ++i) {
            char name[512];
            if (mz_zip_reader_get_filename(const_cast<mz_zip_archive*>(&zip_), i, name,
                                           sizeof(name)) == 0) {
                continue;
            }
            const std::string n(name);
            if (!n.empty() && n.back() == '/') continue; // directory entry
            if (n == want) return static_cast<int>(i);
            if (basenameMatch < 0) {
                const size_t s = n.find_last_of('/');
                const std::string b = (s == std::string::npos) ? n : n.substr(s + 1);
                if (b == base) basenameMatch = static_cast<int>(i);
            }
        }
        return basenameMatch;
    }

    bool extract(int index, std::vector<uint8_t>& out, std::string& err) const {
        size_t size = 0;
        void* p = mz_zip_reader_extract_to_heap(const_cast<mz_zip_archive*>(&zip_),
                                                static_cast<mz_uint>(index), &size, 0);
        if (p == nullptr) {
            err = "zip extract failed";
            return false;
        }
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.assign(b, b + size);
        mz_free(p);
        return true;
    }

private:
    mz_zip_archive zip_{};
    bool open_ = false;
};

struct TrackSource {
    virtual ~TrackSource() = default;
    virtual bool read(const std::string& name, std::vector<uint8_t>& out, std::string& err) = 0;
    std::string description;
};

struct FolderSource : TrackSource {
    fs::path dir;
    bool read(const std::string& name, std::vector<uint8_t>& out, std::string& err) override {
        return readFileBytes(dir / name, out, err);
    }
};

struct ZipTrackSource : TrackSource {
    std::vector<uint8_t> bytes; // keeps the archive memory alive
    ZipSource zip;
    bool read(const std::string& name, std::vector<uint8_t>& out, std::string& err) override {
        const int idx = zip.findEntry(name);
        if (idx < 0) {
            err = "not found in zip: " + name;
            return false;
        }
        return zip.extract(idx, out, err);
    }
};

} // namespace

namespace sde {

LoadResult loadTrack(const std::filesystem::path& path) {
    LoadResult result;

    std::unique_ptr<TrackSource> source;
    if (!fs::exists(path)) {
        result.error = "path not found: " + pathText(path);
        return result;
    }
    const bool isDir = fs::is_directory(path);
    const bool isZip = fs::is_regular_file(path) && path.extension() == ".zip";
    if (!isDir && !isZip) {
        result.error = "not a track folder or .zip file: " + pathText(path);
        return result;
    }

    if (isDir) {
        auto src = std::make_unique<FolderSource>();
        src->dir = path;
        src->description = pathText(path);
        source = std::move(src);
    } else {
        auto src = std::make_unique<ZipTrackSource>();
        std::string err;
        if (!readFileBytes(path, src->bytes, err)) {
            result.error = err;
            return result;
        }
        if (!src->zip.open(src->bytes, err)) {
            result.error = pathText(path) + ": " + err;
            return result;
        }
        src->description = pathText(path);
        source = std::move(src);
    }

    // --- meta.json ---
    std::vector<uint8_t> metaBytes;
    std::string err;
    if (!source->read("meta.json", metaBytes, err)) {
        result.error = source->description + ": " + err;
        return result;
    }
    const std::string metaText(metaBytes.begin(), metaBytes.end());
    json meta = json::parse(metaText, nullptr, false);
    if (meta.is_discarded() || !meta.is_object()) {
        result.error = source->description + ": meta.json is not valid JSON";
        return result;
    }

    auto data = std::make_unique<DeckData>();
    data->title = meta.value("title", "");
    data->artist = meta.value("artist", "");
    data->bpm = static_cast<float>(meta.value("bpm", 0.0));
    data->firstBeatOffsetMs = meta.value("first_beat_offset_ms", 0.0);
    if (data->title.empty()) {
        data->title = isDir ? path.filename().u8string() : path.stem().u8string();
    }

    if (!meta.contains("stems") || !meta["stems"].is_array() || meta["stems"].empty()) {
        result.error = source->description + ": meta.json has no non-empty \"stems\" array";
        return result;
    }

    const uint32_t defaultColors[4] = {0xFFFF5555, 0xFFFFAA00, 0xFF55AAFF, 0xFF66DD88};
    struct Entry {
        std::string name;
        std::string file;
        uint32_t color;
    };
    std::vector<Entry> entries;
    for (const auto& s : meta["stems"]) {
        if (entries.size() == 4) break; // v1 accepts 1..4 stems (SPEC §5)
        if (!s.is_object()) {
            result.error = source->description + ": stems[] entries must be objects";
            return result;
        }
        Entry e;
        const size_t i = entries.size();
        e.name = s.value("name", "Stem " + std::to_string(i + 1));
        e.file = s.value("file", "");
        e.color = parseColor(s.value("color", ""),
                             defaultColors[i % 4]);
        if (e.file.empty()) {
            result.error = source->description + ": stems[" + std::to_string(i) +
                           "] is missing \"file\"";
            return result;
        }
        entries.push_back(std::move(e));
    }

    // --- decode all stems ---
    uint32_t rate = 0;
    int64_t frames = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        std::vector<uint8_t> bytes;
        if (!source->read(e.file, bytes, err)) {
            result.error = source->description + ": " + err;
            return result;
        }
        DecodedStem decoded;
        if (!decodeStem(bytes, e.file, decoded, err)) {
            result.error = source->description + ": " + err;
            return result;
        }
        if (i == 0) {
            rate = decoded.sampleRate;
        } else if (decoded.sampleRate != rate) {
            // SPEC §5: all stems must share one sample rate.
            result.error = source->description + ": sample rate mismatch in \"" + e.file +
                           "\" (" + std::to_string(decoded.sampleRate) + " Hz, expected " +
                           std::to_string(rate) + " Hz)";
            return result;
        }
        frames = std::max(frames, decoded.frames);

        Stem& stem = data->stems[i];
        stem.name = e.name;
        stem.color = e.color;
        stem.data = std::move(decoded.data);
        stem.frames = decoded.frames;
    }

    // Pad to the longest stem (SPEC §5: same length ±1 frame, padded).
    for (auto& stem : data->stems) {
        if (stem.frames > 0 && stem.frames < frames) {
            stem.data.resize(static_cast<size_t>(frames) * 2, 0.0f);
        }
        stem.frames = stem.frames > 0 ? frames : 0;
    }

    data->sampleRate = rate;
    data->frames = frames;
    result.data = std::move(data);
    return result;
}

} // namespace sde
