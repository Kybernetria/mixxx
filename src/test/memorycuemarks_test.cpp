#include <gtest/gtest.h>

#include <climits>

#include "track/track.h"
#include "waveform/beatcountdown.h"
#include "waveform/renderers/waveformmark.h"
#include "waveform/renderers/waveformmarkset.h"

#ifdef MIXXX_USE_QOPENGL
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QScopeGuard>

#include "waveform/renderers/allshader/waveformrendermark.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/waveformwidgetfactory.h"
#endif

namespace {

TEST(MemoryCueMarksTest, BeatCountdownUsesFourBeatBars) {
    EXPECT_EQ(QStringLiteral("3.3"), mixxx::formatBeatCountdown(15));
    EXPECT_EQ(QStringLiteral("3.2"), mixxx::formatBeatCountdown(14));
    EXPECT_EQ(QStringLiteral("3.1"), mixxx::formatBeatCountdown(13));
    EXPECT_EQ(QStringLiteral("3.0"), mixxx::formatBeatCountdown(12));
    EXPECT_EQ(QStringLiteral("2.3"), mixxx::formatBeatCountdown(11));
    EXPECT_EQ(QStringLiteral("4.0"), mixxx::formatBeatCountdown(16));
    EXPECT_EQ(QStringLiteral("1.0"), mixxx::formatBeatCountdown(4));
    EXPECT_EQ(QStringLiteral("0.3"), mixxx::formatBeatCountdown(3));
    EXPECT_EQ(QStringLiteral("0.1"), mixxx::formatBeatCountdown(1));
    EXPECT_TRUE(mixxx::formatBeatCountdown(0).isEmpty());
    EXPECT_TRUE(mixxx::formatBeatCountdown(-1).isEmpty());
    EXPECT_EQ(QStringLiteral("536870911.3"), mixxx::formatBeatCountdown(INT_MAX));
}

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
    EXPECT_FALSE(mark->isShowUntilNext());
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
    EXPECT_TRUE(firstMark->isShowUntilNext());
    EXPECT_TRUE(marks.getHotCueMark(0)->isShowUntilNext());
    EXPECT_EQ(200, firstMark->getSamplePosition());
    first->setStartPosition(mixxx::audio::FramePos(200));
    first->setLabel(QStringLiteral("Moved"));
    marks.syncMemoryCueMarks(QString(), track->getCuePoints(), 128, {});
    marks.update();
    EXPECT_EQ(400, firstMark->getSamplePosition());
    EXPECT_EQ(QStringLiteral("Moved"), firstMark->m_text);
    EXPECT_TRUE(firstMark->isShowUntilNext());
    first->setType(mixxx::CueType::HotCue);
    EXPECT_FALSE(firstMark->isShowUntilNext());
    track->removeCue(second);
    marks.syncMemoryCueMarks(QString(), track->getCuePoints(), 128, {});
    marks.update();
    EXPECT_EQ(marks.cbegin(), marks.cend());
    EXPECT_FALSE(firstMark->isShowUntilNext());
}
#ifdef MIXXX_USE_QOPENGL
class TestWaveformRenderMark : public allshader::WaveformRenderMark {
  public:
    using allshader::WaveformRenderMark::WaveformRenderMark;

    void synchronizeMarks() {
        m_marks.update();
    }
};

TEST(MemoryCueMarksTest, RemovedRenderedMarkExpiresBeforeNextFrame) {
    const auto destroyFactory = [](WaveformWidgetFactory*) {
        WaveformWidgetFactory::destroy();
    };
    std::unique_ptr<WaveformWidgetFactory, decltype(destroyFactory)> factoryOwner(
            WaveformWidgetFactory::isCreated()
                    ? nullptr
                    : WaveformWidgetFactory::createInstance(),
            destroyFactory);
    QOpenGLContext context;
    if (!context.create()) {
        GTEST_SKIP() << "An OpenGL context is required for marker-node retirement";
    }
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    ASSERT_TRUE(context.makeCurrent(&surface));
    const auto releaseContext = qScopeGuard([&context] { context.doneCurrent(); });

    // Destroy both graphics holders and render-owned nodes while the context
    // is still current, including when an assertion exits the test early.
    WaveformWidgetRenderer widget;
    widget.resizeRenderer(400, 120, 1.0f);
    TestWaveformRenderMark renderer(&widget);
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
    auto mark = std::get<WaveformMarkPointer>(std::move(maybeMark));
    mark->setSamplePosition(0.0);
    renderer.addMark(mark);
    renderer.synchronizeMarks();
    renderer.update();
    ASSERT_NE(nullptr, renderer.firstChild()->nextSibling()->firstChild());
    const auto weakMark = mark.toWeakRef();

    // Another renderer (e.g. the slip view) can replace the hover list. Cue
    // removal or a track switch then removes the last strong mark reference,
    // while its visible node still belongs to this renderer's node tree.
    widget.setMarkPositions({});
    renderer.clearMarks();
    mark.reset();
    ASSERT_TRUE(weakMark.isNull());
    renderer.update();
    renderer.update();
}
#endif
} // namespace
