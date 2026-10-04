#include "audio_device.h"

#include "deck.h"
#include "mixer.h"

#define MA_IMPLEMENTATION
#include "miniaudio.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace tng {

struct AudioDevice::Impl {
    ma_device device{};
    bool deviceInitialized = false;
    Deck* deckA = nullptr;
    Deck* deckB = nullptr;
    Mixer* mixer = nullptr;
    // Per-deck scratch buffers, allocated once in init() (SPEC §4.7: the
    // callback itself never allocates). Sized to one period.
    std::vector<float> scratchA;
    std::vector<float> scratchB;
    uint32_t frameCap = 0;
};

namespace {

// Realtime-safe: render each deck into preallocated scratch, then one mixer
// pass sums them with line/cross gains and the master (SPEC §4.7). If the
// driver ever asks for more frames than one period, the block is processed
// in chunks over the same scratch.
void dataCallback(ma_device* device, void* pOutput, const void* /*pInput*/,
                  ma_uint32 frameCount) {
    auto* self = static_cast<AudioDevice::Impl*>(device->pUserData);
    float* out = static_cast<float*>(pOutput);
    const uint32_t cap = self->frameCap;
    if (cap == 0) return; // init() always sets a cap; belt and braces

    uint32_t offset = 0;
    while (offset < frameCount) {
        const uint32_t n = std::min(frameCount - offset, cap);
        float* sa = self->scratchA.data();
        float* sb = self->scratchB.data();

        if (self->deckA != nullptr) {
            self->deckA->render(sa, n);
        } else {
            std::memset(sa, 0, static_cast<size_t>(n) * 2 * sizeof(float));
        }
        if (self->deckB != nullptr) {
            self->deckB->render(sb, n);
        } else {
            std::memset(sb, 0, static_cast<size_t>(n) * 2 * sizeof(float));
        }

        // nullptr deckB = single-deck mode: the mixer bypasses the
        // crossfader so deckA keeps its full line gain.
        self->mixer->process(sa, self->deckB != nullptr ? sb : nullptr, out + offset * 2, n);
        offset += n;
    }
}

} // namespace

AudioDevice::AudioDevice() : impl_(new Impl()) {}

AudioDevice::~AudioDevice() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool AudioDevice::init(Deck* deckA, Deck* deckB, Mixer* mixer, uint32_t sampleRate,
                       uint32_t periodFrames, std::string* error) {
    impl_->deckA = deckA;
    impl_->deckB = deckB;
    impl_->mixer = mixer;
    impl_->frameCap = periodFrames > 0 ? periodFrames : 256;
    impl_->scratchA.assign(static_cast<size_t>(impl_->frameCap) * 2, 0.0f);
    impl_->scratchB.assign(static_cast<size_t>(impl_->frameCap) * 2, 0.0f);

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = sampleRate;
    cfg.periodSizeInFrames = periodFrames;
    cfg.dataCallback = dataCallback;
    cfg.pUserData = impl_;

    const ma_result result = ma_device_init(nullptr, &cfg, &impl_->device);
    if (result != MA_SUCCESS) {
        if (error) *error = ma_result_description(result);
        return false;
    }
    impl_->deviceInitialized = true;
    return true;
}

bool AudioDevice::start(std::string* error) {
    if (!impl_->deviceInitialized) {
        if (error) *error = "device not initialized";
        return false;
    }
    const ma_result result = ma_device_start(&impl_->device);
    if (result != MA_SUCCESS) {
        if (error) *error = ma_result_description(result);
        return false;
    }
    return true;
}

void AudioDevice::stop() {
    if (impl_->deviceInitialized) {
        ma_device_stop(&impl_->device);
    }
}

void AudioDevice::shutdown() {
    if (impl_->deviceInitialized) {
        ma_device_uninit(&impl_->device);
        impl_->deviceInitialized = false;
    }
    impl_->scratchA.clear();
    impl_->scratchB.clear();
    impl_->frameCap = 0;
}

} // namespace tng
