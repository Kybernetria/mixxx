#pragma once

#include <optional>

#include "audio/frame.h"
#include "track/track.h"

/// Operations over the live memory-cue subset of a Track's cue list.
class MemoryCues {
  public:
    // Frame units, independent of stereo/stem interleaving and sample rate.
    static constexpr double kCurrentPositionToleranceFrames = 0.5;
    static constexpr double kNearestToleranceSeconds = 1.0;

    static CuePointer create(Track& track,
            mixxx::audio::FramePos capturedPosition,
            mixxx::audio::FrameDiff_t quantizationFrames = 1);
    // Import does not quantize and merges basic/extended records at the same point.
    static CuePointer importPoint(Track& track,
            mixxx::audio::FramePos position,
            const QString& label,
            std::optional<mixxx::RgbColor> color);
    static CuePointer current(const Track& track, mixxx::audio::FramePos position);
    static CuePointer previous(const Track& track, mixxx::audio::FramePos position);
    static CuePointer next(const Track& track, mixxx::audio::FramePos position);
    static CuePointer nearest(const Track& track,
            mixxx::audio::FramePos position,
            mixxx::audio::SampleRate sampleRate);

    static int removeExact(Track& track, mixxx::audio::FramePos position);
    static int removeCurrent(Track& track, mixxx::audio::FramePos position);
    static int removeNearest(Track& track,
            mixxx::audio::FramePos position,
            mixxx::audio::SampleRate sampleRate);
    static int removePrevious(Track& track, mixxx::audio::FramePos position);
    static int removeNext(Track& track, mixxx::audio::FramePos position);
    static int removeAll(Track& track);

    static bool editLabel(Track& track, const CuePointer& cue, const QString& label);
    static bool editColor(Track& track, const CuePointer& cue, mixxx::RgbColor color);

  private:
    static QList<CuePointer> liveCues(const Track& track);
};
