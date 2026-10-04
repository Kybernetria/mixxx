#include "engine/readaheadmanager.h"

#include <gtest/gtest.h>

#include <optional>

#include <QScopedPointer>
#include <QtDebug>

#include "control/controlobject.h"
#include "control/controlpotmeter.h"
#include "control/controlpushbutton.h"
#include "control/controlttrotary.h"
#include "engine/cachingreader/cachingreader.h"
#include "engine/cachingreader/cachingreaderchunk.h"
#include "engine/controls/cuecontrol.h"
#include "engine/controls/loopingcontrol.h"
#include "engine/controls/ratecontrol.h"
#include "engine/positionscratchcontroller.h"
#include "test/callbackallocationcheck.h"
#include "test/mixxxtest.h"
#include "util/assert.h"
#include "util/defs.h"
#include "util/rotary.h"
#include "util/sample.h"

namespace {
const QString kGroup = "[test]";
} // namespace

class StubReader : public CachingReader {
  public:
    StubReader()
            : CachingReader(kGroup, UserSettingsPointer(), mixxx::audio::ChannelCount::stereo()) {
    }

    CachingReader::ReadResult read(SINT startSample,
            SINT numSamples,
            bool reverse,
            CSAMPLE* buffer,
            mixxx::audio::ChannelCount channelCount) override {
        ++readCount;
        lastReadPosition = startSample;
        lastReadSampleCount = numSamples;
        lastReadReverse = reverse;
        if (miss || readCount == failOnRead) {
            return CachingReader::ReadResult::UNAVAILABLE;
        }
        const SINT firstFrame = startSample / channelCount -
                (reverse ? numSamples / channelCount : 0);
        if (missingChunk && numSamples > 0 &&
                firstFrame < (*missingChunk + 1) * CachingReaderChunk::kFrames &&
                firstFrame + numSamples / channelCount >
                        *missingChunk * CachingReaderChunk::kFrames) {
            return CachingReader::ReadResult::UNAVAILABLE;
        }
        for (SINT i = 0; i < numSamples; ++i) {
            const SINT sourceSample = reverse && preserveSourceDirection
                    ? startSample - channelCount -
                            (i / channelCount) * channelCount + i % channelCount
                    : startSample + i;
            buffer[i] = static_cast<CSAMPLE>(sourceSample);
        }
        if (readCount == partialOnRead) {
            SampleUtil::clear(buffer + numSamples / 2, numSamples - numSamples / 2);
            return partialCacheMiss ? CachingReader::ReadResult::PARTIALLY_UNAVAILABLE
                                    : CachingReader::ReadResult::PARTIALLY_AVAILABLE;
        }
        return CachingReader::ReadResult::AVAILABLE;
    }
    bool miss = false;
    int readCount = 0;
    int failOnRead = -1;
    int partialOnRead = -1;
    bool partialCacheMiss = true;
    bool preserveSourceDirection = false;
    SINT lastReadPosition = 0;
    SINT lastReadSampleCount = 0;
    bool lastReadReverse = false;
    std::optional<SINT> missingChunk;
};

class StubLoopControl : public LoopingControl {
  public:
    StubLoopControl()
            : LoopingControl(kGroup, UserSettingsPointer()) {
    }

    std::uint64_t triggerRevision() const override {
        return useRealControl ? LoopingControl::triggerRevision() : m_triggerRevision;
    }
    void bumpTriggerRevision() {
        ++m_triggerRevision;
    }

    void pushValues(double trigger, double target) {
        m_triggerReturnValues.push_back(
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(trigger));
        m_targetReturnValues.push_back(
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(target));
    }

    mixxx::audio::FramePos nextTrigger(bool reverse,
            mixxx::audio::FramePos currentPosition,
            mixxx::audio::FramePos* pTargetPosition,
            ReadTriggerState* nextState = nullptr) override {
        ++queryCount;
        if (useRealControl) {
            return LoopingControl::nextTrigger(
                    reverse, currentPosition, pTargetPosition, nextState);
        }
        Q_UNUSED(reverse);
        Q_UNUSED(currentPosition);
        Q_UNUSED(pTargetPosition);
        RELEASE_ASSERT(!m_targetReturnValues.isEmpty());
        *pTargetPosition = m_targetReturnValues.takeFirst();
        RELEASE_ASSERT(!m_triggerReturnValues.isEmpty());
        return m_triggerReturnValues.takeFirst();
    }

  protected:
    QList<mixxx::audio::FramePos> m_triggerReturnValues;
    QList<mixxx::audio::FramePos> m_targetReturnValues;

  public:
    int queryCount{0};
    std::uint64_t m_triggerRevision{0};
    bool useRealControl{false};
};

class StubCueControl : public CueControl {
  public:
    StubCueControl()
            : CueControl(kGroup, UserSettingsPointer()) {
    }

    std::uint64_t triggerRevision() const override {
        return useRealControl ? CueControl::triggerRevision() : m_triggerRevision;
    }
    void bumpTriggerRevision() {
        ++m_triggerRevision;
    }

    void pushValues(double trigger, double target) {
        m_triggerReturnValues.push_back(
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(trigger));

        m_targetReturnValues.push_back(
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(target));
    }

    mixxx::audio::FramePos nextTrigger(bool reverse,
            mixxx::audio::FramePos currentPosition,
            mixxx::audio::FramePos* pTargetPosition,
            mixxx::audio::FrameDiff_t lookAheadFrames,
            bool commitState) override {
        ++queryCount;
        if (useRealControl) {
            return CueControl::nextTrigger(reverse,
                    currentPosition,
                    pTargetPosition,
                    lookAheadFrames,
                    commitState);
        }
        if (bumpRevisionOnQuery) {
            bumpTriggerRevision();
            bumpRevisionOnQuery = false;
        }
        RELEASE_ASSERT(!m_targetReturnValues.isEmpty());
        *pTargetPosition = m_targetReturnValues.takeFirst();
        RELEASE_ASSERT(!m_triggerReturnValues.isEmpty());
        return m_triggerReturnValues.takeFirst();
    }

  protected:
    QList<mixxx::audio::FramePos> m_triggerReturnValues;
    QList<mixxx::audio::FramePos> m_targetReturnValues;

  public:
    int queryCount{0};
    std::uint64_t m_triggerRevision{0};
    bool bumpRevisionOnQuery{false};
    bool useRealControl{false};
};

