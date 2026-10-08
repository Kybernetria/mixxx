#include "util/defs.h"

#ifdef __BUNGEE__

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "engine/bufferscalers/enginebufferscalebungee.h"
#include "engine/bufferscalers/enginebufferscalebungeecore.h"
#include "engine/readaheadmanager.h"
#include "test/callbackallocationcheck.h"

namespace {
class BungeeReader final : public ReadAheadManager {
  public:
    SINT getNextSamples(double rate,
            CSAMPLE* output,
            SINT samples,
            mixxx::audio::ChannelCount channels) override {
        const bool wasMissing = miss;
        if (wasMissing && !advanceMisses) {
            return 0;
        }
        miss = false;
        const auto read = getNextSamplesForStretch(rate, output, samples, channels);
        miss = wasMissing;
        if (wasMissing) {
            std::fill_n(output, samples, 0.0f);
        }
        return read.samplesRead;
    }

    StretchReadResult getNextSamplesForStretch(double rate,
            CSAMPLE* output,
            SINT samples,
            mixxx::audio::ChannelCount channels) override {
        ++calls;
        if (miss) {
            return {0, true};
        }
        if (zeroReads > 0) {
            --zeroReads;
            return {0, false};
        }
        const int frames = static_cast<int>(std::min<SINT>(readLimit, samples / channels));
        for (int frame = 0; frame < frames; ++frame) {
            const double sourceFrame = position + frame * (rate < 0 ? -1 : 1);
            const float value = impulse
                    ? (sourceFrame == impulseFrame ? 1.0f : 0.0f)
                    : static_cast<float>(std::sin(sourceFrame * 2 * M_PI * 440 / sourceSampleRate));
            for (int channel = 0; channel < channels; ++channel) {
                output[frame * channels + channel] = variedSignal
                        ? static_cast<float>(0.3 * std::sin(sourceFrame * 0.011 + channel * 0.23) +
                                  0.2 * std::sin(sourceFrame * 0.047))
                        : value;
            }
        }
        position += frames * (rate < 0 ? -1 : 1);
        totalRead += frames;
        return {frames * channels, false};
    }

