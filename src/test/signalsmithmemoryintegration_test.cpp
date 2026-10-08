#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

#include "controllers/scripting/legacy/controllerscriptinterfacelegacy.h"
#include "effects/backends/builtin/echoeffect.h"
#include "effects/backends/effectsbackendmanager.h"
#include "effects/effectchain.h"
#include "effects/effectslot.h"
#include "effects/presets/effectpreset.h"
#include "test/signalpathtest.h"
#include "track/memorycues.h"
#include "util/fpclassify.h"
#include "util/time.h"
#include "waveform/visualplayposition.h"

namespace {
using mixxx::audio::FramePos;

TEST(KeylockEngineCompatibilityTest, HistoricalIdsAndUnavailableSelections) {
    EXPECT_EQ(0, static_cast<int>(EngineBuffer::KeylockEngine::SoundTouch));
    EXPECT_EQ(1, static_cast<int>(EngineBuffer::KeylockEngine::RubberBandFaster));
    EXPECT_EQ(2, static_cast<int>(EngineBuffer::KeylockEngine::RubberBandFiner));
    EXPECT_EQ(3, static_cast<int>(EngineBuffer::KeylockEngine::RubberBandR3ShortWindow));
    EXPECT_EQ(4, static_cast<int>(EngineBuffer::KeylockEngine::Bungee));
    EXPECT_EQ(5, static_cast<int>(EngineBuffer::KeylockEngine::Signalsmith));
#ifdef __BUNGEE__
    EXPECT_EQ(EngineBuffer::KeylockEngine::Bungee, EngineBuffer::defaultKeylockEngine());
    EXPECT_TRUE(EngineBuffer::isKeylockEngineAvailable(EngineBuffer::KeylockEngine::Bungee));
    EXPECT_FALSE(EngineBuffer::isKeylockEngineAvailable(EngineBuffer::KeylockEngine::Signalsmith));
#else
    EXPECT_EQ(EngineBuffer::KeylockEngine::Signalsmith, EngineBuffer::defaultKeylockEngine());
    EXPECT_TRUE(EngineBuffer::isKeylockEngineAvailable(EngineBuffer::KeylockEngine::Signalsmith));
    EXPECT_FALSE(EngineBuffer::isKeylockEngineAvailable(EngineBuffer::KeylockEngine::Bungee));
#endif
    EXPECT_FALSE(EngineBuffer::isKeylockEngineAvailable(EngineBuffer::KeylockEngine::SoundTouch));
    EXPECT_FALSE(EngineBuffer::isKeylockEngineAvailable(
            EngineBuffer::KeylockEngine::RubberBandFaster));
    for (const auto engine : EngineBuffer::kKeylockEngines) {
        EXPECT_EQ(EngineBuffer::defaultKeylockEngine(),
                EngineBuffer::resolveKeylockEngine(
                        static_cast<double>(engine)));
    }
    for (const double id : {0.0,
                 1.0,
                 2.0,
                 3.0,
                 4.0,
                 5.0,
                 -1.0,
                 99.0,
                 0.5,
                 std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_EQ(EngineBuffer::defaultKeylockEngine(),
                EngineBuffer::resolveKeylockEngine(id));
    }
}

class SignalsmithMemoryIntegrationTest : public SignalPathTest {
  protected:
    void SetUp() override {
        SignalPathTest::SetUp();
        ControlObject::set(ConfigKey("[App]", "keylock_engine"),
                static_cast<double>(EngineBuffer::KeylockEngine::Signalsmith));
        for (auto* deck : {m_pChannel1->getEngineBuffer(),
                     m_pChannel2->getEngineBuffer(),
                     m_pChannel3->getEngineBuffer()}) {
            deck->selectSignalsmithKeylockForTest();
        }
        for (const auto& group : {m_sGroup1, m_sGroup2, m_sGroup3}) {
            ControlObject::set(ConfigKey(group, "quantize"), 0);
            ControlObject::set(ConfigKey(group, "keylock"), 1);
        }
        m_pEngineMixer->process(kProcessBufferSize);
        QTest::qWait(20); // Off-callback format preparation and queued track updates.
        engine()->seekExact(FramePos(1000));
        m_pEngineMixer->process(kProcessBufferSize);
    }
    EngineBuffer* engine() {
        return m_pChannel1->getEngineBuffer();
    }
    void press(const char* control) {
        ControlObject::set(ConfigKey(m_sGroup1, control), 0);
        ControlObject::set(ConfigKey(m_sGroup1, control), 1);
    }
    void process() {
        m_pEngineMixer->process(kProcessBufferSize);
    }
    bool startSteadyPlayback() {
        ControlObject::set(ConfigKey(m_sGroup1, "play"), 1);
        int settled = 0;
        for (int i = 0; i < 1000 && settled < 8; ++i) {
            const auto before = engine()->getExactPlayPos();
            process();
            const double expected = kProcessBufferSize / 2.0 * engine()->getRateRatio();
            settled = std::fabs(engine()->getExactPlayPos() - before - expected) < 1e-6
                    ? settled + 1
                    : 0;
            QTest::qSleep(1);
        }
        return settled == 8;
    }
    void synchronizePlayingDecks(FramePos anchor = FramePos(3000)) {
        ControlObject::set(ConfigKey(m_sGroup1, "quantize"), 0);
        ControlObject::set(ConfigKey(m_sGroup2, "quantize"), 0);
        engine()->requestSyncMode(SyncMode::None);
        m_pChannel2->getEngineBuffer()->requestSyncMode(SyncMode::None);
        for (auto* deck : {engine(), m_pChannel2->getEngineBuffer()}) {
            const auto track = deck->getLoadedTrack();
            ASSERT_TRUE(track->trySetBeats(mixxx::Beats::fromConstTempo(
                    track->getSampleRate(), FramePos(0), mixxx::Bpm(120))));
            deck->setKeylockPreparationPausedForTest(true);
            deck->seekExact(anchor);
        }
        const auto submissions1 = engine()->keylockPreparationSubmissionsForTest();
        auto* leader = m_pChannel2->getEngineBuffer();
        const auto submissions2 = leader->keylockPreparationSubmissionsForTest();
        ControlObject::set(ConfigKey(m_sGroup1, "play"), 1);
        ControlObject::set(ConfigKey(m_sGroup2, "play"), 1);
        for (int i = 0; i < 1000 &&
                (engine()->keylockPreparationSubmissionsForTest() == submissions1 ||
                        leader->keylockPreparationSubmissionsForTest() == submissions2);
                ++i) {
            process();
            QTest::qSleep(1);
        }
        ASSERT_GT(engine()->keylockPreparationSubmissionsForTest(), submissions1);
        ASSERT_GT(leader->keylockPreparationSubmissionsForTest(), submissions2);
        for (auto* deck : {engine(), leader}) {
            deck->setKeylockPreparationPausedForTest(false);
        }
        for (int i = 0; i < 1000 &&
                (!engine()->keylockPreparationReadyForTest() ||
                        !leader->keylockPreparationReadyForTest());
                ++i) {
            QTest::qWait(1);
        }
        ASSERT_TRUE(engine()->keylockPreparationReadyForTest());
        ASSERT_TRUE(leader->keylockPreparationReadyForTest());
        int settled = 0;
        for (int i = 0; i < 500 && settled < 8; ++i) {
            const auto before = engine()->getExactPlayPos();
            process();
            settled = engine()->getExactPlayPos() ==
                                    m_pChannel2->getEngineBuffer()->getExactPlayPos() &&
                            engine()->getExactPlayPos() > before
                    ? settled + 1
                    : 0;
            QTest::qSleep(1);
        }
        ASSERT_EQ(8, settled);
        m_pChannel2->getEngineBuffer()->requestSyncMode(SyncMode::LeaderExplicit);
        engine()->requestSyncMode(SyncMode::Follower);
        process();
        ControlObject::set(ConfigKey(m_sGroup1, "quantize"), 1);
        ControlObject::set(ConfigKey(m_sGroup2, "quantize"), 1);
        process();
        ASSERT_EQ(1, ControlObject::get(ConfigKey(m_sGroup1, "sync_mode")));
        ASSERT_EQ(3, ControlObject::get(ConfigKey(m_sGroup2, "sync_mode")));
        ASSERT_EQ(engine()->getExactPlayPos(),
                m_pChannel2->getEngineBuffer()->getExactPlayPos());
    }
    void verifyScratchSlipRestoration(
            bool waveformScratch, bool reverse, double scratchRate = 0.75) {
        ASSERT_TRUE(startSteadyPlayback());
        if (reverse) {
            ControlObject::set(ConfigKey(m_sGroup1, "reverse"), 1);
            for (int i = 0; i < 4; ++i) {
                process();
            }
        }

        engine()->setKeylockPreparationPausedForTest(true);
        if (waveformScratch) {
            ControlObject::set(ConfigKey(m_sGroup1, "scratch_position_enable"), 1);
            ControlObject::set(ConfigKey(m_sGroup1, "scratch_position"), 10000);
        } else {
            ControlObject::set(ConfigKey(m_sGroup1, "scratch2_enable"), 1);
            ControlObject::set(ConfigKey(m_sGroup1, "scratch2"), 0.0);
            process();
            ControlObject::set(ConfigKey(m_sGroup1, "scratch2"), scratchRate);
        }
        process();
        ASSERT_TRUE(engine()->getScratching());
        const auto anchor = engine()->getExactPlayPos();
        const double naturalStep = kProcessBufferSize / 2.0 * engine()->getRateRatio() *
                (reverse ? -1.0 : 1.0);
        ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 1);
        process();
        for (int i = 0; i < 4; ++i) {
            process();
        }

        ControlObject::set(ConfigKey(m_sGroup1,
                                   waveformScratch ? "scratch_position_enable" : "scratch2_enable"),
                0);
        process();
        ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 0);
        process();

        // Slip advances on enable, each held-scratch callback, and the scratch
        // release callback. It was enabled while scratching, so restoring this
        // position proves the captured rate is independent of foreground motion.
        EXPECT_NEAR(0.0,
                engine()->getExactPlayPos() -
                        (anchor + 6 * naturalStep).toNearestFrameBoundary(),
                1e-9);
        engine()->setKeylockPreparationPausedForTest(false);
    }
    void verifyBeatJumpTimeline(SyncMode mode, bool quantize, bool reverse) {
        synchronizePlayingDecks(FramePos(200000));
        ASSERT_FALSE(HasFatalFailure());
        auto* other = m_pChannel2->getEngineBuffer();
        if (mode == SyncMode::None) {
            engine()->requestSyncMode(SyncMode::None);
            other->requestSyncMode(SyncMode::None);
        } else if (mode == SyncMode::LeaderExplicit) {
            engine()->requestSyncMode(SyncMode::LeaderExplicit);
            other->requestSyncMode(SyncMode::Follower);
        }
        for (const auto& group : {m_sGroup1, m_sGroup2}) {
            ControlObject::set(ConfigKey(group, "quantize"), quantize ? 1 : 0);
        }
        process();
        if (reverse) {
            for (auto* deck : {engine(), other}) {
                deck->setKeylockPreparationPausedForTest(true);
            }
            const auto submissions1 = engine()->keylockPreparationSubmissionsForTest();
            const auto submissions2 = other->keylockPreparationSubmissionsForTest();
            for (const auto& group : {m_sGroup1, m_sGroup2}) {
                ControlObject::set(ConfigKey(group, "reverse"), 1);
            }
            for (int i = 0; i < 1000 &&
                    (engine()->keylockPreparationSubmissionsForTest() == submissions1 ||
                            other->keylockPreparationSubmissionsForTest() == submissions2);
                    ++i) {
                process();
                QTest::qSleep(1);
            }
            ASSERT_GT(engine()->keylockPreparationSubmissionsForTest(), submissions1);
            ASSERT_GT(other->keylockPreparationSubmissionsForTest(), submissions2);
            for (auto* deck : {engine(), other}) {
                deck->setKeylockPreparationPausedForTest(false);
            }
            for (int i = 0; i < 1000 &&
                    (!engine()->keylockPreparationReadyForTest() ||
                            !other->keylockPreparationReadyForTest());
                    ++i) {
                QTest::qWait(1);
            }
            ASSERT_TRUE(engine()->keylockPreparationReadyForTest());
            ASSERT_TRUE(other->keylockPreparationReadyForTest());
        }
        int settled = 0;
        const double direction = reverse ? -1 : 1;
        for (int i = 0; i < 1000 && settled < 8; ++i) {
            const auto before = engine()->getExactPlayPos();
            process();
            settled = std::fabs(engine()->getExactPlayPos() - other->getExactPlayPos()) < 1 &&
                            direction * (engine()->getExactPlayPos() - before) > 0
                    ? settled + 1
                    : 0;
            QTest::qSleep(1);
        }
        ASSERT_EQ(8, settled);
        const auto visual = VisualPlayPosition::getVisualPlayPosition(m_sGroup1);
        for (const double beats : {1.0, -1.0, 2.0}) {
            SCOPED_TRACE(beats);
            const auto before = engine()->getExactPlayPos();
            const auto reference = other->getExactPlayPos();
            const double jumpFrames =
                    engine()->getLoadedTrack()->getSampleRate().toDouble() * 0.5 * beats;
            engine()->setKeylockPreparationPausedForTest(true);
            ControlObject::set(ConfigKey(m_sGroup1, "beatjump"), 0);
            ControlObject::set(ConfigKey(m_sGroup1, "beatjump"), beats);
            ASSERT_TRUE(engine()->queuedSeekPosition().isValid());
            EXPECT_NEAR(0, engine()->queuedSeekPosition() - (before + jumpFrames), 1e-6);
            for (int i = 0; i < 12; ++i) {
                process();
                EXPECT_TRUE(engine()->isRecoveringLiveTimeline());
                QTest::qSleep(1);
            }
            ControlObject::set(ConfigKey(m_sGroup1, "beatjump"), 2 * beats);
            ASSERT_TRUE(engine()->queuedSeekPosition().isValid());
            EXPECT_NEAR(0,
                    engine()->queuedSeekPosition() - (before + 3 * jumpFrames),
                    1e-6);
            for (int i = 0; i < 4; ++i) {
                process();
                EXPECT_TRUE(engine()->isRecoveringLiveTimeline());
                QTest::qSleep(1);
            }
            engine()->setKeylockPreparationPausedForTest(false);
            bool resumed = false;
            for (int i = 0; i < 1000 && !resumed; ++i) {
                process();
                resumed = visual->getEnginePlayRateForTest() * direction > 0;
                QTest::qSleep(1);
            }
            ASSERT_TRUE(resumed);
            EXPECT_FALSE(engine()->isRecoveringLiveTimeline());
            const auto expected =
                    before + 3 * jumpFrames + (other->getExactPlayPos() - reference);
            EXPECT_NEAR(0, engine()->getExactPlayPos() - expected, 1.0);
            for (int i = 0; i < 8; ++i) {
                process();
                const auto currentExpected =
                        before + 3 * jumpFrames + (other->getExactPlayPos() - reference);
                EXPECT_NEAR(0, engine()->getExactPlayPos() - currentExpected, 1.0);
                QTest::qSleep(1);
            }
        }
    }
    void verifySlipRecoveryOnMovingTimeline(bool reverse, bool changeTempo) {
        ASSERT_TRUE(startSteadyPlayback());
        const double direction = reverse ? -1.0 : 1.0;
        if (reverse) {
            engine()->seekExact(FramePos(200000));
            ControlObject::set(ConfigKey(m_sGroup1, "reverse"), 1);
            int settled = 0;
            for (int i = 0; i < 1000 && settled < 8; ++i) {
                const auto before = engine()->getExactPlayPos();
                process();
                const double step = direction * kProcessBufferSize / 2.0 *
                        engine()->getRateRatio();
                settled = std::fabs(engine()->getExactPlayPos() - before - step) < 1e-6
                        ? settled + 1
                        : 0;
                QTest::qSleep(1);
            }
            ASSERT_EQ(8, settled);
        }
        const auto start = engine()->getExactPlayPos();
        const double slipStep = direction * kProcessBufferSize / 2.0 *
                engine()->getRateRatio();
        ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 1);
        process();
        engine()->setKeylockPreparationPausedForTest(true);
        engine()->seekExact(FramePos(100000));
        process();
        int slipCallbacks = 2;
        if (changeTempo) {
            ControlObject::set(ConfigKey(m_sGroup1, "rate"), 0.25);
            process();
            ++slipCallbacks;
        }
        const auto restorePosition =
                (start + slipCallbacks * slipStep).toNearestFrameBoundary();
        ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 0);
        process();
        int elapsedCallbacks = 1;
        const double playbackStep = direction * kProcessBufferSize / 2.0 *
                engine()->getRateRatio();
        EXPECT_EQ(restorePosition, engine()->getExactPlayPos());
        EXPECT_TRUE(engine()->isRecoveringLiveTimeline());
        for (int i = 0; i < 12; ++i) {
            process();
            ++elapsedCallbacks;
            EXPECT_EQ(restorePosition, engine()->getExactPlayPos());
            EXPECT_TRUE(engine()->isRecoveringLiveTimeline());
            QTest::qSleep(1);
        }
        engine()->setKeylockPreparationPausedForTest(false);
        const auto visual = VisualPlayPosition::getVisualPlayPosition(m_sGroup1);
        bool resumed = false;
        for (int i = 0; i < 1000 && !resumed; ++i) {
            process();
            ++elapsedCallbacks;
            resumed = visual->getEnginePlayRateForTest() * direction > 0;
            QTest::qSleep(1);
        }
        ASSERT_TRUE(resumed);
        EXPECT_FALSE(engine()->isRecoveringLiveTimeline());
        const double sourceFramesPerOutputFrame =
                std::fabs(playbackStep) / (kProcessBufferSize / 2.0);
        const auto expectTimelinePosition = [&] {
            const auto expected = restorePosition + elapsedCallbacks * playbackStep;
            const double roundingTolerance = 64 * std::numeric_limits<double>::epsilon() *
                    std::max(1.0, std::fabs(expected.value()));
            const double lag = direction * (expected - engine()->getExactPlayPos());
            // AI-generated: Recovery discards whole output frames, leaving less
            // than one output frame of source-time debt. End AI-generated text.
            EXPECT_GE(lag, -roundingTolerance);
            EXPECT_LT(lag, sourceFramesPerOutputFrame + roundingTolerance);
        };
        expectTimelinePosition();
        for (int i = 0; i < 8; ++i) {
            const auto before = engine()->getExactPlayPos();
            process();
            ++elapsedCallbacks;
            EXPECT_NEAR(playbackStep, engine()->getExactPlayPos() - before, 1e-6);
            expectTimelinePosition();
            QTest::qSleep(1);
        }
    }
};