class ReadAheadManagerTest : public MixxxTest {
  public:
    ReadAheadManagerTest()
            : m_beatClosestCO(ConfigKey(kGroup, "beat_closest")),
              m_beatNextCO(ConfigKey(kGroup, "beat_next")),
              m_beatPrevCO(ConfigKey(kGroup, "beat_prev")),
              m_playCO(ConfigKey(kGroup, "play")),
              m_stopCO(ConfigKey(kGroup, "stop")),
              m_vinylControlCO(ConfigKey(kGroup, "vinylcontrol_enabled")),
              m_vinylControlModeCO(ConfigKey(kGroup, "vinylcontrol_mode")),
              m_passthroughCO(ConfigKey(kGroup, "passthrough")),
              m_indicator250msCO(ConfigKey("[App]", "indicator_250ms")),
              m_indicator500msCO(ConfigKey("[App]", "indicator_500ms")),
              m_quantizeCO(ConfigKey(kGroup, "quantize")),
              m_repeatCO(ConfigKey(kGroup, "repeat")),
              m_slipEnabledCO(ConfigKey(kGroup, "slip_enabled")),
              m_trackSamplesCO(ConfigKey(kGroup, "track_samples")),
              m_mainSampleRateCO(ConfigKey("[App]", "samplerate"), true, false, false, 44100),
              m_syncModeCO(ConfigKey(kGroup, "sync_mode")),
              m_pBuffer(SampleUtil::alloc(MAX_BUFFER_LEN)) {
    }
    ~ReadAheadManagerTest() override {
        SampleUtil::free(m_pBuffer);
    }

  protected:
    void SetUp() override {
        SampleUtil::clear(m_pBuffer, MAX_BUFFER_LEN);
        m_pReader.reset(new StubReader());
        m_pLoopControl.reset(new StubLoopControl());
        m_pLoopControl->setFrameInfo(mixxx::audio::FramePos(0),
                mixxx::audio::FramePos(100000),
                mixxx::audio::SampleRate(44100));
        m_pCueControl.reset(new StubCueControl());
        m_pReadAheadManager.reset(new ReadAheadManager(m_pReader.data(),
                m_pLoopControl.data(),
                m_pCueControl.data()));
    }

    ControlObject m_beatClosestCO;
    ControlObject m_beatNextCO;
    ControlObject m_beatPrevCO;
    ControlObject m_playCO;
    ControlObject m_stopCO;
    ControlObject m_vinylControlCO;
    ControlObject m_vinylControlModeCO;
    ControlObject m_passthroughCO;
    ControlObject m_indicator250msCO;
    ControlObject m_indicator500msCO;
    ControlObject m_quantizeCO;
    ControlObject m_repeatCO;
    ControlObject m_slipEnabledCO;
    ControlObject m_trackSamplesCO;
    ControlObject m_mainSampleRateCO;
    ControlObject m_syncModeCO;
    CSAMPLE* m_pBuffer;
    QScopedPointer<StubReader> m_pReader;
    QScopedPointer<StubLoopControl> m_pLoopControl;
    QScopedPointer<StubCueControl> m_pCueControl;
    QScopedPointer<ReadAheadManager> m_pReadAheadManager;
    RateControl m_rateControl{kGroup, UserSettingsPointer()};

    void fillReadLogWithZeroLengthEntries() {
        for (std::size_t i = 0; i < ReadAheadManager::kReadLogCapacity; ++i) {
            const double position = static_cast<double>(i) * 2;
            m_pReadAheadManager->addReadLogEntry(position, position);
        }
    }
    std::size_t readLogSize() const {
        return m_pReadAheadManager->m_readLogSize;
    }
    std::size_t readLogCapacity() const {
        return ReadAheadManager::kReadLogCapacity;
    }
    int rateControlWrapAroundCount() const {
        return m_rateControl.m_wrapAroundCount;
    }
    mixxx::audio::FramePos rateControlJumpPosition() const {
        return m_rateControl.m_jumpPos;
    }
    mixxx::audio::FramePos rateControlTargetPosition() const {
        return m_rateControl.m_targetPos;
    }
    void setChangedLoop(mixxx::audio::FramePos start,
            mixxx::audio::FramePos end,
            LoopingControl::LoopSeekMode mode) {
        m_pLoopControl->setLoopInfo({start, end, mode});
    }
    HotcueControl* hotcue(int index) {
        return m_pCueControl->m_hotcueControls[index];
    }
    bool firstReadLogEntryHasWrap() const {
        return m_pReadAheadManager
                ->m_readAheadLog[m_pReadAheadManager->m_readLogStart]
                .hasWrapAround;
    }
    void setReaderRange(SINT start, SINT end) {
        m_pReader->m_readableFrameIndexRange = mixxx::IndexRange::between(start, end);
        m_pReader->m_state.storeRelease(CachingReader::STATE_TRACK_LOADED);
    }
};

TEST_F(ReadAheadManagerTest, RealLoopControlsInvalidateTriggerRevision) {
    auto revision = m_pLoopControl->LoopingControl::triggerRevision();
    m_pLoopControl->slotLoopStartPos(8);
    EXPECT_GT(m_pLoopControl->LoopingControl::triggerRevision(), revision);
    revision = m_pLoopControl->LoopingControl::triggerRevision();
    ControlObject::set(ConfigKey(kGroup, "slip_enabled"), 1);
    ControlObject::set(ConfigKey(kGroup, "repeat"), 1);
    EXPECT_GT(m_pLoopControl->LoopingControl::triggerRevision(), revision);
    revision = m_pLoopControl->LoopingControl::triggerRevision();
    ControlObject::set(ConfigKey(kGroup, "repeat"), 0);
    EXPECT_GT(m_pLoopControl->LoopingControl::triggerRevision(), revision);
}

TEST_F(ReadAheadManagerTest, RealHotcueControlsInvalidateTriggerRevision) {
    auto revision = m_pCueControl->CueControl::triggerRevision();
    ControlObject::set(ConfigKey(kGroup, "hotcue_1_position"), 8);
    EXPECT_GT(m_pCueControl->CueControl::triggerRevision(), revision);
    revision = m_pCueControl->CueControl::triggerRevision();
    ControlObject::set(ConfigKey(kGroup, "hotcue_1_endposition"), 4);
    EXPECT_GT(m_pCueControl->CueControl::triggerRevision(), revision);
}

