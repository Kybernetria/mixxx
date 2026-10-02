#include "engine/controls/memorycuecontrol.h"

#include <gtest/gtest.h>

#include <future>
#include <thread>

#include "control/controlproxy.h"
#include "test/callbackallocationcheck.h"
#include "test/mixxxtest.h"
#include "track/memorycues.h"

namespace {
const QString kMemoryGroup = QStringLiteral("[MemoryCueTest]");
class MemoryCueControlTest : public MixxxTest {
  protected:
    ControlObject quantize{ConfigKey(kMemoryGroup, "quantize")};
    ControlObject closestBeat{ConfigKey(kMemoryGroup, "beat_closest")};
    MemoryCueControl control{kMemoryGroup, config()};
    TrackPointer track = Track::newTemporary(QStringLiteral("/tmp/mixxx-memory-control-test.mp3"));
    void publish(double frame) {
        control.setFrameInfo(mixxx::audio::FramePos(frame),
                mixxx::audio::FramePos(100000),
                mixxx::audio::SampleRate(44100));
    }
    void SetUp() override {
        control.trackLoaded(track);
        publish(1000);
    }
};

TEST_F(MemoryCueControlTest, PublicCreateControlCapturesCommandTimePosition) {
    ControlProxy button(kMemoryGroup, QStringLiteral("memory_cue_set"));
    button.set(1);
    publish(2000);
    control.drainCommands();
    ASSERT_EQ(1, track->getCuePoints().size());
    EXPECT_EQ(1000, track->getCuePoints().first()->getPosition().value());
}

TEST_F(MemoryCueControlTest, PublicDeletionControlsKeepTheirDistinctOperations) {
    struct Case {
        const char* key;
        double position;
        bool removesAll;
    };
    for (const auto& testCase : {Case{"memory_cue_clear", 20000, false},
                 Case{"memory_cue_clear_nearest", 20500, false},
                 Case{"memory_cue_clear_prev", 25000, false},
                 Case{"memory_cue_clear_next", 15000, false},
                 Case{"memory_cue_clear_all", 20000, true}}) {
        SCOPED_TRACE(testCase.key);
        MemoryCues::removeAll(*track);
        const auto first = MemoryCues::create(*track, mixxx::audio::FramePos(10000));
        const auto middle = MemoryCues::create(*track, mixxx::audio::FramePos(20000));
        const auto last = MemoryCues::create(*track, mixxx::audio::FramePos(30000));
        publish(testCase.position);
        ControlProxy button(kMemoryGroup, QString::fromLatin1(testCase.key));
        button.set(0);
        button.set(1);
        control.drainCommands();
        const auto cues = track->getCuePoints();
        EXPECT_FALSE(cues.contains(middle));
        if (testCase.removesAll) {
            EXPECT_TRUE(cues.isEmpty());
        } else {
            ASSERT_EQ(2, cues.size());
            EXPECT_TRUE(cues.contains(first));
            EXPECT_TRUE(cues.contains(last));
        }
    }
}

TEST_F(MemoryCueControlTest, CreateCapturesCommandTimePosition) {
    control.enqueue(MemoryCueControl::Operation::Create);
    publish(2000);
    control.drainCommands();
    ASSERT_EQ(1, track->getCuePoints().size());
    EXPECT_EQ(1000, track->getCuePoints().first()->getPosition().value());
}

TEST_F(MemoryCueControlTest, TrackReplacementRejectsQueuedCommands) {
    control.enqueue(MemoryCueControl::Operation::Create);
    auto replacement = Track::newTemporary(
            QStringLiteral("/tmp/mixxx-memory-replacement-test.mp3"));
    control.invalidateTrack();
    control.trackLoaded(replacement);
    control.drainCommands();
    EXPECT_TRUE(track->getCuePoints().empty());
    EXPECT_TRUE(replacement->getCuePoints().empty());
    publish(2000);
    control.enqueue(MemoryCueControl::Operation::Create);
    control.drainCommands();
    ASSERT_EQ(1, replacement->getCuePoints().size());
    EXPECT_EQ(2000, replacement->getCuePoints().first()->getPosition().value());
}

TEST_F(MemoryCueControlTest, QuantizationIsCapturedNotRecomputedInDispatcher) {
    quantize.set(1);
    closestBeat.set(2200);
    publish(1000);
    control.enqueue(MemoryCueControl::Operation::Create);
    closestBeat.set(8000);
    publish(4000);
    control.drainCommands();
    ASSERT_EQ(1, track->getCuePoints().size());
    EXPECT_EQ(1100, track->getCuePoints().first()->getPosition().value());
}

TEST_F(MemoryCueControlTest, QuantizeToggleImmediatelyBeforePressTakesEffect) {
    closestBeat.set(2200);
    publish(1000);
    quantize.set(1);
    control.enqueue(MemoryCueControl::Operation::Create);
    control.drainCommands();
    ASSERT_EQ(1, track->getCuePoints().size());
    EXPECT_EQ(1100, track->getCuePoints().first()->getPosition().value());
}

TEST_F(MemoryCueControlTest, OverflowRejectsNewestAndIsObservable) {
    for (int i = 0; i < 100; ++i) {
        publish(i * 1000);
        control.enqueue(MemoryCueControl::Operation::Create);
    }
    EXPECT_EQ(36, control.overflowCount());
    control.drainCommands();
    control.drainCommands();
    EXPECT_EQ(64, track->getCuePoints().size());
    EXPECT_EQ(36, ControlObject::get(ConfigKey(kMemoryGroup, "memory_cue_overflows")));
}

TEST_F(MemoryCueControlTest, TwoDecksAndExternalCueEditsUseLiveMembership) {
    ControlObject otherQuantize(ConfigKey("[MemoryCueTest2]", "quantize"));
    ControlObject otherBeat(ConfigKey("[MemoryCueTest2]", "beat_closest"));
    MemoryCueControl other(QStringLiteral("[MemoryCueTest2]"), config());
    other.trackLoaded(track);
    other.setFrameInfo(mixxx::audio::FramePos(1000),
            mixxx::audio::FramePos(100000),
            mixxx::audio::SampleRate(44100));
    control.enqueue(MemoryCueControl::Operation::Create);
    other.enqueue(MemoryCueControl::Operation::Create);
    control.drainCommands();
    other.drainCommands();
    ASSERT_EQ(1, track->getCuePoints().size());
    auto external = track->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(2000),
            mixxx::audio::kInvalidFramePos);
    control.enqueue(MemoryCueControl::Operation::ClearNext);
    control.drainCommands();
    EXPECT_EQ(1, track->getCuePoints().size());
    EXPECT_FALSE(track->getCuePoints().contains(external));
    auto first = track->getCuePoints().first();
    first->setType(mixxx::CueType::Intro);
    other.enqueue(MemoryCueControl::Operation::ClearAll);
    other.drainCommands();
    EXPECT_EQ(1, track->getCuePoints().size());
}

