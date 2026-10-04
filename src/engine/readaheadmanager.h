#pragma once

#include <cstdint>
#include <gsl/pointers>
#include <memory>

#include "audio/frame.h"
#include "engine/cachingreader/cachingreader.h"
#include "engine/controls/loopingcontrol.h"
#include "engine/stretchinputbounds.h"
#include "util/math.h"
#include "util/types.h"

class LoopingControl;
class CueControl;
class RateControl;

/// ReadAheadManager is a tool for keeping track of the engine's current position
/// in a file. In the case that the engine needs to read ahead of the current
/// play position then this will keep track of how many samples the engine has
/// consumed. The getNextSamples() method encapsulates the logic of determining
/// whether to take a loop or jump into a single method. Whenever the Engine
/// seeks or the current play position is invalidated somehow, the Engine must
/// call notifySeek to inform the ReadAheadManager to reset itself to the seek
/// point.
class ReadAheadManager {
  public:
    ReadAheadManager(); // Only for testing: ReadAheadManagerMock
    ReadAheadManager(CachingReader* reader,
            LoopingControl* pLoopingControl,
            CueControl* pCueControl);
    virtual ~ReadAheadManager();

    /// Call this method to fill buffer with requested_samples out of the
    /// lookahead buffer. Provide rate as dRate so that the manager knows the
    /// direction the audio is progressing in. Returns the total number of
    /// samples read into buffer. Note that it is very common that the total
    /// samples read is less than the requested number of samples.
    virtual SINT getNextSamples(double dRate,
            CSAMPLE* buffer,
            SINT requested_samples,
            mixxx::audio::ChannelCount channelCount);

    struct StretchReadResult {
        SINT samplesRead;
        bool unavailable;
    };

    /// Transactional, single-cache-chunk read for a stretcher. A cache miss
    /// does not advance the read cursor or discard a previously read prefix.
    /// EOF/preroll silence is successful input, not an unavailable cache chunk.
    virtual StretchReadResult getNextSamplesForStretch(double rate,
            CSAMPLE* buffer,
            SINT requestedSamples,
            mixxx::audio::ChannelCount channelCount);

    /// Used to add a new EngineControls that ReadAheadManager will use to decide
    /// which samples to return.
    void addLoopingControl();
    void addRateControl(RateControl* pRateControl);

    /// Get the current read-ahead position in samples.
    /// unused in Mixxx, but needed for testing
    virtual inline double getPlaypos() const {
        return m_currentPosition;
    }

    virtual void notifySeek(double seekPosition);

    /// hintReader allows the ReadAheadManager to provide hints to the reader to
    /// indicate that the given portion of a song is about to be read.
    virtual void hintReader(double dRate,
            gsl::not_null<HintVector*> pHintList,
            mixxx::audio::ChannelCount channelCount);

    /// Return the position in sample
    virtual double getFilePlaypositionFromLog(
            double currentFilePlayposition,
            double numConsumedSamples);
    /// Return the position in frame
    mixxx::audio::FramePos getFilePlaypositionFromLog(
            mixxx::audio::FramePos currentPosition,
            mixxx::audio::FrameDiff_t numConsumedFrames,
            mixxx::audio::ChannelCount channelCount);

  private:
    friend class ReadAheadManagerTest;

    SINT readNextSamples(double rate,
            CSAMPLE* buffer,
            SINT requestedSamples,
            mixxx::audio::ChannelCount channelCount,
            bool suspendOnMiss,
            bool* unavailable);

    /// An entry in the read log indicates the virtual playposition the read
    /// began at and the virtual playposition it ended at.
    struct PendingTriggerPlan {
        bool active{false};
        double position{0};
        double rate{0};
        SINT requestedSamples{0};
        mixxx::audio::ChannelCount channelCount{0};
        mixxx::audio::FramePos loopTrigger;
        mixxx::audio::FramePos loopTarget;
        mixxx::audio::FramePos jumpTrigger;
        mixxx::audio::FramePos jumpTarget;
        std::uint64_t loopRevision{0};
        std::uint64_t cueRevision{0};
        LoopingControl::ReadTriggerState nextLoopState;
    };

