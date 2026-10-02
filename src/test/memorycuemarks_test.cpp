#include <gtest/gtest.h>

#include "track/track.h"
#include "waveform/renderers/waveformmark.h"
#include "waveform/renderers/waveformmarkset.h"

namespace {

TEST(MemoryCueMarksTest, FixedPositionMarksRemainValidAndCanMove) {
    auto maybeMark = WaveformMark::create(QString(),
            QString(),
            QString(),
            QStringLiteral("#ffffff"),
            QStringLiteral("AlignBottom"),
            QStringLiteral("Memory"),
            QString(),
            QString(),
            QColor(Qt::red),
            0);
    ASSERT_TRUE(std::holds_alternative<WaveformMarkPointer>(maybeMark));
    const auto mark = std::get<WaveformMarkPointer>(maybeMark);

    EXPECT_FALSE(mark->isValid());
    mark->setSamplePosition(1234.0);
    EXPECT_TRUE(mark->isValid());
    EXPECT_EQ(mark->getSamplePosition(), 1234.0);

    mark->setSamplePosition(5678.0);
    EXPECT_EQ(mark->getSamplePosition(), 5678.0);
}

TEST(MemoryCueMarksTest, IdentityPositionAndMembershipStaySynchronized) {
    const auto track = Track::newTemporary(QStringLiteral("/tmp/mixxx-memory-mark-test.mp3"));
    const auto first = track->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(100),
            mixxx::audio::kInvalidFramePos);
    const auto second = track->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(100),
            mixxx::audio::kInvalidFramePos);
    WaveformMarkSet marks;
    WaveformMarkSet::DefaultMarkerStyle style{};
    style.textColor = QStringLiteral("#ffffff");
    style.color = Qt::red;
    style.markAlign = QStringLiteral("bottom");
    ASSERT_FALSE(marks.setDefault(QString(), style).has_value());
    marks.syncMemoryCueMarks(QString(), track->getCuePoints(), 128, {});
    marks.update();
    ASSERT_EQ(2, std::distance(marks.cbegin(), marks.cend()));
    const auto found = std::find_if(marks.cbegin(),
            marks.cend(),
            [&](const auto& mark) { return mark->getCue() == first; });
    ASSERT_NE(marks.cend(), found);
    const auto firstMark = *found;
    EXPECT_EQ(200, firstMark->getSamplePosition());
    first->setStartPosition(mixxx::audio::FramePos(200));
    first->setLabel(QStringLiteral("Moved"));
    marks.syncMemoryCueMarks(QString(), track->getCuePoints(), 128, {});
    marks.update();
    EXPECT_EQ(400, firstMark->getSamplePosition());
    EXPECT_EQ(QStringLiteral("Moved"), firstMark->m_text);
    first->setType(mixxx::CueType::HotCue);
    track->removeCue(second);
    marks.syncMemoryCueMarks(QString(), track->getCuePoints(), 128, {});
    marks.update();
    EXPECT_EQ(marks.cbegin(), marks.cend());
}
} // namespace