TEST_F(ReadAheadManagerTest, OwnerHotcueStatusChangeInvalidatesTriggerState) {
    HotcueControl cue(QStringLiteral("[revision]"), 0);
    int changes = 0;
    QObject::connect(
            &cue,
            &HotcueControl::triggerStateChanged,
            &cue,
            [&changes] { ++changes; },
            Qt::DirectConnection);
    cue.setStatus(HotcueControl::Status::Active);
    EXPECT_EQ(1, changes);
    cue.setPosition(mixxx::audio::FramePos(8));
    EXPECT_EQ(2, changes);
    cue.setEndPosition(mixxx::audio::FramePos(4));
    EXPECT_EQ(3, changes);
}

TEST_F(ReadAheadManagerTest, StretchChunkBoundaryAndMissAreTransactional) {
    const SINT boundary = CachingReaderChunk::kFrames * 2;
    for (int i = 0; i < 4; ++i) {
        m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
        m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    }
    m_pReadAheadManager->notifySeek(boundary - 2);
    const auto prefix = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 1024, mixxx::audio::ChannelCount::stereo());
    EXPECT_FALSE(prefix.unavailable);
    EXPECT_EQ(2, prefix.samplesRead);
    EXPECT_EQ(boundary, m_pReadAheadManager->getPlaypos());
    EXPECT_EQ(boundary, m_pReadAheadManager->getFilePlaypositionFromLog(boundary - 2, 2));
    m_pReader->miss = true;
    const auto miss = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 1024, mixxx::audio::ChannelCount::stereo());
    EXPECT_TRUE(miss.unavailable);
    EXPECT_EQ(0, miss.samplesRead);
    EXPECT_EQ(boundary, m_pReadAheadManager->getPlaypos());
    m_pReader->miss = false;
    const auto retry = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 1024, mixxx::audio::ChannelCount::stereo());
    EXPECT_FALSE(retry.unavailable);
    EXPECT_EQ(1024, retry.samplesRead);
    EXPECT_EQ(boundary + 1024, m_pReadAheadManager->getFilePlaypositionFromLog(boundary, 1024));
}

TEST_F(ReadAheadManagerTest, WrapMetadataWaitsForDeliveredReadAndSeekDiscardsIt) {
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(8, 4);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    const auto read = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 16, mixxx::audio::ChannelCount::stereo());
    ASSERT_FALSE(read.unavailable);
    ASSERT_EQ(8, read.samplesRead);
    ASSERT_EQ(1u, readLogSize());
    EXPECT_TRUE(firstReadLogEntryHasWrap());

    m_pReadAheadManager->notifySeek(32);
    EXPECT_EQ(0u, readLogSize());
}

TEST_F(ReadAheadManagerTest, RealLoopEditMissDoesNotCommitQueryHistory) {
    using FramePos = mixxx::audio::FramePos;
    m_pLoopControl->useRealControl = true;
    m_pLoopControl->setLoop(FramePos(0), FramePos(8), true);
    ControlObject::set(ConfigKey(kGroup, "loop_enabled"), 1);
    m_pReadAheadManager->notifySeek(4);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    ASSERT_FALSE(m_pReadAheadManager->getNextSamplesForStretch(
                                            1, m_pBuffer, 2, mixxx::audio::ChannelCount::stereo())
                    .unavailable);
    ASSERT_EQ(FramePos(0), m_pLoopControl->readTriggerState().oldLoopInfo.startPosition);

    setChangedLoop(FramePos(4), FramePos(12), LoopingControl::LoopSeekMode::MovedOut);
    m_pReader->miss = true;
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    ASSERT_TRUE(m_pReadAheadManager->getNextSamplesForStretch(
                                           1, m_pBuffer, 2, mixxx::audio::ChannelCount::stereo())
                    .unavailable);
    // No input from the edited loop was accepted: preserve the old baseline.
    EXPECT_EQ(FramePos(0), m_pLoopControl->readTriggerState().oldLoopInfo.startPosition);

    setChangedLoop(FramePos(16), FramePos(24), LoopingControl::LoopSeekMode::Changed);
    m_pReader->miss = false;
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    const auto move = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 2, mixxx::audio::ChannelCount::stereo());
    EXPECT_FALSE(move.unavailable);
    EXPECT_EQ(0, move.samplesRead);
    // Original frame 3 moves by 16 frames; the failed four-frame edit
    // must not change its phase within the original eight-frame loop.
    EXPECT_DOUBLE_EQ(FramePos(19).toEngineSamplePos(), m_pReadAheadManager->getPlaypos());
    EXPECT_EQ(FramePos(16), m_pLoopControl->readTriggerState().oldLoopInfo.startPosition);
}

TEST_F(ReadAheadManagerTest, RealOneShotCueSurvivesSecondaryMissAndUnrelatedEdit) {
    m_pCueControl->useRealControl = true;
    hotcue(0)->setPosition(mixxx::audio::FramePos(2));
    hotcue(0)->setEndPosition(mixxx::audio::FramePos(4));
    hotcue(0)->setType(mixxx::CueType::Jump);
    hotcue(0)->setStatus(HotcueControl::Status::Active);
    m_pReadAheadManager->notifySeek(2);
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReader->failOnRead = 2;
    ASSERT_TRUE(m_pReadAheadManager->getNextSamplesForStretch(
                                           1, m_pBuffer, 8, mixxx::audio::ChannelCount::stereo())
                    .unavailable);
    EXPECT_EQ(HotcueControl::Status::Active, hotcue(0)->getStatus());
    EXPECT_DOUBLE_EQ(2.0, m_pReadAheadManager->getPlaypos());
    hotcue(1)->setPosition(mixxx::audio::FramePos(20));
    m_pReader->failOnRead = -1;
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    const auto retry = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 8, mixxx::audio::ChannelCount::stereo());
    EXPECT_FALSE(retry.unavailable);
    EXPECT_EQ(6, retry.samplesRead);
    EXPECT_DOUBLE_EQ(4.0, m_pReadAheadManager->getPlaypos());
    EXPECT_EQ(HotcueControl::Status::Set, hotcue(0)->getStatus());
}

TEST_F(ReadAheadManagerTest, CueCommitCannotDisarmAnotherMatchingCueAfterAnEdit) {
    using mixxx::audio::FramePos;
    for (int i = 0; i < 2; ++i) {
        hotcue(i)->setPosition(FramePos(2));
        hotcue(i)->setEndPosition(FramePos(4));
        hotcue(i)->setType(mixxx::CueType::Jump);
        hotcue(i)->setStatus(HotcueControl::Status::Active);
    }
    FramePos target;
    const auto trigger = m_pCueControl->CueControl::nextTrigger(
            false, FramePos(0), &target, 8, false);
    ASSERT_EQ(FramePos(4), trigger);
    ASSERT_EQ(FramePos(2), target);
    hotcue(0)->setEndPosition(FramePos(6));
    m_pCueControl->commitTrigger(trigger, target, false);
    EXPECT_EQ(HotcueControl::Status::Active, hotcue(0)->getStatus());
    EXPECT_EQ(HotcueControl::Status::Active, hotcue(1)->getStatus());
}

