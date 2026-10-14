#include "audio_device.h"

#include "cue_ring.h"
#include "deck.h"
#include "mixer.h"
#include "recorder.h"

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
    Recorder* recorder = nullptr; // M7: null = no tap; outlives the device
    // Per-deck scratch buffers, allocated once in init() (SPEC §4.7: the
    // callback itself never allocates). Sized to one period.
    std::vector<float> scratchA;
    std::vector<float> scratchB;
    uint32_t frameCap = 0;
    // M5c: resolved id of the named settings device; must outlive
    // ma_device_init (cfg.playback.pDeviceID points at it).
    ma_device_id deviceId{};

    // M10 headphone cue (SPEC §4.8): optional second playback device fed
    // from CueRing by the master callback. cueActive is plain because it
    // only changes while no callback can run (init/teardown); both
    // callbacks read it as set-before-start.
    ma_device cueDevice{};
    bool cueDeviceInitialized = false;
    bool cueStarted = false;
    bool cueActive = false;
    ma_device_id cueDeviceId{};
    CueRing ring;
    std::vector<float> cueScratch; // one period, master-callback writes
    std::string cueError;
};

namespace {

// M5c/M10: name -> device id by enumeration (the config takes an id, not
// a name; the id must outlive ma_device_init). Setup-time only.
bool findDeviceId(const char* name, ma_device_id* idOut, std::string* error) {
    ma_context ctx;
    ma_context_config ctxCfg = ma_context_config_init();
    if (ma_context_init(nullptr, 0, &ctxCfg, &ctx) != MA_SUCCESS) {
        if (error) *error = "device enumeration failed";
        return false;
    }
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    bool found = false;
    if (ma_context_get_devices(&ctx, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; ++i) {
            if (std::strcmp(infos[i].name, name) == 0) {
                *idOut = infos[i].id;
                found = true;
                break;
            }
        }
    }
    ma_context_uninit(&ctx);
    if (!found && error) *error = std::string("device not found: ") + name;
    return found;
}

// Realtime-safe: render each deck into preallocated scratch, then one mixer
// pass sums them with line/cross gains and the master (SPEC §4.7). If the
// driver ever asks for more frames than one period, the block is processed
// in chunks over the same scratch. M10: with a cue device active the same
// pass also fills the phones signal into cueScratch, which is pushed into
// the cue ring (atomics + copy only) for the second device's callback.
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
        // crossfader so deckA keeps its full line gain. cueScratch only
        // when the cue device is up - otherwise the mixer takes the exact
        // pre-M10 path.
        float* cue = self->cueActive ? self->cueScratch.data() : nullptr;
        self->mixer->process(sa, self->deckB != nullptr ? sb : nullptr, out + offset * 2, n,
                             cue);
        if (self->cueActive) self->ring.push(self->cueScratch.data(), n);

        // M7 (SPEC 4.1): tap the master bus - the block just written is
        // exactly what reaches the soundcard (post EQ/cross/limiter).
        // Recorder::push is gate + ring copy only (SPEC 4.7).
        if (self->recorder != nullptr) self->recorder->push(out + offset * 2, n);
        offset += n;
    }
}

// M10: the cue/headphone device's own callback - drain the ring, silence
// when it runs dry (counted). Realtime-safe (SPEC §4.7): pop() is atomics
// and copies only.
void cueDataCallback(ma_device* device, void* pOutput, const void* /*pInput*/,
                     ma_uint32 frameCount) {
    auto* self = static_cast<AudioDevice::Impl*>(device->pUserData);
    self->ring.pop(static_cast<float*>(pOutput), frameCount);
}

} // namespace

AudioDevice::AudioDevice() : impl_(new Impl()) {}

AudioDevice::~AudioDevice() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool AudioDevice::listPlaybackDevices(std::vector<std::string>* names) {
    if (names == nullptr) return false;
    names->clear();
    names->emplace_back(); // index 0 = system default ("" -> no name bound)

    ma_context ctx;
    ma_context_config ctxCfg = ma_context_config_init();
    if (ma_context_init(nullptr, 0, &ctxCfg, &ctx) != MA_SUCCESS) return false;

    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&ctx, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; ++i) {
            names->emplace_back(infos[i].name);
        }
    }
    ma_context_uninit(&ctx);
    return true;
}