TEST_F(SignalsmithMemoryIntegrationTest, SyncedBeatJumpResumesOnMovingTimeline) {
    synchronizePlayingDecks();
    ASSERT_FALSE(HasFatalFailure());
    const auto leaderAtRequest = m_pChannel2->getEngineBuffer()->getExactPlayPos();
    engine()->setKeylockPreparationPausedForTest(true);
    ControlObject::set(ConfigKey(m_sGroup1, "beatjump"), 1);
    const auto jumpPosition = engine()->queuedSeekPosition();
    ASSERT_TRUE(jumpPosition.isValid());
    for (int i = 0; i < 12; ++i) {
        process();
        QTest::qSleep(1);
    }
    engine()->setKeylockPreparationPausedForTest(false);
    const auto visual = VisualPlayPosition::getVisualPlayPosition(m_sGroup1);
    bool resumed = false;
    for (int i = 0; i < 500 && !resumed; ++i) {
        process();
        resumed = visual->getEnginePlayRateForTest() > 0;
        QTest::qSleep(1);
    }
    ASSERT_TRUE(resumed);
    const auto expected = jumpPosition +
            (m_pChannel2->getEngineBuffer()->getExactPlayPos() - leaderAtRequest);
    EXPECT_NEAR(0, engine()->getExactPlayPos() - expected, 1.0);
    for (int i = 0; i < 8; ++i) {
        process();
        const auto currentExpected = jumpPosition +
                (m_pChannel2->getEngineBuffer()->getExactPlayPos() - leaderAtRequest);
        EXPECT_NEAR(0, engine()->getExactPlayPos() - currentExpected, 1.0);
        QTest::qSleep(1);
    }
}