    int calls{0};
    int zeroReads{0};
    int readLimit{8192};
    int sourceSampleRate{48000};
    bool miss{false};
    bool advanceMisses{true};
    bool impulse{false};
    bool variedSignal{false};
    int impulseFrame{12000};
    double position{10000};
    double totalRead{0};
};

class EngineBufferScaleBungeeTest : public testing::Test {
  protected:
    void ready(int channels) {
        scaler.setSignal(mixxx::audio::SampleRate(48000),
                mixxx::audio::ChannelCount(channels));
        double tempo = 1;
        double pitch = 1;
        scaler.setScaleParameters(1, &tempo, &pitch);
        bool configured = false;
        std::array<float, 8> frame{};
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (scaler.scaleBuffer(frame.data(), channels) > 0) {
                configured = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_TRUE(configured);
        reader.position = 10000;
        reader.totalRead = 0;
        reader.calls = 0;
        scaler.clear();
    }

    BungeeReader reader;
    EngineBufferScaleBungee scaler{&reader};
};

TEST_F(EngineBufferScaleBungeeTest, RepeatedSeekMaintainsChannelsEnergyAndSourceCount) {
    for (const int channels : {2, 8}) {
        ready(channels);
        for (const int frames : {64, 256, 512}) {
            for (const double rate : {0.73, 1.0, 1.37, -1.0}) {
                SCOPED_TRACE(channels);
                SCOPED_TRACE(frames);
                SCOPED_TRACE(rate);
                double tempo = rate;
                double pitch = 1;
                scaler.setScaleParameters(1, &tempo, &pitch);
                std::vector<float> output(frames * channels);
                double sourceFrames = 0;
                for (int seek = 0; seek < 8; ++seek) {
                    reader.position = 10000 + seek * 24000;
                    scaler.clear();
                    for (int callback = 0; callback < 32; ++callback) {
                        const double consumed = scaler.scaleBuffer(output.data(), output.size());
                        EXPECT_NEAR(frames * std::abs(rate), consumed, 1e-7);
                        sourceFrames += consumed;
                        double energy = 0;
                        for (int frame = 0; frame < frames; ++frame) {
                            energy += output[frame * channels] * output[frame * channels];
                            for (int channel = 1; channel < channels; ++channel) {
                                EXPECT_NEAR(output[frame * channels],
                                        output[frame * channels + channel],
                                        1e-5);
                            }
                        }
                        if (callback == 0) {
                            EXPECT_GT(energy, 0);
                        }
                    }
                }
                EXPECT_NEAR(8 * 32 * frames * std::abs(rate), sourceFrames, 1e-6);
            }
        }
    }
}

TEST_F(EngineBufferScaleBungeeTest, ImpulseLandmarksRemainOnTimeline) {
    for (const int channels : {2, 8}) {
        ready(channels);
        reader.impulse = true;
        reader.position = 0;
        reader.impulseFrame = 0;
        double tempo = 1;
        double pitch = 1;
        scaler.setScaleParameters(1, &tempo, &pitch);
        std::vector<float> output(256 * channels);
        ASSERT_GT(scaler.scaleBuffer(output.data(), output.size()), 0);
        EXPECT_GT(std::abs(output[0]), 0.5f);

        for (const double rate : {0.73, 1.0, 1.37}) {
            SCOPED_TRACE(channels);
            SCOPED_TRACE(rate);
            tempo = rate;
            scaler.setScaleParameters(1, &tempo, &pitch);
            reader.position = 0;
            reader.impulseFrame = 12000;
            scaler.clear();
            std::vector<float> capture(32768);
            for (int offset = 0; offset < static_cast<int>(capture.size()); offset += 256) {
                ASSERT_NEAR(256 * rate,
                        scaler.scaleBuffer(output.data(), output.size()),
                        1e-7);
                for (int frame = 0; frame < 256; ++frame) {
                    capture[offset + frame] = output[frame * channels];
                }
            }
            const auto peak = std::max_element(capture.begin(), capture.end(), [](float a, float b) {
                return std::abs(a) < std::abs(b);
            });
            EXPECT_LE(std::abs(static_cast<double>(peak - capture.begin()) - 12000 / rate), 1.0);
        }
    }
}

TEST_F(EngineBufferScaleBungeeTest, MissAdvancesWithSilenceAndZeroReadsRemainBounded) {
    ready(2);
    std::array<float, 512> output{};
    reader.impulse = true;
    reader.position = 0;
    reader.impulseFrame = 0;
    scaler.clear();
    reader.miss = true;
    for (int callback = 0; callback < 16; ++callback) {
        EXPECT_DOUBLE_EQ(256, scaler.scaleBuffer(output.data(), output.size()));
    }
    EXPECT_GT(reader.position, 0);
    reader.miss = false;
    EXPECT_GT(scaler.scaleBuffer(output.data(), output.size()), 0);

    scaler.clear();
    reader.zeroReads = 63;
    EXPECT_GT(scaler.scaleBuffer(output.data(), output.size()), 0);
    scaler.clear();
    reader.zeroReads = 64;
    const double positionBeforeBudget = reader.position;
    EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    EXPECT_EQ(positionBeforeBudget, reader.position);
    EXPECT_GT(scaler.scaleBuffer(output.data(), output.size()), 0);
}

TEST_F(EngineBufferScaleBungeeTest, CallbackOperationsDoNotAllocate) {
    ready(2);
    std::array<float, 512> output{};
    double tempo = 1;
    double pitch = 1;
    mixxxtest::callbackAllocations = 0;
    mixxxtest::callbackDeallocations = 0;
    mixxxtest::countCallbackAllocations = true;
    scaler.clear();
    scaler.setScaleParameters(1, &tempo, &pitch);
    scaler.scaleBuffer(output.data(), output.size());
    mixxxtest::countCallbackAllocations = false;
    EXPECT_EQ(0u, mixxxtest::callbackAllocations);
    EXPECT_EQ(0u, mixxxtest::callbackDeallocations);
}

TEST(EngineBufferScaleBungeePendingGrainTest, ParameterChangesPreservePendingGrainTimeline) {
    for (const int channels : {2, 8}) {
        for (const double direction : {1.0, -1.0}) {
            SCOPED_TRACE(channels);
            SCOPED_TRACE(direction);
            BungeeReader stalledReader;
            BungeeReader referenceReader;
            stalledReader.variedSignal = true;
            stalledReader.advanceMisses = false;
            referenceReader.variedSignal = true;
            EngineBufferScaleBungeeCore stalled(&stalledReader);
            EngineBufferScaleBungeeCore reference(&referenceReader);
            for (auto* scaler : {&stalled, &reference}) {
                scaler->setSignal(mixxx::audio::SampleRate(48000),
                        mixxx::audio::ChannelCount(channels));
                double tempo = direction;
                double pitch = 1;
                scaler->setScaleParameters(1, &tempo, &pitch);
            }
            std::array<float, 8> actual{};
            std::array<float, 8> expected{};
            for (int frame = 0; frame < 4096; ++frame) {
                ASSERT_GT(stalled.scaleBuffer(actual.data(), channels), 0);
                ASSERT_GT(reference.scaleBuffer(expected.data(), channels), 0);
            }
            stalledReader.miss = true;
            int cachedFrames = 0;
            while (stalled.scaleBuffer(actual.data(), channels) > 0) {
                ASSERT_GT(reference.scaleBuffer(expected.data(), channels), 0);
                ASSERT_LT(++cachedFrames, 20000);
            }
            ASSERT_GT(reference.scaleBuffer(expected.data(), channels), 0);
            for (auto* scaler : {&stalled, &reference}) {
                double tempo = direction * 1.37;
                double pitch = 1.5;
                scaler->setScaleParameters(1, &tempo, &pitch);
            }
            stalledReader.miss = false;
            for (int frame = 0; frame < 4096; ++frame) {
                if (frame > 0) {
                    ASSERT_GT(reference.scaleBuffer(expected.data(), channels), 0);
                }
                ASSERT_GT(stalled.scaleBuffer(actual.data(), channels), 0);
                for (int channel = 0; channel < channels; ++channel) {
                    ASSERT_NEAR(expected[channel], actual[channel], 1e-6);
                }
            }
            EXPECT_DOUBLE_EQ(referenceReader.position, stalledReader.position);
        }
    }
}
} // namespace

#endif
TEST_F(EngineBufferScaleBungeeTest, TempoChangeCountsTheAudibleGrainTimeline) {
    for (const int channels : {2, 8}) {
        ready(channels);
        reader.position = 0;
        reader.impulse = true;
        reader.impulseFrame = 12000;
        scaler.clear();
        double tempo = 1;
        double pitch = 1;
        scaler.setScaleParameters(1, &tempo, &pitch);
        std::array<float, 8> output{};
        double sourceFrames = 0;
        double peakSourceFrame = 0;
        float peak = 0;
        for (int frame = 0; frame < 20000; ++frame) {
            if (frame == 4096) {
                tempo = 1.37;
                scaler.setScaleParameters(1, &tempo, &pitch);
            }
            const double consumed = scaler.scaleBuffer(output.data(), channels);
            if (std::abs(output[0]) > peak) {
                peak = std::abs(output[0]);
                peakSourceFrame = sourceFrames;
            }
            sourceFrames += consumed;
        }
        EXPECT_GT(peak, 0.4f);
        EXPECT_NEAR(12000, peakSourceFrame, 4);
    }
}
