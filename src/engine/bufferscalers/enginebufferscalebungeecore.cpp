#include "engine/bufferscalers/enginebufferscalebungeecore.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#include "engine/readaheadmanager.h"
#include "engine/stretchinputbounds.h"
#include "util/assert.h"
#include "util/fpclassify.h"
#include "util/sample.h"
#include "util/timer.h"

EngineBufferScaleBungeeCore::EngineBufferScaleBungeeCore(ReadAheadManager* pReadAheadManager)
        : m_pReadAheadManager(pReadAheadManager),
          m_pStretcher(nullptr),
          m_bBackwards(false),
          m_channelStride(0),
          m_bufferedInputBeginFrame(0),
          m_bufferedInputEndFrame(0),
          m_bResetNeeded(true),
          m_remainingOutputFrames(0),
          m_outputChunkConsumed(0),
          m_lastReadFramesProcessed(0.0),
          m_outputLatencyFrames(0),
          m_inputBufferFrames(0),
          m_grainPending(false),
          m_cacheReadUnavailable(false),
          m_readAttempts(0),
          m_zeroReadAttempts(0) {
    m_request.position = std::numeric_limits<double>::quiet_NaN();
    m_request.speed = 1.0;
    m_request.pitch = 1.0;
    m_request.reset = false;
    m_request.resampleMode = resampleMode_autoOut;

    m_outputChunk.data = nullptr;
    m_outputChunk.frameCount = 0;
    m_outputChunk.channelStride = 0;
    m_outputChunk.request[0] = nullptr;
    m_outputChunk.request[1] = nullptr;

    m_currentInputChunk.begin = 0;
    m_currentInputChunk.end = 0;

    onSignalChanged();
}

void EngineBufferScaleBungeeCore::onSignalChanged() {
    const int channelCount = static_cast<int>(getOutputSignal().getChannelCount());
    if (m_channelBufferPtrs.size() != static_cast<size_t>(channelCount)) {
        m_channelBufferPtrs.resize(channelCount);
    }

    m_pStretcher.reset();
    m_channelStride = 0;
    m_outputLatencyFrames = 0;

    if (!getOutputSignal().isValid() || channelCount <= 0) {
        m_inputBufferFrames = 2 * kMaxGrainFrames;
        if (channelCount > 0) {
            m_contiguousChannelBuffer =
                    mixxx::SampleBuffer(m_inputBufferFrames * channelCount);
            for (int ch = 0; ch < channelCount; ++ch) {
                m_channelBufferPtrs[ch] =
                        m_contiguousChannelBuffer.data() + (ch * m_inputBufferFrames);
            }
        }
        m_interleavedReadBuffer =
                mixxx::SampleBuffer(std::max(channelCount, 1) * kMaxGrainFrames);
        clear();
        return;
    }

    const int sampleRate = static_cast<int>(getOutputSignal().getSampleRate());
    const int log2SynthesisHop = std::max(
            0,
            static_cast<int>(std::bit_width(static_cast<unsigned>(sampleRate))) -
                    1 -
                    6);
    m_outputLatencyFrames = 2 * (SINT{1} << log2SynthesisHop);

    Bungee::SampleRates sampleRates;
    sampleRates.input = sampleRate;
    sampleRates.output = sampleRate;

    m_pStretcher = std::make_unique<Bungee::Stretcher<Bungee::Basic>>(
            sampleRates, channelCount, 0);

    m_inputBufferFrames = std::max<SINT>(
                                  m_pStretcher->maxInputFrameCount(),
                                  kMaxGrainFrames) +
            kMaxGrainFrames;
    m_channelStride = m_inputBufferFrames;
    m_contiguousChannelBuffer = mixxx::SampleBuffer(m_inputBufferFrames * channelCount);
    m_analysisBuffer = mixxx::SampleBuffer(m_inputBufferFrames * channelCount);

    SampleUtil::clear(m_contiguousChannelBuffer.data(),
            m_inputBufferFrames * channelCount);
    for (int ch = 0; ch < channelCount; ++ch) {
        m_channelBufferPtrs[ch] =
                m_contiguousChannelBuffer.data() + (ch * m_inputBufferFrames);
    }

    m_interleavedReadBuffer = mixxx::SampleBuffer(kMaxGrainFrames * channelCount);
    clear();
}

double EngineBufferScaleBungeeCore::getVisualPlayPositionOffset() const {
    return 0;
}

