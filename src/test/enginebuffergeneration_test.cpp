#include "engine/cachingreader/cachingreader.h"
#include "test/signalpathtest.h"

class EngineBufferGenerationTest : public BaseSignalPathTest {
  protected:
    void SetUp() override {
        BaseSignalPathTest::SetUp();
        QObject::disconnect(engine(),
                &EngineBuffer::trackLoadFailed,
                m_pMixerDeck1.get(),
                &BaseTrackPlayerImpl::slotLoadFailed);
    }

    EngineBuffer* engine() const {
        return m_pChannel1->getEngineBuffer();
    }

    TrackPointer makeTrack(const QString& name, int sampleRate) const {
        const auto track = Track::newTemporary(getTestDir().filePath(name));
        track->setAudioProperties(mixxx::audio::ChannelCount::stereo(),
                mixxx::audio::SampleRate(sampleRate),
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(60));
        return track;
    }

    quint64 currentRequestGeneration() const {
        return engine()->m_readerRequestGeneration;
    }

    quint64 beginNewRequest() {
        const auto generation = engine()->m_pReader->beginTrackRequest(true);
        engine()->m_readerRequestGeneration = generation;
        return generation;
    }

    void reportLoading(quint64 generation) {
        engine()->slotTrackLoading(generation);
    }

    void reportLoaded(const TrackPointer& track, quint64 generation) {
        engine()->slotTrackLoaded(track,
                track->getSampleRate(),
                track->getChannels(),
                mixxx::audio::FramePos(track->getSampleRate() * 60),
                generation);
    }

    void reportFailed(const TrackPointer& track, quint64 generation) {
        engine()->slotTrackLoadFailed(track, QStringLiteral("delayed failure"), generation);
    }

    bool loading() const {
        return engine()->m_iTrackLoading != 0;
    }
};

TEST_F(EngineBufferGenerationTest, StaleCallbacksCannotResurrectAnEjectedTrack) {
    const auto oldTrack = makeTrack(QStringLiteral("old-generation.wav"), 44100);
    engine()->loadFakeTrack(oldTrack, false);
    ASSERT_EQ(oldTrack, engine()->getLoadedTrack());
    const auto oldGeneration = currentRequestGeneration();
    engine()->ejectTrack();
    ASSERT_NE(oldGeneration, currentRequestGeneration());

    reportLoading(oldGeneration);
    EXPECT_FALSE(loading());
    reportLoaded(oldTrack, oldGeneration);
    EXPECT_FALSE(engine()->getLoadedTrack());
    EXPECT_FALSE(engine()->isTrackLoaded());
    reportFailed(oldTrack, oldGeneration);
    EXPECT_FALSE(engine()->getLoadedTrack());
    EXPECT_FALSE(loading());
    EXPECT_EQ(0.0, ControlObject::get(ConfigKey(m_sGroup1, "track_samplerate")));
}

TEST_F(EngineBufferGenerationTest, StaleCallbacksCannotReplaceOrEjectANewerLoadedTrack) {
    const auto oldTrack = makeTrack(QStringLiteral("old-generation.wav"), 44100);
    const auto newTrack = makeTrack(QStringLiteral("new-generation.wav"), 48000);
    engine()->loadFakeTrack(oldTrack, false);
    const auto oldGeneration = currentRequestGeneration();
    const auto newGeneration = beginNewRequest();
    reportLoading(newGeneration);
    ASSERT_TRUE(loading());
    reportLoaded(newTrack, newGeneration);
    ASSERT_EQ(newTrack, engine()->getLoadedTrack());
    ASSERT_TRUE(engine()->isTrackLoaded());
    ASSERT_FALSE(loading());

    reportLoading(oldGeneration);
    EXPECT_FALSE(loading());
    reportLoaded(oldTrack, oldGeneration);
    EXPECT_EQ(newTrack, engine()->getLoadedTrack());
    reportFailed(oldTrack, oldGeneration);
    EXPECT_EQ(newTrack, engine()->getLoadedTrack());
    EXPECT_TRUE(engine()->isTrackLoaded());
    EXPECT_FALSE(loading());
    EXPECT_EQ(48000.0, ControlObject::get(ConfigKey(m_sGroup1, "track_samplerate")));

    reportFailed(newTrack, newGeneration);
    EXPECT_NE(newGeneration, currentRequestGeneration());
    EXPECT_FALSE(engine()->getLoadedTrack());
    EXPECT_FALSE(engine()->isTrackLoaded());
}

TEST_F(EngineBufferGenerationTest, StaleGuiEjectCannotClearAReloadOfTheSameTrack) {
    const auto sameTrack = m_pMixerDeck1->loadFakeTrack(false, 120);
    ASSERT_EQ(sameTrack, m_pMixerDeck1->getLoadedTrack());
    const auto oldGeneration = currentRequestGeneration();
    const auto newGeneration = beginNewRequest();
    reportLoading(newGeneration);
    reportLoaded(sameTrack, newGeneration);
    ASSERT_EQ(sameTrack, engine()->getLoadedTrack());

    m_pMixerDeck1->slotTrackLoaded({}, sameTrack, oldGeneration);
    EXPECT_EQ(sameTrack, m_pMixerDeck1->getLoadedTrack());
    EXPECT_EQ(sameTrack, engine()->getLoadedTrack());
    EXPECT_TRUE(engine()->isTrackLoaded());
}
