#include "engine/bufferscalers/enginebufferscalerubberband.h"

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include "engine/readaheadmanager.h"
#include "engine/stretchinputbounds.h"
#include "util/defs.h"
#include "util/fpclassify.h"
#include "util/sample.h"

namespace {
constexpr int kInputFrames = kMaxEngineFrames;
constexpr int kReadBudget = mixxx::engine::stretch::kReadAttemptBudget;
constexpr int kZeroReadBudget = mixxx::engine::stretch::kZeroReadAttemptBudget;
constexpr std::uint64_t kInvalidFormat = 0;
constexpr double kMinimumRatio = 1.0 / 16;
constexpr double kMaximumRatio = 8;
constexpr double kMinimumPitch = 0.25;
constexpr double kMaximumPitch = 4;

std::uint64_t formatKey(const mixxx::audio::SignalInfo& signal) {
    return signal.isValid()
            ? (static_cast<std::uint64_t>(signal.getSampleRate().toDouble()) << 8) |
                    signal.getChannelCount()
            : kInvalidFormat;
}

struct Configuration {
    explicit Configuration(std::uint64_t format)
            : key(format),
              channels(format & 255),
              sampleRate(format >> 8),
              input(channels * kInputFrames, 0),
              output(channels * kInputFrames, 0),
              interleaved(channels * kInputFrames, 0) {
        using Stretcher = RubberBand::RubberBandStretcher;
        const auto options = Stretcher::OptionProcessRealTime |
                Stretcher::OptionThreadingNever |
                Stretcher::OptionWindowShort |
                Stretcher::OptionChannelsTogether;
        stretcher = std::make_unique<Stretcher>(sampleRate, channels, options);
        stretcher->setMaxProcessSize(kInputFrames);
        stretcher->setTimeRatio(16.0);
        stretcher->setTimeRatio(1.0 / 8);
        stretcher->setTimeRatio(1.0);
        for (int channel = 0; channel < channels; ++channel) {
            inputPointers[channel] = input.data() + channel * kInputFrames;
            outputPointers[channel] = output.data() + channel * kInputFrames;
        }
    }

    const std::uint64_t key;
    const int channels;
    const int sampleRate;
    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
    std::vector<float> input;
    std::vector<float> output;
    std::vector<float> interleaved;
    std::array<float*, mixxx::kMaxEngineChannelInputCount> inputPointers{};
    std::array<float*, mixxx::kMaxEngineChannelInputCount> outputPointers{};
};
static_assert(std::atomic<Configuration*>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

}

struct EngineBufferScaleRubberBand::State {
    explicit State(ReadAheadManager* reader)
            : reader(reader), worker([this] { prepare(); }) {
    }

    ~State() {
        stopping.store(true, std::memory_order_release);
        worker.join();
        delete ready.exchange(nullptr);
        delete retired.exchange(nullptr);
        delete active;
    }