void EngineBufferScaleBungeeCore::setScaleParameters(double base_rate,
        double* pTempoRatio,
        double* pPitchRatio) {
    const bool wasBackwards = m_bBackwards;
    m_bBackwards = *pTempoRatio < 0;

    double speedAbs = util_isfinite(*pTempoRatio) ? fabs(*pTempoRatio) : 0;
    if (speedAbs > MAX_SEEK_SPEED) {
        speedAbs = MAX_SEEK_SPEED;
    } else if (speedAbs < MIN_SEEK_SPEED) {
        speedAbs = 0.0;
    }

    *pTempoRatio = m_bBackwards ? -speedAbs : speedAbs;

    m_dBaseRate = util_isfinite(base_rate) ? std::fabs(base_rate) : 0.0;
    m_dTempoRatio = speedAbs;
    m_dPitchRatio = *pPitchRatio;
    const double requestedEffectiveRate = m_dBaseRate * m_dTempoRatio;
    if (!util_isfinite(requestedEffectiveRate)) {
        m_dBaseRate = 0;
        m_dTempoRatio = 0;
        *pTempoRatio = 0;
    }
    if (m_remainingOutputFrames <= 0 && !m_grainPending) {
        m_effectiveRate = m_dBaseRate * m_dTempoRatio;
    }

    const double pitchScale = fabs(m_dBaseRate * *pPitchRatio);
    const double requestedPitch = util_isfinite(pitchScale) && pitchScale > 0.0
            ? std::clamp(pitchScale, 0.25, 4.0)
            : 1.0;
    *pPitchRatio = m_dBaseRate > 0 ? requestedPitch / m_dBaseRate : 1.0;
    m_dPitchRatio = *pPitchRatio;
    if (!m_grainPending) {
        m_request.pitch = requestedPitch;
        m_request.speed = m_dBaseRate * m_dTempoRatio;
    }

    if (wasBackwards != m_bBackwards) {
        clear();
    }
}

void EngineBufferScaleBungeeCore::deinterleaveInput(
        const CSAMPLE* pBuffer,
        SINT destOffsetFrames,
        SINT frames) {
    const int channelCount = static_cast<int>(getOutputSignal().getChannelCount());
    if (channelCount <= 0 || frames <= 0) {
        return;
    }

    DEBUG_ASSERT(destOffsetFrames >= 0);
    DEBUG_ASSERT(destOffsetFrames + frames <= m_channelStride);

    switch (getOutputSignal().getChannelCount()) {
    case mixxx::audio::ChannelCount::stereo():
        SampleUtil::deinterleaveBuffer(
                m_channelBufferPtrs[0] + destOffsetFrames,
                m_channelBufferPtrs[1] + destOffsetFrames,
                pBuffer,
                frames);
        break;
    default: {
        for (SINT frame = 0; frame < frames; ++frame) {
            for (int ch = 0; ch < channelCount; ++ch) {
                m_channelBufferPtrs[ch][destOffsetFrames + frame] =
                        pBuffer[frame * channelCount + ch];
            }
        }
    } break;
    }
}

SINT EngineBufferScaleBungeeCore::consumeReadAheadGap(
        double signedEffectiveRate,
        SINT framesToConsume) {
    if (framesToConsume <= 0) {
        return 0;
    }
    if (!m_pReadAheadManager || !getOutputSignal().isValid()) {
        return framesToConsume;
    }

    SINT consumedFrames = 0;
    while (consumedFrames < framesToConsume &&
            m_readAttempts < mixxx::engine::stretch::kReadAttemptBudget) {
        const SINT framesRequested = std::min<SINT>(
                framesToConsume - consumedFrames,
                kMaxGrainFrames);
        const SINT samplesRequested = getOutputSignal().frames2samples(framesRequested);
        ++m_readAttempts;
        auto read = m_pReadAheadManager->getNextSamplesForStretch(
                signedEffectiveRate,
                m_interleavedReadBuffer.data(),
                samplesRequested,
                getOutputSignal().getChannelCount());
        if (read.unavailable) {
            read.samplesRead = m_pReadAheadManager->getNextSamples(
                    signedEffectiveRate,
                    m_interleavedReadBuffer.data(),
                    samplesRequested,
                    getOutputSignal().getChannelCount());
            read.unavailable = false;
        }
        const SINT availableFrames = getOutputSignal().samples2frames(read.samplesRead);
        if (availableFrames <= 0) {
            if (++m_zeroReadAttempts >= mixxx::engine::stretch::kZeroReadAttemptBudget) {
                m_cacheReadUnavailable = true;
                break;
            }
            continue;
        }
        consumedFrames += std::min(availableFrames, framesRequested);
    }

    if (consumedFrames < framesToConsume &&
            m_readAttempts >= mixxx::engine::stretch::kReadAttemptBudget) {
        m_cacheReadUnavailable = true;
    }
    return consumedFrames;
}