TEST_F(SignalsmithMemoryIntegrationTest, LeaderBeatJumpPreservesMovingTimeline) {
    verifyBeatJumpTimeline(SyncMode::LeaderExplicit, true, false);
}

TEST_F(SignalsmithMemoryIntegrationTest, RepeatedSyncedBeatJumpPreservesMovingTimeline) {
    verifyBeatJumpTimeline(SyncMode::Follower, true, false);
}

TEST_F(SignalsmithMemoryIntegrationTest, UnsyncedBeatJumpPreservesMovingTimeline) {
    verifyBeatJumpTimeline(SyncMode::None, true, false);
}

TEST_F(SignalsmithMemoryIntegrationTest, UnquantizedFollowerBeatJumpPreservesMovingTimeline) {
    verifyBeatJumpTimeline(SyncMode::Follower, false, false);
}

TEST_F(SignalsmithMemoryIntegrationTest, ReverseFollowerBeatJumpPreservesMovingTimeline) {
    verifyBeatJumpTimeline(SyncMode::Follower, true, true);
}

TEST_F(SignalsmithMemoryIntegrationTest, SyncedScratchReleaseDoesNotPrepareObsoleteAnchor) {
    synchronizePlayingDecks();
    ASSERT_FALSE(HasFatalFailure());
    engine()->setKeylockPreparationPausedForTest(true);
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2_enable"), 1);
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2"), -0.5);
    for (int i = 0; i < 4; ++i) {
        process();
    }
    const auto submissions = engine()->keylockPreparationSubmissionsForTest();
    const auto beforeRelease = engine()->getExactPlayPos();
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2_enable"), 0);
    process();
    EXPECT_GT(engine()->getExactPlayPos(), beforeRelease);
    EXPECT_EQ(submissions, engine()->keylockPreparationSubmissionsForTest());
    process();
    EXPECT_EQ(submissions + 1, engine()->keylockPreparationSubmissionsForTest());
    for (int i = 0; i < 12; ++i) {
        process();
        QTest::qSleep(1);
    }
    engine()->setKeylockPreparationPausedForTest(false);
    const auto visual = VisualPlayPosition::getVisualPlayPosition(m_sGroup1);
    bool resumed = false;
    for (int i = 0; i < 500 && !resumed; ++i) {
        process();
        resumed = visual->getEnginePlayRateForTest() > 0;
        QTest::qSleep(1);
    }
    ASSERT_TRUE(resumed);
    EXPECT_NEAR(0,
            std::remainder(engine()->getExactPlayPos() -
                            m_pChannel2->getEngineBuffer()->getExactPlayPos(),
                    22050.0),
            1.0);
}

