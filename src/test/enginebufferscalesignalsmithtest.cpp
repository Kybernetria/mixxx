#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "engine/bufferscalers/enginebufferscalesignalsmith.h"
#include "engine/readaheadmanager.h"
#include "test/callbackallocationcheck.h"
#include "util/defs.h"
#include "util/fpclassify.h"

namespace {
class StretchReader final : public ReadAheadManager {
  public:
    StretchReadResult getNextSamplesForStretch(double rate,
            CSAMPLE* output,
            SINT samples,
            mixxx::audio::ChannelCount channels) override {
        ++calls;
        if (miss || calls == missOnCall) {
            return {0, true};
        }
        const int frames = static_cast<int>(std::min<SINT>(samples / channels, maxFrames));
        for (int frame = 0; frame < frames; ++frame) {
            for (int ch = 0; ch < channels; ++ch) {
                const double sourceFrame = position + frame * (rate < 0 ? -1 : 1);
                output[frame * channels + ch] = impulse
                        ? (sourceFrame == impulseFrame ? 1.0f : 0.0f)
                        : static_cast<float>(std::sin(sourceFrame *
                                  (ch + 1) * 2 * M_PI * 440 / sourceSampleRate));
            }
        }
        position += (rate < 0 ? -frames : frames);
        totalRead += frames;
        return {frames * channels, false};
    }
    int calls = 0;
    int missOnCall = -1;
    int maxFrames = 8192;
    bool miss = false;
    bool impulse = false;
    int impulseFrame = 10000;
    int sourceSampleRate = 44100;
    double position = 0;
    double totalRead = 0;
};

class EngineBufferScaleSignalsmithTest : public testing::Test {
  protected:
    void ready(int channelCount = 2, int sampleRate = 44100) {
        scaler.setSignal(mixxx::audio::SampleRate(sampleRate),
                mixxx::audio::ChannelCount(channelCount));
        double tempo = 1;
        double pitch = 1;
        scaler.setScaleParameters(1, &tempo, &pitch);
        bool configured = false;
        for (int attempt = 0; attempt < 1000; ++attempt) {
            // One complete output frame detects readiness without a public
            // API which could accidentally permit blocking preparation in audio.
            std::array<float, mixxx::kMaxEngineChannelInputCount> frame{};
            if (scaler.scaleBuffer(frame.data(), channelCount) > 0) {
                configured = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_TRUE(configured);
        reader.calls = 0;
        reader.position = 0;
        reader.totalRead = 0;
        scaler.clear();
    }
    double scaleEventually(CSAMPLE* output, SINT samples) {
        for (int attempt = 0; attempt < 1000; ++attempt) {
            const double consumed = scaler.scaleBuffer(output, samples);
            if (consumed != 0) {
                return consumed;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 0;
    }
    StretchReader reader;
    EngineBufferScaleSignalsmith scaler{&reader};
};

TEST_F(EngineBufferScaleSignalsmithTest, LiveRecoveryPreservesImpulseTimelineAndWorkBounds) {
    for (const int channels : {2, 8}) {
        for (const int frames : {64, 256, 511, 1024}) {
            SCOPED_TRACE(channels);
            SCOPED_TRACE(frames);
            ready(channels);
            reader.impulse = true;
            reader.impulseFrame = 24000;
            scaler.setPreparationPausedForTest(true);
            scaler.setLiveTimeline(true);
            std::vector<float> output(40000 * channels, 0);
            std::vector<float> buffer(frames * channels);
            double traversed = 0;
            int elapsed = 0;
            for (int i = 0; elapsed + frames <= 40000; ++i) {
                if (i == 8) {
                    scaler.setPreparationPausedForTest(false);
                    for (int wait = 0; wait < 1000 && !scaler.preparationReadyForTest(); ++wait) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    ASSERT_TRUE(scaler.preparationReadyForTest());
                }
                mixxxtest::callbackAllocations = 0;
                mixxxtest::callbackDeallocations = 0;
                mixxxtest::countCallbackAllocations = true;
                const double delivered = scaler.scaleBuffer(buffer.data(), buffer.size());
                mixxxtest::countCallbackAllocations = false;
                EXPECT_EQ(0u, mixxxtest::callbackAllocations);
                EXPECT_EQ(0u, mixxxtest::callbackDeallocations);
                traversed += delivered + scaler.discardedFrames();
                elapsed += frames;
                if (delivered > 0) {
                    EXPECT_NEAR(elapsed, traversed, 1.0);
                }
                const int workFrames = frames * 2;
                const int blockFrames = std::min(frames, 256);
                EXPECT_LE(scaler.processedBlocksForTest(),
                        (workFrames + blockFrames - 1) / blockFrames);
                std::copy(buffer.begin(),
                        buffer.end(),
                        output.begin() + (elapsed - frames) * channels);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            int peak = 0;
            for (int frame = 1; frame < elapsed; ++frame) {
                if (std::abs(output[frame * channels]) > std::abs(output[peak * channels])) {
                    peak = frame;
                }
            }
            EXPECT_GT(std::abs(output[peak * channels]), 0.1);
            EXPECT_NEAR(24000, peak, 2);
            EXPECT_FALSE(scaler.isRecoveringLiveTimeline());
        }
    }
}

TEST_F(EngineBufferScaleSignalsmithTest, ExactSeekCancelsLiveTimelineDebt) {
    ready();
    std::array<float, 512> output{};
    scaler.setPreparationPausedForTest(true);
    scaler.setLiveTimeline(true);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    }
    scaler.clear();
    reader.position = 20000;
    scaler.setPreparationPausedForTest(false);
    EXPECT_EQ(256, scaleEventually(output.data(), output.size()));
    EXPECT_EQ(0, scaler.discardedFrames());
    EXPECT_FALSE(scaler.isRecoveringLiveTimeline());
}

TEST_F(EngineBufferScaleSignalsmithTest, HeldWorkerDoesNotReadOrAllocateOnCallbackAndCancels) {
    ready();
    scaler.setPreparationPausedForTest(true);
    std::array<float, 512> output{};
    ASSERT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    const auto calls = reader.calls;
    scaler.clear();
    reader.position = 20000;
    reader.totalRead = 0;
    mixxxtest::callbackAllocations = 0;
    mixxxtest::callbackDeallocations = 0;
    mixxxtest::countCallbackAllocations = true;
    for (int i = 0; i < 64; ++i) {
        scaler.scaleBuffer(output.data(), output.size());
    }
    mixxxtest::countCallbackAllocations = false;
    EXPECT_EQ(calls, reader.calls);
    EXPECT_EQ(0u, mixxxtest::callbackAllocations);
    EXPECT_EQ(0u, mixxxtest::callbackDeallocations);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }));
    scaler.setPreparationPausedForTest(false);
    ASSERT_EQ(256, scaleEventually(output.data(), output.size()));
    EXPECT_GT(reader.totalRead, 256);
    EXPECT_GT(reader.position, 20000);
}

TEST_F(EngineBufferScaleSignalsmithTest, CrossfadeDoesNotSubmitHeldPreroll) {
    ready();
    scaler.setPreparationPausedForTest(true);
    std::array<float, 512> output{};
    ASSERT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    const auto calls = reader.calls;
    reader.position = 20000;
    reader.totalRead = 0;
    for (int i = 0; i < 32; ++i) {
        std::fill(output.begin(), output.end(), 1.0f);
        EXPECT_EQ(0, scaler.scaleBufferForCrossfade(output.data(), output.size()));
        EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }));
    }
    EXPECT_EQ(calls, reader.calls);
    EXPECT_EQ(0, reader.totalRead);
    scaler.setPreparationPausedForTest(false);
}

