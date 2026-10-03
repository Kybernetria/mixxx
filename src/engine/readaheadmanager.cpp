#include "engine/readaheadmanager.h"

#include "audio/frame.h"
#include "engine/cachingreader/cachingreader.h"
#include "engine/controls/cuecontrol.h"
#include "engine/controls/loopingcontrol.h"
#include "engine/controls/ratecontrol.h"
#include "util/defs.h"
#include "util/sample.h"

ReadAheadManager::ReadAheadManager()
        : m_pLoopingControl(nullptr),
          m_pCueControl(nullptr),
          m_pRateControl(nullptr),
          m_currentPosition(0),
          m_pReader(nullptr),
          m_pCrossFadeBuffer(SampleUtil::alloc(MAX_BUFFER_LEN)),
          m_cacheMissCount(0),
          m_cacheMissExpected(false) {
    // For testing only: ReadAheadManagerMock
}

ReadAheadManager::ReadAheadManager(CachingReader* pReader,
        LoopingControl* pLoopingControl,
        CueControl* pCueControl)
        : m_pLoopingControl(pLoopingControl),
          m_pCueControl(pCueControl),
          m_pRateControl(nullptr),
          m_currentPosition(0),
          m_pReader(pReader),
          m_pCrossFadeBuffer(SampleUtil::alloc(MAX_BUFFER_LEN)),
          m_cacheMissCount(0),
          m_cacheMissExpected(false) {
    DEBUG_ASSERT(m_pLoopingControl != nullptr);
    DEBUG_ASSERT(m_pCueControl != nullptr);
    DEBUG_ASSERT(m_pReader != nullptr);
}

ReadAheadManager::~ReadAheadManager() {
    SampleUtil::free(m_pCrossFadeBuffer);
}

SINT ReadAheadManager::getNextSamples(double dRate,
        CSAMPLE* pOutput,
        SINT requested_samples,
        mixxx::audio::ChannelCount channelCount) {
    bool unavailable = false;
    return readNextSamples(dRate, pOutput, requested_samples, channelCount, false, &unavailable);
}

ReadAheadManager::StretchReadResult ReadAheadManager::getNextSamplesForStretch(
        double rate,
        CSAMPLE* buffer,
        SINT requestedSamples,
        mixxx::audio::ChannelCount channelCount) {
    if (requestedSamples <= 0 || channelCount <= 0 ||
            requestedSamples % channelCount != 0 || m_readLogSize == kReadLogCapacity) {
        return {0, true};
    }
    const SINT chunkSamples = CachingReaderChunk::kFrames * channelCount;
    const SINT start = SampleUtil::roundPlayPosToFrameStart(m_currentPosition, channelCount);
    const SINT offset = ((start % chunkSamples) + chunkSamples) % chunkSamples;
    const SINT untilBoundary = rate < 0
            ? (offset == 0 ? chunkSamples : offset)
            : chunkSamples - offset;
    bool unavailable = false;
    const SINT read = readNextSamples(rate,
            buffer,
            std::min(requestedSamples, untilBoundary),
            channelCount,
            true,
            &unavailable);
    return {read, unavailable};
}

