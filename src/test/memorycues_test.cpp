#include "track/memorycues.h"

#include <gtest/gtest.h>

#include <future>
#include <type_traits>
#include <utility>

namespace mixxx {
namespace {

TrackPointer makeTrack() {
    return Track::newTemporary(QStringLiteral("/tmp/mixxx-memory-cues-test.mp3"));
}

static_assert(std::is_same_v<
        decltype(std::declval<const Track&>().cueRevisionToken()),
        std::shared_ptr<const std::atomic<std::uint64_t>>>);

TEST(MemoryCuesTest, QuantizesAndSuppressesDuplicateCreation) {
    auto track = makeTrack();
    const auto first = MemoryCues::create(*track, audio::FramePos(105), 10);
    const auto second = MemoryCues::create(*track, audio::FramePos(106), 10);
    ASSERT_TRUE(first);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first->getPosition(), audio::FramePos(110));
    EXPECT_EQ(track->getCuePoints().size(), 1);
}

TEST(MemoryCuesTest, ConcurrentDeckCreationReturnsSingleLiveCue) {
    auto track = makeTrack();
    auto first = std::async(std::launch::async, [&] {
        return MemoryCues::create(*track, audio::FramePos(100), 1);
    });
    auto second = std::async(std::launch::async, [&] {
        return MemoryCues::create(*track, audio::FramePos(100), 1);
    });
    EXPECT_EQ(first.get(), second.get());
    EXPECT_EQ(track->getCuePoints().size(), 1);
}

TEST(MemoryCuesTest, NavigationReadsLiveCueTypesAndPositions) {
    auto track = makeTrack();
    const auto memory = track->createAndAddCue(CueType::Memory,
            Cue::kNoHotCue,
            audio::FramePos(100),
            audio::kInvalidFramePos);
    const auto ordinary = track->createAndAddCue(CueType::Intro,
            Cue::kNoHotCue,
            audio::FramePos(200),
            audio::kInvalidFramePos);
    ASSERT_TRUE(memory);
    ASSERT_TRUE(ordinary);
    EXPECT_EQ(MemoryCues::next(*track, audio::FramePos(100)), CuePointer());
    ordinary->setType(CueType::Memory);
    EXPECT_EQ(MemoryCues::next(*track, audio::FramePos(100)), ordinary);
    ordinary->setStartPosition(audio::FramePos(90));
    EXPECT_EQ(MemoryCues::previous(*track, audio::FramePos(100)), ordinary);
}

TEST(MemoryCuesTest, RemoveAllLeavesNonMemoryCues) {
    auto track = makeTrack();
    const auto memory = track->createAndAddCue(CueType::Memory,
            Cue::kNoHotCue,
            audio::FramePos(100),
            audio::kInvalidFramePos);
    track->createAndAddCue(CueType::Intro,
            Cue::kNoHotCue,
            audio::FramePos(200),
            audio::kInvalidFramePos);
    ASSERT_TRUE(memory);
    EXPECT_EQ(MemoryCues::removeAll(*track), 1);
    ASSERT_EQ(track->getCuePoints().size(), 1);
    EXPECT_EQ(track->getCuePoints().first()->getType(), CueType::Intro);
}

TEST(MemoryCuesTest, EditsRequireCueToRemainInTrack) {
    auto track = makeTrack();
    const auto cue = track->createAndAddCue(CueType::Memory,
            Cue::kNoHotCue,
            audio::FramePos(100),
            audio::kInvalidFramePos);
    ASSERT_TRUE(cue);
    EXPECT_TRUE(MemoryCues::editLabel(*track, cue, QStringLiteral("A")));
    track->setCuePoints({});
    EXPECT_FALSE(MemoryCues::editLabel(*track, cue, QStringLiteral("B")));
    EXPECT_EQ(cue->getLabel(), QStringLiteral("A"));
}

TEST(MemoryCuesTest, ImportedPointsMergeWithoutQuantizationOrHotcueConversion) {
    auto track = makeTrack();
    const audio::FramePos position(100.25);
    const RgbColor color(qRgb(11, 12, 13));
    const auto basic = MemoryCues::importPoint(*track, position, QString(), std::nullopt);
    const auto extended = MemoryCues::importPoint(
            *track, position, QStringLiteral("Imported"), color);
    EXPECT_EQ(basic, extended);
    EXPECT_EQ(1, track->getCuePoints().size());
    MemoryCues::importPoint(*track, position, QString(), std::nullopt);
    EXPECT_EQ(position, basic->getPosition());
    EXPECT_EQ(QStringLiteral("Imported"), basic->getLabel());
    EXPECT_EQ(color, basic->getColor());
    EXPECT_EQ(CueType::Memory, basic->getType());
    EXPECT_EQ(Cue::kNoHotCue, basic->getHotCue());
    track->setMainCuePosition(position);
    const auto mainCue = track->findCueByType(CueType::MainCue);
    const auto hotcue = track->createAndAddCue(
            CueType::HotCue, 0, position, audio::kInvalidFramePos);
    EXPECT_EQ(1, MemoryCues::removeAll(*track));
    EXPECT_TRUE(track->getCuePoints().contains(mainCue));
    EXPECT_TRUE(track->getCuePoints().contains(hotcue));
}

TEST(MemoryCuesTest, DuplicateMembershipDetachesOnlyAfterFinalRemoval) {
    auto track = makeTrack();
    const auto cue = track->createAndAddCue(CueType::Memory,
            Cue::kNoHotCue,
            audio::FramePos(100),
            audio::kInvalidFramePos);
    ASSERT_TRUE(cue);
    const auto token = track->cueRevisionToken();
    track->setCuePoints({cue, cue});
    const auto membershipRevision = token->load(std::memory_order_acquire);
    EXPECT_EQ(membershipRevision, 2);

    track->removeCue(cue);
    EXPECT_EQ(token->load(std::memory_order_acquire), membershipRevision + 1);
    cue->setLabel(QStringLiteral("still attached"));
    EXPECT_EQ(token->load(std::memory_order_acquire), membershipRevision + 2);

    track->removeCue(cue);
    const auto finalRemovalRevision = token->load(std::memory_order_acquire);
    EXPECT_EQ(finalRemovalRevision, membershipRevision + 3);
    cue->setLabel(QStringLiteral("detached"));
    EXPECT_EQ(token->load(std::memory_order_acquire), finalRemovalRevision);
}

TEST(MemoryCuesTest, BulkReplacementAndDestructionDetachRetainedObservers) {
    auto track = makeTrack();
    const auto oldCue = track->createAndAddCue(CueType::Memory,
            Cue::kNoHotCue,
            audio::FramePos(100),
            audio::kInvalidFramePos);
    const auto oldToken = track->cueRevisionToken();
    track->setCuePoints({oldCue, oldCue});
    const auto beforeReplacement = oldToken->load(std::memory_order_acquire);
    const auto newCue = track->createAndAddCue(CueType::Memory,
            Cue::kNoHotCue,
            audio::FramePos(200),
            audio::kInvalidFramePos);
    track->setCuePoints({newCue});
    const auto afterReplacement = oldToken->load(std::memory_order_acquire);
    EXPECT_EQ(afterReplacement, beforeReplacement + 2);
    oldCue->setLabel(QStringLiteral("replaced"));
    EXPECT_EQ(oldToken->load(std::memory_order_acquire), afterReplacement);

    const auto destructionToken = track->cueRevisionToken();
    const auto beforeDestruction = destructionToken->load(std::memory_order_acquire);
    track.reset();
    EXPECT_EQ(destructionToken->load(std::memory_order_acquire), beforeDestruction);
    newCue->setLabel(QStringLiteral("track destroyed"));
    EXPECT_EQ(destructionToken->load(std::memory_order_acquire), beforeDestruction);
}

TEST(MemoryCuesTest, MainAndTemporaryLoopMembershipAdvanceOnceAndEmitOnce) {
    auto track = makeTrack();
    int updates = 0;
    QObject::connect(track.get(), &Track::cuesUpdated, track.get(), [&updates] { ++updates; });
    const auto token = track->cueRevisionToken();

    track->setMainCuePosition(audio::FramePos(100));
    EXPECT_EQ(token->load(std::memory_order_acquire), 1);
    EXPECT_EQ(updates, 1);
    EXPECT_TRUE(track->isDirty());

    track->markClean();
    track->setMainCuePosition(audio::kInvalidFramePos);
    EXPECT_EQ(token->load(std::memory_order_acquire), 2);
    EXPECT_EQ(updates, 2);
    EXPECT_TRUE(track->isDirty());

    track->markClean();
    const auto loop = track->createAndAddCue(CueType::Loop,
            Cue::kNoHotCue,
            audio::FramePos(200),
            audio::kInvalidFramePos);
    ASSERT_TRUE(loop);
    EXPECT_EQ(token->load(std::memory_order_acquire), 3);
    EXPECT_EQ(updates, 3);
    EXPECT_TRUE(track->isDirty());

    track->markClean();
    track->removeTempLoopCue();
    EXPECT_EQ(token->load(std::memory_order_acquire), 4);
    EXPECT_EQ(updates, 4);
    EXPECT_TRUE(track->isDirty());
}

TEST(MemoryCuesTest, ConditionalEditsDoNotMutateConvertedOrRemovedCues) {
    auto track = makeTrack();
    const auto cue = MemoryCues::create(*track, mixxx::audio::FramePos(100));
    cue->setType(mixxx::CueType::HotCue);
    EXPECT_FALSE(track->removeMemoryCue(cue));
    EXPECT_FALSE(MemoryCues::editLabel(*track, cue, QStringLiteral("Wrong type")));
    EXPECT_TRUE(track->getCuePoints().contains(cue));
    cue->setType(mixxx::CueType::Memory);
    track->removeCue(cue);
    EXPECT_FALSE(track->removeMemoryCue(cue));
    EXPECT_FALSE(MemoryCues::editLabel(*track, cue, QStringLiteral("Removed")));
    EXPECT_TRUE(cue->getLabel().isEmpty());
}
} // namespace
} // namespace mixxx
