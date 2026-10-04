#pragma once

#include "deck.h"

#include <filesystem>
#include <memory>
#include <string>

namespace sde {

struct LoadResult {
    std::unique_ptr<DeckData> data; // null on failure
    std::string error;              // human-readable reason

    bool ok() const { return data != nullptr; }
};

// Loads a StemDeck track: either a folder or a .zip containing meta.json and
// 1..4 stem audio files (SPEC §5). All stems are fully decoded to RAM as
// stereo float32 and padded to equal length. Missing stems stay silent.
LoadResult loadTrack(const std::filesystem::path& path);

} // namespace sde