TEST_F(EngineBufferScaleSignalsmithTest, HeldWorkerFormatCancellationAndShutdown) {
    ready();
    scaler.setPreparationPausedForTest(true);
    std::array<float, 512> output{};
    ASSERT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    scaler.setSignal(mixxx::audio::SampleRate(48000), mixxx::audio::ChannelCount(8));
    EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    scaler.setSignal(mixxx::audio::SampleRate(44100), mixxx::audio::ChannelCount(2));
    scaler.setPreparationPausedForTest(false);
    EXPECT_EQ(256, scaleEventually(output.data(), output.size()));
    // Teardown with an outstanding job must release its worker-only pause and
    // reclaim its configuration exactly once, without waiting on the callback.
    scaler.clear();
    scaler.setPreparationPausedForTest(true);
    EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
}

TEST_F(EngineBufferScaleSignalsmithTest, FractionalSourceAccountingAndCallbackSizes) {
    ready();
    double tempo = 1.003;
    double pitch = 1;
    scaler.setScaleParameters(48000.0 / 44100, &tempo, &pitch);
    std::array<float, kMaxEngineFrames * 2> output{};
    double consumed = 0;
    int outputFrames = 0;
    for (int i = 0; i < 1000; ++i) {
        const int frames = i % 2 ? 257 : 511;
        consumed += scaleEventually(output.data(), frames * 2);
        outputFrames += frames;
    }
    EXPECT_NEAR(consumed, outputFrames * tempo * 48000.0 / 44100, 1e-6);
    // Integer source cursor includes bounded lookahead and buffered output, not
    // an accumulating fractional-frame loss.
    EXPECT_LT(reader.totalRead - consumed, 16000);
    EXPECT_GT(reader.totalRead - consumed, 0);
}