SINT ReadAheadManager::readNextSamples(double dRate,
        CSAMPLE* pOutput,
        SINT requested_samples,
        mixxx::audio::ChannelCount channelCount,
        bool suspendOnMiss,
        bool* unavailable) {
    *unavailable = false;

    // Refuse before querying stateful controls: a full bounded log cannot
    // commit this read, so trigger decisions must remain available for retry.
    if (m_readLogSize == kReadLogCapacity) {
        SampleUtil::clear(pOutput, requested_samples);
        if (suspendOnMiss) {
            *unavailable = true;
        }
        return 0;
    }

    int modSamples = requested_samples % channelCount;
    if (modSamples != 0) {
        qDebug() << "ERROR: Non-aligned requested_samples to ReadAheadManager::getNextSamples";
        requested_samples -= modSamples;
    }
    bool in_reverse = dRate < 0;

    const bool reusePendingPlan = suspendOnMiss && m_pendingTriggerPlan.active &&
            m_pendingTriggerPlan.position == m_currentPosition &&
            m_pendingTriggerPlan.rate == dRate &&
            m_pendingTriggerPlan.requestedSamples == requested_samples &&
            m_pendingTriggerPlan.channelCount == channelCount &&
            m_pendingTriggerPlan.loopRevision == m_pLoopingControl->triggerRevision() &&
            m_pendingTriggerPlan.cueRevision == m_pCueControl->triggerRevision();
    mixxx::audio::FramePos loopTargetPosition;
    mixxx::audio::FramePos jumpTargetPosition;
    mixxx::audio::FramePos loopTriggerPosition;
    mixxx::audio::FramePos jumpTriggerPosition;
    std::uint64_t loopRevision = m_pLoopingControl->triggerRevision();
    std::uint64_t cueRevision = m_pCueControl->triggerRevision();
    auto nextLoopState = m_pLoopingControl->readTriggerState();
    if (reusePendingPlan) {
        loopRevision = m_pendingTriggerPlan.loopRevision;
        cueRevision = m_pendingTriggerPlan.cueRevision;
        loopTriggerPosition = m_pendingTriggerPlan.loopTrigger;
        loopTargetPosition = m_pendingTriggerPlan.loopTarget;
        jumpTriggerPosition = m_pendingTriggerPlan.jumpTrigger;
        jumpTargetPosition = m_pendingTriggerPlan.jumpTarget;
        nextLoopState = m_pendingTriggerPlan.nextLoopState;
    } else {
        m_pendingTriggerPlan.active = false;
        const auto loopRevisionBeforeQuery = loopRevision;
        const auto cueRevisionBeforeQuery = cueRevision;
        loopTriggerPosition = m_pLoopingControl->nextTrigger(in_reverse,
                mixxx::audio::FramePos::fromSamplePosMaybeInvalid(
                        m_currentPosition, channelCount),
                &loopTargetPosition,
                &nextLoopState);
        loopRevision = m_pLoopingControl->triggerRevision();
        jumpTriggerPosition = m_pCueControl->nextTrigger(in_reverse,
                mixxx::audio::FramePos::fromSamplePosMaybeInvalid(
                        m_currentPosition, channelCount),
                &jumpTargetPosition,
                static_cast<mixxx::audio::FrameDiff_t>(requested_samples / channelCount),
                false);
        cueRevision = m_pCueControl->triggerRevision();
        if (loopRevision != loopRevisionBeforeQuery ||
                cueRevision != cueRevisionBeforeQuery ||
                loopRevision != m_pLoopingControl->triggerRevision() ||
                cueRevision != m_pCueControl->triggerRevision()) {
            // Never tag decisions gathered across a provider edit with its
            // newer revision. Retry a coherent query without reading input or
            // committing speculative loop history or cue state.
            SampleUtil::clear(pOutput, requested_samples);
            *unavailable = suspendOnMiss;
            return 0;
        }
    }
    mixxx::audio::FramePos targetPosition = loopTargetPosition;
    // A loop (beat loop or track on repeat) will only limit the amount we
    // can read in one shot.
    const double loop_trigger = loopTriggerPosition.toSamplePosMaybeInvalid(channelCount);
    double target = targetPosition.toSamplePosMaybeInvalid(channelCount);

    SINT preseek_samples = 0;
    double samplesToSeekTrigger = 0.0;

    bool reachedTrigger = false;
    bool jumpSelected = false;

    // By default, we are reading as many sampler as requested
    SINT samples_from_reader = requested_samples;
    if (loop_trigger != kNoTrigger) {
        samplesToSeekTrigger = in_reverse ? m_currentPosition - loop_trigger
                                          : loop_trigger - m_currentPosition;
        if (samplesToSeekTrigger >= 0.0) {
            // We can only read whole frames from the reader.
            // Use ceil here, to be sure to reach the loop trigger.
            preseek_samples = SampleUtil::ceilPlayPosToFrameStart(
                    samplesToSeekTrigger, channelCount);
            // clamp requested samples from the caller to the loop trigger point
            if (preseek_samples <= requested_samples) {
                reachedTrigger = true;
                samples_from_reader = preseek_samples;
            }
        }
    }

    // A saved jump cue will only limit the amount we can read in one shot.
    double jump_trigger = jumpTriggerPosition.toSamplePosMaybeInvalid(channelCount);

    // If there is both a loop and saved jump that are armed, and they both
    // cancel each other (Loop from A -> B, jump from A -> B), we no-op the jump
    // to prevent an infinite silent play loop
    if (jump_trigger != kNoTrigger && loop_trigger != kNoTrigger &&
            jumpTriggerPosition == targetPosition &&
            loopTriggerPosition == jumpTargetPosition) {
        jump_trigger = kNoTrigger;
    }

    SINT prejump_samples = 0;
    double samplesToJumpTrigger = 0.0;

    if (jump_trigger != kNoTrigger) {
        samplesToJumpTrigger = in_reverse ? m_currentPosition - jump_trigger
                                          : jump_trigger - m_currentPosition;
        if (samplesToJumpTrigger >= 0.0) {
            // We can only read whole frames from the reader.
            // Use ceil here, to be sure to reach the jump trigger.
            prejump_samples = SampleUtil::ceilPlayPosToFrameStart(
                    samplesToJumpTrigger, channelCount);
            // clamp requested samples from the caller to the jump trigger point
            if (prejump_samples <= requested_samples) {
                reachedTrigger = true;
                // A loop end may be before the jump. If the jump is first, this
                // should be our new target
                if (loop_trigger == kNoTrigger || prejump_samples < preseek_samples) {
                    samples_from_reader = prejump_samples;
                    preseek_samples = prejump_samples;
                    samplesToSeekTrigger = samplesToJumpTrigger;
                    target = jumpTargetPosition.toSamplePosMaybeInvalid(channelCount);
                    targetPosition = jumpTargetPosition;
                    jumpSelected = true;
                }
            }
        }
    }

    // Sanity checks.
    VERIFY_OR_DEBUG_ASSERT(samples_from_reader >= 0) {
        qDebug() << "Need negative samples in ReadAheadManager::getNextSamples. Ignoring read";
        return 0;
    }

    SINT start_sample = SampleUtil::roundPlayPosToFrameStart(
            m_currentPosition, channelCount);

    const double originalPosition = m_currentPosition;
    const std::size_t originalLogStart = m_readLogStart;
    const std::size_t originalLogSize = m_readLogSize;
    const int originalCacheMissCount = m_cacheMissCount;
    const bool originalCacheMissExpected = m_cacheMissExpected;
    ReadLogEntry originalLastEntry;
    const bool hadLastEntry = m_readLogSize > 0;
    if (hadLastEntry) {
        originalLastEntry = m_readAheadLog[(m_readLogStart + m_readLogSize - 1) %
                kReadLogCapacity];
    }

    const auto readResult = m_pReader->read(
            start_sample, samples_from_reader, in_reverse, pOutput, channelCount);
    if (readResult == CachingReader::ReadResult::UNAVAILABLE) {
        // Cache miss - no samples written
        SampleUtil::clear(pOutput, samples_from_reader);
        // Set the cache miss flag to decide when to apply ramping
        // after the following read attempts.
        m_cacheMissCount++;
        if (suspendOnMiss) {
            m_pendingTriggerPlan = {true,
                    m_currentPosition,
                    dRate,
                    requested_samples,
                    channelCount,
                    loopTriggerPosition,
                    loopTargetPosition,
                    jumpTriggerPosition,
                    jumpTargetPosition,
                    loopRevision,
                    cueRevision,
                    nextLoopState};
            *unavailable = true;
            return 0;
        }
    } else if (m_cacheMissCount > 0) {
        // Previous read was a cache miss, but now we got something back.
        // Apply ramping gain, because the last buffer has unwanted silence
        // and new samples without fading are causing a pop.
        SampleUtil::applyRampingGain(pOutput,
                CSAMPLE_GAIN_ZERO,
                CSAMPLE_GAIN_ONE,
                samples_from_reader);
        // Reset the cache miss flag, because we are now back on track.
        if (!m_cacheMissExpected && !suspendOnMiss) {
            qDebug() << "ReadAheadManager: continue after number cache misses:" << m_cacheMissCount;
        }
        m_cacheMissCount = 0;
        m_cacheMissExpected = false;
    }

    m_pendingTriggerPlan.active = false;

    // Increment or decrement current read-ahead position
    // Mixing int and double here is desired, because the fractional frame should
    // be resist
    const auto selectedTriggerPosition = jumpSelected
            ? jumpTriggerPosition
            : loopTriggerPosition;
    if (in_reverse) {
        addReadLogEntry(m_currentPosition,
                m_currentPosition - samples_from_reader,
                reachedTrigger,
                selectedTriggerPosition,
                targetPosition);
        m_currentPosition -= samples_from_reader;
    } else {
        addReadLogEntry(m_currentPosition,
                m_currentPosition + samples_from_reader,
                reachedTrigger,
                selectedTriggerPosition,
                targetPosition);
        m_currentPosition += samples_from_reader;
    }

    // Activate on this trigger if necessary
    if (reachedTrigger) {
        DEBUG_ASSERT(target != kNoTrigger);
        // TODO probably also useful for hotcue_X_indicator in CueControl::updateIndicators()

        // Jump to other end of loop or track.
        m_currentPosition = target;
        if (preseek_samples > 0) {
            // we are up to one frame ahead of the loop trigger
            double overshoot = preseek_samples - samplesToSeekTrigger;
            // start the loop later accordingly to be sure the loop length is as desired
            // e.g. exactly one bar.
            m_currentPosition += overshoot;

            // Example in frames;
            // loop start 1.1 loop end 3.3 loop length 2.2
            // m_currentPosition samplesToLoopTrigger preloop_samples
            // 2.0               1.3                  2
            // 1.8               1.5                  2
            // 1.6               1.7                  2
            // 1.4               1.9                  2
            // 1.2               2.1                  3
            // Average preloop_samples = 2.2
        }

        // start reading before the loop start point or the saved jump, to crossfade these samples
        // with the samples we need to the loop end
        const SINT seek_read_position = SampleUtil::roundPlayPosToFrameStart(
                m_currentPosition +
                        (in_reverse ? preseek_samples : -preseek_samples),
                channelCount);

        SINT crossFadeStart = 0;
        SINT crossFadeSamples = samples_from_reader;
        if (seek_read_position < 0) {
            // Pre-roll cannot contribute more than the primary read's span.
            crossFadeStart = std::min(samples_from_reader, -seek_read_position);
            crossFadeSamples -= crossFadeStart;
        } else {
            const auto trackEnd = m_pLoopingControl->getTrackFrame();
            if (!trackEnd.isValid()) {
                // Missing metadata must not become an overflowing buffer size.
                crossFadeSamples = 0;
            } else {
                const double trackSamples = trackEnd.toSamplePos(channelCount);
                if (seek_read_position > trackSamples) {
                    // Reverse post-roll is bounded by the primary read too.
                    crossFadeStart = static_cast<SINT>(std::min<double>(
                            samples_from_reader, seek_read_position - trackSamples));
                    crossFadeSamples -= crossFadeStart;
                }
            }
        }

        if (crossFadeSamples > 0) {
            const auto readResult = m_pReader->read(seek_read_position +
                            (in_reverse ? crossFadeStart : -crossFadeStart),
                    crossFadeSamples,
                    in_reverse,
                    m_pCrossFadeBuffer,
                    channelCount);
            if (readResult == CachingReader::ReadResult::UNAVAILABLE) {
                if (suspendOnMiss) {
                    m_currentPosition = originalPosition;
                    m_readLogStart = originalLogStart;
                    m_readLogSize = originalLogSize;
                    if (hadLastEntry) {
                        m_readAheadLog[(m_readLogStart + m_readLogSize - 1) %
                                kReadLogCapacity] = originalLastEntry;
                    }
                    m_cacheMissCount = originalCacheMissCount;
                    m_cacheMissExpected = originalCacheMissExpected;
                    m_pendingTriggerPlan = {true,
                            originalPosition,
                            dRate,
                            requested_samples,
                            channelCount,
                            loopTriggerPosition,
                            loopTargetPosition,
                            jumpTriggerPosition,
                            jumpTargetPosition,
                            loopRevision,
                            cueRevision,
                            nextLoopState};
                    SampleUtil::clear(pOutput, requested_samples);
                    *unavailable = true;
                    return 0;
                }
                if (!suspendOnMiss) {
                    qDebug() << "ERROR: Couldn't get all needed samples for crossfade.";
                }
                // Cache miss - no samples written
                SampleUtil::clear(m_pCrossFadeBuffer, samples_from_reader);
                // Set the cache miss flag to decide when to apply ramping
                // after the following read attempts.
                m_cacheMissCount++;
            }

            // do crossfade from the current buffer into the new loop beginning
            if (samples_from_reader != 0) { // avoid division by zero
                SampleUtil::linearCrossfadeBuffersOut(
                        pOutput + SampleUtil::ceilPlayPosToFrameStart(crossFadeStart, channelCount),
                        m_pCrossFadeBuffer,
                        crossFadeSamples,
                        channelCount);
            }
        } else {
            // No samples for crossfading, ramp to zero
            SampleUtil::applyRampingGain(pOutput,
                    CSAMPLE_GAIN_ONE,
                    CSAMPLE_GAIN_ZERO,
                    samples_from_reader);
        }
    }

    m_pLoopingControl->commitReadTriggerState(nextLoopState);
    // The cue query above is speculative: only disarm a one-shot after the
    // selected jump and its complete read transaction have been accepted.
    if (reachedTrigger && jumpSelected &&
            readResult == CachingReader::ReadResult::AVAILABLE) {
        m_pCueControl->commitTrigger(jumpTriggerPosition,
                jumpTargetPosition,
                in_reverse);
    }

    // qDebug() << "read" << m_currentPosition << samples_from_reader;
    return samples_from_reader;
}

