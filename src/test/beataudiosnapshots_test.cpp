#include <gtest/gtest.h>

#include "control/controlobject.h"
#include "engine/controls/clockcontrol.h"
#include "engine/controls/quantizecontrol.h"
#include "test/mixxxtest.h"
#include "track/beats.h"

class BeatAudioSnapshotsTest : public MixxxTest {
};

TEST_F(BeatAudioSnapshotsTest, QuantizeReplacementRefreshesCachedBeatRange) {
    const auto group = QStringLiteral("[SnapshotQuantize]");
    const auto sampleRate = mixxx::audio::SampleRate(44100);
    QuantizeControl control(group, config());
    control.setFrameInfo(mixxx::audio::FramePos(1000.0),
            mixxx::audio::FramePos(441000.0), sampleRate);
    control.trackBeatsUpdated(mixxx::Beats::fromConstTempo(
            sampleRate, mixxx::audio::kStartFramePos, mixxx::Bpm(60.0)));
    EXPECT_EQ(ControlObject::get(ConfigKey(group, "beat_next")), 88200.0);
    control.trackBeatsUpdated(mixxx::Beats::fromConstTempo(
            sampleRate, mixxx::audio::kStartFramePos, mixxx::Bpm(120.0)));
    ControlObject::set(ConfigKey(group, "beat_next"), 88200.0);
    control.setFrameInfo(mixxx::audio::FramePos(1001.0),
            mixxx::audio::FramePos(441000.0), sampleRate);
    EXPECT_EQ(ControlObject::get(ConfigKey(group, "beat_prev")), 0.0);
    EXPECT_EQ(ControlObject::get(ConfigKey(group, "beat_next")), 44100.0);
    control.trackBeatsUpdated(nullptr);
    for (const auto key : {"beat_prev", "beat_next", "beat_closest"}) {
        EXPECT_FALSE(mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
                ControlObject::get(ConfigKey(group, key))).isValid());
    }
}

TEST_F(BeatAudioSnapshotsTest, ClockReplacementRefreshesBlinkIntervalInsideOldBeat) {
    const auto group = QStringLiteral("[SnapshotClock]");
    const auto sampleRate = mixxx::audio::SampleRate(44100);
    ControlObject loopEnabled(ConfigKey(group, "loop_enabled"));
    ControlObject loopStart(ConfigKey(group, "loop_start_position"));
    ControlObject loopEnd(ConfigKey(group, "loop_end_position"));
    ClockControl control(group, config());
    control.trackBeatsUpdated(mixxx::Beats::fromConstTempo(
            sampleRate, mixxx::audio::kStartFramePos, mixxx::Bpm(60.0)));
    control.updateIndicators(1.0, mixxx::audio::FramePos(10000.0), sampleRate);
    EXPECT_EQ(ControlObject::get(ConfigKey(group, "beat_active")), 0.0);
    control.trackBeatsUpdated(mixxx::Beats::fromConstTempo(
            sampleRate, mixxx::audio::kStartFramePos, mixxx::Bpm(30.0)));
    control.updateIndicators(1.0, mixxx::audio::FramePos(10001.0), sampleRate);
    EXPECT_EQ(ControlObject::get(ConfigKey(group, "beat_active")), 1.0);
    control.trackBeatsUpdated(nullptr);
    control.updateIndicators(1.0, mixxx::audio::FramePos(10002.0), sampleRate);
    EXPECT_EQ(ControlObject::get(ConfigKey(group, "beat_active")), 0.0);
}
