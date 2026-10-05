#include <chrono>
#include <future>
#include <thread>

#include <QtDebug>

#include "library/coverart.h"
#include "sources/soundsourceproxy.h"
#include "test/mixxxtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/beats.h"
#include "track/track.h"

// Test for updating track metadata and cover art from files.
class TrackUpdateTest : public MixxxTest, SoundSourceProviderRegistration {
  protected:
    static bool hasTrackMetadata(const TrackPointer& pTrack) {
        return !pTrack->getArtist().isEmpty();
    }

    static bool hasCoverArt(const TrackPointer& pTrack) {
        return pTrack->getCoverInfo().type != CoverInfo::NONE;
    }

    static TrackPointer newTestTrack() {
        return Track::newTemporary(
                QDir(MixxxTest::getOrInitTestDir().filePath(QStringLiteral("id3-test-data"))),
                "TOAL_TPE2.mp3");
    }

    TrackPointer newTestTrackParsed() const {
        auto pTrack = newTestTrack();
        EXPECT_EQ(
                SoundSourceProxy::UpdateTrackFromSourceResult::MetadataImportedAndUpdated,
                SoundSourceProxy(pTrack).updateTrackFromSource(
                        SoundSourceProxy::UpdateTrackFromSourceMode::Once,
                        SyncTrackMetadataParams{}));
        EXPECT_TRUE(pTrack->checkSourceSynchronized());
        EXPECT_TRUE(hasTrackMetadata(pTrack));
        EXPECT_TRUE(hasCoverArt(pTrack));
        pTrack->markClean();
        EXPECT_FALSE(pTrack->isDirty());
        return pTrack;
    }

    TrackPointer newTestTrackParsedModified() const {
        auto pTrack = newTestTrackParsed();
        pTrack->setArtist(pTrack->getArtist() + pTrack->getArtist());
        auto coverInfo = pTrack->getCoverInfo();
        coverInfo.type = CoverInfo::FILE;
        coverInfo.source = CoverInfo::USER_SELECTED;
        coverInfo.setImageDigest(QImage(1, 1, QImage::Format_Mono));
        pTrack->setCoverInfo(coverInfo);
        EXPECT_TRUE(pTrack->isDirty());
        return pTrack;
    }
};

TEST_F(TrackUpdateTest, UndoBeatsChangeEmitsAfterReleasingTrackMutex) {
    auto pTrack = newTestTrack();
    const auto previousBeats = mixxx::Beats::fromConstTempo(
            mixxx::audio::SampleRate(48000), mixxx::audio::kStartFramePos, mixxx::Bpm(120));
    ASSERT_TRUE(pTrack->trySetBeats(previousBeats));
    std::this_thread::sleep_for(std::chrono::milliseconds(810));
    const auto currentBeats = mixxx::Beats::fromConstTempo(
            mixxx::audio::SampleRate(48000), mixxx::audio::kStartFramePos, mixxx::Bpm(100));
    ASSERT_TRUE(pTrack->trySetBeats(currentBeats));
    ASSERT_TRUE(pTrack->canUndoBeatsChange());

    std::promise<mixxx::BeatsPointer> observedBeats;
    auto observation = observedBeats.get_future();
    std::thread reader;
    QObject::connect(pTrack.get(), &Track::beatsUpdated, pTrack.get(), [&] {
        reader = std::thread([&] {
            observedBeats.set_value(pTrack->getBeats());
        });
        EXPECT_EQ(std::future_status::ready, observation.wait_for(std::chrono::seconds(1)));
    }, Qt::DirectConnection);

    pTrack->undoBeatsChange();

    ASSERT_TRUE(reader.joinable());
    reader.join();
    EXPECT_EQ(previousBeats, observation.get());
    EXPECT_FALSE(pTrack->canUndoBeatsChange());
}

