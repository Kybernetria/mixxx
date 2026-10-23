#include "engine/bufferscalers/enginebufferscalesignalsmith.h"

#include <algorithm>
#include <limits>

#include "engine/engine.h"
#include "engine/readaheadmanager.h"
#include "moc_enginebufferscalesignalsmith.cpp"
#include "util/assert.h"
#include "util/defs.h"
#include "util/fpclassify.h"
#include "util/sample.h"
#include "util/timer.h"

EngineBufferScaleSignalSmith::EngineBufferScaleSignalSmith(
        ReadAheadManager* pReadAheadManager)
        : m_pReadAheadManager(pReadAheadManager),
          m_interleavedBuffer(mixxx::kMaxSupportedStems * MAX_BUFFER_LEN *
                  mixxx::audio::ChannelCount::stereo()) {
    for (std::size_t channel = 0; channel < m_outputBufferPtrs.size(); ++channel) {
        m_outputBufferPtrs[channel] = m_interleavedBuffer.data(channel * MAX_BUFFER_LEN);
    }
    onSignalChanged();
}

void EngineBufferScaleSignalSmith::setScaleParameters(
        double base_rate, double* pTempoRatio, double* pPitchRatio) {
    m_parametersValid = util_isfinite(base_rate) && base_rate > 0 &&
            util_isfinite(*pTempoRatio) && util_isfinite(*pPitchRatio);
    if (m_parametersValid) {
        const double rate = std::min(base_rate * std::fabs(*pTempoRatio), MAX_SEEK_SPEED);
        const double transpose = std::clamp(base_rate * *pPitchRatio,
                1.0 / MAX_SEEK_SPEED,
                MAX_SEEK_SPEED);
        *pTempoRatio = std::copysign(rate / base_rate, *pTempoRatio);
        *pPitchRatio = transpose / base_rate;
        m_parametersValid = util_isfinite(*pTempoRatio) && util_isfinite(*pPitchRatio);
    }
    if (m_parametersValid) {
        m_dBaseRate = base_rate;
        m_bBackwards = *pTempoRatio < 0;
        m_dTempoRatio = std::fabs(*pTempoRatio);
        m_dPitchRatio = *pPitchRatio;
        m_effectiveRate = m_dBaseRate * m_dTempoRatio;
        m_pStretch->setTransposeFactor(static_cast<float>(m_dBaseRate * m_dPitchRatio));
        m_pStretch->setFormantFactor(1.0);
        updateLatency();
    }
    if (!m_parametersValid) {
        *pTempoRatio = 0;
        *pPitchRatio = 1;
    }
}

void EngineBufferScaleSignalSmith::updateLatency() {
    // As documented in
    // https://signalsmith-audio.co.uk/code/stretch/#how-to-use-latency-starting-and-ending,
    // stretch factor should be used when computing total latency.
    const double latency = m_pStretch->inputLatency() +
            std::round(m_effectiveRate * m_pStretch->outputLatency());
    if (!util_isfinite(latency) || latency < 0 ||
            latency >= static_cast<double>(std::numeric_limits<SINT>::max())) {
        m_parametersValid = false;
        return;
    }
    m_expectedFrameLatency = static_cast<SINT>(latency);
}

void EngineBufferScaleSignalSmith::onSignalChanged() {
    if (!getOutputSignal().isValid()) {
        return;
    }
    const auto channelCount = getOutputSignal().getChannelCount();
    if (channelCount > m_outputBufferPtrs.size()) {
        return;
    }
    m_buffers.resize(channelCount);
    m_bufferPtrs.resize(channelCount);
    for (int channel = 0; channel < channelCount; ++channel) {
        if (m_buffers[channel].size() != MAX_BUFFER_LEN) {
            m_buffers[channel] = mixxx::SampleBuffer(MAX_BUFFER_LEN);
        }
        m_bufferPtrs[channel] = m_buffers[channel].data();
    }
    m_stretchDefault.presetDefault(channelCount, getOutputSignal().getSampleRate());
    m_stretchCheaper.presetCheaper(channelCount, getOutputSignal().getSampleRate());
    applyPreset();
    clear();
}

void EngineBufferScaleSignalSmith::applyPreset() {
    m_currentPreset = m_requestedPreset.load(std::memory_order_relaxed);
    m_pStretch = m_currentPreset == Preset::Cheaper ? &m_stretchCheaper : &m_stretchDefault;
    m_pStretch->setTransposeFactor(static_cast<float>(m_dBaseRate * m_dPitchRatio));
    m_pStretch->setFormantFactor(1.0);
    updateLatency();
}

void EngineBufferScaleSignalSmith::clear() {
    if (getOutputSignal().isValid() &&
            getOutputSignal().getChannelCount() <= m_outputBufferPtrs.size() &&
            isPresetChangePending()) {
        applyPreset();
    }
    m_pStretch->reset();
    m_currentFrameOffset = 0;
    m_frameFractionalLeftover = 0;
}

