#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "engine/bufferscalers/enginebufferscalerubberband.h"
#include "engine/readaheadmanager.h"
#include "test/callbackallocationcheck.h"

namespace {
class RubberBandReader final : public ReadAheadManager {
  public:
    StretchReadResult getNextSamplesForStretch(double rate,
            CSAMPLE* output,
            SINT samples,
            mixxx::audio::ChannelCount channels) override {
        ++calls;
        if (miss) {
            return {0, true};
        }
        const int frames = static_cast<int>(samples / channels);
        for (int frame = 0; frame < frames; ++frame) {
            const double sourceFrame = position + frame * (rate < 0 ? -1 : 1);
            const float sample = impulse
                    ? (sourceFrame == 12000 ? 1.0f : 0.0f)
                    : static_cast<float>(std::sin(sourceFrame * 2 * M_PI * 440 / 48000));
            for (int ch = 0; ch < channels; ++ch) {
                output[frame * channels + ch] = isolatedChannel < 0 || ch == isolatedChannel
                        ? sample : 0;
            }
        }
        position += frames * (rate < 0 ? -1 : 1);
        return {samples, false};
    }
    double position = 0;
    int calls = 0;
    bool miss = false;
    bool impulse = false;
    int isolatedChannel = -1;
};

class EngineBufferScaleRubberBandTest : public testing::Test {
  protected:
    void ready(int channels) {
        scaler.setSignal(mixxx::audio::SampleRate(48000),
                mixxx::audio::ChannelCount(channels));
        double tempo = 1;
        double pitch = 1;
        scaler.setScaleParameters(1, &tempo, &pitch);
        std::array<float, 8 * 256> buffer{};
        bool configured = false;
        for (int i = 0; i < 1000; ++i) {
            if (scaler.scaleBuffer(buffer.data(), channels * 256) > 0) {
                configured = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_TRUE(configured);
        reader.position = 10000;
        scaler.clear();
    }
    RubberBandReader reader;
    EngineBufferScaleRubberBand scaler{&reader};
};

TEST_F(EngineBufferScaleRubberBandTest, RepeatedSeeksDeliverImmediatelyWithoutFrameDrift) {
    for (const int channels : {2, 8}) {
        ready(channels);
        ASSERT_FALSE(HasFatalFailure());
        for (const int frames : {64, 256, 511}) {
            for (const double rate : {0.73, 1.0, 1.37, -1.0}) {
                SCOPED_TRACE(channels);
                SCOPED_TRACE(frames);
                SCOPED_TRACE(rate);
                double tempo = rate;
                double pitch = 1;
                scaler.setScaleParameters(1, &tempo, &pitch);
                std::vector<float> output(channels * frames);
                for (int jump = 0; jump < 8; ++jump) {
                    reader.position = 10000 + jump * 24000;
                    scaler.clear();
                    double delivered = 0;
                    for (int callback = 0; callback < 16; ++callback) {
                        mixxxtest::callbackAllocations = 0;
                        mixxxtest::callbackDeallocations = 0;
                        mixxxtest::countCallbackAllocations = true;
                        const double consumed = scaler.scaleBuffer(output.data(), output.size());
                        mixxxtest::countCallbackAllocations = false;
                        EXPECT_EQ(0u, mixxxtest::callbackAllocations);
                        EXPECT_EQ(0u, mixxxtest::callbackDeallocations);
                        EXPECT_NEAR(frames * std::abs(rate), consumed, 1e-8);
                        delivered += consumed;
                        double energy = 0;
                        for (int frame = 0; frame < frames; ++frame) {
                            energy += output[frame * channels] * output[frame * channels];
                            for (int ch = 1; ch < channels; ++ch) {
                                EXPECT_NEAR(output[frame * channels],
                                        output[frame * channels + ch], 1e-6);
                            }
                        }
                        EXPECT_GT(energy, 0);
                    }
                    EXPECT_NEAR(16 * frames * std::abs(rate), delivered, 1e-7);
                }
            }
        }
    }
}

TEST_F(EngineBufferScaleRubberBandTest, UnitySeekAlignsAcousticLandmarkWithoutStemCrosstalk) {
    for (const int channels : {2, 8}) {
        ready(channels);
        ASSERT_FALSE(HasFatalFailure());
        reader.impulse = true;
        reader.isolatedChannel = channels - 1;
        reader.position = 0;
        scaler.clear();
        std::array<float, 8 * 256> output{};
        std::vector<float> capture(16384);
        for (int offset = 0; offset < 16384; offset += 256) {
            ASSERT_EQ(256, scaler.scaleBuffer(output.data(), channels * 256));
            for (int frame = 0; frame < 256; ++frame) {
                capture[offset + frame] = output[frame * channels + channels - 1];
                for (int channel = 0; channel < channels - 1; ++channel) {
                    EXPECT_EQ(0, output[frame * channels + channel]);
                }
            }
        }
        const auto peak = std::max_element(capture.begin(), capture.end(),
                [](float left, float right) { return std::fabs(left) < std::fabs(right); });
        EXPECT_NEAR(12000, peak - capture.begin(), 1);
        EXPECT_GT(std::fabs(*peak), 0.9);
        reader.impulse = false;
        reader.isolatedChannel = -1;
    }
}

TEST(EngineBufferScaleRubberBandParametersTest, ExtremeDspRatiosAreRejected) {
    EXPECT_TRUE(EngineBufferScaleRubberBand::supportsParameters(1, 0.73, 1));
    EXPECT_TRUE(EngineBufferScaleRubberBand::supportsParameters(2, -1.9, 1));
    EXPECT_FALSE(EngineBufferScaleRubberBand::supportsParameters(1, 0.01, 1));
    EXPECT_FALSE(EngineBufferScaleRubberBand::supportsParameters(1, 16, 1));
    EXPECT_FALSE(EngineBufferScaleRubberBand::supportsParameters(1, 1, 0.0625));
    EXPECT_FALSE(EngineBufferScaleRubberBand::supportsParameters(1, 1, 8));
    EXPECT_FALSE(EngineBufferScaleRubberBand::supportsParameters(0, 1, 1));
}

TEST_F(EngineBufferScaleRubberBandTest, CacheMissDoesNotFabricateTransportProgress) {
    ready(8);
    ASSERT_FALSE(HasFatalFailure());
    reader.miss = true;
    std::array<float, 8 * 256> output{};
    reader.calls = 0;
    EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    EXPECT_LE(reader.calls, 8);
    for (const auto sample : output) {
        EXPECT_EQ(0, sample);
    }
    reader.miss = false;
    EXPECT_EQ(256, scaler.scaleBuffer(output.data(), output.size()));
}
}