TEST_F(SignalsmithMemoryIntegrationTest, OrdinaryPhaseRequestCannotDowngradeLiveRelease) {
    synchronizePlayingDecks();
    ASSERT_FALSE(HasFatalFailure());
    engine()->setKeylockPreparationPausedForTest(true);
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2_enable"), 1);
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2"), -0.5);
    for (int i = 0; i < 4; ++i) {
        process();
    }
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2_enable"), 0);
    process();
    engine()->requestSyncPhase();
    process();
    EXPECT_TRUE(engine()->isRecoveringLiveTimeline());
    engine()->setKeylockPreparationPausedForTest(false);
    const auto visual = VisualPlayPosition::getVisualPlayPosition(m_sGroup1);
    bool resumed = false;
    for (int i = 0; i < 500 && !resumed; ++i) {
        process();
        resumed = visual->getEnginePlayRateForTest() > 0;
        QTest::qSleep(1);
    }
    ASSERT_TRUE(resumed);
    EXPECT_NEAR(0,
            std::remainder(engine()->getExactPlayPos() -
                            m_pChannel2->getEngineBuffer()->getExactPlayPos(),
                    22050.0),
            1.0);
}

TEST_F(SignalsmithMemoryIntegrationTest, ExactCueSupersedesQueuedScratchReleasePhase) {
    synchronizePlayingDecks();
    ASSERT_FALSE(HasFatalFailure());
    engine()->setKeylockPreparationPausedForTest(true);
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2_enable"), 1);
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2"), -0.5);
    for (int i = 0; i < 4; ++i) {
        process();
    }
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2_enable"), 0);
    process();
    engine()->requestSyncPhase();
    engine()->seekExact(FramePos(5001));
    for (int i = 0; i < 12; ++i) {
        process();
        EXPECT_EQ(FramePos(5001), engine()->getExactPlayPos());
        EXPECT_FALSE(engine()->isRecoveringLiveTimeline());
    }
    engine()->setKeylockPreparationPausedForTest(false);
}

TEST_F(SignalsmithMemoryIntegrationTest, ScratchReleaseUsesCallbackPhaseNotDeckProcessOrder) {
    synchronizePlayingDecks();
    ASSERT_FALSE(HasFatalFailure());
    auto* follower = m_pChannel2->getEngineBuffer();
    engine()->requestSyncMode(SyncMode::LeaderExplicit);
    follower->requestSyncMode(SyncMode::Follower);
    process();
    ASSERT_EQ(1, ControlObject::get(ConfigKey(m_sGroup2, "sync_mode")));
    follower->setKeylockPreparationPausedForTest(true);
    ControlObject::set(ConfigKey(m_sGroup2, "scratch2_enable"), 1);
    ControlObject::set(ConfigKey(m_sGroup2, "scratch2"), -0.5);
    for (int i = 0; i < 4; ++i) {
        process();
    }
    ControlObject::set(ConfigKey(m_sGroup2, "scratch2_enable"), 0);
    process();
    for (int i = 0; i < 12; ++i) {
        process();
        QTest::qSleep(1);
    }
    follower->setKeylockPreparationPausedForTest(false);
    const auto visual = VisualPlayPosition::getVisualPlayPosition(m_sGroup2);
    bool resumed = false;
    for (int i = 0; i < 500 && !resumed; ++i) {
        process();
        resumed = visual->getEnginePlayRateForTest() > 0;
        QTest::qSleep(1);
    }
    ASSERT_TRUE(resumed);
    EXPECT_NEAR(0,
            std::remainder(
                    follower->getExactPlayPos() - engine()->getExactPlayPos(),
                    22050.0),
            1.0);
}

TEST_F(SignalsmithMemoryIntegrationTest, NaturalPitchBeatJumpDoesNotLatchLiveRecovery) {
    synchronizePlayingDecks();
    ASSERT_FALSE(HasFatalFailure());
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0);
    process();
    const auto before = engine()->getExactPlayPos();
    ControlObject::set(ConfigKey(m_sGroup1, "beatjump"), 1);
    process();
    EXPECT_GT(engine()->getExactPlayPos().value(), before.value() + 22050);
    EXPECT_FALSE(engine()->isRecoveringLiveTimeline());
}

TEST_F(SignalsmithMemoryIntegrationTest, ScratchReentryCancelsQueuedReleasePhase) {
    synchronizePlayingDecks();
    ASSERT_FALSE(HasFatalFailure());
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2_enable"), 1);
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2"), -0.5);
    for (int i = 0; i < 4; ++i) {
        process();
    }
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2_enable"), 0);
    process();
    const auto before = engine()->getExactPlayPos();
    ControlObject::set(ConfigKey(m_sGroup1, "scratch2_enable"), 1);
    process();
    EXPECT_FALSE(engine()->didSeekForTest());
    EXPECT_TRUE(engine()->getScratching());
    EXPECT_LT(std::abs(engine()->getExactPlayPos() - before), kProcessBufferSize);
}

