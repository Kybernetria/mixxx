#include "engine/controls/enginecontrol.h"
#include "test/mockedenginebackendtest.h"
#include "util/audiocallbackscope.h"
#include "waveform/visualplayposition.h"

#include <functional>
#include <future>

class SeekMailboxObserver : public EngineControl {
  public:
    SeekMailboxObserver(const QString& group,
            UserSettingsPointer config,
            std::function<void()> onFirstSeek)
            : EngineControl(group, config), onFirstSeek(std::move(onFirstSeek)) {
    }

    void notifySeek(mixxx::audio::FramePos) override {
        ++notificationCount;
        if (notificationCount == 1) {
            beforePublication = getEngineBuffer()->queuedSeekPosition();
            onFirstSeek();
            afterPublication = getEngineBuffer()->queuedSeekPosition();
        }
    }

    const std::function<void()> onFirstSeek;
    int notificationCount = 0;
    mixxx::audio::FramePos beforePublication;
    mixxx::audio::FramePos afterPublication;
};

class EngineBufferSeekMailboxTest : public MockedEngineBackendTest {
  protected:
    EngineBuffer* engine() const {
        return m_pChannel1->getEngineBuffer();
    }

    void SetUp() override {
        MockedEngineBackendTest::SetUp();
        engine()->m_playPos = mixxx::audio::kStartFramePos;
        engine()->m_trackEndPositionOld = engine()->getTrackEndPosition();
        engine()->m_rate_old = 0;
        engine()->m_queuedSeek.publish(EngineBuffer::kNoQueuedSeek);
        processSeek();
    }

    void processSeek() {
        const mixxx::AudioCallbackScope scope;
        engine()->processSeek(true);
    }

    SeekMailboxObserver* observe(std::function<void()> onFirstSeek) {
        auto* observer = new SeekMailboxObserver(m_sGroup1, m_pConfig, std::move(onFirstSeek));
        engine()->addControl(observer);
        return observer;
    }

    bool processingPositionIsValid() const {
        return engine()->m_processingSeekPosition.isValid();
    }

    void queueClone(mixxx::audio::FramePos position) {
        auto* other = m_pChannel2->getEngineBuffer();
        const mixxx::AudioCallbackScope scope;
        other->m_visualPlayPos->set(position.value() / other->getTrackEndPosition().value(),
                0, 0, 0, 0, SlipModeState::Disabled, false, false, false, 0, 0, 0, 0);
        engine()->m_queuedSeek.publish(
                {mixxx::audio::kInvalidFramePos, EngineBuffer::SEEK_CLONE, m_pChannel2});
    }

    void queueSlipRestore(mixxx::audio::FramePos position) {
        engine()->m_pendingSlipRestorePosition = position;
    }

    int queuedPhase() const {
        return engine()->m_iSeekPhaseQueued.loadAcquire();
    }

    void queueNoSeekAtTrackEnd() {
        engine()->m_playPos = engine()->getTrackEndPosition();
        engine()->m_queuedSeek.publish(EngineBuffer::kNoQueuedSeek);
    }

    bool mayPlay() const {
        return engine()->updateIndicatorsAndModifyPlay(true, false);
    }

    void queueNewCloneMetadata() {
        engine()->m_pChannelToCloneFrom = m_pChannel3;
    }

    bool newCloneMetadataIsIntact() const {
        return engine()->m_pChannelToCloneFrom.loadAcquire() == m_pChannel3;
    }
};

TEST_F(EngineBufferSeekMailboxTest, AudioSeekQueuedDuringNotificationSurvivesCapturedSeek) {
    auto* observer = observe([&] {
        engine()->queueNewPlaypos(mixxx::audio::FramePos(200), EngineBuffer::SEEK_EXACT);
    });
    engine()->queueNewPlaypos(mixxx::audio::FramePos(100), EngineBuffer::SEEK_EXACT);
    processSeek();
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(100), engine()->getPlayPos());
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(100), observer->beforePublication);
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(100), observer->afterPublication);
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(200), engine()->queuedSeekPosition());
    EXPECT_FALSE(processingPositionIsValid());
    processSeek();
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(200), engine()->getPlayPos());
    EXPECT_FALSE(engine()->queuedSeekPosition().isValid());
}

TEST_F(EngineBufferSeekMailboxTest, NonAudioSeekQueuedDuringNotificationSurvivesCapturedSeek) {
    std::promise<void> start;
    auto producer = std::async(std::launch::async, [&] {
        start.get_future().wait();
        engine()->queueNewPlaypos(mixxx::audio::FramePos(200), EngineBuffer::SEEK_EXACT);
        return engine()->queuedSeekPosition();
    });
    mixxx::audio::FramePos producerPosition;
    auto* observer = observe([&] {
        start.set_value();
        producerPosition = producer.get();
    });
    engine()->queueNewPlaypos(mixxx::audio::FramePos(100), EngineBuffer::SEEK_EXACT);
    processSeek();
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(100), engine()->getPlayPos());
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(200), producerPosition);
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(100), observer->afterPublication);
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(200), engine()->queuedSeekPosition());
    processSeek();
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(200), engine()->getPlayPos());
    EXPECT_FALSE(engine()->queuedSeekPosition().isValid());
}

TEST_F(EngineBufferSeekMailboxTest, CloneSeekResolvesItsPositionAndPreservesNewerExactSeek) {
    auto* observer = observe([&] {
        engine()->queueNewPlaypos(mixxx::audio::FramePos(200), EngineBuffer::SEEK_EXACT);
    });
    queueClone(mixxx::audio::FramePos(100));
    queueNewCloneMetadata();
    processSeek();
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(100), engine()->getPlayPos());
    EXPECT_TRUE(newCloneMetadataIsIntact());
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(100), observer->beforePublication);
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(200), engine()->queuedSeekPosition());
    processSeek();
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(200), engine()->getPlayPos());
    EXPECT_FALSE(processingPositionIsValid());
}

TEST_F(EngineBufferSeekMailboxTest, SlipFallbackPreservesNewExactSeekAndLivePhaseCannotChangeIt) {
    observe([&] {
        engine()->queueNewPlaypos(mixxx::audio::FramePos(200), EngineBuffer::SEEK_EXACT);
    });
    queueSlipRestore(mixxx::audio::FramePos(100));
    processSeek();
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(100), engine()->getPlayPos());
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(200), engine()->queuedSeekPosition());
    engine()->requestSyncPhase(true);
    processSeek();
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(200), engine()->getPlayPos());
    EXPECT_EQ(0, queuedPhase());
    EXPECT_FALSE(engine()->queuedSeekPosition().isValid());
}

TEST_F(EngineBufferSeekMailboxTest, ResetCommandDoesNotAllowPlaybackPastTrackEnd) {
    queueNoSeekAtTrackEnd();
    EXPECT_FALSE(mayPlay());
    engine()->queueNewPlaypos(mixxx::audio::FramePos(100), EngineBuffer::SEEK_EXACT);
    EXPECT_TRUE(mayPlay());
}