TEST_F(EngineBufferScaleSignalsmithTest, AsyncSeekSubmissionReturnsSilenceWithoutConsumption) {
    ready();
    std::array<float, 512> output;
    output.fill(999);
    const int callsBefore = reader.calls;
    EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    EXPECT_GT(reader.calls, callsBefore);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }));
    EXPECT_EQ(256, scaleEventually(output.data(), output.size()));
}

TEST_F(EngineBufferScaleSignalsmithTest, PartialSuccessThenMissPreservesInputPrefix) {
    ready();
    std::array<float, 512> expected{};
    ASSERT_EQ(256, scaleEventually(expected.data(), expected.size()));
    const auto uninterruptedRead = reader.totalRead;

    scaler.clear();
    reader.calls = 0;
    reader.position = 0;
    reader.totalRead = 0;
    reader.maxFrames = 1024;
    reader.missOnCall = 2;
    std::array<float, 512> actual;
    actual.fill(999);
    EXPECT_EQ(0, scaler.scaleBuffer(actual.data(), actual.size()));
    EXPECT_EQ(1024, reader.totalRead);
    EXPECT_TRUE(std::all_of(actual.begin(), actual.end(), [](float x) { return x == 0; }));
    reader.missOnCall = -1;
    ASSERT_EQ(256, scaleEventually(actual.data(), actual.size()));
    EXPECT_EQ(uninterruptedRead, reader.totalRead);
    // Recovery has an intentional fade. The input prefix must nevertheless
    // reproduce the uninterrupted output away from that fade.
    EXPECT_NEAR(expected.back(), actual.back(), 0.001);
}

TEST_F(EngineBufferScaleSignalsmithTest, PendingParametersAreFrozenUntilDelivered) {
    ready();
    reader.maxFrames = 512;
    reader.missOnCall = 2;
    std::array<float, 512> output{};
    EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    double tempo = 1.25;
    double pitch = 1.1;
    scaler.setScaleParameters(1, &tempo, &pitch);
    reader.missOnCall = -1;
    // Preroll finishes at its captured rate; the output request starts at the
    // new rate. No prefix is overwritten or re-read.
    EXPECT_EQ(320, scaleEventually(output.data(), output.size()));
}

TEST_F(EngineBufferScaleSignalsmithTest, PartialOrdinaryBatchKeepsCapturedParameters) {
    ready();
    std::array<float, 512> output{};
    ASSERT_EQ(256, scaleEventually(output.data(), output.size()));
    reader.maxFrames = 128;
    reader.missOnCall = reader.calls + 2;
    EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    double tempo = 1.25;
    double pitch = 1.1;
    scaler.setScaleParameters(1, &tempo, &pitch);
    reader.missOnCall = -1;
    EXPECT_EQ(256, scaleEventually(output.data(), output.size()));
}

TEST_F(EngineBufferScaleSignalsmithTest, SeekCancelsObsoletePrefix) {
    ready();
    reader.maxFrames = 512;
    reader.missOnCall = 2;
    std::array<float, 512> output{};
    EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    scaler.clear();
    reader.position = 44100;
    reader.totalRead = 0;
    reader.missOnCall = -1;
    ASSERT_EQ(256, scaleEventually(output.data(), output.size()));
    EXPECT_GT(reader.totalRead, 512);
    EXPECT_GT(reader.position, 44100);
}

TEST_F(EngineBufferScaleSignalsmithTest, SeekInvalidatesUnadoptedPrerollCompletion) {
    ready();
    std::array<float, 512> output{};
    EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    scaler.clear();
    reader.position = 44100;
    reader.totalRead = 0;
    ASSERT_EQ(256, scaleEventually(output.data(), output.size()));
    EXPECT_GT(reader.totalRead, 512);
    EXPECT_GT(reader.position, 44100);
}

TEST_F(EngineBufferScaleSignalsmithTest, StarvationAndZeroReadsHaveBoundedRecovery) {
    ready();
    std::array<float, 512> output;
    output.fill(999);
    reader.miss = true;
    EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    EXPECT_EQ(1, reader.calls);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }));
    reader.miss = false;
    reader.maxFrames = 0;
    reader.calls = 0;
    EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    EXPECT_LE(reader.calls, 64);
    reader.maxFrames = 8192;
    EXPECT_EQ(256, scaleEventually(output.data(), output.size()));
}