    struct ReadLogEntry {
        double virtualPlaypositionStart;
        double virtualPlaypositionEndNonInclusive;
        bool hasWrapAround = false;
        mixxx::audio::FramePos wrapTrigger;
        mixxx::audio::FramePos wrapTarget;

        ReadLogEntry() = default;

        ReadLogEntry(double virtualPlaypositionStart,
                     double virtualPlaypositionEndNonInclusive) {
            this->virtualPlaypositionStart = virtualPlaypositionStart;
            this->virtualPlaypositionEndNonInclusive =
                    virtualPlaypositionEndNonInclusive;
        }

        bool direction() const {
            // NOTE(rryan): We try to avoid 0-length ReadLogEntry's when
            // possible but they have happened in the past. We treat 0-length
            // ReadLogEntry's as forward reads because this prevents them from
            // being interpreted as a seek in the common case.
            return virtualPlaypositionStart <= virtualPlaypositionEndNonInclusive;
        }

        double length() const {
            return fabs(virtualPlaypositionEndNonInclusive -
                       virtualPlaypositionStart);
        }

        /// Moves the start position forward or backward (depending on
        /// direction()) by numSamples.
        /// Caller should check if length() is 0 after consumption in
        /// order to expire the ReadLogEntry.
        double advancePlayposition(double* pNumConsumedSamples) {
            double available = math_min(*pNumConsumedSamples, length());
            virtualPlaypositionStart += (direction() ? 1 : -1) * available;
            *pNumConsumedSamples -= available;
            return virtualPlaypositionStart;
        }

        bool merge(const ReadLogEntry& other) {
            if (hasWrapAround || other.hasWrapAround) {
                return false;
            }
            // Allow 0-length ReadLogEntry's to merge regardless of their
            // direction if they have the right start point.
            if ((other.length() == 0 || direction() == other.direction()) &&
                virtualPlaypositionEndNonInclusive == other.virtualPlaypositionStart) {
                virtualPlaypositionEndNonInclusive =
                        other.virtualPlaypositionEndNonInclusive;
                return true;
            }
            return false;
        }
    };

    /// virtualPlaypositionEnd is the first sample in the direction that was
    /// read that was NOT read as part of this log entry.
    void addReadLogEntry(double virtualPlaypositionStart,
            double virtualPlaypositionEndNonInclusive,
            bool hasWrapAround = false,
            mixxx::audio::FramePos wrapTrigger = {},
            mixxx::audio::FramePos wrapTarget = {});

    LoopingControl* m_pLoopingControl;
    CueControl* m_pCueControl;
    RateControl* m_pRateControl;
    // Preserve the bounded storage policy: two maximum input batches plus two
    // endpoint entries, allocated at construction. Positive-length entries
    // represent at least one source frame; fractional consumption can retain a
    // partial leading entry. Zero-length loop transitions add entries without
    // input progress and can accumulate across refills. Caller-side consumption
    // happens after scaleBuffer returns, so this sizing does not prove that every
    // reachable sequence fits. Saturation refuses transactional reads until log
    // consumption or a seek reset frees space; it does not guarantee progress.
    static constexpr std::size_t kReadLogCapacity =
            std::size_t{2} * mixxx::engine::stretch::kMaxInputFrames + 2;
    std::unique_ptr<ReadLogEntry[]> m_readAheadLog{
            std::make_unique<ReadLogEntry[]>(kReadLogCapacity)};
    std::size_t m_readLogStart = 0;
    std::size_t m_readLogSize = 0;
    PendingTriggerPlan m_pendingTriggerPlan;
    Hint m_pendingReadHint{};
    bool m_hasPendingReadHint{false};
    double m_currentPosition; // In absolute samples
    CachingReader* m_pReader;
    CSAMPLE* m_pCrossFadeBuffer;
    int m_cacheMissCount;
    bool m_cacheMissExpected;
};
