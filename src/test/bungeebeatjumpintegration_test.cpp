#include "util/defs.h"

#ifdef __BUNGEE__

#include <gtest/gtest.h>

#include <cmath>

#include "audio/frame.h"
#include "engine/enginebuffer.h"
#include "engine/sync/syncable.h"
#include "test/signalpathtest.h"
#include "track/beats.h"

namespace {
class BungeeBeatJumpIntegrationTest : public SignalPathTest {
  protected:
    void SetUp() override {
        SignalPathTest::SetUp();
        ControlObject::set(ConfigKey("[App]", "keylock_engine"),
                static_cast<double>(EngineBuffer::KeylockEngine::Bungee));
        for (const auto& group : {m_sGroup1, m_sGroup2}) {
            ControlObject::set(ConfigKey(group, "keylock"), 1);
            ControlObject::set(ConfigKey(group, "quantize"), 0);
        }
        QTest::qWait(20);
        for (auto* deck : {jumpDeck(), referenceDeck()}) {
            const auto track = deck->getLoadedTrack();
            ASSERT_TRUE(track->trySetBeats(mixxx::Beats::fromConstTempo(
                    track->getSampleRate(), mixxx::audio::FramePos(0), mixxx::Bpm(120))));
        }
        EXPECT_EQ(static_cast<double>(EngineBuffer::KeylockEngine::Bungee),
                ControlObject::get(ConfigKey("[App]", "keylock_engine")));
        warmOutputAndCache();
    }

    EngineBuffer* jumpDeck() {
        return m_pChannel1->getEngineBuffer();
    }

    EngineBuffer* referenceDeck() {
        return m_pChannel2->getEngineBuffer();
    }

    void process() {
        m_pEngineMixer->process(kProcessBufferSize);
    }

    void warmOutputAndCache() {
        ControlObject::set(ConfigKey(m_sGroup1, "play"), 1);
        ControlObject::set(ConfigKey(m_sGroup2, "play"), 1);
        int settled = 0;
        for (int i = 0; i < 1000 && settled < 8; ++i) {
            const auto beforeJump = jumpDeck()->getExactPlayPos();
            const auto beforeReference = referenceDeck()->getExactPlayPos();
            process();
            const double step = kProcessBufferSize / 2.0 * jumpDeck()->getRateRatio();
            const bool decksAdvanced = jumpDeck()->getExactPlayPos() > beforeJump &&
                    referenceDeck()->getExactPlayPos() > beforeReference;
            settled = decksAdvanced &&
                            std::fabs(jumpDeck()->getExactPlayPos() - beforeJump - step) < 1e-6 &&
                            std::fabs(referenceDeck()->getExactPlayPos() - beforeReference - step) <
                                    1e-6
                    ? settled + 1
                    : 0;
            QTest::qSleep(1);
        }
        ASSERT_EQ(8, settled);
        ControlObject::set(ConfigKey(m_sGroup1, "play"), 0);
        ControlObject::set(ConfigKey(m_sGroup2, "play"), 0);
        process();
    }

    void configureMode(SyncMode mode) {
        if (mode == SyncMode::None) {
            jumpDeck()->requestSyncMode(SyncMode::None);
            referenceDeck()->requestSyncMode(SyncMode::None);
        } else if (mode == SyncMode::LeaderExplicit) {
            jumpDeck()->requestSyncMode(SyncMode::LeaderExplicit);
            referenceDeck()->requestSyncMode(SyncMode::None);
        } else {
            jumpDeck()->requestSyncMode(SyncMode::Follower);
            referenceDeck()->requestSyncMode(SyncMode::LeaderExplicit);
        }
    }

    void seekAndStart(SyncMode mode, bool quantize, bool reverse) {
        ControlObject::set(ConfigKey(m_sGroup1, "play"), 0);
        ControlObject::set(ConfigKey(m_sGroup2, "play"), 0);
        ControlObject::set(ConfigKey(m_sGroup1, "reverse"), reverse ? 1 : 0);
        ControlObject::set(ConfigKey(m_sGroup2, "reverse"), reverse ? 1 : 0);
        for (auto* deck : {jumpDeck(), referenceDeck()}) {
            deck->seekExact(mixxx::audio::FramePos(200000));
        }
        process();
        EXPECT_EQ(200000, jumpDeck()->getExactPlayPos().value());
        EXPECT_EQ(200000, referenceDeck()->getExactPlayPos().value());
        QTest::qWait(20);
        configureMode(mode);
        for (const auto& group : {m_sGroup1, m_sGroup2}) {
            ControlObject::set(ConfigKey(group, "quantize"), quantize ? 1 : 0);
        }
        process();
        ControlObject::set(ConfigKey(m_sGroup1, "play"), 1);
        ControlObject::set(ConfigKey(m_sGroup2, "play"), 1);
        const double direction = reverse ? -1 : 1;
        int settled = 0;
        for (int i = 0; i < 1000 && settled < 8; ++i) {
            const auto beforeJump = jumpDeck()->getExactPlayPos();
            process();
            const double step = direction * kProcessBufferSize / 2.0 *
                    jumpDeck()->getRateRatio();
            settled = std::fabs(jumpDeck()->getExactPlayPos() - referenceDeck()->getExactPlayPos()) <
                                    1 &&
                            direction * (jumpDeck()->getExactPlayPos() - beforeJump) > 0 &&
                            std::fabs(jumpDeck()->getExactPlayPos() - beforeJump - step) < 1e-6
                    ? settled + 1
                    : 0;
            QTest::qSleep(1);
        }
        ASSERT_EQ(8, settled);
        EXPECT_NEAR(jumpDeck()->getExactPlayPos().value(),
                referenceDeck()->getExactPlayPos().value(),
                1.0);
    }

    void verifyBeatJumpTimeline(SyncMode mode, bool quantize, bool reverse) {
        SCOPED_TRACE(static_cast<int>(mode));
        SCOPED_TRACE(quantize);
        SCOPED_TRACE(reverse);
        seekAndStart(mode, quantize, reverse);
        const double framesPerBeat = jumpDeck()->getLoadedTrack()->getSampleRate().toDouble() * 0.5;
        for (const double beats : {1.0, -1.0, 2.0}) {
            SCOPED_TRACE(beats);
            const auto beforeJump = jumpDeck()->getExactPlayPos();
            const auto beforeReference = referenceDeck()->getExactPlayPos();
            const double jumpFrames = beats * framesPerBeat;
            ControlObject::set(ConfigKey(m_sGroup1, "beatjump"), 0);
            ControlObject::set(ConfigKey(m_sGroup1, "beatjump"), beats);
            for (int callback = 0; callback < 12; ++callback) {
                process();
                const auto elapsedReference = referenceDeck()->getExactPlayPos() - beforeReference;
                const auto expected = beforeJump + jumpFrames + elapsedReference;
                EXPECT_NEAR(expected.value(), jumpDeck()->getExactPlayPos().value(), 1.0);
                QTest::qSleep(1);
            }
        }
    }
};

TEST_F(BungeeBeatJumpIntegrationTest, BeatJumpRetainsElapsedDeckTimeline) {
    for (const auto mode : {SyncMode::LeaderExplicit, SyncMode::Follower, SyncMode::None}) {
        for (const bool quantize : {false, true}) {
            for (const bool reverse : {false, true}) {
                verifyBeatJumpTimeline(mode, quantize, reverse);
            }
        }
    }
}
} // namespace

#endif