TEST_F(ReadAheadManagerTest, CueCommitConsumesFrozenDecisionOnceWithoutAllocation) {
    using mixxx::audio::FramePos;
    hotcue(0)->setPosition(FramePos(2));
    hotcue(0)->setEndPosition(FramePos(4));
    hotcue(0)->setType(mixxx::CueType::Jump);
    hotcue(0)->setStatus(HotcueControl::Status::Active);
    FramePos target;
    const auto trigger = m_pCueControl->CueControl::nextTrigger(
            false, FramePos(0), &target, 8, false);
    mixxxtest::callbackAllocations = 0;
    mixxxtest::callbackDeallocations = 0;
    const bool previousCounting = mixxxtest::countCallbackAllocations;
    mixxxtest::countCallbackAllocations = true;
    m_pCueControl->commitTrigger(trigger, target, false);
    mixxxtest::countCallbackAllocations = previousCounting;
    EXPECT_EQ(HotcueControl::Status::Set, hotcue(0)->getStatus());
    EXPECT_EQ(0u, mixxxtest::callbackAllocations);
    EXPECT_EQ(0u, mixxxtest::callbackDeallocations);
    hotcue(0)->setStatus(HotcueControl::Status::Active);
    m_pCueControl->commitTrigger(trigger, target, false);
    EXPECT_EQ(HotcueControl::Status::Active, hotcue(0)->getStatus());
}

TEST_F(ReadAheadManagerTest, CueQuantizationChangesInvalidatePendingPlans) {
    const auto beforeQuantize = m_pCueControl->CueControl::triggerRevision();
    ControlObject::set(ConfigKey(kGroup, "quantize"), 1);
    EXPECT_GT(m_pCueControl->CueControl::triggerRevision(), beforeQuantize);
    const auto beforeBeats = m_pCueControl->CueControl::triggerRevision();
    m_pCueControl->trackBeatsUpdated(nullptr);
    EXPECT_GT(m_pCueControl->CueControl::triggerRevision(), beforeBeats);
}

TEST_F(ReadAheadManagerTest, RetryableMissReusesStatefulTriggerDecisions) {
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(8, 4);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReader->miss = true;
    const auto miss = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 16, mixxx::audio::ChannelCount::stereo());
    ASSERT_TRUE(miss.unavailable);
    EXPECT_EQ(1, m_pLoopControl->queryCount);
    EXPECT_EQ(1, m_pCueControl->queryCount);

    m_pReader->miss = false;
    const auto retry = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 16, mixxx::audio::ChannelCount::stereo());
    EXPECT_FALSE(retry.unavailable);
    EXPECT_EQ(8, retry.samplesRead);
    EXPECT_EQ(1, m_pLoopControl->queryCount);
    EXPECT_EQ(1, m_pCueControl->queryCount);
}

TEST_F(ReadAheadManagerTest, ProviderRevisionChangeDuringQueryRejectsStaleDecision) {
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(4, 8);
    m_pCueControl->bumpRevisionOnQuery = true;
    m_pReader->miss = true;
    const auto edited = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 16, mixxx::audio::ChannelCount::stereo());
    ASSERT_TRUE(edited.unavailable);
    EXPECT_EQ(0, edited.samplesRead);
    EXPECT_EQ(0, m_pReader->readCount);
    EXPECT_DOUBLE_EQ(0.0, m_pReadAheadManager->getPlaypos());
    EXPECT_EQ(0u, readLogSize());
    for (int i = 0; i < 16; ++i) {
        EXPECT_FLOAT_EQ(0.0f, m_pBuffer[i]);
    }

    m_pReader->miss = false;
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(6, 10);
    const auto retry = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 16, mixxx::audio::ChannelCount::stereo());
    EXPECT_FALSE(retry.unavailable);
    EXPECT_EQ(6, retry.samplesRead);
    EXPECT_DOUBLE_EQ(10.0, m_pReadAheadManager->getPlaypos());
    EXPECT_EQ(2, m_pLoopControl->queryCount);
    EXPECT_EQ(2, m_pCueControl->queryCount);
}

TEST_F(ReadAheadManagerTest, LoopTopologyChangeInvalidatesPendingPlan) {
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(8, 4);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReader->miss = true;
    ASSERT_TRUE(m_pReadAheadManager->getNextSamplesForStretch(
                                           1, m_pBuffer, 16, mixxx::audio::ChannelCount::stereo())
                    .unavailable);

    m_pLoopControl->bumpTriggerRevision();
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReader->miss = false;
    const auto retry = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 16, mixxx::audio::ChannelCount::stereo());
    EXPECT_FALSE(retry.unavailable);
    EXPECT_EQ(16, retry.samplesRead);
    EXPECT_EQ(2, m_pLoopControl->queryCount);
    EXPECT_EQ(2, m_pCueControl->queryCount);
}

TEST_F(ReadAheadManagerTest, CueTopologyChangeInvalidatesPendingPlan) {
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(8, 4);
    m_pReader->miss = true;
    ASSERT_TRUE(m_pReadAheadManager->getNextSamplesForStretch(
                                           1, m_pBuffer, 16, mixxx::audio::ChannelCount::stereo())
                    .unavailable);

    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->bumpTriggerRevision();
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReader->miss = false;
    const auto retry = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 16, mixxx::audio::ChannelCount::stereo());
    EXPECT_FALSE(retry.unavailable);
    EXPECT_EQ(16, retry.samplesRead);
    EXPECT_EQ(2, m_pLoopControl->queryCount);
    EXPECT_EQ(2, m_pCueControl->queryCount);
}

TEST_F(ReadAheadManagerTest, ScalarReadInvalidatesPendingStretchTriggerPlan) {
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReader->miss = true;
    ASSERT_TRUE(m_pReadAheadManager->getNextSamplesForStretch(
                                           1, m_pBuffer, 10, mixxx::audio::ChannelCount::stereo())
                    .unavailable);

    m_pReader->miss = false;
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    EXPECT_EQ(10,
            m_pReadAheadManager->getNextSamples(
                    1, m_pBuffer, 10, mixxx::audio::ChannelCount::stereo()));
    EXPECT_EQ(2, m_pLoopControl->queryCount);
    EXPECT_EQ(2, m_pCueControl->queryCount);
}