void EngineBufferScaleBungeeCore::discardBufferedInputBefore(
        SINT framePosition,
        double signedEffectiveRate) {
    if (framePosition <= m_bufferedInputBeginFrame) {
        return;
    }

    const SINT bufferedFrames = m_bufferedInputEndFrame - m_bufferedInputBeginFrame;
    if (bufferedFrames <= 0) {
        const SINT consumedGapFrames = consumeReadAheadGap(
                signedEffectiveRate,
                framePosition - m_bufferedInputEndFrame);
        m_bufferedInputBeginFrame = m_bufferedInputEndFrame + consumedGapFrames;
        m_bufferedInputEndFrame = m_bufferedInputBeginFrame;
        return;
    }

    const SINT oldBufferedInputEndFrame = m_bufferedInputEndFrame;
    const SINT discardFrames = std::min(framePosition - m_bufferedInputBeginFrame,
            bufferedFrames);
    const SINT remainingFrames = bufferedFrames - discardFrames;
    for (float* pChannel : m_channelBufferPtrs) {
        std::memmove(pChannel,
                pChannel + discardFrames,
                remainingFrames * sizeof(float));
    }

    m_bufferedInputBeginFrame += discardFrames;
    if (remainingFrames <= 0) {
        const SINT consumedGapFrames = consumeReadAheadGap(
                signedEffectiveRate,
                framePosition - oldBufferedInputEndFrame);

        m_bufferedInputBeginFrame = oldBufferedInputEndFrame + consumedGapFrames;
        m_bufferedInputEndFrame = m_bufferedInputBeginFrame;
    }
}

SINT EngineBufferScaleBungeeCore::appendInputFrames(
        double signedEffectiveRate,
        SINT framesToRead) {
    if (framesToRead <= 0 || !m_pReadAheadManager) {
        return 0;
    }

    const SINT bufferedFrames = m_bufferedInputEndFrame - m_bufferedInputBeginFrame;
    const SINT availableCapacity = m_inputBufferFrames - bufferedFrames;
    const SINT framesRequested = std::min(framesToRead,
            std::min(availableCapacity, kMaxGrainFrames));
    if (framesRequested <= 0) {
        return 0;
    }

    const SINT samplesRequested = getOutputSignal().frames2samples(framesRequested);
    if (m_readAttempts >= mixxx::engine::stretch::kReadAttemptBudget) {
        m_cacheReadUnavailable = true;
        return 0;
    }
    ++m_readAttempts;
    auto read = m_pReadAheadManager->getNextSamplesForStretch(
            signedEffectiveRate,
            m_interleavedReadBuffer.data(),
            samplesRequested,
            getOutputSignal().getChannelCount());
    if (read.unavailable) {
        read.samplesRead = m_pReadAheadManager->getNextSamples(
                signedEffectiveRate,
                m_interleavedReadBuffer.data(),
                samplesRequested,
                getOutputSignal().getChannelCount());
        read.unavailable = false;
    }
    const SINT availableFrames = getOutputSignal().samples2frames(read.samplesRead);
    if (availableFrames <= 0) {
        if (++m_zeroReadAttempts >= mixxx::engine::stretch::kZeroReadAttemptBudget) {
            m_cacheReadUnavailable = true;
        }
        return 0;
    }

    deinterleaveInput(m_interleavedReadBuffer.data(), bufferedFrames, availableFrames);
    m_bufferedInputEndFrame += availableFrames;
    return availableFrames;
}

SINT EngineBufferScaleBungeeCore::ensureInputForCurrentChunk(double signedEffectiveRate) {
    if (m_currentInputChunk.end <= m_currentInputChunk.begin) {
        return 0;
    }

    if (m_bufferedInputBeginFrame < m_currentInputChunk.begin) {
        discardBufferedInputBefore(m_currentInputChunk.begin, signedEffectiveRate);
    }

    while (m_bufferedInputEndFrame < m_currentInputChunk.end) {
        const SINT missingFrames = m_currentInputChunk.end - m_bufferedInputEndFrame;
        appendInputFrames(signedEffectiveRate, missingFrames);
        if (m_cacheReadUnavailable) {
            break;
        }
    }

    const SINT availableBegin = std::max(m_bufferedInputBeginFrame,
            static_cast<SINT>(m_currentInputChunk.begin));
    const SINT availableEnd = std::max(availableBegin,
            std::min(m_bufferedInputEndFrame,
                    static_cast<SINT>(m_currentInputChunk.end)));
    return availableEnd - availableBegin;
}