TEST_F(MemoryCueControlTest, SharedCueAdvancesEachTrackRevisionAndDetaches) {
    auto first = Track::newTemporary(QStringLiteral("/tmp/shared-cue-first.mp3"));
    auto second = Track::newTemporary(QStringLiteral("/tmp/shared-cue-second.mp3"));
    auto cue = first->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(2000),
            mixxx::audio::FramePos(3000));
    second->setCuePoints({cue});

    const auto firstBeforeEdit = first->cueRevisionToken()->load();
    const auto secondBeforeEdit = second->cueRevisionToken()->load();
    cue->setLabel(QStringLiteral("shared"));
    EXPECT_GT(first->cueRevisionToken()->load(), firstBeforeEdit);
    EXPECT_GT(second->cueRevisionToken()->load(), secondBeforeEdit);

    first->removeCue(cue);
    const auto firstAfterRemoval = first->cueRevisionToken()->load();
    const auto secondAfterRemoval = second->cueRevisionToken()->load();
    cue->setStartPosition(mixxx::audio::FramePos(2500));
    EXPECT_EQ(firstAfterRemoval, first->cueRevisionToken()->load());
    EXPECT_GT(second->cueRevisionToken()->load(), secondAfterRemoval);
}

TEST_F(MemoryCueControlTest, CueRevisionTracksEveryMutationPath) {
    auto cue = track->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(2000),
            mixxx::audio::FramePos(3000));
    const auto expectRevisionAdvance = [&](auto edit) {
        const auto before = track->cueRevisionToken()->load();
        edit();
        EXPECT_GT(track->cueRevisionToken()->load(), before);
    };
    expectRevisionAdvance([&] { cue->setType(mixxx::CueType::Intro); });
    expectRevisionAdvance([&] { cue->setStartPosition(mixxx::audio::FramePos(2100)); });
    expectRevisionAdvance([&] { cue->setEndPosition(mixxx::audio::FramePos(3100)); });
    expectRevisionAdvance([&] {
        cue->setStartAndEndPosition(mixxx::audio::FramePos(2200),
                mixxx::audio::FramePos(3200));
    });
    expectRevisionAdvance([&] { cue->shiftPositionFrames(10); });
    expectRevisionAdvance([&] { cue->setHotCue(2); });
    expectRevisionAdvance([&] { cue->setLabel(QStringLiteral("revision")); });
    expectRevisionAdvance([&] { cue->setColor(mixxx::RgbColor(0x123456)); });
}