TEST_F(ReadAheadManagerTest, RateControlNotifiedOnlyAfterPositiveWrappedConsumption) {
    m_pReadAheadManager->addRateControl(&m_rateControl);
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(8, 4);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    const auto read = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 16, mixxx::audio::ChannelCount::stereo());
    ASSERT_EQ(8, read.samplesRead);

    EXPECT_DOUBLE_EQ(0.0, m_pReadAheadManager->getFilePlaypositionFromLog(0, 0));
    EXPECT_EQ(0, rateControlWrapAroundCount());
    EXPECT_DOUBLE_EQ(1.0, m_pReadAheadManager->getFilePlaypositionFromLog(0, 1));
    EXPECT_EQ(0, rateControlWrapAroundCount());
    EXPECT_DOUBLE_EQ(8.0, m_pReadAheadManager->getFilePlaypositionFromLog(1, 7));
    EXPECT_EQ(0, rateControlWrapAroundCount());

    m_pLoopControl->pushValues(8, 4);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    ASSERT_EQ(2,
            m_pReadAheadManager
                    ->getNextSamplesForStretch(1,
                            m_pBuffer,
                            2,
                            mixxx::audio::ChannelCount::stereo())
                    .samplesRead);
    EXPECT_DOUBLE_EQ(5.0, m_pReadAheadManager->getFilePlaypositionFromLog(8, 1));
    EXPECT_EQ(1, rateControlWrapAroundCount());
}

TEST_F(ReadAheadManagerTest, MultipleWrapsRetireOnceWithoutCallbackAllocation) {
    m_pReadAheadManager->addRateControl(&m_rateControl);
    m_pReadAheadManager->notifySeek(0);
    for (int i = 0; i < 3; ++i) {
        m_pLoopControl->pushValues(8, 4);
        m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
        const SINT requested = i == 2 ? 2 : 16;
        ASSERT_FALSE(m_pReadAheadManager
                        ->getNextSamplesForStretch(1,
                                m_pBuffer,
                                requested,
                                mixxx::audio::ChannelCount::stereo())
                        .unavailable);
    }
    EXPECT_EQ(0, rateControlWrapAroundCount());
    mixxxtest::callbackAllocations = 0;
    mixxxtest::callbackDeallocations = 0;
    const bool previousCounting = mixxxtest::countCallbackAllocations;
    mixxxtest::countCallbackAllocations = true;
    const double atBoundary = m_pReadAheadManager->getFilePlaypositionFromLog(0, 12);
    const int firstWraps = rateControlWrapAroundCount();
    const double afterBoundary = m_pReadAheadManager->getFilePlaypositionFromLog(atBoundary, 2);
    mixxxtest::countCallbackAllocations = previousCounting;
    EXPECT_DOUBLE_EQ(8.0, atBoundary);
    EXPECT_EQ(1, firstWraps);
    EXPECT_DOUBLE_EQ(6.0, afterBoundary);
    EXPECT_EQ(2, rateControlWrapAroundCount());
    EXPECT_EQ(0u, mixxxtest::callbackAllocations);
    EXPECT_EQ(0u, mixxxtest::callbackDeallocations);
}

TEST_F(ReadAheadManagerTest, ReverseWrapWaitsForPostBoundaryConsumption) {
    m_pReadAheadManager->addRateControl(&m_rateControl);
    m_pReadAheadManager->notifySeek(12);
    m_pLoopControl->pushValues(4, 12);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    ASSERT_EQ(8,
            m_pReadAheadManager
                    ->getNextSamplesForStretch(-1,
                            m_pBuffer,
                            16,
                            mixxx::audio::ChannelCount::stereo())
                    .samplesRead);
    m_pLoopControl->pushValues(4, 12);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    ASSERT_EQ(2,
            m_pReadAheadManager
                    ->getNextSamplesForStretch(-1,
                            m_pBuffer,
                            2,
                            mixxx::audio::ChannelCount::stereo())
                    .samplesRead);
    EXPECT_EQ(0, rateControlWrapAroundCount());
    EXPECT_DOUBLE_EQ(11.0, m_pReadAheadManager->getFilePlaypositionFromLog(12, 1));
    EXPECT_EQ(0, rateControlWrapAroundCount());
    EXPECT_DOUBLE_EQ(4.0, m_pReadAheadManager->getFilePlaypositionFromLog(11, 7));
    EXPECT_EQ(0, rateControlWrapAroundCount());
    EXPECT_DOUBLE_EQ(11.0, m_pReadAheadManager->getFilePlaypositionFromLog(4, 1));
    EXPECT_EQ(1, rateControlWrapAroundCount());
}

TEST_F(ReadAheadManagerTest, InvalidTrackEndDoesNotOverflowReverseCrossfade) {
    m_pLoopControl->setFrameInfo(mixxx::audio::FramePos(0),
            mixxx::audio::kInvalidFramePos,
            mixxx::audio::SampleRate(44100));
    m_pReadAheadManager->notifySeek(12);
    m_pLoopControl->pushValues(4, 12);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    const auto read = m_pReadAheadManager->getNextSamplesForStretch(
            -1, m_pBuffer, 16, mixxx::audio::ChannelCount::stereo());
    EXPECT_FALSE(read.unavailable);
    ASSERT_EQ(8, read.samplesRead);
    EXPECT_EQ(1, m_pReader->readCount);
    for (SINT i = 0; i < read.samplesRead; ++i) {
        EXPECT_TRUE(std::isfinite(m_pBuffer[i]));
    }
}

TEST_F(ReadAheadManagerTest, RateControlRetainsWrapAcrossEmptyMappingUntilPositiveRead) {
    m_pReadAheadManager->addRateControl(&m_rateControl);
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(0, 4);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    ASSERT_EQ(0,
            m_pReadAheadManager
                    ->getNextSamplesForStretch(1,
                            m_pBuffer,
                            2,
                            mixxx::audio::ChannelCount::stereo())
                    .samplesRead);
    EXPECT_DOUBLE_EQ(0.0, m_pReadAheadManager->getFilePlaypositionFromLog(0, 0));
    EXPECT_EQ(0, rateControlWrapAroundCount());
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    const auto afterBoundary = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 2, mixxx::audio::ChannelCount::stereo());
    ASSERT_EQ(2, afterBoundary.samplesRead);
    EXPECT_DOUBLE_EQ(5.0, m_pReadAheadManager->getFilePlaypositionFromLog(0, 1));
    EXPECT_EQ(1, rateControlWrapAroundCount());
}