void EngineBufferScaleBungeeCore::copyOutputFrames(
        CSAMPLE* pDest, SINT offsetInChunk, SINT nFrames) const {
    DEBUG_ASSERT(m_outputChunk.data != nullptr);
    DEBUG_ASSERT(nFrames > 0);
    DEBUG_ASSERT(offsetInChunk + nFrames <=
            static_cast<SINT>(m_outputChunk.frameCount));

    switch (getOutputSignal().getChannelCount()) {
    case mixxx::audio::ChannelCount::stereo():

        SampleUtil::interleaveBuffer(
                pDest,
                m_outputChunk.data + offsetInChunk,
                m_outputChunk.data + offsetInChunk + m_outputChunk.channelStride,
                nFrames);
        break;
    default: {
        const int channelCount =
                static_cast<int>(getOutputSignal().getChannelCount());
        for (SINT frame = 0; frame < nFrames; ++frame) {
            for (int ch = 0; ch < channelCount; ++ch) {
                pDest[frame * channelCount + ch] =
                        m_outputChunk.data[offsetInChunk + frame +
                                ch * m_outputChunk.channelStride];
            }
        }
    } break;
    }
}

bool EngineBufferScaleBungeeCore::hasValidOutputChunk() const {
    return m_outputChunk.frameCount > 0 &&
            m_outputChunk.data != nullptr;
}

double EngineBufferScaleBungeeCore::copyFlushOutputFrames(
        CSAMPLE*& pOutput,
        SINT& remainingFrames) {
    if (!hasValidOutputChunk()) {
        return 0.0;
    }

    const SINT framesToCopy = std::min(
            static_cast<SINT>(m_outputChunk.frameCount) - m_outputChunkConsumed,
            remainingFrames);
    if (framesToCopy <= 0) {
        return 0.0;
    }
    copyOutputFrames(pOutput, m_outputChunkConsumed, framesToCopy);

    remainingFrames -= framesToCopy;
    pOutput += getOutputSignal().frames2samples(framesToCopy);
    m_outputChunkConsumed += framesToCopy;
    m_remainingOutputFrames =
            static_cast<SINT>(m_outputChunk.frameCount) - m_outputChunkConsumed;
    if (m_remainingOutputFrames <= 0) {
        m_outputChunkConsumed = 0;
    }
    return m_outputSourceRate * static_cast<double>(framesToCopy);
}

