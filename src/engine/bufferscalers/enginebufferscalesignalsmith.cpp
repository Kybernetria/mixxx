#include "engine/bufferscalers/enginebufferscalesignalsmith.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

#include "engine/readaheadmanager.h"
#include "engine/stretchinputbounds.h"
#include "util/defs.h"
#include "util/fpclassify.h"
#include "util/sample.h"

namespace {
constexpr int kOutputFrames = 256;
constexpr int kInputFrames = mixxx::engine::stretch::kMaxInputFrames;
constexpr int kReadBudget = mixxx::engine::stretch::kReadAttemptBudget;
constexpr int kZeroReadBudget = mixxx::engine::stretch::kZeroReadAttemptBudget;

std::uint64_t formatKey(const mixxx::audio::SignalInfo& signal) {
    return signal.isValid()
            ? (static_cast<std::uint64_t>(signal.getSampleRate().toDouble()) << 8) |
                    signal.getChannelCount()
            : 0;
}

struct Configuration {
    explicit Configuration(std::uint64_t key)
            : key(key),
              channels(key & 255),
              input(channels * kInputFrames),
              output(channels * kOutputFrames),
              interleaved(channels * kInputFrames) {
        stretch.presetDefault(channels, key >> 8, true);
        stretch.reset();
        for (int ch = 0; ch < channels; ++ch) {
            inputPointers[ch] = input.data() + ch * kInputFrames;
            outputPointers[ch] = output.data() + ch * kOutputFrames;
        }
    }

    const std::uint64_t key;
    const int channels;
    std::uint64_t seekGeneration = 0;
    int seekFrames = 0;
    double seekPitch = 1;
    signalsmith::stretch::SignalsmithStretch<float> stretch{0};
    std::vector<float> input;
    std::vector<float> output;
    std::vector<float> interleaved;
    std::array<float*, mixxx::kMaxEngineChannelInputCount> inputPointers{};
    std::array<float*, mixxx::kMaxEngineChannelInputCount> outputPointers{};
};
static_assert(std::atomic<Configuration*>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
} // namespace

struct EngineBufferScaleSignalsmith::State {
    explicit State(ReadAheadManager* reader)
            : reader(reader),
              worker([this] { prepare(); }) {
    }
    ~State() {
        stopping.store(true, std::memory_order_release);
        worker.join(); // Owner has stopped callbacks; never called on the audio thread.
        delete ready.exchange(nullptr);
        delete seekRequest.exchange(nullptr);
        delete seekCompleted.exchange(nullptr);
        delete retired.exchange(nullptr);
        delete active;
    }

