#pragma once

#include <gsl/pointers>
#include <memory>

#include "audio/frame.h"
#include "engine/cachingreader/cachingreader.h"
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
    SINT readNextSamples(double rate,
            CSAMPLE* buffer,
            SINT requestedSamples,
            mixxx::audio::ChannelCount channelCount,
            bool suspendOnMiss,
            bool* unavailable);

    /// An entry in the read log indicates the virtual playposition the read
    /// began at and the virtual playposition it ended at.
    struct ReadLogEntry {
        double virtualPlaypositionStart;
        double virtualPlaypositionEndNonInclusive;

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
                         double virtualPlaypositionEndNonInclusive);

    LoopingControl* m_pLoopingControl;
    CueControl* m_pCueControl;
    RateControl* m_pRateControl;
    // Enough entries for two maximum-sized outstanding input batches even
    // when every source frame crosses a loop boundary. Storage is allocated
    // during reader construction, never by read/log consumption in a callback.
    static constexpr std::size_t kReadLogCapacity = 320002;
    std::unique_ptr<ReadLogEntry[]> m_readAheadLog{
            std::make_unique<ReadLogEntry[]>(kReadLogCapacity)};
    std::size_t m_readLogStart = 0;
    std::size_t m_readLogSize = 0;
    double m_currentPosition; // In absolute samples
    CachingReader* m_pReader;
    CSAMPLE* m_pCrossFadeBuffer;
    int m_cacheMissCount;
    bool m_cacheMissExpected;
};