TEST_F(ReadAheadManagerTest, RateControlReceivesSelectedJumpTrigger) {
    m_pReadAheadManager->addRateControl(&m_rateControl);
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(8, 2);
    m_pCueControl->pushValues(6, 4);
    const auto read = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 16, mixxx::audio::ChannelCount::stereo());
    ASSERT_EQ(6, read.samplesRead);
    EXPECT_DOUBLE_EQ(6.0, m_pReadAheadManager->getFilePlaypositionFromLog(0, 6));
    EXPECT_EQ(0, rateControlWrapAroundCount());
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    ASSERT_EQ(2,
            m_pReadAheadManager
                    ->getNextSamplesForStretch(1,
                            m_pBuffer,
                            2,
                            mixxx::audio::ChannelCount::stereo())
                    .samplesRead);
    EXPECT_DOUBLE_EQ(5.0, m_pReadAheadManager->getFilePlaypositionFromLog(6, 1));
    EXPECT_EQ(1, rateControlWrapAroundCount());
    EXPECT_EQ(mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(6),
            rateControlJumpPosition());
    EXPECT_EQ(mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(4),
            rateControlTargetPosition());
}

TEST_F(ReadAheadManagerTest, StretchCrossfadeMissCanRetryWithoutCommittingRead) {
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(8, 4);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReader->failOnRead = 2; // Primary succeeds; crossfade read misses.

    const auto unavailable = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 16, mixxx::audio::ChannelCount::stereo());
    ASSERT_TRUE(unavailable.unavailable);
    EXPECT_EQ(0, unavailable.samplesRead);
    EXPECT_EQ(0, m_pReadAheadManager->getPlaypos());
    EXPECT_EQ(0u, readLogSize());

    m_pReader->failOnRead = -1;
    m_pReader->readCount = 0;
    const auto retry = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 16, mixxx::audio::ChannelCount::stereo());
    EXPECT_FALSE(retry.unavailable);
    EXPECT_EQ(8, retry.samplesRead);
    EXPECT_EQ(4, m_pReadAheadManager->getPlaypos());
    EXPECT_EQ(1, m_pLoopControl->queryCount);
    EXPECT_EQ(1, m_pCueControl->queryCount);

    // Re-anchor the same controls for the all-available reference; creating
    // another control owner in this group would violate ControlObject identity.
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(8, 4);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReader->readCount = 0;
    CSAMPLE referenceBuffer[16]{};
    const auto reference = m_pReadAheadManager->getNextSamplesForStretch(
            1, referenceBuffer, 16, mixxx::audio::ChannelCount::stereo());
    ASSERT_FALSE(reference.unavailable);
    ASSERT_EQ(retry.samplesRead, reference.samplesRead);
    for (SINT i = 0; i < retry.samplesRead; ++i) {
        EXPECT_FLOAT_EQ(referenceBuffer[i], m_pBuffer[i]);
    }
}

TEST_F(ReadAheadManagerTest, CrossfadeCacheMissRequestsMissingRangeBeforeRetry) {
    for (const int channels : {2, 8}) {
        for (const bool reverse : {false, true}) {
            SCOPED_TRACE(channels);
            SCOPED_TRACE(reverse);
            const auto channelCount = mixxx::audio::ChannelCount(channels);
            const SINT trigger = reverse
                    ? 10 * CachingReaderChunk::kFrames - 128
                    : 20 * CachingReaderChunk::kFrames + 128;
            const SINT target = (reverse ? 20 : 10) * CachingReaderChunk::kFrames;
            const SINT position = trigger + (reverse ? 64 : -64);
            const SINT prefix = target + (reverse ? 0 : -64);
            m_pLoopControl->setFrameInfo(mixxx::audio::FramePos(position),
                    mixxx::audio::FramePos(1000000),
                    mixxx::audio::SampleRate(44100));
            m_pReadAheadManager->notifySeek(position * channels);
            m_pLoopControl->pushValues(trigger * 2, target * 2);
            m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
            m_pReader->missingChunk = prefix / CachingReaderChunk::kFrames;

            const double rate = reverse ? -1 : 1;
            const auto missing = m_pReadAheadManager->getNextSamplesForStretch(
                    rate, m_pBuffer, 64 * channels, channelCount);
            ASSERT_TRUE(missing.unavailable);
            EXPECT_EQ(position * channels, m_pReadAheadManager->getPlaypos());

            HintVector hints;
            m_pReadAheadManager->hintReader(rate, &hints, channelCount);
            ASSERT_GE(hints.size(), 2);
            EXPECT_EQ(prefix, hints[0].frame);
            EXPECT_EQ(64, hints[0].frameCount);
            EXPECT_EQ(Hint::Type::CurrentPosition, hints[0].type);
            for (const auto& hint : hints) {
                if (hint.frame <= prefix && hint.frame + hint.frameCount >= prefix + 64) {
                    m_pReader->missingChunk.reset();
                }
            }
            const auto retry = m_pReadAheadManager->getNextSamplesForStretch(
                    rate, m_pBuffer, 64 * channels, channelCount);
            ASSERT_FALSE(retry.unavailable);
            EXPECT_EQ(64 * channels, retry.samplesRead);
            EXPECT_EQ(target * channels, m_pReadAheadManager->getPlaypos());
            hints.clear();
            m_pReadAheadManager->hintReader(rate, &hints, channelCount);
            EXPECT_EQ(1, hints.size());
        }
    }
}