    void prepare() {
        while (!stopping.load(std::memory_order_acquire)) {
            delete retired.exchange(nullptr, std::memory_order_acq_rel);
            const auto key = requested.load(std::memory_order_acquire);
            auto* waiting = ready.load(std::memory_order_acquire);
            if (key != kInvalidFormat && adopted.load(std::memory_order_acquire) != key &&
                    (!waiting || waiting->key != key)) {
                auto config = std::make_unique<Configuration>(key);
                delete ready.exchange(config.release(), std::memory_order_acq_rel);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool adopt() {
        if (retired.load(std::memory_order_acquire)) {
            return active && active->key == requested.load(std::memory_order_acquire);
        }
        if (auto* config = ready.exchange(nullptr, std::memory_order_acq_rel)) {
            const auto key = requested.load(std::memory_order_acquire);
            if (config->key == key) {
                auto* previous = active;
                active = config;
                adopted.store(key, std::memory_order_release);
                if (previous) {
                    retired.store(previous, std::memory_order_release);
                }
            } else {
                retired.store(config, std::memory_order_release);
            }
        }
        return active && active->key == requested.load(std::memory_order_acquire);
    }

    ReadAheadManager* const reader;
    Configuration* active = nullptr;
    std::atomic<std::uint64_t> requested{kInvalidFormat};
    std::atomic<std::uint64_t> adopted{kInvalidFormat};
    std::atomic<Configuration*> ready{nullptr};
    std::atomic<Configuration*> retired{nullptr};
    std::atomic<bool> stopping{false};
    std::thread worker;

    bool needsReset = true;
    bool backwards = false;
    int remainingStartDelay = 0;
    int pendingRequired = 0;
    int pendingCollected = 0;
    double requestedRate = 1;
    double pendingRate = 1;
    double pendingPitch = 1;
};

EngineBufferScaleRubberBand::EngineBufferScaleRubberBand(ReadAheadManager* reader)
        : m_state(std::make_unique<State>(reader)) {
}

EngineBufferScaleRubberBand::~EngineBufferScaleRubberBand() = default;

void EngineBufferScaleRubberBand::onSignalChanged() {
    const auto channels = getOutputSignal().getChannelCount();
    const auto key = channels == mixxx::audio::ChannelCount::stereo() ||
                            channels == mixxx::audio::ChannelCount::stem()
            ? formatKey(getOutputSignal())
            : kInvalidFormat;
    m_state->requested.store(key, std::memory_order_release);
    clear();
}

bool EngineBufferScaleRubberBand::supportsParameters(
        double baseRate, double tempo, double pitch) {
    const double rate = baseRate * std::fabs(tempo);
    const double transpose = baseRate * pitch;
    return util_isfinite(rate) && util_isfinite(transpose) &&
            rate >= kMinimumRatio && rate <= kMaximumRatio &&
            transpose >= kMinimumPitch && transpose <= kMaximumPitch;
}

void EngineBufferScaleRubberBand::setScaleParameters(
        double baseRate, double* tempo, double* pitch) {
    const bool backwards = util_isfinite(*tempo) && *tempo < 0;
    if (backwards != m_state->backwards) {
        clear();
    }
    m_state->backwards = backwards;
    const double speed = util_isfinite(*tempo) ? std::fabs(*tempo) : 0;
    constexpr double minimumBaseRate = static_cast<double>(mixxx::audio::SampleRate::kValueMin) /
            mixxx::audio::SampleRate::kValueMax;
    m_dBaseRate = util_isfinite(baseRate) && baseRate >= minimumBaseRate &&
                    baseRate <= 1.0 / minimumBaseRate
            ? baseRate : 0;
    m_dTempoRatio = speed >= MIN_SEEK_SPEED && m_dBaseRate > 0
            ? std::clamp(speed, kMinimumRatio / m_dBaseRate, kMaximumRatio / m_dBaseRate)
            : 0;
    m_dPitchRatio = util_isfinite(*pitch) && *pitch > 0 ? *pitch : 1;
    m_state->requestedRate = m_dBaseRate * m_dTempoRatio;
    if (m_dBaseRate > 0) {
        m_dPitchRatio = std::clamp(m_dPitchRatio,
                kMinimumPitch / m_dBaseRate, kMaximumPitch / m_dBaseRate);
    }
    *tempo = backwards ? -m_dTempoRatio : m_dTempoRatio;
    *pitch = m_dPitchRatio;
}

void EngineBufferScaleRubberBand::clear() {
    auto& state = *m_state;
    state.needsReset = true;
    state.remainingStartDelay = 0;
    state.pendingRequired = 0;
    state.pendingCollected = 0;
}

double EngineBufferScaleRubberBand::scaleBuffer(CSAMPLE* output, SINT samples) {
    if (!output || samples <= 0) {
        return 0;
    }
    SampleUtil::clear(output, samples);
    const int channels = getOutputSignal().getChannelCount();
    if (channels <= 0 || samples % channels != 0 || !m_state->adopt()) {
        return 0;
    }
    const int outputFrames = samples / channels;
    if (outputFrames > kInputFrames || m_dBaseRate <= 0 || m_dTempoRatio <= 0) {
        return 0;
    }
    auto& state = *m_state;
    auto& config = *state.active;
    auto& stretcher = *config.stretcher;

    if (state.needsReset) {
        stretcher.reset();
        stretcher.setTimeRatio(1.0 / state.requestedRate);
        stretcher.setPitchScale(m_dBaseRate * m_dPitchRatio);
        m_effectiveRate = state.requestedRate;
        state.remainingStartDelay = static_cast<int>(stretcher.getStartDelay());
        std::fill(config.input.begin(), config.input.end(), 0);
        const int padFrames = static_cast<int>(stretcher.getPreferredStartPad());
        int padded = 0;
        while (padded < padFrames) {
            const int count = std::min(kInputFrames, padFrames - padded);
            stretcher.process(config.inputPointers.data(), count, false);
            padded += count;
        }
        state.needsReset = false;
    }

    int written = 0;
    int reads = 0;
    int zeroReads = 0;
    double consumedFrames = 0;
    while (written < outputFrames && reads < kReadBudget &&
            zeroReads < kZeroReadBudget) {
        const int available = stretcher.available();
        if (available > 0) {
            const int retrieveFrames = std::min(
                    {available, kInputFrames,
                            state.remainingStartDelay + outputFrames - written});
            const int received = static_cast<int>(stretcher.retrieve(
                    config.outputPointers.data(), retrieveFrames));
            if (received <= 0) {
                break;
            }
            const int dropped = std::min(received, state.remainingStartDelay);
            state.remainingStartDelay -= dropped;
            const int deliver = std::min(received - dropped, outputFrames - written);
            for (int frame = 0; frame < deliver; ++frame) {
                for (int channel = 0; channel < channels; ++channel) {
                    output[(written + frame) * channels + channel] =
                            config.outputPointers[channel][dropped + frame];
                }
            }
            written += deliver;
            consumedFrames += m_effectiveRate * deliver;
            continue;
        }

        if (state.pendingRequired == 0) {
            const auto required = stretcher.getSamplesRequired();
            if (required == 0) {
                break;
            }
            state.pendingRequired = std::min<int>(required, kInputFrames);
            state.pendingCollected = 0;
            state.pendingRate = state.backwards ? -state.requestedRate : state.requestedRate;
            state.pendingPitch = m_dBaseRate * m_dPitchRatio;
        }

        const int need = state.pendingRequired - state.pendingCollected;
        ++reads;
        const auto result = state.reader->getNextSamplesForStretch(
                state.pendingRate,
                config.interleaved.data(),
                need * channels,
                getOutputSignal().getChannelCount());
        if (result.unavailable) {
            return consumedFrames;
        }
        const int readFrames = result.samplesRead / channels;
        if (result.samplesRead < 0 || result.samplesRead % channels != 0 || readFrames > need) {
            break;
        }
        if (readFrames == 0) {
            ++zeroReads;
            continue;
        }
        zeroReads = 0;
        for (int frame = 0; frame < readFrames; ++frame) {
            for (int channel = 0; channel < channels; ++channel) {
                config.inputPointers[channel][state.pendingCollected + frame] =
                        config.interleaved[frame * channels + channel];
            }
        }
        state.pendingCollected += readFrames;
        if (state.pendingCollected == state.pendingRequired) {
            const double rate = std::fabs(state.pendingRate);
            stretcher.setTimeRatio(1.0 / rate);
            stretcher.setPitchScale(state.pendingPitch);
            m_effectiveRate = rate;
            stretcher.process(config.inputPointers.data(), state.pendingRequired, false);
            state.pendingRequired = 0;
            state.pendingCollected = 0;
        }
    }
    return consumedFrames;
}
