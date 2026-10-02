#include "track/memorycues.h"

#include <algorithm>
#include <cmath>

#include "util/assert.h"

QList<CuePointer> MemoryCues::liveCues(const Track& track) {
    QList<CuePointer> result;
    for (const auto& cue : track.getCuePoints()) {
        if (cue && cue->getType() == mixxx::CueType::Memory && cue->getPosition().isValid()) {
            result.push_back(cue);
        }
    }
    return result;
}

CuePointer MemoryCues::create(Track& track,
        mixxx::audio::FramePos capturedPosition,
        mixxx::audio::FrameDiff_t quantizationFrames) {
    VERIFY_OR_DEBUG_ASSERT(capturedPosition.isValid() && quantizationFrames > 0) {
        return {};
    }
    const auto frame = capturedPosition.value();
    const auto quantum = static_cast<double>(quantizationFrames);
    const auto snapped = mixxx::audio::FramePos(std::round(frame / quantum) * quantum);
    return track.createOrFindMemoryCue(snapped, kCurrentPositionToleranceFrames);
}

CuePointer MemoryCues::importPoint(Track& track,
        mixxx::audio::FramePos position,
        const QString& label,
        std::optional<mixxx::RgbColor> color) {
    const auto cue = track.createOrFindMemoryCue(position, kCurrentPositionToleranceFrames);
    // Missing basic-record metadata must not overwrite extended-record metadata.
    track.updateMemoryCue(cue, label.isEmpty() ? std::nullopt : std::make_optional(label), color);
    return cue;
}

CuePointer MemoryCues::current(const Track& track, mixxx::audio::FramePos position) {
    if (!position.isValid())
        return {};
    CuePointer best;
    double distance = kCurrentPositionToleranceFrames;
    for (const auto& cue : liveCues(track)) {
        const auto delta = std::abs(cue->getPosition() - position);
        if (delta <= kCurrentPositionToleranceFrames &&
                (delta < distance ||
                        (delta == distance &&
                                (!best || cue->getPosition() < best->getPosition())))) {
            best = cue;
            distance = delta;
        }
    }
    return best;
}

CuePointer MemoryCues::previous(const Track& track, mixxx::audio::FramePos position) {
    if (!position.isValid())
        return {};
    CuePointer best;
    for (const auto& cue : liveCues(track)) {
        if (cue->getPosition().value() < position.value() - kCurrentPositionToleranceFrames &&
                (!best || cue->getPosition() > best->getPosition()))
            best = cue;
    }
    return best;
}

CuePointer MemoryCues::next(const Track& track, mixxx::audio::FramePos position) {
    if (!position.isValid())
        return {};
    CuePointer best;
    for (const auto& cue : liveCues(track)) {
        if (cue->getPosition().value() > position.value() + kCurrentPositionToleranceFrames &&
                (!best || cue->getPosition() < best->getPosition()))
            best = cue;
    }
    return best;
}

CuePointer MemoryCues::nearest(const Track& track,
        mixxx::audio::FramePos position,
        mixxx::audio::SampleRate sampleRate) {
    if (!position.isValid() || !sampleRate.isValid())
        return {};
    const auto tolerance = sampleRate * kNearestToleranceSeconds;
    CuePointer best;
    double bestDistance = tolerance + 1.0;
    for (const auto& cue : liveCues(track)) {
        const auto delta = std::abs(cue->getPosition() - position);
        if (delta <= tolerance &&
                (delta < bestDistance ||
                        (delta == bestDistance &&
                                (!best ||
                                        cue->getPosition() <
                                                best->getPosition())))) {
            best = cue;
            bestDistance = delta;
        }
    }
    return best;
}

int MemoryCues::removeExact(Track& track, mixxx::audio::FramePos position) {
    int count = 0;
    for (const auto& cue : liveCues(track)) {
        if (cue->getPosition() == position) {
            count += track.removeMemoryCue(cue);
        }
    }
    return count;
}

int MemoryCues::removeCurrent(Track& track, mixxx::audio::FramePos position) {
    return track.removeMemoryCue(current(track, position));
}

int MemoryCues::removeNearest(Track& track,
        mixxx::audio::FramePos position,
        mixxx::audio::SampleRate sampleRate) {
    return track.removeMemoryCue(nearest(track, position, sampleRate));
}

int MemoryCues::removePrevious(Track& track, mixxx::audio::FramePos position) {
    return track.removeMemoryCue(previous(track, position));
}

int MemoryCues::removeNext(Track& track, mixxx::audio::FramePos position) {
    return track.removeMemoryCue(next(track, position));
}

int MemoryCues::removeAll(Track& track) {
    int count = 0;
    for (const auto& cue : liveCues(track)) {
        count += track.removeMemoryCue(cue);
    }
    return count;
}

bool MemoryCues::editLabel(Track& track, const CuePointer& cue, const QString& label) {
    return track.updateMemoryCue(cue, label, std::nullopt);
}

bool MemoryCues::editColor(Track& track, const CuePointer& cue, mixxx::RgbColor color) {
    return track.updateMemoryCue(cue, std::nullopt, color);
}