    void prepare() {
        while (!stopping.load(std::memory_order_acquire)) {
            delete retired.exchange(nullptr, std::memory_order_acq_rel);
            if (auto* config = seekRequest.exchange(nullptr, std::memory_order_acq_rel)) {
#ifdef BUILD_TESTING
                while (testPreparationPaused.load(std::memory_order_acquire) &&
                        !stopping.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
#endif
                if (!stopping.load(std::memory_order_acquire) &&
                        config->seekGeneration == generation.load(std::memory_order_acquire)) {
                    config->stretch.setTransposeFactor(config->seekPitch);
                    config->stretch.setFormantFactor(1);
                    config->stretch.outputSeek(config->inputPointers.data(), config->seekFrames);
                }
                // Ownership path: callback active -> request mailbox -> worker local
                // -> completion mailbox -> callback active or retired. Exactly one
                // request exists, so neither handoff can replace another pointer.
                seekCompleted.store(config, std::memory_order_release);
                continue;
            }
            const auto key = requested.load(std::memory_order_acquire);
            // Only this worker destroys configurations. A ready pointer can
            // move to active/retired here, but its immutable key remains alive
            // until this worker's next retirement sweep.
            const auto* waiting = ready.load(std::memory_order_acquire);
            if (key && adopted.load(std::memory_order_acquire) != key &&
                    (!waiting || waiting->key != key)) {
                auto config = std::make_unique<Configuration>(key);
                // A newer request may supersede this one before it is adopted.
                delete ready.exchange(config.release(), std::memory_order_acq_rel);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    struct PendingInputBatch {
        double rate;
        double pitch;
        int outputFrames;
        int inputRequired;
        int collected;
        double resultFraction;
    };

    bool acceptAndAdoptForCallback() {
        if (auto* completed = seekCompleted.exchange(nullptr, std::memory_order_acq_rel)) {
            seekOutstanding = false;
            const auto key = requested.load(std::memory_order_acquire);
            if (completed->key == key &&
                    completed->seekGeneration == generation.load(std::memory_order_acquire)) {
                active = completed;
                needsPreroll = false;
                resuming = true;
                availableOutput = 0;
                fraction = 0;
            } else if (completed->key == key) {
                active = completed;
                needsPreroll = true;
                pending.reset();
                availableOutput = 0;
            } else {
                if (retired.load(std::memory_order_acquire)) {
                    seekCompleted.store(completed, std::memory_order_release);
                    seekOutstanding = true;
                    return false;
                }
                retired.store(completed, std::memory_order_release);
                adopted.store(0, std::memory_order_release);
            }
        }
        if (!seekOutstanding) {
            const auto key = requested.load(std::memory_order_relaxed);
            if (!retired.load(std::memory_order_acquire)) {
                if (auto* config = ready.exchange(nullptr, std::memory_order_acq_rel)) {
                    if (config->key != key || (active && active->key == key)) {
                        retired.store(config, std::memory_order_release);
                    } else {
                        auto* previous = active;
                        active = config;
                        adopted.store(key, std::memory_order_release);
                        pending.reset();
                        availableOutput = 0;
                        retired.store(previous, std::memory_order_release);
                    }
                }
            }
        }
        return active && active->key == requested.load(std::memory_order_acquire);
    }

    // All following state belongs exclusively to the callback.
    ReadAheadManager* const reader;
    Configuration* active = nullptr;
    bool needsPreroll = true;
    bool seekOutstanding = false;
    std::optional<PendingInputBatch> pending;
    bool backwards = false;
    int availableOutput = 0;
    int outputOffset = 0;
    double fraction = 0;
    double outputRate = 0;
    bool resuming = false;

    // One-slot publication and retirement. While a seek is outstanding, active
    // is null but adopted remains unchanged to avoid building a duplicate format.
    // Callback never deletes configs, wakes the worker, or waits for it.
    std::atomic<std::uint64_t> requested{0};
    std::atomic<std::uint64_t> adopted{0};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<Configuration*> seekRequest{nullptr};
    std::atomic<Configuration*> seekCompleted{nullptr};
    std::atomic<Configuration*> ready{nullptr};
    std::atomic<Configuration*> retired{nullptr};
    std::atomic<bool> stopping{false};
#ifdef BUILD_TESTING
    std::atomic<bool> testPreparationPaused{false};
#endif
    std::thread worker;
};

EngineBufferScaleSignalsmith::EngineBufferScaleSignalsmith(ReadAheadManager* reader)
        : m_state(std::make_unique<State>(reader)) {
}
EngineBufferScaleSignalsmith::~EngineBufferScaleSignalsmith() = default;

#ifdef BUILD_TESTING
void EngineBufferScaleSignalsmith::setPreparationPausedForTest(bool paused) {
    m_state->testPreparationPaused.store(paused, std::memory_order_release);
}
#endif

void EngineBufferScaleSignalsmith::onSignalChanged() {
    const auto channels = getOutputSignal().getChannelCount();
    const auto key = (channels == mixxx::audio::ChannelCount::stereo() ||
                             channels == mixxx::audio::ChannelCount::stem())
            ? formatKey(getOutputSignal())
            : 0;
    m_state->requested.store(key, std::memory_order_release);
    clear();
}

void EngineBufferScaleSignalsmith::setScaleParameters(double baseRate,
        double* tempo,
        double* pitch) {
    const bool tempoFinite = util_isfinite(*tempo);
    const bool backwards = tempoFinite && *tempo < 0;
    if (backwards != m_state->backwards) {
        clear();
    }
    m_state->backwards = backwards;
    constexpr double minimumBaseRate = static_cast<double>(mixxx::audio::SampleRate::kValueMin) /
            mixxx::audio::SampleRate::kValueMax;
    constexpr double maximumBaseRate = 1 / minimumBaseRate;
    m_dBaseRate = util_isfinite(baseRate) && baseRate >= minimumBaseRate &&
                    baseRate <= maximumBaseRate
            ? baseRate
            : 0;
    const auto speed = tempoFinite ? std::fabs(*tempo) : 0;
    m_dTempoRatio = speed >= MIN_SEEK_SPEED ? std::min(speed, MAX_SEEK_SPEED) : 0;
    m_dPitchRatio = util_isfinite(*pitch) && *pitch > 0 ? *pitch : 1;
    const double transpose = m_dBaseRate * m_dPitchRatio;
    // Vendor frequency maps cast mapped FFT bins to int. A finite float alone
    // is insufficient; keep even the largest supported FFT below that range.
    constexpr double maximumTranspose =
            std::numeric_limits<int>::max() / (2.0 * kInputFrames);
    if (!util_isfinite(transpose) || transpose > maximumTranspose ||
            transpose < std::numeric_limits<float>::min()) {
        m_dPitchRatio = 1;
    }
    *tempo = backwards ? -m_dTempoRatio : m_dTempoRatio;
    *pitch = m_dPitchRatio;
    m_effectiveRate = m_dBaseRate * m_dTempoRatio;
}

void EngineBufferScaleSignalsmith::clear() {
    auto& state = *m_state;
    state.generation.fetch_add(1, std::memory_order_acq_rel);
    state.needsPreroll = true;
    state.pending.reset();
    state.availableOutput = 0;
    state.fraction = 0;
    state.resuming = false;
}

double EngineBufferScaleSignalsmith::scaleBufferForCrossfade(CSAMPLE* output, SINT samples) {
    if (!output || samples <= 0) {
        return 0;
    }
    SampleUtil::clear(output, samples);
    auto& state = *m_state;
    if (!state.acceptAndAdoptForCallback() || state.needsPreroll || state.seekOutstanding ||
            !state.active || state.active->key != state.requested.load(std::memory_order_acquire)) {
        return 0;
    }
    return scaleBuffer(output, samples);
}

double EngineBufferScaleSignalsmith::scaleBuffer(CSAMPLE* output, SINT samples) {
    if (!output || samples <= 0)
        return 0;
    SampleUtil::clear(output, samples);
    auto& state = *m_state;
    if (!state.acceptAndAdoptForCallback() || samples <= 0 || m_effectiveRate <= 0) {
        return 0;
    }
    auto& config = *state.active;
    const int channels = config.channels;
    if (samples % channels != 0 || samples / channels > static_cast<int>(kMaxEngineFrames)) {
        return 0;
    }
    const int frames = samples / channels;
    int written = 0;
    int reads = 0;
    int zeroReads = 0;
    double consumedFrames = 0;
    while (written < frames) {
        if (state.availableOutput) {
            const int count = std::min(state.availableOutput, frames - written);
            for (int frame = 0; frame < count; ++frame) {
                for (int ch = 0; ch < channels; ++ch) {
                    output[(written + frame) * channels + ch] =
                            config.outputPointers[ch][state.outputOffset + frame];
                }
            }
            if (state.resuming) {
                SampleUtil::applyRampingGain(output + written * channels,
                        CSAMPLE_GAIN_ZERO,
                        CSAMPLE_GAIN_ONE,
                        count * channels);
                state.resuming = false;
            }
            state.outputOffset += count;
            state.availableOutput -= count;
            written += count;
            consumedFrames += state.outputRate * count;
            continue;
        }
        if (!state.pending) {
            const double rate = m_effectiveRate;
            const double pitch = m_dBaseRate * m_dPitchRatio;
            const double required = state.needsPreroll
                    ? config.stretch.inputLatency() + rate * config.stretch.outputLatency()
                    : rate * kOutputFrames + state.fraction;
            if (!util_isfinite(required) || required > kInputFrames) {
                break;
            }
            const int inputRequired = static_cast<int>(required);
            state.pending.emplace(State::PendingInputBatch{rate,
                    pitch,
                    state.needsPreroll ? 0 : kOutputFrames,
                    inputRequired,
                    0,
                    state.needsPreroll ? state.fraction : required - inputRequired});
        }
        auto& batch = *state.pending;
        while (batch.collected < batch.inputRequired && reads < kReadBudget &&
                zeroReads < kZeroReadBudget) {
            ++reads;
            const auto result = state.reader->getNextSamplesForStretch(
                    state.backwards ? -batch.rate : batch.rate,
                    config.interleaved.data(),
                    (batch.inputRequired - batch.collected) * channels,
                    getOutputSignal().getChannelCount());
            if (result.unavailable) {
                state.resuming = true;
                return consumedFrames;
            }
            const int readFrames = result.samplesRead / channels;
            if (readFrames == 0)
                ++zeroReads;
            for (int frame = 0; frame < readFrames; ++frame) {
                for (int ch = 0; ch < channels; ++ch) {
                    config.inputPointers[ch][batch.collected + frame] =
                            config.interleaved[frame * channels + ch];
                }
            }
            batch.collected += readFrames;
        }
        if (batch.collected < batch.inputRequired) {
            // Zero-length loop transitions are allowed, but recovery is bounded.
            break;
        }
        if (state.needsPreroll) {
            config.seekGeneration = state.generation.load(std::memory_order_acquire);
            config.seekFrames = batch.inputRequired;
            config.seekPitch = batch.pitch;
            state.active = nullptr;
            state.seekOutstanding = true;
            state.needsPreroll = false;
            state.pending.reset();
            state.seekRequest.store(&config, std::memory_order_release);
            return consumedFrames;
        } else {
            config.stretch.setTransposeFactor(batch.pitch);
            config.stretch.setFormantFactor(1);
            config.stretch.process(config.inputPointers.data(),
                    batch.inputRequired,
                    config.outputPointers.data(),
                    batch.outputFrames);
            state.outputRate = batch.rate;
            state.outputOffset = 0;
            state.availableOutput = batch.outputFrames;
            state.fraction = batch.resultFraction;
        }
        state.pending.reset();
    }
    return consumedFrames;
}