TEST_F(ReadAheadManagerTest, FractionalWrapPreservesDirectionalOvershoot) {
    for (const int channels : {2, 8}) {
        for (const bool reverse : {false, true}) {
            const auto channelCount = mixxx::audio::ChannelCount(channels);
            const double position = reverse ? 12 : 8;
            m_pReadAheadManager->notifySeek(position * channels);
            m_pLoopControl->pushValues(10.3 * 2, 20.5 * 2);
            m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
            const double rate = reverse ? -1 : 1;
            const auto read = m_pReadAheadManager->getNextSamplesForStretch(
                    rate, m_pBuffer, 8 * channels, channelCount);
            EXPECT_FALSE(read.unavailable);
            EXPECT_EQ((reverse ? 2 : 3) * channels, read.samplesRead);
            EXPECT_NEAR((reverse ? 20.2 : 21.2) * channels,
                    m_pReadAheadManager->getPlaypos(),
                    1e-10);
            EXPECT_NEAR((reverse ? 10 : 11) * channels,
                    m_pReadAheadManager->getFilePlaypositionFromLog(
                            position * channels, read.samplesRead),
                    1e-10);
            m_pLoopControl->pushValues(10.3 * 2, 20.5 * 2);
            m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
            const auto next = m_pReadAheadManager->getNextSamplesForStretch(
                    rate, m_pBuffer, channels, channelCount);
            EXPECT_FALSE(next.unavailable);
            EXPECT_EQ(channels, next.samplesRead);
            EXPECT_NEAR((reverse ? 19.2 : 22.2) * channels,
                    m_pReadAheadManager->getFilePlaypositionFromLog(
                            (reverse ? 10 : 11) * channels, next.samplesRead),
                    1e-10);
        }
    }
}

TEST_F(ReadAheadManagerTest, PartialCacheMissDoesNotCommitStretchInput) {
    for (const int channels : {2, 8}) {
        for (const bool reverse : {false, true}) {
            for (const int partialRead : {1, 2}) {
                SCOPED_TRACE(channels);
                SCOPED_TRACE(reverse);
                SCOPED_TRACE(partialRead);
                const auto channelCount = mixxx::audio::ChannelCount(channels);
                const SINT trigger = reverse
                        ? 10 * CachingReaderChunk::kFrames - 128
                        : 20 * CachingReaderChunk::kFrames + 128;
                const SINT target = 15 * CachingReaderChunk::kFrames +
                        (reverse ? -32 : 32);
                const SINT position = trigger + (reverse ? 64 : -64);
                m_pLoopControl->setFrameInfo(mixxx::audio::FramePos(position),
                        mixxx::audio::FramePos(1000000),
                        mixxx::audio::SampleRate(44100));
                m_pReadAheadManager->notifySeek(position * channels);
                m_pLoopControl->pushValues(trigger * 2, target * 2);
                m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
                m_pReader->readCount = 0;
                m_pReader->partialOnRead = partialRead;
                const double rate = reverse ? -1 : 1;
                const auto missing = m_pReadAheadManager->getNextSamplesForStretch(
                        rate, m_pBuffer, 64 * channels, channelCount);
                ASSERT_TRUE(missing.unavailable);
                EXPECT_EQ(0, missing.samplesRead);
                EXPECT_EQ(position * channels, m_pReadAheadManager->getPlaypos());
                EXPECT_EQ(0u, readLogSize());
                for (SINT sample = 0; sample < 64 * channels; ++sample) {
                    EXPECT_EQ(0, m_pBuffer[sample]);
                }
                if (partialRead == 2) {
                    HintVector hints;
                    m_pReadAheadManager->hintReader(rate, &hints, channelCount);
                    ASSERT_GE(hints.size(), 2);
                    EXPECT_EQ(target + (reverse ? 0 : -64), hints[0].frame);
                    EXPECT_EQ(64, hints[0].frameCount);
                }
                m_pReader->partialOnRead = -1;
                const auto retry = m_pReadAheadManager->getNextSamplesForStretch(
                        rate, m_pBuffer, 64 * channels, channelCount);
                ASSERT_FALSE(retry.unavailable);
                EXPECT_EQ(64 * channels, retry.samplesRead);
                EXPECT_EQ(target * channels, m_pReadAheadManager->getPlaypos());
                EXPECT_EQ(1u, readLogSize());
            }
        }
    }
}

TEST_F(ReadAheadManagerTest, PartialEofPaddingRemainsSuccessfulStretchInput) {
    for (const int channels : {2, 8}) {
        for (const bool reverse : {false, true}) {
            const auto channelCount = mixxx::audio::ChannelCount(channels);
            m_pReadAheadManager->notifySeek(100 * channels);
            m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
            m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
            m_pReader->readCount = 0;
            m_pReader->partialOnRead = 1;
            m_pReader->partialCacheMiss = false;
            const auto read = m_pReadAheadManager->getNextSamplesForStretch(
                    reverse ? -1 : 1, m_pBuffer, 64 * channels, channelCount);
            EXPECT_FALSE(read.unavailable);
            EXPECT_EQ(64 * channels, read.samplesRead);
            EXPECT_EQ((reverse ? 36 : 164) * channels, m_pReadAheadManager->getPlaypos());
            m_pReader->partialOnRead = -1;
            m_pReader->partialCacheMiss = true;
        }
    }
}

TEST_F(ReadAheadManagerTest, ReaderDistinguishesPaddedCacheMissFromTrackPadding) {
    setReaderRange(0, 1000);
    for (const int channels : {2, 8}) {
        for (const bool reverse : {false, true}) {
            const auto channelCount = mixxx::audio::ChannelCount(channels);
            const auto missing = m_pReader->CachingReader::read(
                    (reverse ? 32 : -32) * channels,
                    64 * channels,
                    reverse,
                    m_pBuffer,
                    channelCount);
            EXPECT_EQ(CachingReader::ReadResult::PARTIALLY_UNAVAILABLE, missing);
            const auto padding = m_pReader->CachingReader::read(
                    (reverse ? -32 : 1000) * channels,
                    64 * channels,
                    reverse,
                    m_pBuffer,
                    channelCount);
            EXPECT_EQ(CachingReader::ReadResult::PARTIALLY_AVAILABLE, padding);
            for (SINT sample = 0; sample < 64 * channels; ++sample) {
                EXPECT_EQ(0, m_pBuffer[sample]);
            }
        }
    }
}