SINT EngineBufferScaleBungeeCore::processGrain(CSAMPLE* pOutputBuffer, SINT maxFrames) {
    m_lastReadFramesProcessed = 0.0;

    if (!m_pStretcher || !getOutputSignal().isValid()) {
        return 0;
    }

    if (m_remainingOutputFrames > 0 && m_outputChunk.data != nullptr) {
        if (!hasValidOutputChunk()) {
            m_remainingOutputFrames = 0;
            m_outputChunkConsumed = 0;
            return 0;
        }

        const SINT framesToCopy = std::min(m_remainingOutputFrames, maxFrames);
        m_lastReadFramesProcessed =
                m_outputSourceRate * static_cast<double>(framesToCopy);
        copyOutputFrames(pOutputBuffer, m_outputChunkConsumed, framesToCopy);

        m_outputChunkConsumed += framesToCopy;
        m_remainingOutputFrames -= framesToCopy;
        if (m_remainingOutputFrames <= 0) {
            m_outputChunkConsumed = 0;
        }
        return framesToCopy;
    }

    if (!m_grainPending) {
        m_effectiveRate = m_dBaseRate * m_dTempoRatio;
    }
    const double signedEffectiveRate = (m_bBackwards ? -1.0 : 1.0) * m_effectiveRate;

    if (m_bResetNeeded) {
        m_request.position = 0.0;
        m_request.speed = m_effectiveRate;
        m_request.reset = true;
        for (int i = 0; i < 3; ++i) {
            m_pStretcher->preroll(m_request);
        }
        m_prerolling = true;
        m_bResetNeeded = false;
        m_currentInputChunk.begin = 0;
        m_currentInputChunk.end = 0;
        m_bufferedInputBeginFrame = 0;
        m_bufferedInputEndFrame = 0;
    } else {
        m_request.reset = false;
        if (util_isnan(m_request.position)) {
            m_request.position = 0.0;
        }
    }

    m_request.speed = m_effectiveRate;

    m_cacheReadUnavailable = false;
    if (!m_grainPending) {
        m_currentInputChunk = m_pStretcher->specifyGrain(m_request);
        m_grainPending = true;
    }
    const SINT framesNeeded = m_currentInputChunk.end - m_currentInputChunk.begin;
    if (framesNeeded <= 0 || framesNeeded > m_channelStride) {
        m_cacheReadUnavailable = true;
        return 0;
    }

    ensureInputForCurrentChunk(signedEffectiveRate);
    if (m_cacheReadUnavailable) {
        return 0;
    }

    const SINT availableBegin = std::max(m_bufferedInputBeginFrame,
            static_cast<SINT>(m_currentInputChunk.begin));
    const SINT availableEnd = std::max(availableBegin,
            std::min(m_bufferedInputEndFrame,
                    static_cast<SINT>(m_currentInputChunk.end)));
    const int muteHead = availableBegin - m_currentInputChunk.begin;
    const int muteTail = m_currentInputChunk.end - availableEnd;
    const SINT dataOffset = std::max<SINT>(0, availableBegin - m_bufferedInputBeginFrame);

    const SINT grainSize = static_cast<SINT>(
            m_currentInputChunk.end - m_currentInputChunk.begin);
    if (dataOffset + grainSize > m_channelStride) {
        m_bResetNeeded = true;
        return 0;
    }
    DEBUG_ASSERT(m_channelBufferPtrs.empty() || dataOffset + grainSize <= m_channelStride);
    const float* analysis = m_channelBufferPtrs[0] + dataOffset;
    if (muteHead > 0) {
        const int channels = getOutputSignal().getChannelCount();
        const SINT activeFrames = availableEnd - availableBegin;
        for (int channel = 0; channel < channels; ++channel) {
            auto* padded = m_analysisBuffer.data() + channel * m_channelStride;
            SampleUtil::clear(padded, grainSize);
            std::copy_n(m_channelBufferPtrs[channel] + dataOffset,
                    activeFrames,
                    padded + muteHead);
        }
        analysis = m_analysisBuffer.data();
    }
    m_pStretcher->analyseGrain(analysis, m_channelStride, muteHead, muteTail);
    m_pStretcher->synthesiseGrain(m_outputChunk);
    m_pStretcher->next(m_request);
    m_grainPending = false;
    m_request.speed = m_effectiveRate;
    const double pitchScale = fabs(m_dBaseRate * m_dPitchRatio);
    m_request.pitch = util_isfinite(pitchScale) && pitchScale > 0.0
            ? std::clamp(pitchScale, 0.25, 4.0)
            : 1.0;

    if (!hasValidOutputChunk() || !m_outputChunk.request[0] ||
            !m_outputChunk.request[1] ||
            !util_isfinite(m_outputChunk.request[1]->position) ||
            !util_isfinite(m_outputChunk.request[0]->position) ||
            m_outputChunk.request[0]->position < -1e-8) {
        return 0;
    }
    m_outputSourceRate = (m_outputChunk.request[1]->position -
                                 m_outputChunk.request[0]->position) /
            m_outputChunk.frameCount;
    if (!util_isfinite(m_outputSourceRate) || m_outputSourceRate < 0) {
        return 0;
    }
    m_prerolling = false;

    m_remainingOutputFrames = m_outputChunk.frameCount;
    m_outputChunkConsumed = 0;

    const SINT framesToCopy = std::min(static_cast<SINT>(m_outputChunk.frameCount), maxFrames);
    m_lastReadFramesProcessed = m_outputSourceRate * static_cast<double>(framesToCopy);
    copyOutputFrames(pOutputBuffer, 0, framesToCopy);
    m_outputChunkConsumed = framesToCopy;
    m_remainingOutputFrames = m_outputChunk.frameCount - framesToCopy;
    if (m_remainingOutputFrames <= 0) {
        m_outputChunkConsumed = 0;
    }

    return framesToCopy;
}