void ReadAheadManager::addRateControl(RateControl* pRateControl) {
    m_pRateControl = pRateControl;
}

// Not thread-save, call from engine thread only
void ReadAheadManager::notifySeek(double seekPosition) {
    m_currentPosition = seekPosition;
    m_cacheMissCount = 0;
    m_cacheMissExpected = true;
    m_readLogStart = 0;
    m_readLogSize = 0;
    m_pendingTriggerPlan.active = false;
}

void ReadAheadManager::hintReader(double dRate,
        gsl::not_null<HintVector*> pHintList,
        mixxx::audio::ChannelCount channelCount) {
    bool in_reverse = dRate < 0;
    Hint current_position;

    // Always keep 2 chunks ahead in cache for stretch processing.
    SINT frameCountToCache = 2 * CachingReaderChunk::kFrames;
    current_position.frameCount = frameCountToCache;

    // this called after the precious chunk was consumed
    if (in_reverse) {
        current_position.frame =
                static_cast<SINT>(ceil(m_currentPosition / channelCount)) -
                frameCountToCache;
    } else {
        current_position.frame =
                static_cast<SINT>(floor(m_currentPosition / channelCount));
    }

    // If we are trying to cache before the start of the track,
    // Then we don't need to cache because it's all zeros!
    if (current_position.frame < 0 &&
            current_position.frame + current_position.frameCount < 0)
    {
    	return;
    }

    // top priority, we need to read this data immediately
    current_position.type = Hint::Type::CurrentPosition;
    pHintList->append(current_position);
}

