#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include "engine/bufferscalers/enginebufferscalesignalsmith.h"
#include "engine/readaheadmanager.h"
#include "signalsmith-stretch.h"
#include "test/mixxxtest.h"
#include "util/defs.h"
#include "util/sample.h"

namespace {

class ScriptedReader final : public ReadAheadManager {
  public:
    SINT getNextSamples(double rate,
            CSAMPLE* buffer,
            SINT requested,
            mixxx::audio::ChannelCount) override {
        lastRate = rate;
        if (++calls > callLimit) {
            throw std::runtime_error("Scaler exceeded bounded reader-call limit");
        }
        SINT count = requested;
        if (!reads.empty()) {
            count = std::min(count, reads.front());
            reads.erase(reads.begin());
        }
        for (SINT i = 0; i < count; ++i) {
            buffer[i] = samples.empty() ? 0.0f : samples[position++ % samples.size()];
        }
        consumed += count;
        return count;
    }

    std::vector<CSAMPLE> samples;
    std::vector<SINT> reads;
    SINT position{0};
    SINT consumed{0};
    SINT calls{0};
    SINT callLimit{100};
    double lastRate{0};
};

class EngineBufferScaleSignalSmithTest : public MixxxTest {
  protected:
    void initialize(EngineBufferScaleSignalSmith* scaler,
            mixxx::audio::ChannelCount channels = mixxx::audio::ChannelCount::stereo(),
            double tempo = 1.0,
            double pitch = 1.0) {
        scaler->setSignal(mixxx::audio::SampleRate(44100), channels);
        scaler->setScaleParameters(1.0, &tempo, &pitch);
    }

    void fillTone(ScriptedReader* reader, SINT channels) {
        for (int frame = 0; frame < 8192; ++frame) {
            const CSAMPLE sample = static_cast<CSAMPLE>(0.4 * std::sin(2.0 * M_PI * frame / 48.0));
            for (SINT channel = 0; channel < channels; ++channel) {
                reader->samples.push_back(sample);
            }
        }
    }

    void compare(const std::vector<CSAMPLE>& actual, const std::vector<CSAMPLE>& expected) {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            EXPECT_NEAR(actual[i], expected[i], 1e-5) << i;
        }
    }
};

TEST_F(EngineBufferScaleSignalSmithTest, InstancesAndRecreatedScalerKeepIndependentOutput) {
    ScriptedReader readers[3];
    auto channels = {mixxx::audio::ChannelCount::stereo(), mixxx::audio::ChannelCount::stem()};
    for (auto channelCount : channels) {
        for (auto& reader : readers) {
            reader.samples.clear();
            reader.position = 0;
            fillTone(&reader, channelCount);
        }
        EngineBufferScaleSignalSmith first(&readers[0]);
        EngineBufferScaleSignalSmith second(&readers[1]);
        initialize(&first, channelCount);
        initialize(&second, channelCount);
        std::vector<CSAMPLE> output(512 * channelCount);
        std::vector<CSAMPLE> expected(output.size());
        first.scaleBuffer(output.data(), output.size());
        second.scaleBuffer(expected.data(), expected.size());
        compare(output, expected);
        auto survivor = std::make_unique<EngineBufferScaleSignalSmith>(&readers[2]);
        initialize(survivor.get(), channelCount);
        std::vector<CSAMPLE> survivorOutput(output.size());
        survivor->scaleBuffer(survivorOutput.data(), survivorOutput.size());
        compare(survivorOutput, expected);
        survivor.reset();
        readers[2].position = 0;
        survivor = std::make_unique<EngineBufferScaleSignalSmith>(&readers[2]);
        initialize(survivor.get(), channelCount);
        std::vector<CSAMPLE> recreated(output.size());
        survivor->scaleBuffer(recreated.data(), recreated.size());
        compare(recreated, expected);
    }
}

TEST_F(EngineBufferScaleSignalSmithTest, PitchShiftProcessesMonoInput) {
    ScriptedReader reader;
    fillTone(&reader, 1);
    EngineBufferScaleSignalSmith scaler(&reader);
    initialize(&scaler, mixxx::audio::ChannelCount::mono(), 1.0, 1.5);
    std::vector<CSAMPLE> warmup(512);
    scaler.scaleBuffer(warmup.data(), warmup.size());
    const SINT inputStart = reader.position;
    std::vector<CSAMPLE> output(512);
    scaler.scaleBuffer(output.data(), output.size());
    bool changed = false;
    for (size_t i = 0; i < output.size(); ++i) {
        EXPECT_TRUE(std::isfinite(output[i]));
        changed |= std::abs(output[i] - reader.samples[(inputStart + i) % reader.samples.size()]) > 1e-3;
    }
    EXPECT_TRUE(changed);
}

