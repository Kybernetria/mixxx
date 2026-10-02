#include "track/memorycues.h"

#include <gtest/gtest.h>

#include <future>

namespace mixxx {
namespace {

TrackPointer makeTrack() {
    return Track::newTemporary(QStringLiteral("/tmp/mixxx-memory-cues-test.mp3"));
}

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