TEST_F(SignalsmithMemoryIntegrationTest, NativeScratchRampHandsOffWithoutObsoletePreparation) {
    synchronizePlayingDecks(FramePos(30000));
    ASSERT_FALSE(HasFatalFailure());
    mixxx::Time::start();
    const RuntimeLoggingCategory logger(QByteArrayLiteral("test.nativeScratch"));
    ControllerScriptInterfaceLegacy native(nullptr, logger);
    constexpr double alpha = 1.0 / 8;
    native.scratchEnable(1, 248, 33 + 1.0 / 3, alpha, alpha / 32);
    for (int i = 0; i < 16; ++i) {
        native.scratchTick(1, -1);
        mixxx::Time::addTestTime(std::chrono::milliseconds(2));
        QTest::qWait(2);
        process();
    }
    ASSERT_TRUE(engine()->getScratching());
    native.scratchDisable(1);
    ASSERT_TRUE(native.isScratching(1));
    bool released = false;
    for (int i = 0; i < 1000 && !released; ++i) {
        mixxx::Time::addTestTime(std::chrono::milliseconds(1));
        QTest::qWait(1);
        released = !native.isScratching(1);
        const auto before = engine()->getExactPlayPos();
        const auto submissions = engine()->keylockPreparationSubmissionsForTest();
        process();
        if (released) {
            EXPECT_GT(engine()->getExactPlayPos(), before);
            EXPECT_FALSE(engine()->getScratching());
            EXPECT_EQ(submissions, engine()->keylockPreparationSubmissionsForTest());
        }
    }
    ASSERT_TRUE(released);
    const auto leaderAtPhase = m_pChannel2->getEngineBuffer()->getExactPlayPos();
    process();
    const auto phaseAnchor = engine()->getExactPlayPos();
    ASSERT_TRUE(engine()->didSeekForTest());
    EXPECT_NEAR(0, std::remainder(phaseAnchor - leaderAtPhase, 22050.0), 1.0)
            << "follower ratio " << engine()->getRateRatio()
            << " leader ratio " << m_pChannel2->getEngineBuffer()->getRateRatio()
            << " offset " << engine()->getUserOffset();
    const auto visual = VisualPlayPosition::getVisualPlayPosition(m_sGroup1);
    bool resumed = false;
    for (int i = 0; i < 500 && !resumed; ++i) {
        process();
        resumed = visual->getEnginePlayRateForTest() > 0;
        QTest::qSleep(1);
    }
    ASSERT_TRUE(resumed);
    EXPECT_NEAR(0,
            engine()->getExactPlayPos() - phaseAnchor -
                    (m_pChannel2->getEngineBuffer()->getExactPlayPos() -
                            leaderAtPhase),
            1.0)
            << "recovery ratio " << engine()->getRateRatio();
    EXPECT_NEAR(0,
            std::remainder(engine()->getExactPlayPos() -
                            m_pChannel2->getEngineBuffer()->getExactPlayPos(),
                    22050.0),
            1.0);
}

TEST_F(SignalsmithMemoryIntegrationTest, PendingPrerollHoldsCueAndOtherDecksContinue) {
    for (const auto& group : {m_sGroup1, m_sGroup2, m_sGroup3}) {
        ControlObject::set(ConfigKey(group, "play"), 1);
    }
    for (int i = 0; i < 20; ++i) {
        process();
        QTest::qSleep(1);
    }
    const auto otherStart = m_pChannel2->getEngineBuffer()->getExactPlayPos();
    engine()->setKeylockPreparationPausedForTest(true);
    engine()->seekExact(FramePos(5000));
    for (int i = 0; i < 8; ++i) {
        process();
        EXPECT_EQ(FramePos(5000), engine()->getExactPlayPos());
        QTest::qSleep(1);
    }
    EXPECT_GT(m_pChannel2->getEngineBuffer()->getExactPlayPos(), otherStart);
    const auto visual = VisualPlayPosition::getVisualPlayPosition(m_sGroup1);
    const auto heldPosition = visual->getEnginePlayPos();
    EXPECT_EQ(0, visual->getEnginePlayRateForTest());
    QTest::qSleep(10);
    EXPECT_EQ(heldPosition, visual->getEnginePlayPos());
    engine()->seekExact(FramePos(9000));
    process();
    EXPECT_EQ(FramePos(9000), engine()->getExactPlayPos());
    engine()->setKeylockPreparationPausedForTest(false);
    for (int i = 0; i < 100 && engine()->getExactPlayPos() == FramePos(9000); ++i) {
        process();
        QTest::qSleep(1);
    }
    EXPECT_GT(engine()->getExactPlayPos(), FramePos(9000));
    EXPECT_LT(engine()->getExactPlayPos(), FramePos(10000));
    EXPECT_GT(visual->getEnginePlayRateForTest(), 0);
}

TEST_F(SignalsmithMemoryIntegrationTest, RepeatQuantizeDoesNotSeekWhilePrerollHoldsTransport) {
    const auto track = engine()->getLoadedTrack();
    const auto beats = mixxx::Beats::fromConstTempo(
            track->getSampleRate(), FramePos(0), mixxx::Bpm(120));
    ASSERT_TRUE(track->trySetBeats(beats));
    for (const auto& group : {m_sGroup1, m_sGroup2, m_sGroup3}) {
        ControlObject::set(ConfigKey(group, "play"), 1);
    }
    for (int i = 0; i < 20; ++i) {
        process();
        QTest::qSleep(1);
    }
    const auto otherStart = m_pChannel2->getEngineBuffer()->getExactPlayPos();
    engine()->setKeylockPreparationPausedForTest(true);
    ControlObject::set(ConfigKey(m_sGroup1, "quantize"), 1);
    for (const int repeat : {0, 1}) {
        ControlObject::set(ConfigKey(m_sGroup1, "repeat"), repeat);
        const FramePos anchor(repeat ? 7001 : 5001);
        engine()->seekExact(anchor);
        process();
        ASSERT_EQ(anchor, engine()->getExactPlayPos());
        for (int i = 0; i < 12; ++i) {
            process();
            EXPECT_EQ(anchor, engine()->getExactPlayPos())
                    << "repeat=" << repeat << " callback=" << i;
            QTest::qSleep(1);
        }
    }
    EXPECT_GT(m_pChannel2->getEngineBuffer()->getExactPlayPos(), otherStart);
    engine()->setKeylockPreparationPausedForTest(false);
}

TEST_F(SignalsmithMemoryIntegrationTest, ScratchEntryDoesNotWaitForPendingPreroll) {
    for (const bool waveform : {false, true}) {
        SCOPED_TRACE(waveform ? "mouse" : "controller");
        ASSERT_TRUE(startSteadyPlayback());
        engine()->setKeylockPreparationPausedForTest(true);
        const FramePos anchor(10000);
        engine()->seekExact(anchor);
        process();
        process();
        ASSERT_EQ(anchor, engine()->getExactPlayPos());
        if (waveform) {
            ControlObject::set(ConfigKey(m_sGroup1, "scratch_position"), 0);
            ControlObject::set(ConfigKey(m_sGroup1, "scratch_position_enable"), 1);
        } else {
            ControlObject::set(ConfigKey(m_sGroup1, "scratch2"), 1);
            ControlObject::set(ConfigKey(m_sGroup1, "scratch2_enable"), 1);
        }
        process();
        EXPECT_TRUE(engine()->getScratching());
        EXPECT_GT(engine()->getExactPlayPos(), anchor);
        ControlObject::set(ConfigKey(m_sGroup1,
                                   waveform ? "scratch_position_enable" : "scratch2_enable"),
                0);
        engine()->setKeylockPreparationPausedForTest(false);
        process();
    }
}