TEST_F(MemoryCueControlTest, CueRevisionInvalidatesNavigationWithoutCallbackAllocation) {
    auto cue = track->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(2000),
            mixxx::audio::kInvalidFramePos);
    control.enqueue(MemoryCueControl::Operation::Next);
    control.drainCommands();
    const auto revision = track->cueRevisionToken()->load(std::memory_order_acquire);
    std::thread editor([&] {
        cue->setStartPosition(mixxx::audio::FramePos(3000));
        cue->setType(mixxx::CueType::Intro);
    });
    editor.join();
    const auto editedRevision = track->cueRevisionToken()->load(std::memory_order_acquire);
    EXPECT_GT(editedRevision, revision);
    track->removeCue(cue);
    EXPECT_GT(track->cueRevisionToken()->load(std::memory_order_acquire), editedRevision);

    mixxxtest::callbackAllocations = 0;
    mixxxtest::callbackDeallocations = 0;
    mixxxtest::countCallbackAllocations = true;
    control.process(0, mixxx::audio::FramePos(1000), 1);
    mixxxtest::countCallbackAllocations = false;
    EXPECT_EQ(0u, mixxxtest::callbackAllocations);
    EXPECT_EQ(0u, mixxxtest::callbackDeallocations);
    control.drainCommands(); // reclaim the retired immutable snapshot
}

TEST_F(MemoryCueControlTest, ControllerSignalsQuiesceBeforeOwnedControlsTeardown) {
    const QString group = QStringLiteral("[MemoryCueTeardown]");
    ControlObject transientQuantize(ConfigKey(group, "quantize"));
    ControlObject transientBeat(ConfigKey(group, "beat_closest"));
    auto transient = std::make_unique<MemoryCueControl>(group, config());
    transient->trackLoaded(track);
    ControlProxy pressControl(ConfigKey(group, "memory_cue_set"));
    std::atomic<int> emissions{0};
    std::thread updater([&] {
        for (int i = 0; i < 10000; ++i) {
            pressControl.set(1);
            pressControl.set(0);
            ++emissions;
        }
    });
    // CoreServices destroys ControllerManager before EngineMixer. ControlObject's
    // direct private-value forwarding must stop before deleting its QObject
    // wrapper; the command gate only protects MemoryCueControl queue storage.
    updater.join();
    transient.reset();
    EXPECT_EQ(10000, emissions.load());
}

TEST_F(MemoryCueControlTest, TeardownDuringExternalCueEditsKeepsRevisionStorageAlive) {
    const QString group = QStringLiteral("[MemoryCueTeardown]");
    ControlObject transientQuantize(ConfigKey(group, "quantize"));
    ControlObject transientBeat(ConfigKey(group, "beat_closest"));
    auto transient = std::make_unique<MemoryCueControl>(group, config());
    transient->trackLoaded(track);
    const auto cue = MemoryCues::create(*track, mixxx::audio::FramePos(2000));
    transient->setFrameInfo(mixxx::audio::FramePos(1000),
            mixxx::audio::FramePos(100000),
            mixxx::audio::SampleRate(44100));
    transient->enqueue(MemoryCueControl::Operation::Next);
    transient->drainCommands();
    std::atomic<int> emissions{0};
    std::thread updater([&] {
        for (int i = 0; i < 10000; ++i) {
            cue->setStartPosition(mixxx::audio::FramePos(2001 + i));
            ++emissions;
        }
    });
    while (emissions.load() < 100)
        std::this_thread::yield();
    transient.reset();
    updater.join();
    EXPECT_EQ(10000, emissions.load());
}

TEST_F(MemoryCueControlTest, ConcurrentProducersDoNotCorruptQueue) {
    auto producer = [&] {
        for (int i = 0; i < 1000; ++i)
            control.enqueue(MemoryCueControl::Operation::Create);
    };
    auto first = std::async(std::launch::async, producer);
    auto second = std::async(std::launch::async, producer);
    first.get();
    second.get();
    EXPECT_EQ(1936, control.overflowCount());
    control.drainCommands();
    control.drainCommands();
    EXPECT_EQ(1, track->getCuePoints().size());
}
} // namespace
