#include "engine/bufferscalers/enginebufferscalebungee.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

#include "engine/bufferscalers/enginebufferscalebungeecore.h"
#include "util/sample.h"
namespace {
using Key = std::uint64_t;
Key formatKey(const mixxx::audio::SignalInfo& signal) {
    const int channels = signal.getChannelCount();
    return signal.isValid() && (channels == 2 || channels == 8)
            ? (static_cast<Key>(signal.getSampleRate().toDouble()) << 8) | channels
            : 0;
}
struct Configuration {
    const Key key;
    EngineBufferScaleBungeeCore scaler;
    Configuration(Key k, ReadAheadManager* reader)
            : key(k),
              scaler(reader) {
        scaler.setSignal(mixxx::audio::SampleRate(static_cast<int>(k >> 8)),
                mixxx::audio::ChannelCount(static_cast<int>(k & 255)));
    }
};
static_assert(std::atomic<Key>::is_always_lock_free);
static_assert(std::atomic<Configuration*>::is_always_lock_free);
} // namespace
struct EngineBufferScaleBungee::State {
    ReadAheadManager* const reader;
    Configuration* active = nullptr;
    std::atomic<Key> requested{0}, adopted{0};
    std::atomic<Configuration*> ready{nullptr}, retired{nullptr};
    std::atomic<bool> stopping{false};
    bool needsReset = true;
    double baseRate = 1, tempo = 1, pitch = 1;
    std::thread worker;
    explicit State(ReadAheadManager* r)
            : reader(r),
              worker([this] { prepare(); }) {
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
            if (key && adopted.load(std::memory_order_acquire) != key && (!waiting || waiting->key != key)) {
                auto config = std::make_unique<Configuration>(key, reader);
                delete ready.exchange(config.release(), std::memory_order_acq_rel);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    bool adopt() {
        if (!retired.load(std::memory_order_acquire)) {
            if (auto* config = ready.exchange(nullptr, std::memory_order_acq_rel)) {
                if (config->key == requested.load(std::memory_order_acquire)) {
                    auto* previous = active;
                    active = config;
                    needsReset = true;
                    adopted.store(config->key, std::memory_order_release);
                    if (previous)
                        retired.store(previous, std::memory_order_release);
                } else
                    retired.store(config, std::memory_order_release);
            }
        }
        return active && active->key == requested.load(std::memory_order_acquire);
    }
};
EngineBufferScaleBungee::EngineBufferScaleBungee(ReadAheadManager* reader)
        : m_state(std::make_unique<State>(reader)) {
}
EngineBufferScaleBungee::~EngineBufferScaleBungee() = default;
void EngineBufferScaleBungee::onSignalChanged() {
    m_state->requested.store(formatKey(getOutputSignal()), std::memory_order_release);
    clear();
}
void EngineBufferScaleBungee::clear() {
    m_state->needsReset = true;
}
void EngineBufferScaleBungee::setScaleParameters(double baseRate, double* tempo, double* pitch) {
    m_state->baseRate = baseRate;
    m_state->tempo = *tempo;
    m_state->pitch = *pitch;
    if (m_state->active) {
        m_state->active->scaler.setScaleParameters(baseRate, tempo, pitch);
        m_state->tempo = *tempo;
        m_state->pitch = *pitch;
    }
}
double EngineBufferScaleBungee::scaleBuffer(CSAMPLE* output, SINT samples) {
    if (!output || samples <= 0)
        return 0;
    SampleUtil::clear(output, samples);
    const int channels = getOutputSignal().getChannelCount();
    if (channels <= 0 || samples % channels || !m_state->adopt())
        return 0;
    auto& core = m_state->active->scaler;
    core.setScaleParameters(m_state->baseRate, &m_state->tempo, &m_state->pitch);
    if (m_state->needsReset) {
        core.clear();
        m_state->needsReset = false;
    }
    return core.scaleBuffer(output, samples);
}
double EngineBufferScaleBungee::getVisualPlayPositionOffset() const {
    return m_state->active ? m_state->active->scaler.getVisualPlayPositionOffset() : 0;
}