TEST_F(SignalsmithMemoryIntegrationTest, RealCachingReaderAdvancesFractionalTransport) {
    ControlObject::set(ConfigKey(m_sGroup1, "rate"), 0.25);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1);
    // Wait for actual delivered transport, not a scheduler-dependent worker
    // sleep. Sanitizer instrumentation can lengthen off-callback preparation.
    int settled = 0;
    for (int i = 0; i < 1000 && settled < 8; ++i) {
        const auto before = engine()->getExactPlayPos();
        process();
        const double expected = kProcessBufferSize / 2.0 * engine()->getRateRatio();
        settled = std::fabs(engine()->getExactPlayPos() - before - expected) < 1e-6
                ? settled + 1
                : 0;
        QTest::qSleep(1);
    }
    ASSERT_EQ(8, settled);
    const auto start = engine()->getExactPlayPos();
    const auto rate = engine()->getRateRatio();
    for (int i = 0; i < 24; ++i) {
        process();
        QTest::qSleep(1);
        for (const auto sample :
                m_pEngineMixer->getChannelBuffer(m_sGroup1).first(
                        kProcessBufferSize)) {
            ASSERT_TRUE(util_isfinite(static_cast<double>(sample)));
        }
    }
    EXPECT_NEAR(24 * kProcessBufferSize / 2.0 * rate,
            engine()->getExactPlayPos() - start,
            1e-6);
    double energy = 0;
    for (const auto sample : m_pEngineMixer->getChannelBuffer(m_sGroup1).first(kProcessBufferSize))
        energy += sample * sample;
    EXPECT_GT(energy, 0.01);
}

TEST_F(SignalsmithMemoryIntegrationTest, Scratch2SlipUsesNaturalRateIncludingReverseButton) {
    verifyScratchSlipRestoration(false, true);
}

TEST_F(SignalsmithMemoryIntegrationTest, ZeroScratchSlipUsesNaturalRate) {
    verifyScratchSlipRestoration(false, false, 0.0);
}

TEST_F(SignalsmithMemoryIntegrationTest, BackwardScratchSlipUsesForwardNaturalRate) {
    verifyScratchSlipRestoration(false, false, -0.75);
}

TEST_F(SignalsmithMemoryIntegrationTest, WaveformMouseScratchSlipUsesNaturalRate) {
    verifyScratchSlipRestoration(true, false);
}

TEST_F(SignalsmithMemoryIntegrationTest, SlipKeepsCapturedRateAfterTempoChange) {
    ASSERT_TRUE(startSteadyPlayback());
    const auto anchor = engine()->getExactPlayPos();
    const double step = kProcessBufferSize / 2.0 * engine()->getRateRatio();
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 1);
    process();
    engine()->setKeylockPreparationPausedForTest(true);
    engine()->seekExact(FramePos(100000));
    process();
    ControlObject::set(ConfigKey(m_sGroup1, "rate"), 0.25);
    process();
    process();
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 0);
    process();
    EXPECT_NEAR(0.0,
            engine()->getExactPlayPos() -
                    (anchor + 4 * step).toNearestFrameBoundary(),
            1e-9);
    engine()->setKeylockPreparationPausedForTest(false);
}

TEST_F(SignalsmithMemoryIntegrationTest, PausedTransportDoesNotAdvanceSlipPosition) {
    ASSERT_TRUE(startSteadyPlayback());
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 0);
    process();
    const auto anchor = engine()->getExactPlayPos();
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 1);
    for (int i = 0; i < 4; ++i) {
        process();
    }
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 0);
    process();
    EXPECT_EQ(anchor, engine()->getExactPlayPos());
}

TEST_F(SignalsmithMemoryIntegrationTest, FractionalRateSlipToggleDoesNotRestartPreparation) {
    ControlObject::set(ConfigKey(m_sGroup1, "rate"), 0.25);
    ASSERT_TRUE(startSteadyPlayback());

    const auto beforeEnable = engine()->getExactPlayPos();
    const double enableStep = kProcessBufferSize / 2.0 * engine()->getRateRatio();
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 1);
    process();
    EXPECT_NEAR(enableStep, engine()->getExactPlayPos() - beforeEnable, 1e-6);
    engine()->setKeylockPreparationPausedForTest(true);
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 0);
    const auto beforeToggle = engine()->getExactPlayPos();
    const double expected = kProcessBufferSize / 2.0 * engine()->getRateRatio();
    process();
    EXPECT_NEAR(expected,
            engine()->getExactPlayPos() - beforeToggle,
            1e-6);
    engine()->setKeylockPreparationPausedForTest(false);
}

TEST_F(SignalsmithMemoryIntegrationTest, SlipRestoresCounterfactualPositionAfterRealSeek) {
    ASSERT_TRUE(startSteadyPlayback());
    const auto start = engine()->getExactPlayPos();
    const double step = kProcessBufferSize / 2.0 * engine()->getRateRatio();
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 1);
    process();
    engine()->setKeylockPreparationPausedForTest(true);
    engine()->seekExact(FramePos(100000));
    process();
    process();
    EXPECT_EQ(FramePos(100000), engine()->getExactPlayPos());
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 0);
    process();
    EXPECT_EQ((start + 3 * step).toNearestFrameBoundary(), engine()->getExactPlayPos());
    EXPECT_EQ(1, ControlObject::get(ConfigKey(m_sGroup1, "play")));
    engine()->setKeylockPreparationPausedForTest(false);
}

TEST_F(SignalsmithMemoryIntegrationTest, SlipRestoreResumesOnUnsyncedMovingTimeline) {
    verifySlipRecoveryOnMovingTimeline(false, false);
}

TEST_F(SignalsmithMemoryIntegrationTest, ReverseSlipRestoreResumesOnMovingTimeline) {
    verifySlipRecoveryOnMovingTimeline(true, false);
}

TEST_F(SignalsmithMemoryIntegrationTest, SlipRestoreRecoveryUsesCurrentTempoAfterCapturedAnchor) {
    verifySlipRecoveryOnMovingTimeline(false, true);
}

TEST_F(SignalsmithMemoryIntegrationTest, ReverseSlipRecoveryUsesCurrentTempoAfterCapturedAnchor) {
    verifySlipRecoveryOnMovingTimeline(true, true);
}

TEST_F(SignalsmithMemoryIntegrationTest, ExactCueCancelsPendingSlipRecovery) {
    ASSERT_TRUE(startSteadyPlayback());
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 1);
    process();
    engine()->setKeylockPreparationPausedForTest(true);
    engine()->seekExact(FramePos(100000));
    process();
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 0);
    process();
    ASSERT_TRUE(engine()->isRecoveringLiveTimeline());
    engine()->seekExact(FramePos(5001));
    for (int i = 0; i < 12; ++i) {
        process();
        EXPECT_EQ(FramePos(5001), engine()->getExactPlayPos());
        EXPECT_FALSE(engine()->isRecoveringLiveTimeline());
    }
    engine()->setKeylockPreparationPausedForTest(false);
}

TEST_F(SignalsmithMemoryIntegrationTest, ExactCueQueuedBeforeSlipReleaseKeepsPriority) {
    ASSERT_TRUE(startSteadyPlayback());
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 1);
    process();
    engine()->setKeylockPreparationPausedForTest(true);
    engine()->seekExact(FramePos(100000));
    process();
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 0);
    engine()->seekExact(FramePos(5001));
    for (int i = 0; i < 12; ++i) {
        process();
        EXPECT_EQ(FramePos(5001), engine()->getExactPlayPos());
        EXPECT_FALSE(engine()->isRecoveringLiveTimeline());
    }
    engine()->setKeylockPreparationPausedForTest(false);
}