TEST_F(EngineBufferScaleSignalsmithTest, StereoStemAndSampleRateTransitions) {
    ready();
    ready(8, 48000);
    std::array<float, 2048> output{};
    ASSERT_EQ(256, scaleEventually(output.data(), output.size()));
    for (int ch = 0; ch < 8; ++ch) {
        double energy = 0;
        for (int frame = 0; frame < 256; ++frame) {
            ASSERT_TRUE(util_isfinite(static_cast<double>(output[frame * 8 + ch])));
            energy += output[frame * 8 + ch] * output[frame * 8 + ch];
        }
        EXPECT_GT(energy, 1);
    }
    ready(2, 96000);
    EXPECT_EQ(1024, scaleEventually(output.data(), output.size()));
}

TEST_F(EngineBufferScaleSignalsmithTest, PauseReverseAndMaximumCallback) {
    ready();
    std::array<float, kMaxEngineFrames * 2> output{};
    double tempo = 0;
    double pitch = 1;
    scaler.setScaleParameters(1, &tempo, &pitch);
    EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    EXPECT_EQ(0, reader.totalRead);
    tempo = -0.73;
    scaler.setScaleParameters(1, &tempo, &pitch);
    EXPECT_NEAR(0.73 * kMaxEngineFrames,
            scaleEventually(output.data(), output.size()),
            1e-8);
    EXPECT_LT(reader.position, 0);
}

TEST_F(EngineBufferScaleSignalsmithTest, PreservesPitchAcrossDjTempoRange) {
    ready();
    std::vector<float> output(44100 * 2);
    for (const double speed : {0.5, 0.73, 1.003, 1.25, 1.9}) {
        scaler.clear();
        reader.position = 0;
        double tempo = speed;
        double pitch = 1;
        scaler.setScaleParameters(1, &tempo, &pitch);
        std::array<float, 512> buffer{};
        for (int i = 0; i < 173; ++i) {
            scaleEventually(buffer.data(), buffer.size());
            if (i < 172) {
                std::copy(buffer.begin(), buffer.end(), output.begin() + i * 512);
            }
        }
        int crossings = 0;
        for (int frame = 22050; frame < 44000; ++frame) {
            if (output[(frame - 1) * 2] <= 0 && output[frame * 2] > 0) {
                ++crossings;
            }
        }
        EXPECT_NEAR(crossings * 44100.0 / (44000 - 22050), 440, 4) << speed;
    }
}
TEST_F(EngineBufferScaleSignalsmithTest, ImpulseMatchesTransportAtUnity) {
    ready();
    reader.impulse = true;
    std::vector<float> output(44100 * 2);
    std::array<float, 512> buffer{};
    for (int i = 0; i < 172; ++i) {
        ASSERT_EQ(256, scaleEventually(buffer.data(), buffer.size()));
        std::copy(buffer.begin(), buffer.end(), output.begin() + i * 512);
    }
    int peak = 0;
    for (int frame = 1; frame < 44000; ++frame) {
        if (std::abs(output[frame * 2]) > std::abs(output[peak * 2]))
            peak = frame;
    }
    EXPECT_GT(std::abs(output[peak * 2]), 0.1);
    EXPECT_NEAR(10000, peak, 2);
}

TEST_F(EngineBufferScaleSignalsmithTest, ImpulseMatchesFractionalRateAndResampling) {
    ready();
    reader.impulse = true;
    std::vector<float> output(44100 * 2);
    std::array<float, 512> buffer{};
    for (const double speed : {0.73, 1.003, 1.25, 1.9}) {
        scaler.clear();
        reader.position = 0;
        double tempo = speed;
        double pitch = 1;
        const double baseRate = 48000.0 / 44100;
        scaler.setScaleParameters(baseRate, &tempo, &pitch);
        for (int i = 0; i < 172; ++i) {
            ASSERT_NEAR(speed * baseRate * 256,
                    scaleEventually(buffer.data(), buffer.size()),
                    1e-8);
            std::copy(buffer.begin(), buffer.end(), output.begin() + i * 512);
        }
        int peak = 0;
        for (int frame = 1; frame < 44000; ++frame) {
            if (std::abs(output[frame * 2]) > std::abs(output[peak * 2]))
                peak = frame;
        }
        EXPECT_NEAR(10000 / (speed * baseRate), peak, 64) << speed;
    }
}