TEST_F(EngineBufferScaleSignalSmithTest, PartialAndZeroPrerollMatchesContiguousReference) {
    ScriptedReader partial;
    ScriptedReader contiguous;
    fillTone(&partial, 2);
    fillTone(&contiguous, 2);
    EngineBufferScaleSignalSmith scaler(&partial);
    EngineBufferScaleSignalSmith reference(&contiguous);
    initialize(&scaler);
    initialize(&reference);
    partial.reads = {6, 0};
    std::vector<CSAMPLE> actual(1024);
    std::vector<CSAMPLE> expected(1024);
    EXPECT_EQ(scaler.scaleBuffer(actual.data(), actual.size()), 512.0);
    reference.scaleBuffer(expected.data(), expected.size());
    EXPECT_EQ(partial.consumed, contiguous.consumed);
    compare(actual, expected);
}

TEST_F(EngineBufferScaleSignalSmithTest, RepeatedZeroReadsPadPrerollAndProcessing) {
    for (auto preset : {EngineBufferScaleSignalSmith::Preset::Default,
                 EngineBufferScaleSignalSmith::Preset::Cheaper}) {
        for (bool preroll : {false, true}) {
            ScriptedReader partial;
            ScriptedReader padded;
            fillTone(&partial, 2);
            padded.samples = partial.samples;
            EngineBufferScaleSignalSmith scaler(&partial);
            EngineBufferScaleSignalSmith reference(&padded);
            scaler.setPreset(preset);
            reference.setPreset(preset);
            initialize(&scaler);
            initialize(&reference);
            std::vector<CSAMPLE> actual(1024), expected(1024);
            if (!preroll) {
                scaler.scaleBuffer(actual.data(), actual.size());
                reference.scaleBuffer(expected.data(), expected.size());
            }
            signalsmith::stretch::SignalsmithStretch<float> stretch;
            if (preset == EngineBufferScaleSignalSmith::Preset::Cheaper) {
                stretch.presetCheaper(2, 44100);
            } else {
                stretch.presetDefault(2, 44100);
            }
            const SINT requested = preroll
                    ? (stretch.inputLatency() + stretch.outputLatency()) * 2
                    : actual.size();
            padded.samples.insert(padded.samples.begin() + padded.position + 6,
                    requested - 6,
                    0.0f);
            partial.reads = {6, 0, 0};
            partial.callLimit = partial.calls + (preroll ? 4 : 3);
            EXPECT_EQ(scaler.scaleBuffer(actual.data(), actual.size()), 512.0);
            reference.scaleBuffer(expected.data(), expected.size());
            compare(actual, expected);
            EXPECT_EQ(partial.calls, partial.callLimit);
        }
    }
}

TEST_F(EngineBufferScaleSignalSmithTest, FractionalDemandSurvivesLatencyReductionAcrossTinyCallbacks) {
    ScriptedReader reader;
    EngineBufferScaleSignalSmith scaler(&reader);
    initialize(&scaler);
    std::vector<CSAMPLE> warmup(1024);
    scaler.scaleBuffer(warmup.data(), warmup.size());
    const SINT before = reader.consumed;
    double tempo = 0.125;
    double pitch = 1.0;
    scaler.setScaleParameters(1.0, &tempo, &pitch);
    reader.callLimit = reader.calls + 24000;
    CSAMPLE output[2];
    double reported = 0;
    for (int i = 0; i < 24000; ++i) {
        reported += scaler.scaleBuffer(output, 2);
    }
    signalsmith::stretch::SignalsmithStretch<float> reference;
    reference.presetDefault(2, 44100);
    const SINT latencyReduction = static_cast<SINT>(
                                          std::round(tempo * reference.outputLatency())) -
            reference.outputLatency();
    EXPECT_DOUBLE_EQ(reported, 3000.0);
    EXPECT_EQ(reader.consumed - before, (3000 + latencyReduction) * 2);
    EXPECT_LE(reader.calls, reader.callLimit);
}

TEST_F(EngineBufferScaleSignalSmithTest, ZeroTempoHoldsAudioAndReverseConsumesPositively) {
    ScriptedReader reader;
    fillTone(&reader, 2);
    EngineBufferScaleSignalSmith scaler(&reader);
    initialize(&scaler);
    std::vector<CSAMPLE> output(1024);
    scaler.scaleBuffer(output.data(), output.size());
    double tempo = 0.0;
    double pitch = 1.0;
    scaler.setScaleParameters(1.0, &tempo, &pitch);
    const SINT before = reader.consumed;
    scaler.scaleBuffer(output.data(), output.size());
    EXPECT_EQ(reader.consumed, before);
    EXPECT_TRUE(std::any_of(output.begin(), output.end(), [](CSAMPLE x) { return std::abs(x) > 1e-4; }));
    tempo = -0.75;
    scaler.setScaleParameters(1.0, &tempo, &pitch);
    scaler.clear();
    EXPECT_EQ(scaler.scaleBuffer(output.data(), output.size()), 384.0);
    EXPECT_LT(reader.lastRate, 0.0);
    const SINT consumed = reader.consumed;
    EXPECT_EQ(scaler.scaleBuffer(output.data(), output.size()), 384.0);
    EXPECT_EQ(reader.consumed - consumed, 768);
}

