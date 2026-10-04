#include "audio_device.h"

#include "deck.h"
#include "mixer.h"

#define MA_IMPLEMENTATION
#include "miniaudio.h"

#include <cstring>

namespace tng {

struct AudioDevice::Impl {
    ma_device device{};
    bool deviceInitialized = false;
    Deck* deck = nullptr;
    Mixer* mixer = nullptr;
};

namespace {

// Realtime-safe: deck render + master mix only (SPEC §4.7). The mixer does
// the atomic gain read, multiply and clamp; nothing else happens in here.
void dataCallback(ma_device* device, void* pOutput, const void* /*pInput*/,
                  ma_uint32 frameCount) {
    auto* self = static_cast<AudioDevice::Impl*>(device->pUserData);
    float* out = static_cast<float*>(pOutput);

    self->deck->render(out, frameCount);
    self->mixer->process(out, frameCount);
}

} // namespace

AudioDevice::AudioDevice() : impl_(new Impl()) {}

AudioDevice::~AudioDevice() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool AudioDevice::init(Deck* deck, Mixer* mixer, uint32_t sampleRate, uint32_t periodFrames,
                       std::string* error) {
    impl_->deck = deck;
    impl_->mixer = mixer;

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

} // namespace tng