TEST_F(EngineBufferScaleSignalsmithTest, SubFrameRequestsAtMinimumSpeedRemainBounded) {
    ready(2, 192000);
    reader.sourceSampleRate = 16000;
    double tempo = MIN_SEEK_SPEED;
    double pitch = 1;
    const double baseRate = 16000.0 / 192000;
    scaler.setScaleParameters(baseRate, &tempo, &pitch);
    std::array<float, 512> output{};
    for (int i = 0; i < 100; ++i) {
        ASSERT_NEAR(baseRate * tempo * 256,
                scaleEventually(output.data(), output.size()),
                1e-8);
        EXPECT_LT(reader.calls, 200);
        for (const auto sample : output)
            ASSERT_TRUE(util_isfinite(static_cast<double>(sample)));
    }
}

TEST_F(EngineBufferScaleSignalsmithTest, MaximumBufferAndShortPositiveReadsDoNotStarve) {
    ready(2, 8000);
    reader.sourceSampleRate = 192000;
    reader.maxFrames = 150;
    double tempo = 1.9;
    double pitch = 1;
    scaler.setScaleParameters(24, &tempo, &pitch);
    std::vector<float> output(kMaxEngineFrames * 2);
    EXPECT_NEAR(kMaxEngineFrames * 24 * 1.9,
            scaleEventually(output.data(), output.size()),
            1e-6);
    EXPECT_LT(reader.calls, 4096);
    for (const auto sample : output)
        EXPECT_TRUE(util_isfinite(static_cast<double>(sample)));
}

TEST_F(EngineBufferScaleSignalsmithTest, RapidRevertedFormatRequestStillBecomesReady) {
    ready();
    double tempo = 0;
    double pitch = 1;
    scaler.setScaleParameters(1, &tempo, &pitch);
    scaler.setSignal(mixxx::audio::SampleRate(48000), mixxx::audio::ChannelCount::stem());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    scaler.setSignal(mixxx::audio::SampleRate(44100), mixxx::audio::ChannelCount::stereo());
    std::array<float, 16> output{};
    scaler.scaleBuffer(output.data(), 2); // Discard the now-obsolete prepared format.
    scaler.setSignal(mixxx::audio::SampleRate(48000), mixxx::audio::ChannelCount::stem());
    tempo = 1;
    scaler.setScaleParameters(1, &tempo, &pitch);
    double consumed = 0;
    for (int i = 0; i < 2000 && consumed == 0; ++i) {
        consumed = scaler.scaleBuffer(output.data(), output.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GT(consumed, 0);
}

TEST_F(EngineBufferScaleSignalsmithTest, InvalidParametersHaveFiniteBoundedRecovery) {
    ready();
    std::array<float, 512> output{};
    for (const double base : {util_double_nan(),
                 util_double_infinity(),
                 -1.0,
                 0.0,
                 1e300,
                 1e-300}) {
        double tempo = 1;
        double pitch = 1;
        scaler.setScaleParameters(base, &tempo, &pitch);
        EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
        for (const auto sample : output)
            EXPECT_EQ(0, sample);
    }
    for (const double invalid : {util_double_nan(), util_double_infinity()}) {
        double tempo = invalid;
        double pitch = 1;
        scaler.setScaleParameters(1, &tempo, &pitch);
        EXPECT_EQ(0, tempo);
        EXPECT_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    }
    for (const double invalid : {util_double_nan(),
                 util_double_infinity(),
                 -1.0,
                 0.0,
                 1e10,
                 1e300,
                 1e-300}) {
        double tempo = 1;
        double pitch = invalid;
        scaler.setScaleParameters(1, &tempo, &pitch);
        EXPECT_EQ(1, pitch);
        EXPECT_EQ(256, scaleEventually(output.data(), output.size()));
        for (const auto sample : output)
            EXPECT_TRUE(util_isfinite(static_cast<double>(sample)));
    }
}

TEST_F(EngineBufferScaleSignalsmithTest, CallbackDoesNotAllocateOrDestroyConfigurations) {
    ready();
    std::array<float, 512> output{};
    mixxxtest::callbackAllocations = 0;
    mixxxtest::callbackDeallocations = 0;
    mixxxtest::countCallbackAllocations = true;
    for (int i = 0; i < 100; ++i) {
        double tempo = i % 2 ? 0.73 : 1.25;
        double pitch = 1.1;
        scaler.setScaleParameters(1, &tempo, &pitch);
        if (i % 5 == 0)
            scaler.clear();
        scaler.scaleBuffer(output.data(), output.size());
    }
    scaler.setSignal(mixxx::audio::SampleRate(48000), mixxx::audio::ChannelCount::stem());
    scaler.scaleBuffer(output.data(), output.size());
    mixxxtest::countCallbackAllocations = false;
    EXPECT_EQ(0, mixxxtest::callbackAllocations);
    EXPECT_EQ(0, mixxxtest::callbackDeallocations);
}
} // namespace
