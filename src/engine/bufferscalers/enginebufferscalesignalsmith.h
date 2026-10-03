#pragma once

#include <memory>

#include "engine/bufferscalers/enginebufferscale.h"

class ReadAheadManager;

/// Signalsmith deck adapter. setSignal/clear/scaleBuffer are audio-thread-only
/// after construction. Format preparation and seek-preroll run off the callback;
/// a pending preroll produces silence and zero consumption until it completes.
/// The owner must stop callbacks before destroying the adapter.
class EngineBufferScaleSignalsmith final : public EngineBufferScale {
  public:
    explicit EngineBufferScaleSignalsmith(ReadAheadManager* reader);
    ~EngineBufferScaleSignalsmith() override;

    void setScaleParameters(double baseRate, double* tempo, double* pitch) override;
    void clear() override;
    double scaleBuffer(CSAMPLE* output, SINT samples) override;
    // Crossfade capture may use existing output, but must never initiate a
    // reader preroll that the newly selected scaler will immediately invalidate.
    double scaleBufferForCrossfade(CSAMPLE* output, SINT samples);

#ifdef BUILD_TESTING
    // Holds only the worker, never the callback; for deterministic handoff tests.
    void setPreparationPausedForTest(bool paused);
#endif

  private:
    void onSignalChanged() override;
    struct State;
    std::unique_ptr<State> m_state;
};
