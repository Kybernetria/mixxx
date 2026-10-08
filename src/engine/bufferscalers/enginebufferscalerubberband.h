#pragma once

#include <memory>

#include "engine/bufferscalers/enginebufferscale.h"

class ReadAheadManager;

class EngineBufferScaleRubberBand final : public EngineBufferScale {
  public:
    static bool supportsParameters(double baseRate, double tempo, double pitch);

    explicit EngineBufferScaleRubberBand(ReadAheadManager* reader);
    ~EngineBufferScaleRubberBand() override;

    void setScaleParameters(double baseRate, double* tempo, double* pitch) override;
    void clear() override;
    double scaleBuffer(CSAMPLE* output, SINT samples) override;

  private:
    void onSignalChanged() override;
    struct State;
    std::unique_ptr<State> m_state;
};