double EngineBufferScaleBungeeCore::scaleBuffer(CSAMPLE* pOutputBuffer,
        SINT iOutputBufferSize) {
    if (!m_pStretcher || m_dBaseRate == 0.0 || m_dTempoRatio == 0.0 ||
            !getOutputSignal().isValid()) {
        SampleUtil::clear(pOutputBuffer, iOutputBufferSize);
        return 0.0;
    }

    ScopedTimer t(QStringLiteral("EngineBufferScaleBungeeCore::scaleBuffer"));

    m_readAttempts = 0;
    m_zeroReadAttempts = 0;
    double readFramesProcessed = 0.0;
    SINT remainingFrames = getOutputSignal().samples2frames(iOutputBufferSize);
    CSAMPLE* pOutput = pOutputBuffer;
    bool lastProcessFailed = false;
    int prerollAttempts = 0;

    while (remainingFrames > 0) {
        const SINT framesProduced = processGrain(pOutput, remainingFrames);
        if (framesProduced > 0) {
            remainingFrames -= framesProduced;
            pOutput += getOutputSignal().frames2samples(framesProduced);
            readFramesProcessed += m_lastReadFramesProcessed;
            lastProcessFailed = false;
            continue;
        }

        if (m_cacheReadUnavailable) {
            if (remainingFrames > 0) {
                SampleUtil::clear(
                        pOutput, getOutputSignal().frames2samples(remainingFrames));
            }
            break;
        }

        if (m_prerolling && ++prerollAttempts < 10) {
            continue;
        }
        if (lastProcessFailed) {
            if (!m_pStretcher->isFlushed()) {
                Bungee::Request flushRequest{};
                flushRequest.position = std::numeric_limits<double>::quiet_NaN();
                flushRequest.speed = m_request.speed;
                flushRequest.pitch = m_request.pitch;
                flushRequest.reset = true;
                flushRequest.resampleMode = resampleMode_autoOut;

                m_pStretcher->specifyGrain(flushRequest);
                m_pStretcher->analyseGrain(nullptr, m_channelStride, 0, 0);
                m_pStretcher->synthesiseGrain(m_outputChunk);
                m_outputChunkConsumed = 0;
                m_remainingOutputFrames = 0;

                readFramesProcessed +=
                        copyFlushOutputFrames(pOutput, remainingFrames);
            }

            if (remainingFrames > 0) {
                SampleUtil::clear(
                        pOutput, getOutputSignal().frames2samples(remainingFrames));
            }
            break;
        }

        lastProcessFailed = true;
    }

    return readFramesProcessed;
}

void EngineBufferScaleBungeeCore::clear() {
    if (m_pStretcher) {
        if (m_grainPending) {
            const int frames = m_currentInputChunk.end - m_currentInputChunk.begin;
            m_pStretcher->analyseGrain(m_channelBufferPtrs[0], m_channelStride, frames, 0);
            m_pStretcher->synthesiseGrain(m_outputChunk);
        }
        Bungee::Request flush{};
        flush.position = std::numeric_limits<double>::quiet_NaN();
        flush.speed = 1;
        flush.pitch = 1;
        flush.resampleMode = resampleMode_autoOut;
        for (int i = 0; i < 4; ++i) {
            m_pStretcher->specifyGrain(flush);
            m_pStretcher->analyseGrain(nullptr, m_channelStride, 0, 0);
            m_pStretcher->synthesiseGrain(m_outputChunk);
        }
    }
    m_prerolling = true;
    m_bResetNeeded = true;
    m_remainingOutputFrames = 0;
    m_outputChunkConsumed = 0;
    m_lastReadFramesProcessed = 0.0;
    m_currentInputChunk.begin = 0;
    m_currentInputChunk.end = 0;
    m_grainPending = false;
    m_cacheReadUnavailable = false;
    m_bufferedInputBeginFrame = 0;
    m_bufferedInputEndFrame = 0;

    m_effectiveRate = m_dBaseRate * m_dTempoRatio;

    m_request.position = std::numeric_limits<double>::quiet_NaN();
    m_request.speed = m_effectiveRate;
    const double pitchScale = fabs(m_dBaseRate * m_dPitchRatio);
    m_request.pitch = util_isfinite(pitchScale) && pitchScale > 0.0
            ? std::clamp(pitchScale, 0.25, 4.0)
            : 1.0;
    m_request.reset = true;
    m_request.resampleMode = resampleMode_autoOut;

    m_outputChunk.data = nullptr;
    m_outputChunk.frameCount = 0;
    m_outputChunk.channelStride = 0;
    m_outputChunk.request[0] = nullptr;
    m_outputChunk.request[1] = nullptr;

    if (m_contiguousChannelBuffer.size() > 0) {
        SampleUtil::clear(
                m_contiguousChannelBuffer.data(),
                m_contiguousChannelBuffer.size());
    }
}
