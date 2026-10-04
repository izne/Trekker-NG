#include "audio_device.h"

#include "deck.h"

#define MA_IMPLEMENTATION
#include "miniaudio.h"

#include <atomic>
#include <cstring>

namespace sde {

struct AudioDevice::Impl {
    ma_device device{};
    bool deviceInitialized = false;
    Deck* deck = nullptr;
    // M1: fixed master gain until the limiter + master section arrive (M4/M5).
    std::atomic<float> masterGain{0.7f};
};

namespace {

// Realtime-safe: only deck rendering, a multiply and a clamp in here
// (SPEC §4.7). Anything fancier (limiter, crossfader) arrives in M4/M5.
void dataCallback(ma_device* device, void* pOutput, const void* /*pInput*/,
                  ma_uint32 frameCount) {
    auto* self = static_cast<AudioDevice::Impl*>(device->pUserData);
    float* out = static_cast<float*>(pOutput);

    self->deck->render(out, frameCount);

    const float gain = self->masterGain.load(std::memory_order_relaxed);
    const ma_uint32 count = frameCount * 2; // stereo
    for (ma_uint32 i = 0; i < count; ++i) {
        float v = out[i] * gain;
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        out[i] = v;
    }
}

} // namespace

AudioDevice::AudioDevice() : impl_(new Impl()) {}

AudioDevice::~AudioDevice() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool AudioDevice::init(Deck* deck, uint32_t sampleRate, uint32_t periodFrames,
                       std::string* error) {
    impl_->deck = deck;

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
}

void AudioDevice::setMasterGain(float gain) noexcept {
    impl_->masterGain.store(gain, std::memory_order_relaxed);
}

float AudioDevice::masterGain() const noexcept {
    return impl_->masterGain.load(std::memory_order_relaxed);
}

} // namespace sde
