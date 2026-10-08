#pragma once
#include <memory>

#include "engine/bufferscalers/enginebufferscale.h"
class ReadAheadManager;
class EngineBufferScaleBungee final : public EngineBufferScale {
  public:
    explicit EngineBufferScaleBungee(ReadAheadManager* reader);
    ~EngineBufferScaleBungee() override;
    void setScaleParameters(double baseRate, double* tempo, double* pitch) override;
    double scaleBuffer(CSAMPLE* output, SINT samples) override;
    void clear() override;
    double getVisualPlayPositionOffset() const;

  private:
    void onSignalChanged() override;
    struct State;
    std::unique_ptr<State> m_state;
};