TEST_F(TrackUpdateTest, parseModifiedCleanOnce) {
    auto pTrack = newTestTrackParsedModified();
    pTrack->markClean();

    const auto trackMetadataBefore = pTrack->getMetadata();
    const auto coverInfoBefore = pTrack->getCoverInfo();

    // Re-update from source should have no effect
    ASSERT_EQ(
            SoundSourceProxy::UpdateTrackFromSourceResult::NotUpdated,
            SoundSourceProxy(pTrack).updateTrackFromSource(
                    SoundSourceProxy::UpdateTrackFromSourceMode::Once,
                    SyncTrackMetadataParams{}));

    const auto trackMetadataAfter = pTrack->getMetadata();
    const auto coverInfoAfter = pTrack->getCoverInfo();

    // Verify that the track has not been modified
    ASSERT_TRUE(pTrack->checkSourceSynchronized());
    ASSERT_FALSE(pTrack->isDirty());
    ASSERT_EQ(trackMetadataBefore, trackMetadataAfter);
    ASSERT_EQ(coverInfoBefore, coverInfoAfter);
}

TEST_F(TrackUpdateTest, parseModifiedCleanAgainSkipCover) {
    auto pTrack = newTestTrackParsedModified();
    pTrack->markClean();

    const auto trackMetadataBefore = pTrack->getMetadata();
    const auto coverInfoBefore = pTrack->getCoverInfo();

    EXPECT_EQ(
            SoundSourceProxy::UpdateTrackFromSourceResult::MetadataImportedAndUpdated,
            SoundSourceProxy(pTrack).updateTrackFromSource(
                    SoundSourceProxy::UpdateTrackFromSourceMode::Always,
                    SyncTrackMetadataParams{}));

    const auto trackMetadataAfter = pTrack->getMetadata();
    const auto coverInfoAfter = pTrack->getCoverInfo();

    // Updated
    EXPECT_TRUE(pTrack->checkSourceSynchronized());
    EXPECT_TRUE(pTrack->isDirty());
    EXPECT_NE(trackMetadataBefore, trackMetadataAfter);
    EXPECT_EQ(coverInfoBefore, coverInfoAfter);
}

TEST_F(TrackUpdateTest, parseModifiedCleanAgainUpdateCover) {
    auto pTrack = newTestTrackParsedModified();
    auto coverInfo = pTrack->getCoverInfo();
    coverInfo.type = CoverInfo::METADATA;
    coverInfo.source = CoverInfo::GUESSED;
    pTrack->setCoverInfo(coverInfo);
    pTrack->markClean();

    const auto trackMetadataBefore = pTrack->getMetadata();
    const auto coverInfoBefore = pTrack->getCoverInfo();

    EXPECT_EQ(
            SoundSourceProxy::UpdateTrackFromSourceResult::MetadataImportedAndUpdated,
            SoundSourceProxy(pTrack).updateTrackFromSource(
                    SoundSourceProxy::UpdateTrackFromSourceMode::Always,
                    SyncTrackMetadataParams{}));

    const auto trackMetadataAfter = pTrack->getMetadata();
    const auto coverInfoAfter = pTrack->getCoverInfo();

    // Updated
    EXPECT_TRUE(pTrack->checkSourceSynchronized());
    EXPECT_TRUE(pTrack->isDirty());
    EXPECT_NE(trackMetadataBefore, trackMetadataAfter);
    EXPECT_NE(coverInfoBefore, coverInfoAfter);
}

TEST_F(TrackUpdateTest, parseModifiedDirtyAgain) {
    auto pTrack = newTestTrackParsedModified();

    const auto trackMetadataBefore = pTrack->getMetadata();
    const auto coverInfoBefore = pTrack->getCoverInfo();

    EXPECT_EQ(
            SoundSourceProxy::UpdateTrackFromSourceResult::MetadataImportedAndUpdated,
            SoundSourceProxy(pTrack).updateTrackFromSource(
                    SoundSourceProxy::UpdateTrackFromSourceMode::Always,
                    SyncTrackMetadataParams{}));

    const auto trackMetadataAfter = pTrack->getMetadata();
    const auto coverInfoAfter = pTrack->getCoverInfo();

    // Updated
    EXPECT_TRUE(pTrack->checkSourceSynchronized());
    EXPECT_TRUE(pTrack->isDirty());
    EXPECT_NE(trackMetadataBefore, trackMetadataAfter);
    EXPECT_EQ(coverInfoBefore, coverInfoAfter);
}

// TODO: Add tests for SoundSourceProxy::UpdateTrackFromSourceMode::Newer
