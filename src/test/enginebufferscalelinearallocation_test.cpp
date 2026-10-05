#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include "engine/bufferscalers/enginebufferscalelinear.h"
#include "engine/engine.h"
#include "engine/readaheadmanager.h"
#include "test/callbackallocationcheck.h"

namespace {
class LinearAllocationReader final : public ReadAheadManager {
  public:
    SINT getNextSamples(double,
            CSAMPLE* output,
            SINT samples,
            mixxx::audio::ChannelCount) override {
        for (SINT i = 0; i < samples; ++i) {
            output[i] = 0.25f;
        }
        return samples;
    }
};
}

class EngineBufferScaleLinearAllocationTest : public testing::Test {
  protected:
    static std::array<const CSAMPLE*, 3> scratchPointers(const EngineBufferScaleLinear& scaler) {
        return {scaler.m_floorSampleOld.data(),
                scaler.m_floorSample.data(),
                scaler.m_ceilSample.data()};
    }

    static std::array<SINT, 3> scratchSizes(const EngineBufferScaleLinear& scaler) {
        return {scaler.m_floorSampleOld.size(),
                scaler.m_floorSample.size(),
                scaler.m_ceilSample.size()};
    }
};

TEST_F(EngineBufferScaleLinearAllocationTest, SignalChangesAndScratchReuseMaximumStorage) {
    LinearAllocationReader reader;
    EngineBufferScaleLinear scaler(&reader);
    const auto initialPointers = scratchPointers(scaler);
    const auto initialSizes = scratchSizes(scaler);
    for (const auto size : initialSizes) {
        ASSERT_GE(size, static_cast<SINT>(mixxx::kMaxEngineChannelInputCount));
    }
    bool storageUnchanged = true;
    bool outputFinite = true;
    std::array<CSAMPLE, 8 * mixxx::kMaxEngineChannelInputCount> output{};
    mixxxtest::callbackAllocations = 0;
    mixxxtest::callbackDeallocations = 0;
    mixxxtest::countCallbackAllocations = true;
    for (const auto channels : {mixxx::audio::ChannelCount::mono(),
                 mixxx::audio::ChannelCount::stereo(),
                 mixxx::kMaxEngineChannelInputCount,
                 mixxx::audio::ChannelCount::stereo(),
                 mixxx::kMaxEngineChannelInputCount}) {
        scaler.setSignal(mixxx::audio::SampleRate(48000), channels);
        scaler.clear();
        storageUnchanged &= scratchSizes(scaler) == initialSizes;
        for (const auto* pointer : scratchPointers(scaler)) {
            bool original = false;
            for (const auto* initial : initialPointers) {
                original |= pointer == initial;
            }
            storageUnchanged &= original;
        }
        double tempo = 0.5;
        double pitch = 1;
        scaler.setScaleParameters(1, &tempo, &pitch);
        scaler.scaleBuffer(output.data(), 8 * channels);
        tempo = -0.5;
        scaler.setScaleParameters(1, &tempo, &pitch);
        scaler.scaleBuffer(output.data(), 8 * channels);
        for (const auto sample : output) {
            outputFinite &= std::isfinite(sample);
        }
        const auto currentPointers = scratchPointers(scaler);
        for (const auto* pointer : currentPointers) {
            bool original = false;
            for (const auto* initial : initialPointers) {
                original |= pointer == initial;
            }
            storageUnchanged &= original;
        }
    }
    mixxxtest::countCallbackAllocations = false;
    EXPECT_TRUE(storageUnchanged);
    EXPECT_TRUE(outputFinite);
    EXPECT_EQ(0u, mixxxtest::callbackAllocations);
    EXPECT_EQ(0u, mixxxtest::callbackDeallocations);
}
