#include <gtest/gtest.h>

#include "control/controlobject.h"
#include "test/mixxxtest.h"
#include "track/track.h"
#include "widget/wcuemenupopup.h"

namespace {
class MemoryCuePopupTest : public MixxxTest {};

TEST_F(MemoryCuePopupTest, OnlyValidMemoryActionsAndNoUnindexedHotcueConversion) {
    ControlObject beatLoopSize(ConfigKey("[MemoryPopupTest]", "beatloop_size"));
    ControlObject playposition(ConfigKey("[MemoryPopupTest]", "playposition"));
    ControlObject trackSamples(ConfigKey("[MemoryPopupTest]", "track_samples"));
    ControlObject quantize(ConfigKey("[MemoryPopupTest]", "quantize"));
    QWidget parent;
    WCueMenuPopup popup(config(), &parent);
    const auto track = Track::newTemporary(QStringLiteral("/tmp/mixxx-memory-popup-test.mp3"));
    const auto cue = track->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(1000),
            mixxx::audio::kInvalidFramePos);
    popup.setTrackCueGroup(track, cue, QStringLiteral("[MemoryPopupTest]"));
    for (const auto* name : {"CueStandardButton", "CueSavedLoopButton", "CueSavedJumpButton"}) {
        ASSERT_TRUE(popup.findChild<QPushButton*>(name));
        EXPECT_TRUE(popup.findChild<QPushButton*>(name)->isHidden());
    }
    ASSERT_TRUE(popup.findChild<QPushButton*>("CueDeleteButton"));
    EXPECT_FALSE(popup.findChild<QPushButton*>("CueDeleteButton")->isHidden());
    for (const auto* slot : {"slotStandardCue",
                 "slotSavedLoopCueAuto",
                 "slotSavedLoopCueManual",
                 "slotSavedJumpCueAuto",
                 "slotSavedJumpCueManual"}) {
        ASSERT_TRUE(QMetaObject::invokeMethod(&popup, slot, Qt::DirectConnection));
        EXPECT_EQ(mixxx::CueType::Memory, cue->getType());
        EXPECT_EQ(Cue::kNoHotCue, cue->getHotCue());
        EXPECT_EQ(1000, cue->getPosition().value());
        EXPECT_FALSE(cue->getEndPosition().isValid());
    }
    auto* label = popup.findChild<QLineEdit*>("CueLabelEdit");
    ASSERT_TRUE(label);
    label->setText(QStringLiteral("Memory label"));
    ASSERT_TRUE(QMetaObject::invokeMethod(&popup, "slotEditLabel", Qt::DirectConnection));
    EXPECT_EQ(QStringLiteral("Memory label"), cue->getLabel());
    // An external conversion must not make an unindexed cue convertible.
    cue->setType(mixxx::CueType::MainCue);
    QMetaObject::invokeMethod(&popup, "slotUpdate", Qt::DirectConnection);
    QMetaObject::invokeMethod(&popup, "slotStandardCue", Qt::DirectConnection);
    QMetaObject::invokeMethod(&popup, "slotSavedJumpCueAuto", Qt::DirectConnection);
    EXPECT_EQ(mixxx::CueType::MainCue, cue->getType());
    cue->setType(mixxx::CueType::Memory);
    ASSERT_TRUE(QMetaObject::invokeMethod(&popup, "slotDeleteCue", Qt::DirectConnection));
    EXPECT_TRUE(track->getCuePoints().empty());
    label->setText(QStringLiteral("Obsolete edit"));
    ASSERT_TRUE(QMetaObject::invokeMethod(&popup, "slotEditLabel", Qt::DirectConnection));
    EXPECT_EQ(QStringLiteral("Memory label"), cue->getLabel());
}
} // namespace