bool AudioDevice::init(Deck* deckA, Deck* deckB, Mixer* mixer, uint32_t sampleRate,
                       uint32_t periodFrames, const char* deviceName, std::string* error,
                       Recorder* recorder, const char* cueDeviceName) {
    impl_->deckA = deckA;
    impl_->deckB = deckB;
    impl_->mixer = mixer;
    impl_->recorder = recorder;
    impl_->frameCap = periodFrames > 0 ? periodFrames : 256;
    impl_->scratchA.assign(static_cast<size_t>(impl_->frameCap) * 2, 0.0f);
    impl_->scratchB.assign(static_cast<size_t>(impl_->frameCap) * 2, 0.0f);
    impl_->cueError.clear(); // restarts re-report any fresh cue problem

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = sampleRate;
    cfg.periodSizeInFrames = periodFrames;
    cfg.dataCallback = dataCallback;
    cfg.pUserData = impl_;
    // M5c: named device from the settings screen; null/empty keeps the
    // system default (matches the "" entry of listPlaybackDevices()).
    if (deviceName != nullptr && deviceName[0] != '\0') {
        if (!findDeviceId(deviceName, &impl_->deviceId, error)) {
            return false;
        }
        cfg.playback.pDeviceID = &impl_->deviceId;
    }

    const ma_result result = ma_device_init(nullptr, &cfg, &impl_->device);
    if (result != MA_SUCCESS) {
        if (error) *error = ma_result_description(result);
        return false;
    }
    impl_->deviceInitialized = true;

    // M10 (SPEC §4.8): optional second output. Any failure here degrades
    // to "cue feature off" with an explanation - the master device above
    // already initialized and must not be taken down by headphones.
    if (cueDeviceName != nullptr) {
        ma_device_config cueCfg = ma_device_config_init(ma_device_type_playback);
        cueCfg.playback.format = ma_format_f32;
        cueCfg.playback.channels = 2;
        // Same nominal rate as the master; miniaudio's converter handles
        // devices whose native rate differs (WASAPI/DirectSound shared).
        cueCfg.sampleRate = sampleRate;
        cueCfg.periodSizeInFrames = periodFrames;
        cueCfg.dataCallback = cueDataCallback;
        cueCfg.pUserData = impl_;
        if (cueDeviceName[0] != '\0') {
            if (!findDeviceId(cueDeviceName, &impl_->cueDeviceId, &impl_->cueError)) {
                // cueError already set by the helper.
            } else {
                cueCfg.playback.pDeviceID = &impl_->cueDeviceId;
            }
        }
        if (impl_->cueError.empty()) {
            const ma_result cr =
                ma_device_init(nullptr, &cueCfg, &impl_->cueDevice);
            if (cr != MA_SUCCESS) {
                impl_->cueError =
                    std::string("cue device init failed: ") + ma_result_description(cr);
            } else {
                impl_->cueDeviceInitialized = true;
            }
        }
        if (impl_->cueDeviceInitialized) {
            // ~250 ms of frames: enough slack for both drivers' jitter and
            // the two cards' clock drift (SPEC §4.8). Setup-time only.
            impl_->ring.resize(sampleRate / 4);
            impl_->cueScratch.assign(static_cast<size_t>(impl_->frameCap) * 2, 0.0f);
            impl_->cueActive = true;
        } else {
            impl_->ring.resize(0);
            impl_->cueScratch.clear();
            impl_->cueActive = false;
        }
    }
    return true;
}

bool AudioDevice::start(std::string* error) {
    if (!impl_->deviceInitialized) {
        if (error) *error = "device not initialized";
        return false;
    }
    // Cue first: the consumer must be ready before the producer (master)
    // starts feeding the ring. A cue start failure demotes the feature
    // (master keeps its own start below).
    if (impl_->cueActive && impl_->cueDeviceInitialized) {
        const ma_result cs = ma_device_start(&impl_->cueDevice);
        if (cs != MA_SUCCESS) {
            ma_device_uninit(&impl_->cueDevice);
            impl_->cueDeviceInitialized = false;
            impl_->cueActive = false;
            impl_->cueError =
                std::string("cue device start failed: ") + ma_result_description(cs);
        } else {
            impl_->cueStarted = true;
        }
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
    if (impl_->cueStarted) {
        ma_device_stop(&impl_->cueDevice);
        impl_->cueStarted = false;
    }
}

void AudioDevice::shutdown() {
    if (impl_->deviceInitialized) {
        ma_device_uninit(&impl_->device); // master callback stops here
        impl_->deviceInitialized = false;
    }
    if (impl_->cueDeviceInitialized) {
        ma_device_uninit(&impl_->cueDevice);
        impl_->cueDeviceInitialized = false;
    }
    // Both callbacks are gone now - the ring may be freed safely.
    impl_->cueActive = false;
    impl_->cueStarted = false;
    impl_->ring.resize(0);
    impl_->cueScratch.clear();
    impl_->cueError.clear();
    impl_->scratchA.clear();
    impl_->scratchB.clear();
    impl_->frameCap = 0;
}

const std::string& AudioDevice::cueError() const { return impl_->cueError; }

bool AudioDevice::cueActive() const { return impl_->cueActive; }

uint64_t AudioDevice::cueDropped() const { return impl_->ring.dropped(); }

uint64_t AudioDevice::cueUnderrun() const { return impl_->ring.underrun(); }

} // namespace tng
