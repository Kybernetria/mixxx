#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

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
    EXPECT_EQ(4, static_cast<int>(EngineBuffer::KeylockEngine::Reserved));
    EXPECT_EQ(5, static_cast<int>(EngineBuffer::KeylockEngine::Signalsmith));
    EXPECT_EQ(EngineBuffer::KeylockEngine::Signalsmith,
            EngineBuffer::defaultKeylockEngine());
    EXPECT_TRUE(EngineBuffer::isKeylockEngineAvailable(EngineBuffer::KeylockEngine::Signalsmith));
    EXPECT_FALSE(EngineBuffer::isKeylockEngineAvailable(EngineBuffer::KeylockEngine::SoundTouch));
    EXPECT_FALSE(EngineBuffer::isKeylockEngineAvailable(
            EngineBuffer::KeylockEngine::RubberBandFaster));
    for (const auto engine : EngineBuffer::kKeylockEngines) {
        EXPECT_EQ(EngineBuffer::isKeylockEngineAvailable(engine)
                        ? engine
                        : EngineBuffer::KeylockEngine::Signalsmith,
                EngineBuffer::resolveKeylockEngine(
                        static_cast<double>(engine)));
    }
    for (const double id : {0.0,
                 1.0,
                 2.0,
                 3.0,
                 4.0,
                 -1.0,
                 99.0,
                 0.5,
                 std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_EQ(EngineBuffer::KeylockEngine::Signalsmith,
                EngineBuffer::resolveKeylockEngine(id));
    }
}

class SignalsmithMemoryIntegrationTest : public SignalPathTest {
  protected:
    void SetUp() override {
        SignalPathTest::SetUp();
        ControlObject::set(ConfigKey("[App]", "keylock_engine"),
                static_cast<double>(EngineBuffer::KeylockEngine::Signalsmith));
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
};

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