// Not thread-save, call from engine thread only
void ReadAheadManager::addReadLogEntry(double virtualPlaypositionStart,
        double virtualPlaypositionEndNonInclusive,
        bool hasWrapAround,
        mixxx::audio::FramePos wrapTrigger,
        mixxx::audio::FramePos wrapTarget) {
    ReadLogEntry newEntry(virtualPlaypositionStart,
                          virtualPlaypositionEndNonInclusive);
    newEntry.hasWrapAround = hasWrapAround;
    newEntry.wrapTrigger = wrapTrigger;
    newEntry.wrapTarget = wrapTarget;
    if (m_readLogSize > 0) {
        ReadLogEntry& last =
                m_readAheadLog[(m_readLogStart + m_readLogSize - 1) %
                        kReadLogCapacity];
        if (last.merge(newEntry)) {
            return;
        }
    }
    VERIFY_OR_DEBUG_ASSERT(m_readLogSize < kReadLogCapacity) {
        return;
    }
    m_readAheadLog[(m_readLogStart + m_readLogSize) % kReadLogCapacity] = newEntry;
    ++m_readLogSize;
}

// Not thread-save, call from engine thread only
double ReadAheadManager::getFilePlaypositionFromLog(
        double currentFilePlayposition,
        double numConsumedSamples) {
    if (numConsumedSamples == 0) {
        return currentFilePlayposition;
    }

    if (m_readLogSize == 0) {
        // No log entries to read from.
        qDebug() << this << "No read ahead log entries to read from. Case not currently handled.";
        // TODO(rryan) log through a stats pipe eventually
        return currentFilePlayposition;
    }

    double filePlayposition = 0;
    while (m_readLogSize > 0 && numConsumedSamples > 0) {
        ReadLogEntry& entry = m_readAheadLog[m_readLogStart];
        // Advance our idea of the current virtual playposition to this
        // ReadLogEntry's start position.
        filePlayposition = entry.advancePlayposition(&numConsumedSamples);

        if (entry.length() == 0) {
            // Reaching the exclusive endpoint is not a wrap yet. Keep this
            // marker until positive consumption enters the post-wrap region.
            if (entry.hasWrapAround && numConsumedSamples == 0) {
                break;
            }
            if (entry.hasWrapAround && m_pRateControl) {
                m_pRateControl->notifyWrapAround(entry.wrapTrigger, entry.wrapTarget);
            }
            m_readLogStart = (m_readLogStart + 1) % kReadLogCapacity;
            --m_readLogSize;
        }
    }

    return filePlayposition;
}

mixxx::audio::FramePos ReadAheadManager::getFilePlaypositionFromLog(
        mixxx::audio::FramePos currentPosition,
        mixxx::audio::FrameDiff_t numConsumedFrames,
        mixxx::audio::ChannelCount channelCount) {
    const double positionSamples =
            getFilePlaypositionFromLog(currentPosition.toSamplePos(channelCount),
                    numConsumedFrames * channelCount);
    return mixxx::audio::FramePos::fromSamplePos(positionSamples, channelCount);
}