TEST_F(ReadAheadManagerTest, CrossfadeClipsToTrackAndReadsCorrectDirectionalSamples) {
    for (const int channels : {2, 8}) {
        for (const bool reverse : {false, true}) {
            for (const SINT trackFrames : {16, 1000}) {
                SCOPED_TRACE(channels);
                SCOPED_TRACE(reverse);
                SCOPED_TRACE(trackFrames);
                const auto channelCount = mixxx::audio::ChannelCount(channels);
                const SINT trigger = reverse ? 128 : 256;
                const SINT position = trigger + (reverse ? 64 : -64);
                const SINT target = reverse ? trackFrames - 8 : 8;
                m_pLoopControl->setFrameInfo(mixxx::audio::FramePos(position),
                        mixxx::audio::FramePos(trackFrames),
                        mixxx::audio::SampleRate(44100));
                m_pReadAheadManager->notifySeek(position * channels);
                m_pLoopControl->pushValues(trigger * 2, target * 2);
                m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
                m_pReader->readCount = 0;
                m_pReader->preserveSourceDirection = true;
                const auto read = m_pReadAheadManager->getNextSamplesForStretch(
                        reverse ? -1 : 1, m_pBuffer, 64 * channels, channelCount);
                ASSERT_FALSE(read.unavailable);
                ASSERT_EQ(64 * channels, read.samplesRead);
                EXPECT_EQ(2, m_pReader->readCount);
                EXPECT_EQ((reverse ? trackFrames : 0) * channels,
                        m_pReader->lastReadPosition);
                EXPECT_EQ(8 * channels, m_pReader->lastReadSampleCount);
                EXPECT_EQ(reverse, m_pReader->lastReadReverse);
                for (SINT channel = 0; channel < channels; ++channel) {
                    const SINT primarySample =
                            (reverse ? position - 64 : position + 63) * channels + channel;
                    const SINT secondarySample =
                            (reverse ? trackFrames - 8 : 7) * channels + channel;
                    const CSAMPLE expected = primarySample * (1.0f / 8) +
                            secondarySample * (7.0f / 8);
                    EXPECT_FLOAT_EQ(expected, m_pBuffer[63 * channels + channel]);
                }
                m_pReader->preserveSourceDirection = false;
            }
        }
    }
}

TEST_F(ReadAheadManagerTest, SeekCancelsPendingCrossfadeHint) {
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(8, 100);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReader->failOnRead = 2;
    ASSERT_TRUE(m_pReadAheadManager
                        ->getNextSamplesForStretch(1,
                                m_pBuffer,
                                16,
                                mixxx::audio::ChannelCount::stereo())
                        .unavailable);
    m_pReadAheadManager->notifySeek(200);
    HintVector hints;
    m_pReadAheadManager->hintReader(1, &hints, mixxx::audio::ChannelCount::stereo());
    ASSERT_EQ(1, hints.size());
    EXPECT_EQ(100, hints[0].frame);
}

TEST_F(ReadAheadManagerTest, ReadLogCapacityGuardsZeroLengthEntrySaturationAndRecovers) {
    fillReadLogWithZeroLengthEntries();
    ASSERT_EQ(readLogCapacity(), readLogSize());
    const auto saturated = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 2, mixxx::audio::ChannelCount::stereo());
    EXPECT_TRUE(saturated.unavailable);
    EXPECT_EQ(0, saturated.samplesRead);
    EXPECT_EQ(0, m_pLoopControl->queryCount);
    EXPECT_EQ(0, m_pCueControl->queryCount);

    EXPECT_EQ(static_cast<double>(readLogCapacity() - 1) * 2,
            m_pReadAheadManager->getFilePlaypositionFromLog(0, 1));
    EXPECT_EQ(0u, readLogSize());
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    const auto recovered = m_pReadAheadManager->getNextSamplesForStretch(
            1, m_pBuffer, 2, mixxx::audio::ChannelCount::stereo());
    EXPECT_FALSE(recovered.unavailable);
    EXPECT_EQ(2, recovered.samplesRead);
}

TEST_F(ReadAheadManagerTest, StretchReverseStopsAtCacheBoundary) {
    const SINT boundary = CachingReaderChunk::kFrames * 2;
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReadAheadManager->notifySeek(boundary + 2);
    const auto read = m_pReadAheadManager->getNextSamplesForStretch(
            -1, m_pBuffer, 1024, mixxx::audio::ChannelCount::stereo());
    EXPECT_FALSE(read.unavailable);
    EXPECT_EQ(2, read.samplesRead);
    EXPECT_EQ(boundary, m_pReadAheadManager->getPlaypos());
    EXPECT_EQ(boundary, m_pReadAheadManager->getFilePlaypositionFromLog(boundary + 2, 2));
}

TEST_F(ReadAheadManagerTest, SavedJump) {
    m_pReadAheadManager->notifySeek(0.5);

    for (int i = 0; i < 2; i++) {
        m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    }

    m_pCueControl->pushValues(20, 6);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);

    EXPECT_EQ(20,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 30, mixxx::audio::ChannelCount::stereo()));
    EXPECT_NEAR(6.5, m_pReadAheadManager->getPlaypos(), 1);
    EXPECT_EQ(80,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 80, mixxx::audio::ChannelCount::stereo()));

    EXPECT_NEAR(86.5, m_pReadAheadManager->getPlaypos(), 1);
}

TEST_F(ReadAheadManagerTest, TriggerOnJumpOrLoop) {
    m_pReadAheadManager->notifySeek(0);

    // The jump trigger is located before the loop end
    m_pLoopControl->pushValues(50, 10);
    m_pCueControl->pushValues(40, 20);

    EXPECT_EQ(40,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 100, mixxx::audio::ChannelCount::stereo()));
    EXPECT_NEAR(20, m_pReadAheadManager->getPlaypos(), 1);

    m_pReadAheadManager->notifySeek(0);

    // The jump trigger is located after the loop end
    m_pLoopControl->pushValues(50, 40);
    m_pCueControl->pushValues(60, 30);

    EXPECT_EQ(50,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 100, mixxx::audio::ChannelCount::stereo()));
    EXPECT_NEAR(40, m_pReadAheadManager->getPlaypos(), 1);
}

TEST_F(ReadAheadManagerTest, FractionalFrameLoop) {
    // If we are in reverse, a loop is enabled, and the current playposition
    // is before of the loop, we should seek to the out point of the loop.
    m_pReadAheadManager->notifySeek(0.5);
    // Trigger value means, the sample that triggers the loop (loop in) and the
    // sample we should seek to.
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, kNoTrigger);

    for (int i = 0; i < 6; i++) {
        m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    }

    // read from start to loop trigger, overshoot 0.3
    EXPECT_EQ(20,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 100, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(18,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 80, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(16,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 62, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(18,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 46, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(16,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 28, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(12,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 12, mixxx::audio::ChannelCount::stereo()));

    // start 0.5 to 20.2 = 19.7
    // loop 3.3 to 20.2 = 16.9
    // 100 - 19,7 - 4 * 16,9 = 12,7
    // 12.7 + 3.3 = 16

    // The rounding error must not exceed a half frame (one samples in stereo)
    EXPECT_NEAR(16, m_pReadAheadManager->getPlaypos(), 1);
}
