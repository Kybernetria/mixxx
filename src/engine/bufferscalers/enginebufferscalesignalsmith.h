#pragma once

#include <qtmetamacros.h>

#include <array>
#include <atomic>

#include "engine/bufferscalers/enginebufferscale.h"
#include "signalsmith-stretch.h"
#include "util/samplebuffer.h"

class ReadAheadManager;

class EngineBufferScaleSignalSmith final : public EngineBufferScale {
    Q_OBJECT
  public:
    enum class Preset {
        Default,
        Cheaper
    };
    Q_ENUM(Preset);
    static_assert(std::atomic<Preset>::is_always_lock_free);

    explicit EngineBufferScaleSignalSmith(ReadAheadManager* pReadAheadManager);
    ~EngineBufferScaleSignalSmith() override = default;

    void setScaleParameters(double base_rate, double* pTempoRatio, double* pPitchRatio) override;
    void setPreset(Preset preset) {
        m_requestedPreset.store(preset == Preset::Cheaper ? Preset::Cheaper : Preset::Default,
                std::memory_order_relaxed);
    }
    bool isPresetChangePending() const {
        return m_requestedPreset.load(std::memory_order_relaxed) != m_currentPreset;
    }
    void clear() override;
    double scaleBuffer(CSAMPLE* pOutputBuffer, SINT iOutputBufferSize) override;

  private:
    void onSignalChanged() override;
    void applyPreset();
    void updateLatency();
    void readFrames(SINT frames);
    SINT fetchAndDeinterleave(SINT samples, SINT offset = 0);

    ReadAheadManager* m_pReadAheadManager;
    signalsmith::stretch::SignalsmithStretch<float> m_stretchDefault;
    signalsmith::stretch::SignalsmithStretch<float> m_stretchCheaper;
    signalsmith::stretch::SignalsmithStretch<float>* m_pStretch{&m_stretchDefault};

    std::vector<mixxx::SampleBuffer> m_buffers;
    std::vector<float*> m_bufferPtrs;
    mixxx::SampleBuffer m_interleavedBuffer;
    // AI-assisted explanation (start).
    // Reading finishes before processing, so the interleaved scratch can also
    // hold planar output. Keep its pointers per instance, never function-static.
    // AI-assisted explanation (end).
    std::array<float*, mixxx::audio::ChannelCount::stem()> m_outputBufferPtrs{};

    double m_frameFractionalLeftover{0};
    SINT m_expectedFrameLatency{0};
    double m_currentFrameOffset{0};
    bool m_bBackwards{false};
    bool m_parametersValid{true};
    Preset m_currentPreset{Preset::Default};
    std::atomic<Preset> m_requestedPreset{Preset::Default};
};
