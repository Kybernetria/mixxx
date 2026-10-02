#include "analyzer/analyzerbeats.h"

#include <gtest/gtest.h>

#include <QComboBox>
#include <QDir>
#include <QStringList>
#include <QVector>

#include "analyzer/analyzertrack.h"
#include "library/rekordbox/rekordboxconstants.h"
#include "preferences/beatdetectionsettings.h"
#include "preferences/dialog/dlgprefbeats.h"
#include "test/mixxxtest.h"
#include "track/beats.h"
#include "track/track.h"
#include "util/fileinfo.h"

namespace {

constexpr auto kLegacySoundTouchPluginId = "mixxxbpmdetection";
constexpr auto kSampleRate = 44100;

TrackPointer makeTrack() {
    const mixxx::FileInfo fileInfo(
            QDir(QDir::tempPath()), QStringLiteral("analyzer-beats-test.mp3"));
    auto track = Track::newTemporary(mixxx::FileAccess(fileInfo));
    track->setDuration(180);
    return track;
}

mixxx::BeatsPointer makeGrid(
        mixxx::audio::FramePos firstBeat,
        const QString& subVersion = QString()) {
    return mixxx::Beats::fromConstTempo(
            mixxx::audio::SampleRate(kSampleRate),
            firstBeat,
            mixxx::Bpm(120),
            subVersion);
}

} // namespace

class AnalyzerBeatsTest : public MixxxTest {
  protected:
    void selectLegacyAnalyzer() {
        BeatDetectionSettings(config()).setBeatPluginId(
                QString::fromLatin1(kLegacySoundTouchPluginId));
    }

    bool initialize(AnalyzerBeats* analyzer, const TrackPointer& track) {
        BeatDetectionSettings(config()).setBpmDetectionEnabled(true);
        return analyzer->initialize(
                AnalyzerTrack(track),
                mixxx::audio::SampleRate(kSampleRate),
                mixxx::audio::ChannelCount::stereo(),
                kSampleRate * 180);
    }
};

TEST_F(AnalyzerBeatsTest, PreferencesApplyDoesNotMigrateLegacyAnalyzerIdentity) {
    selectLegacyAnalyzer();
    DlgPrefBeats preferences(nullptr, config());
    preferences.slotApply();
    EXPECT_EQ(QString::fromLatin1(kLegacySoundTouchPluginId),
            BeatDetectionSettings(config()).getBeatPluginId());

    // Explicit activation of the displayed Queen Mary entry is a real choice,
    // even when its index was already displayed as the unavailable fallback.
    auto* combo = preferences.findChild<QComboBox*>(QStringLiteral("comboBoxBeatPlugin"));
    ASSERT_TRUE(combo);
    ASSERT_TRUE(QMetaObject::invokeMethod(combo,
            "activated",
            Qt::DirectConnection,
            Q_ARG(int, 0)));
    preferences.slotApply();
    EXPECT_EQ(AnalyzerBeats::defaultPlugin().id(),
            BeatDetectionSettings(config()).getBeatPluginId());
}

TEST_F(AnalyzerBeatsTest, OnlyQueenMaryRemainsAvailable) {
    const auto plugins = AnalyzerBeats::availablePlugins();
    ASSERT_EQ(plugins.size(), 1);
    EXPECT_EQ(plugins.front().id(), AnalyzerBeats::defaultPlugin().id());
    EXPECT_NE(plugins.front().id(), QString::fromLatin1(kLegacySoundTouchPluginId));
}

TEST_F(AnalyzerBeatsTest, LegacySelectionUsesQueenMaryWithoutRewritingPreference) {
    selectLegacyAnalyzer();
    const auto track = makeTrack();

    AnalyzerBeats analyzer(config());
    ASSERT_TRUE(initialize(&analyzer, track));
    EXPECT_EQ(BeatDetectionSettings(config()).getBeatPluginId(),
            QString::fromLatin1(kLegacySoundTouchPluginId));
    analyzer.cleanup();
}

TEST_F(AnalyzerBeatsTest, LegacySelectionPreservesExistingValidGrid) {
    selectLegacyAnalyzer();
    BeatDetectionSettings(config()).setReanalyzeWhenSettingsChange(false);
    BeatDetectionSettings(config()).setReanalyzeImported(false);

    const QVector<mixxx::audio::FramePos> firstBeats = {
            mixxx::audio::kStartFramePos,
            mixxx::audio::FramePos(kSampleRate * 2),
    };
    const QStringList subVersions = {
            QString(),
            QStringLiteral("vamp_plugin_id=mixxxbpmdetection|rounding=V4"),
            QStringLiteral("custom=grid"),
            mixxx::rekordboxconstants::beatsSubversion,
    };
    for (const auto& firstBeat : firstBeats) {
        for (const auto& subVersion : subVersions) {
            const auto track = makeTrack();
            const auto savedGrid = makeGrid(firstBeat, subVersion);
            ASSERT_TRUE(track->trySetBeats(savedGrid));

            AnalyzerBeats analyzer(config());
            EXPECT_FALSE(initialize(&analyzer, track))
                    << "first beat=" << firstBeat.value()
                    << ", subversion=" << subVersion.toStdString();
            EXPECT_EQ(track->getBeats(), savedGrid);
        }
    }
}