TEST_F(EngineBufferScaleSignalSmithTest, InvalidAndOverCapacityRequestsDoNotReadOrCrossCanaries) {
    ScriptedReader reader;
    EngineBufferScaleSignalSmith scaler(&reader);
    initialize(&scaler);
    CSAMPLE malformed[3] = {0.75f, 0.75f, 0.75f};
    EXPECT_EQ(scaler.scaleBuffer(malformed, -1), 0.0);
    EXPECT_EQ(scaler.scaleBuffer(nullptr, 2), 0.0);
    EXPECT_EQ(reader.calls, 0);
    EXPECT_EQ(scaler.scaleBuffer(malformed, 3), 0.0);
    EXPECT_EQ(reader.calls, 0);
    EXPECT_TRUE(std::all_of(std::begin(malformed), std::end(malformed), [](CSAMPLE x) { return x == 0.0f; }));
    std::vector<CSAMPLE> tooLarge((MAX_BUFFER_LEN + 1) * 2 + 2, 0.75f);
    EXPECT_EQ(scaler.scaleBuffer(tooLarge.data() + 1, tooLarge.size() - 2), 0.0);
    EXPECT_EQ(reader.calls, 0);
    EXPECT_EQ(tooLarge.front(), 0.75f);
    EXPECT_EQ(tooLarge.back(), 0.75f);
    EXPECT_TRUE(std::all_of(tooLarge.begin() + 1, tooLarge.end() - 1, [](CSAMPLE x) { return x == 0.0f; }));
    initialize(&scaler, mixxx::audio::ChannelCount(9));
    CSAMPLE unsupported[9];
    std::fill_n(unsupported, 9, 0.75f);
    EXPECT_EQ(scaler.scaleBuffer(unsupported, 9), 0.0);
    EXPECT_EQ(reader.calls, 0);
    EXPECT_TRUE(std::all_of(std::begin(unsupported), std::end(unsupported), [](CSAMPLE x) { return x == 0.0f; }));
}

TEST_F(EngineBufferScaleSignalSmithTest, ReportsClampedParametersAndRecoversFromInvalidInput) {
    ScriptedReader reader;
    EngineBufferScaleSignalSmith scaler(&reader);
    initialize(&scaler);
    double tempo = -101;
    double pitch = 0;
    scaler.setScaleParameters(2, &tempo, &pitch);
    EXPECT_DOUBLE_EQ(tempo, -MAX_SEEK_SPEED / 2);
    EXPECT_DOUBLE_EQ(pitch, 1.0 / MAX_SEEK_SPEED / 2);
    CSAMPLE output[2];
    EXPECT_DOUBLE_EQ(scaler.scaleBuffer(output, 2), MAX_SEEK_SPEED);
    EXPECT_LT(reader.lastRate, 0);
    tempo = std::numeric_limits<double>::quiet_NaN();
    scaler.setScaleParameters(1, &tempo, &pitch);
    EXPECT_DOUBLE_EQ(scaler.scaleBuffer(output, 2), 0);
    EXPECT_EQ(output[0], 0);
    EXPECT_EQ(output[1], 0);
    tempo = pitch = 1;
    scaler.setScaleParameters(1, &tempo, &pitch);
    EXPECT_DOUBLE_EQ(scaler.scaleBuffer(output, 2), 1);
}

TEST_F(EngineBufferScaleSignalSmithTest, CheaperPresetSwitchesAtClearLikeFreshScaler) {
    ScriptedReader switchedReader;
    ScriptedReader freshReader;
    fillTone(&switchedReader, 2);
    fillTone(&freshReader, 2);
    EngineBufferScaleSignalSmith switched(&switchedReader);
    EngineBufferScaleSignalSmith fresh(&freshReader);
    initialize(&switched);
    std::vector<CSAMPLE> discard(1024);
    switched.scaleBuffer(discard.data(), discard.size());
    const SINT consumedBeforeSwitch = switchedReader.consumed;
    switched.setPreset(EngineBufferScaleSignalSmith::Preset::Cheaper);
    switched.clear();
    switched.setSignal(mixxx::audio::SampleRate(44100), mixxx::audio::ChannelCount::stereo());
    switchedReader.position = 0;
    fresh.setPreset(EngineBufferScaleSignalSmith::Preset::Cheaper);
    initialize(&fresh);
    std::vector<CSAMPLE> actual(1024);
    std::vector<CSAMPLE> expected(1024);
    switched.scaleBuffer(actual.data(), actual.size());
    fresh.scaleBuffer(expected.data(), expected.size());
    compare(actual, expected);
    EXPECT_EQ(switchedReader.consumed - consumedBeforeSwitch, freshReader.consumed);
}

} // namespace