SINT EngineBufferScaleSignalSmith::fetchAndDeinterleave(SINT sampleToRead, SINT frameOffset) {
    auto frameRead = getOutputSignal().samples2frames(
            m_pReadAheadManager->getNextSamples(
                    // The value doesn't matter here. All that matters is we
                    // are going forward or backward.
                    (m_bBackwards ? -1 : 1) * m_dBaseRate * m_dTempoRatio,
                    m_interleavedBuffer.data(),
                    sampleToRead,
                    getOutputSignal().getChannelCount()));

    switch (getOutputSignal().getChannelCount()) {
    case mixxx::audio::ChannelCount::stereo():
        SampleUtil::deinterleaveBuffer(
                m_buffers[0].data(frameOffset),
                m_buffers[1].data(frameOffset),
                m_interleavedBuffer.data(),
                frameRead);
        break;
    case mixxx::audio::ChannelCount::stem():
        SampleUtil::deinterleaveBuffer(
                m_buffers[0].data(frameOffset),
                m_buffers[1].data(frameOffset),
                m_buffers[2].data(frameOffset),
                m_buffers[3].data(frameOffset),
                m_buffers[4].data(frameOffset),
                m_buffers[5].data(frameOffset),
                m_buffers[6].data(frameOffset),
                m_buffers[7].data(frameOffset),
                m_interleavedBuffer.data(),
                frameRead);
        break;
    default: {
        int chCount = getOutputSignal().getChannelCount();
        // The sampler are ordered as following in pBuffer
        //    1234..X1234...X...
        // And need to be reordered as following
        // m_buffers#1 = 11..
        // m_buffers#2 = 22..
        // m_buffers#3 = 33..
        // m_buffers#4 = 44..fff
        // m_buffers#X = XX..
        //
        // Because of the unanticipated number of buffer and channel, we cannot
        // use any SampleUtil in this case
        for (SINT frameIdx = 0; frameIdx < frameRead; ++frameIdx) {
            for (int channel = 0; channel < chCount; channel++) {
                m_buffers[channel].data(frameOffset)[frameIdx] =
                        m_interleavedBuffer.data()[frameIdx * chCount + channel];
            }
        }
    } break;
    }
    return frameRead;
}

void EngineBufferScaleSignalSmith::readFrames(SINT frames) {
    SINT read = 0;
    bool lastReadFailed = false;
    while (read < frames) {
        const auto count = fetchAndDeinterleave(
                getOutputSignal().frames2samples(frames - read), read);
        read += count;
        if (lastReadFailed && count == 0) {
            for (int channel = 0; channel < getOutputSignal().getChannelCount(); ++channel) {
                SampleUtil::clear(m_bufferPtrs[channel] + read, frames - read);
            }
            break;
        }
        lastReadFailed = count == 0;
    }
}

double EngineBufferScaleSignalSmith::scaleBuffer(CSAMPLE* pOutputBuffer, SINT iOutputBufferSize) {
    ScopedTimer t(QStringLiteral("EngineBufferScaleSignalsmith::scaleBuffer"));
    if (iOutputBufferSize <= 0 || pOutputBuffer == nullptr) {
        return 0;
    }
    const auto channels = getOutputSignal().getChannelCount();
    if (!m_parametersValid || !getOutputSignal().isValid() ||
            channels > m_outputBufferPtrs.size() || iOutputBufferSize % channels != 0 ||
            iOutputBufferSize / channels > MAX_BUFFER_LEN) {
        SampleUtil::clear(pOutputBuffer, iOutputBufferSize);
        return 0;
    }
    const SINT outputFrames = getOutputSignal().samples2frames(iOutputBufferSize);
    const SINT preroll = m_currentFrameOffset == 0 && m_dTempoRatio > 0
            ? std::min(m_expectedFrameLatency, SINT(MAX_BUFFER_LEN))
            : 0;
    double offset = m_currentFrameOffset + preroll;
    double demand = m_effectiveRate * outputFrames + m_frameFractionalLeftover;
    if (offset != m_expectedFrameLatency && demand > 0) {
        const double correction = std::max(-demand, m_expectedFrameLatency - offset);
        demand += correction;
        offset += correction;
    }
    if (!(demand >= 0 && demand < MAX_BUFFER_LEN + 1.0)) {
        SampleUtil::clear(pOutputBuffer, iOutputBufferSize);
        return 0;
    }
    const SINT inputFrames = static_cast<SINT>(demand);
    m_currentFrameOffset = offset;
    m_frameFractionalLeftover = demand - inputFrames;
    DEBUG_ASSERT(0 <= m_frameFractionalLeftover && m_frameFractionalLeftover < 1);
    if (preroll > 0) {
        readFrames(preroll);
        m_pStretch->outputSeek(m_bufferPtrs.data(), preroll);
    }
    readFrames(inputFrames);
    {
        ScopedTimer t(QStringLiteral("Signalsmith::process"));
        m_pStretch->process(m_bufferPtrs.data(), inputFrames, m_outputBufferPtrs.data(), outputFrames);
    }
    switch (channels) {
    case mixxx::audio::ChannelCount::stereo():
        SampleUtil::interleaveBuffer(pOutputBuffer,
                m_outputBufferPtrs[0],
                m_outputBufferPtrs[1],
                outputFrames);
        break;
    case mixxx::audio::ChannelCount::stem():
        SampleUtil::interleaveBuffer(pOutputBuffer,
                m_outputBufferPtrs[0],
                m_outputBufferPtrs[1],
                m_outputBufferPtrs[2],
                m_outputBufferPtrs[3],
                m_outputBufferPtrs[4],
                m_outputBufferPtrs[5],
                m_outputBufferPtrs[6],
                m_outputBufferPtrs[7],
                outputFrames);
        break;
    default:
        for (SINT frame = 0; frame < outputFrames; ++frame) {
            for (int channel = 0; channel < channels; ++channel) {
                pOutputBuffer[frame * channels + channel] = m_outputBufferPtrs[channel][frame];
            }
        }
    }
    return m_effectiveRate * outputFrames;
}