TEST_F(AnalyzerBeatsTest, LegacyFallbackDoesNotReplaceExistingGridsWhenReanalysisIsEnabled) {
    selectLegacyAnalyzer();
    BeatDetectionSettings(config()).setReanalyzeWhenSettingsChange(true);
    BeatDetectionSettings(config()).setReanalyzeImported(true);

    const QStringList subVersions = {
            QString(),
            QStringLiteral("vamp_plugin_id=mixxxbpmdetection|rounding=V4"),
            QStringLiteral("vamp_plugin_id=qm-tempotracker:0|rounding=V4"),
            QStringLiteral("custom=grid"),
    };
    for (const auto& subVersion : subVersions) {
        const auto track = makeTrack();
        const auto savedGrid = makeGrid(
                mixxx::audio::FramePos(kSampleRate * 2), subVersion);
        ASSERT_TRUE(track->trySetBeats(savedGrid));

        AnalyzerBeats analyzer(config());
        EXPECT_FALSE(initialize(&analyzer, track))
                << "subversion=" << subVersion.toStdString();
        EXPECT_EQ(track->getBeats(), savedGrid);
    }

    // Imported-grid handling is explicitly controlled by its own preference,
    // even when the configured analyzer is unavailable.
    const auto importedTrack = makeTrack();
    const auto importedGrid = makeGrid(
            mixxx::audio::kStartFramePos,
            mixxx::rekordboxconstants::beatsSubversion);
    ASSERT_TRUE(importedTrack->trySetBeats(importedGrid));
    AnalyzerBeats importedAnalyzer(config());
    EXPECT_TRUE(initialize(&importedAnalyzer, importedTrack));
    EXPECT_EQ(importedTrack->getBeats(), importedGrid);
    importedAnalyzer.cleanup();
}

TEST_F(AnalyzerBeatsTest, ExplicitQueenMarySelectionStillReanalyzesLegacyGrid) {
    BeatDetectionSettings(config()).setBeatPluginId(AnalyzerBeats::defaultPlugin().id());
    BeatDetectionSettings(config()).setReanalyzeWhenSettingsChange(true);
    const auto track = makeTrack();
    const auto savedGrid = makeGrid(
            mixxx::audio::FramePos(kSampleRate * 2),
            QStringLiteral("vamp_plugin_id=mixxxbpmdetection|rounding=V4"));
    ASSERT_TRUE(track->trySetBeats(savedGrid));

    AnalyzerBeats analyzer(config());
    EXPECT_TRUE(initialize(&analyzer, track));
    EXPECT_EQ(track->getBeats(), savedGrid);
    analyzer.cleanup();
}

TEST_F(AnalyzerBeatsTest, MetadataGridAtZeroIsPreservedWhenReanalysisIsEnabled) {
    selectLegacyAnalyzer();
    BeatDetectionSettings(config()).setReanalyzeWhenSettingsChange(true);
    const auto track = makeTrack();
    const auto savedGrid = makeGrid(mixxx::audio::kStartFramePos);
    ASSERT_TRUE(track->trySetBeats(savedGrid));

    AnalyzerBeats analyzer(config());
    EXPECT_FALSE(initialize(&analyzer, track));
    EXPECT_EQ(track->getBeats(), savedGrid);
}

TEST_F(AnalyzerBeatsTest, BpmLockedTrackWithExistingGridIsNotAnalyzed) {
    selectLegacyAnalyzer();
    const auto track = makeTrack();
    const auto savedGrid = makeGrid(mixxx::audio::kStartFramePos);
    ASSERT_TRUE(track->trySetBeats(savedGrid));
    track->setBpmLocked(true);

    AnalyzerBeats analyzer(config());
    EXPECT_FALSE(initialize(&analyzer, track));
    EXPECT_EQ(track->getBeats(), savedGrid);
}

TEST_F(AnalyzerBeatsTest, EmptySelectionUsesQueenMaryByDefault) {
    BeatDetectionSettings(config()).setBeatPluginId(QString());
    const auto track = makeTrack();

    AnalyzerBeats analyzer(config());
    ASSERT_TRUE(initialize(&analyzer, track));
    EXPECT_EQ(AnalyzerBeats::defaultPlugin().id(),
            AnalyzerBeats::availablePlugins().front().id());
    analyzer.cleanup();
}

TEST_F(AnalyzerBeatsTest, MissingGridFallsBackWithoutChangingSavedSelection) {
    selectLegacyAnalyzer();
    const auto track = makeTrack();

    AnalyzerBeats analyzer(config());
    ASSERT_TRUE(initialize(&analyzer, track));
    EXPECT_EQ(BeatDetectionSettings(config()).getBeatPluginId(),
            QString::fromLatin1(kLegacySoundTouchPluginId));
    analyzer.cleanup();
}