TEST_F(SignalsmithMemoryIntegrationTest, PausedSlipRestoreKeepsExactAnchorWithoutLiveDebt) {
    ASSERT_TRUE(startSteadyPlayback());
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 0);
    process();
    const auto anchor = engine()->getExactPlayPos();
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 1);
    process();
    engine()->setKeylockPreparationPausedForTest(true);
    engine()->seekExact(FramePos(100000));
    process();
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 0);
    process();
    EXPECT_EQ(anchor.toNearestFrameBoundary(), engine()->getExactPlayPos());
    EXPECT_FALSE(engine()->isRecoveringLiveTimeline());
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1);
    for (int i = 0; i < 12; ++i) {
        process();
        EXPECT_EQ(anchor.toNearestFrameBoundary(), engine()->getExactPlayPos());
        EXPECT_FALSE(engine()->isRecoveringLiveTimeline());
    }
    engine()->setKeylockPreparationPausedForTest(false);
}

TEST_F(SignalsmithMemoryIntegrationTest, SlipQuitAndAdoptPreservesRelocatedPosition) {
    ASSERT_TRUE(startSteadyPlayback());
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 1);
    process();
    engine()->setKeylockPreparationPausedForTest(true);
    engine()->seekExact(FramePos(100000));
    process();
    process();
    engine()->slipQuitAndAdopt();
    ControlObject::set(ConfigKey(m_sGroup1, "slip_enabled"), 0);
    process();
    EXPECT_EQ(FramePos(100000), engine()->getExactPlayPos());
    EXPECT_EQ(1, ControlObject::get(ConfigKey(m_sGroup1, "play")));
    engine()->setKeylockPreparationPausedForTest(false);
}

TEST_F(SignalsmithMemoryIntegrationTest, KeylockOffKeepsNaturalRateLinearAndKeyAdjustIndependent) {
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0);
    ControlObject::set(ConfigKey(m_sGroup1, "rate"), 0.25);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1);

    // With keylock disabled and no independent key adjustment, transport and
    // pitch follow the same linear rate (the pitch indicator remains natural).
    int settled = 0;
    for (int i = 0; i < 1000 && settled < 8; ++i) {
        const auto before = engine()->getExactPlayPos();
        process();
        const double expected = kProcessBufferSize / 2.0 * engine()->getRateRatio();
        settled = std::fabs(engine()->getExactPlayPos() - before - expected) < 1e-6
                ? settled + 1
                : 0;
        QTest::qSleep(1);
    }
    ASSERT_EQ(8, settled);
    const auto naturalPitch = ControlObject::get(ConfigKey(m_sGroup1, "pitch"));
    EXPECT_TRUE(util_isfinite(naturalPitch));

    // A key adjustment is an independent pitch scaler even with keylock off.
    ControlObject::set(ConfigKey(m_sGroup1, "pitch_adjust"), 1.0);
    EXPECT_NEAR(naturalPitch + 1.0,
            ControlObject::get(ConfigKey(m_sGroup1, "pitch")),
            0.05);
    const auto start = engine()->getExactPlayPos();
    // Changing from linear to independent pitch prepares a format off-thread.
    // Wait for observable delivery, not a fixed number of silent callbacks.
    for (int i = 0; i < 1000 && engine()->getExactPlayPos() == start; ++i) {
        process();
        QTest::qSleep(1);
    }
    EXPECT_GT(engine()->getExactPlayPos(), start);
    EXPECT_TRUE(util_isfinite(ControlObject::get(ConfigKey(m_sGroup1, "pitch"))));
}

TEST_F(SignalsmithMemoryIntegrationTest, ReverseLoopAdvancesWithinLoopBounds) {
    engine()->seekExact(FramePos(3900));
    ControlObject::set(ConfigKey(m_sGroup1, "loop_start_position"),
            FramePos(2000).toEngineSamplePos());
    ControlObject::set(ConfigKey(m_sGroup1, "loop_end_position"),
            FramePos(4000).toEngineSamplePos());
    ControlObject::set(ConfigKey(m_sGroup1, "loop_enabled"), 1);
    ControlObject::set(ConfigKey(m_sGroup1, "reverse"), 1);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 1);

    FramePos previous = engine()->getExactPlayPos();
    bool sawReverseAdvance = false;
    bool sawWrap = false;
    for (int i = 0; i < 200; ++i) {
        process();
        const FramePos current = engine()->getExactPlayPos();
        EXPECT_GE(current, FramePos(2000));
        EXPECT_LE(current, FramePos(4000));
        if (current < previous) {
            sawReverseAdvance = true;
        } else if (current > previous && sawReverseAdvance) {
            sawWrap = true;
        }
        previous = current;
        QTest::qSleep(1);
    }
    EXPECT_TRUE(sawReverseAdvance);
    EXPECT_TRUE(sawWrap);
}

TEST_F(SignalsmithMemoryIntegrationTest, MemoryCueJumpAdoptsExactPositionWhileOtherDecksRun) {
    const auto track = engine()->getLoadedTrack();
    MemoryCues::removeAll(*track);
    MemoryCues::create(*track, FramePos(5000));
    for (const auto& group : {m_sGroup1, m_sGroup2, m_sGroup3}) {
        ControlObject::set(ConfigKey(group, "play"), 1);
    }

    press("memory_cue_next");
    bool published = false;
    for (int i = 0; i < 1000 && !published; ++i) {
        process();
        published = engine()->queuedSeekPosition() == FramePos(5000);
        if (!published) {
            // Navigation snapshots are prepared by the GUI timer.
            QTest::qWait(2);
        }
    }
    ASSERT_TRUE(published);
    const auto otherStart = m_pChannel2->getEngineBuffer()->getExactPlayPos();
    process();
    EXPECT_EQ(FramePos(5000), engine()->getExactPlayPos());
    for (int i = 0; i < 20; ++i) {
        process();
        QTest::qSleep(1);
    }
    EXPECT_GT(m_pChannel2->getEngineBuffer()->getExactPlayPos(), otherStart);
    EXPECT_GT(engine()->getExactPlayPos(), FramePos(5000));
}

TEST_F(SignalsmithMemoryIntegrationTest, EchoReverbAutomationAndHeadphoneRoutingDuringSeeks) {
    // Effect parameter soft takeover uses the application clock, which normal
    // CoreServices startup initializes but an isolated fixture does not.
    mixxx::Time::start();
    m_pEffectsManager->setup();
    const auto chain = m_pEffectsManager->getStandardEffectChain(0);
    ASSERT_TRUE(chain);
    const auto echo = chain->getEffectSlot(0);
    const auto reverb = chain->getEffectSlot(1);
    ASSERT_TRUE(echo);
    ASSERT_TRUE(reverb);
    const auto backend = m_pEffectsManager->getBackendManager();
    const auto echoManifest = backend->getManifest(EchoEffect::getId(), EffectBackendType::BuiltIn);
    const auto reverbId = QStringLiteral("org.mixxx.effects.reverb");
    const auto reverbManifest = backend->getManifest(reverbId, EffectBackendType::BuiltIn);
    ASSERT_TRUE(echoManifest);
    ASSERT_TRUE(reverbManifest);
    echo->loadEffectWithDefaults(echoManifest);
    reverb->loadEffectWithDefaults(reverbManifest);
    ASSERT_TRUE(echo->isLoaded());
    ASSERT_TRUE(reverb->isLoaded());
    const auto snapshot = EffectPresetPointer::create(echo);
    echo->loadEffectFromPreset(snapshot);
    EXPECT_EQ(EchoEffect::getId(), echo->id());
    EXPECT_EQ(reverbId, reverb->id());
    echo->setEnabled(true);
    reverb->setEnabled(true);
    for (const auto& key : {ConfigKey(chain->getGroup(), "enabled"),
                 ConfigKey(chain->getGroup(), "mix"),
                 ConfigKey(chain->getGroup(), QString("group_%1_enable").arg(m_sGroup1)),
                 ConfigKey(chain->getGroup(), "group_[Headphone]_enable"),
                 ConfigKey(m_sGroup1, "pfl")}) {
        ASSERT_TRUE(ControlObject::getControl(key));
        ControlObject::set(key, 1);
    }
    for (const auto& group : {m_sGroup1, m_sGroup2, m_sGroup3}) {
        ControlObject::set(ConfigKey(group, "play"), 1);
    }
    double mainEnergy = 0;
    double headphoneEnergy = 0;
    const auto otherStart = m_pChannel2->getEngineBuffer()->getExactPlayPos();
    for (int i = 0; i < 160; ++i) {
        if (i % 20 == 0) {
            engine()->seekExact(FramePos(1000 + i * 100));
            echo->setMetaParameter((i % 40 == 0) ? 0.2 : 0.7, true);
            reverb->setMetaParameter((i % 40 == 0) ? 0.3 : 0.6, true);
        }
        process();
        for (const auto sample : m_pEngineMixer->getMainBuffer().first(kProcessBufferSize)) {
            ASSERT_TRUE(util_isfinite(static_cast<double>(sample)));
            mainEnergy += sample * sample;
        }
        for (const auto sample : m_pEngineMixer->getHeadphoneBuffer().first(kProcessBufferSize)) {
            ASSERT_TRUE(util_isfinite(static_cast<double>(sample)));
            headphoneEnergy += sample * sample;
        }
        QTest::qSleep(1);
    }
    EXPECT_GT(mainEnergy, 0.01);
    EXPECT_GT(headphoneEnergy, 0.01);
    EXPECT_GT(m_pChannel2->getEngineBuffer()->getExactPlayPos(), otherStart);
    EXPECT_EQ(EchoEffect::getId(), echo->id());
    EXPECT_EQ(reverbId, reverb->id());
}

TEST_F(SignalsmithMemoryIntegrationTest, CreationUsesPositionBeforeQueuedSeek) {
    const auto track = engine()->getLoadedTrack();
    MemoryCues::removeAll(*track);
    press("memory_cue_set");
    engine()->seekExact(FramePos(3000));
    process();
    QTest::qWait(8);
    const auto cues = track->getCuePoints();
    ASSERT_EQ(1, std::count_if(cues.cbegin(), cues.cend(), [](const auto& cue) {
        return cue->getType() == mixxx::CueType::Memory;
    }));
    ASSERT_TRUE(MemoryCues::current(*track, FramePos(1000)));
}

TEST_F(SignalsmithMemoryIntegrationTest, PublicNextAndPreviousControlsKeepTheirDirections) {
    const auto track = engine()->getLoadedTrack();
    MemoryCues::removeAll(*track);
    MemoryCues::create(*track, FramePos(500));
    MemoryCues::create(*track, FramePos(5000));
    for (const auto& [key, target] :
            {std::pair{"memory_cue_next", FramePos(5000)},
                    std::pair{"memory_cue_prev", FramePos(500)}}) {
        press(key);
        QTest::qWait(8);
        process();
        EXPECT_EQ(target, engine()->queuedSeekPosition());
        process();
        EXPECT_EQ(target, engine()->getExactPlayPos());
    }
}

TEST_F(SignalsmithMemoryIntegrationTest, NavigationWorksAndExplicitSeekRejectsPublishedTarget) {
    const auto track = engine()->getLoadedTrack();
    MemoryCues::removeAll(*track);
    MemoryCues::create(*track, FramePos(5000));
    press("memory_cue_next");
    QTest::qWait(8);
    process();
    EXPECT_EQ(FramePos(5000), engine()->queuedSeekPosition());
    process();
    EXPECT_EQ(FramePos(5000), engine()->getExactPlayPos());
    engine()->seekExact(FramePos(1000));
    process();
    press("memory_cue_next");
    QTest::qWait(8);
    engine()->seekExact(FramePos(2000));
    process();
    EXPECT_FALSE(engine()->queuedSeekPosition().isValid());
    process();
    EXPECT_EQ(FramePos(2000), engine()->getExactPlayPos());
}

TEST_F(SignalsmithMemoryIntegrationTest, CueDeletionRejectsPublishedTarget) {
    const auto track = engine()->getLoadedTrack();
    MemoryCues::removeAll(*track);
    const auto cue = MemoryCues::create(*track, FramePos(5000));
    press("memory_cue_next");
    QTest::qWait(8);
    track->removeCue(cue);
    process();
    EXPECT_FALSE(engine()->queuedSeekPosition().isValid());
    process();
    EXPECT_EQ(FramePos(1000), engine()->getExactPlayPos());
}

TEST_F(SignalsmithMemoryIntegrationTest, CueMoveRejectsPublishedTarget) {
    const auto track = engine()->getLoadedTrack();
    MemoryCues::removeAll(*track);
    const auto cue = MemoryCues::create(*track, FramePos(5000));
    press("memory_cue_next");
    QTest::qWait(8);
    cue->setStartPosition(FramePos(8000));
    process();
    EXPECT_FALSE(engine()->queuedSeekPosition().isValid());
}

TEST_F(SignalsmithMemoryIntegrationTest, WorkerCueMoveRejectsTargetBeforeGuiForwarding) {
    const auto track = engine()->getLoadedTrack();
    MemoryCues::removeAll(*track);
    const auto cue = MemoryCues::create(*track, FramePos(5000));
    press("memory_cue_next");
    QTest::qWait(8);
    std::thread worker([&] { cue->setStartPosition(FramePos(8000)); });
    worker.join();
    // Do not dispatch the queued Track::slotCueUpdated forwarding signal.
    process();
    EXPECT_FALSE(engine()->queuedSeekPosition().isValid());
}

TEST_F(SignalsmithMemoryIntegrationTest, WorkerOtherCueMoveRejectsChangedNavigationOrder) {
    const auto track = engine()->getLoadedTrack();
    MemoryCues::removeAll(*track);
    MemoryCues::create(*track, FramePos(5000));
    const auto other = MemoryCues::create(*track, FramePos(9000));
    press("memory_cue_next");
    QTest::qWait(8);
    std::thread worker([&] { other->setStartPosition(FramePos(3000)); });
    worker.join();
    process();
    EXPECT_FALSE(engine()->queuedSeekPosition().isValid());
}

TEST_F(SignalsmithMemoryIntegrationTest, TrackReplacementRejectsPublishedTarget) {
    const auto track = engine()->getLoadedTrack();
    MemoryCues::removeAll(*track);
    MemoryCues::create(*track, FramePos(5000));
    press("memory_cue_next");
    QTest::qWait(8);
    auto replacement = Track::newTemporary(getTestDir().filePath("sine-30.wav"));
    loadTrack(m_pMixerDeck1.get(), replacement);
    process();
    EXPECT_FALSE(engine()->queuedSeekPosition().isValid());
    EXPECT_NE(FramePos(5000), engine()->getExactPlayPos());
}
} // namespace
