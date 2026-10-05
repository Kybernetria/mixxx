#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>

#include <atomic>
#include <cmath>
#include <memory>

#include "test/signalpathtest.h"
#include "util/fpclassify.h"

namespace {

class ReaderLoadingStressTest : public BaseSignalPathTest {
  protected:
    void SetUp() override {
        BaseSignalPathTest::SetUp();
        ControlObject::set(ConfigKey("[App]", "keylock_engine"),
                static_cast<double>(EngineBuffer::KeylockEngine::Signalsmith));
        for (const auto& group : {m_sGroup1, m_sGroup2}) {
            ControlObject::set(ConfigKey(group, "keylock"), 1);
            ControlObject::set(ConfigKey(group, "quantize"), 0);
            ControlObject::set(ConfigKey(group, "repeat"), 1);
            ControlObject::set(ConfigKey(group, "rate"), 0.25);
        }
        auto* engine = m_pChannel1->getEngineBuffer();
        QObject::connect(engine,
                &EngineBuffer::trackLoaded,
                engine,
                [acceptedGeneration = m_acceptedGeneration](
                        TrackPointer, TrackPointer, quint64 generation) {
                    acceptedGeneration->store(generation, std::memory_order_release);
                },
                Qt::DirectConnection);
#ifdef __STEM__
        for (int stem = 0; stem < mixxx::kMaxSupportedStems; ++stem) {
            const auto handle = m_pEngineMixer->registerChannelGroup(
                    EngineDeck::getGroupForStem(m_sGroup1, stem));
            m_pChannel1->addStemHandle(handle);
            m_pEffectsManager->addStem(handle);
        }
#endif
    }

    void requestLoad(const TrackPointer& track) {
        m_pMixerDeck1->slotLoadTrack(track,
#ifdef __STEM__
                mixxx::StemChannelSelection(),
#endif
                false);
    }

    void process() {
        const auto before = m_pChannel2->getEngineBuffer()->getExactPlayPos();
        m_pEngineMixer->process(kProcessBufferSize);
        const auto after = m_pChannel2->getEngineBuffer()->getExactPlayPos();
        if (after != before) {
            ++m_steadyDeckProgress;
        }
        if (audible(m_sGroup2)) {
            ++m_steadyDeckAudible;
        }
    }

    bool audible(const QString& group) {
        double energy = 0;
        for (const auto sample :
                m_pEngineMixer->getChannelBuffer(group).first(kProcessBufferSize)) {
            if (!util_isfinite(static_cast<double>(sample))) {
                ADD_FAILURE() << "Non-finite audio in " << qPrintable(group);
                return false;
            }
            energy += static_cast<double>(sample) * sample;
        }
        return energy > 1e-8;
    }

    bool waitForLoadWithoutGuiDelivery(const TrackPointer& track) {
        QElapsedTimer timer;
        timer.start();
        auto* engine = m_pChannel1->getEngineBuffer();
        while (timer.elapsed() < 5000) {
            process();
            if (engine->isTrackLoaded() && engine->getLoadedTrack() == track &&
                    engine->isCurrentTrackRequest(
                            m_acceptedGeneration->load(std::memory_order_acquire))) {
                return true;
            }
            QTest::qSleep(1);
        }
        return false;
    }

    bool waitForPlayback(EngineDeck* channel,
            const QString& group,
            const TrackPointer& track,
            mixxx::audio::ChannelCount expectedChannels) {
        auto* engine = channel->getEngineBuffer();
        ControlObject::set(ConfigKey(group, "play"), 1);
        QElapsedTimer timer;
        timer.start();
        int settled = 0;
        while (timer.elapsed() < 5000 && settled < 8) {
            const auto before = engine->getExactPlayPos();
            process();
            const bool progressing = engine->getExactPlayPos() != before;
            settled = engine->isTrackLoaded() &&
                            engine->getLoadedTrack() == track &&
                            engine->getChannelCount() == expectedChannels &&
                            progressing && audible(group)
                    ? settled + 1
                    : 0;
            QTest::qWait(1);
        }
        return settled == 8;
    }

#ifdef __STEM__
    TrackPointer makeEightChannelTrack() {
        const QString path = getTestDataDir().filePath(QStringLiteral("eight-channel.wav"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return {};
        }
        constexpr quint32 kSampleRate = 44100;
        constexpr quint16 kChannels = 8;
        constexpr quint32 kFrames = 5 * kSampleRate;
        constexpr quint32 kDataBytes = kFrames * kChannels * sizeof(qint16);
        QByteArray data;
        data.reserve(44 + kDataBytes);
        QDataStream stream(&data, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream.writeRawData("RIFF", 4);
        stream << quint32(36 + kDataBytes);
        stream.writeRawData("WAVEfmt ", 8);
        stream << quint32(16) << quint16(1) << kChannels << kSampleRate
               << quint32(kSampleRate * kChannels * sizeof(qint16))
               << quint16(kChannels * sizeof(qint16)) << quint16(16);
        stream.writeRawData("data", 4);
        stream << kDataBytes;
        for (quint32 frame = 0; frame < kFrames; ++frame) {
            const auto sample = static_cast<qint16>(4096 *
                    std::sin(2 * 3.14159265358979323846 * 220 * frame / kSampleRate));
            for (quint16 channel = 0; channel < kChannels; ++channel) {
                stream << sample;
            }
        }
        if (stream.status() != QDataStream::Ok ||
                file.write(data) != data.size() || !file.flush()) {
            return {};
        }
        file.close();
        return Track::newTemporary(path);
    }
#endif

    int m_steadyDeckProgress{0};
    int m_steadyDeckAudible{0};
    std::shared_ptr<std::atomic<quint64>> m_acceptedGeneration{
            std::make_shared<std::atomic<quint64>>(0)};
};

TEST_F(ReaderLoadingStressTest, ReloadEjectAndFormatChangesPreserveOtherDeckPlayback) {
    const auto stereo = Track::newTemporary(
            getTestDir().filePath(QStringLiteral("sine-30.wav")));
    const auto steady = Track::newTemporary(
            getTestDir().filePath(QStringLiteral("sine-30.wav")));
    loadTrack(m_pMixerDeck2.get(), steady);
    ASSERT_TRUE(waitForPlayback(m_pChannel2,
            m_sGroup2,
            steady,
            mixxx::audio::ChannelCount::stereo()));

#ifdef __STEM__
    const auto replacement = makeEightChannelTrack();
    ASSERT_TRUE(replacement);
    const auto replacementChannels = mixxx::audio::ChannelCount::stem();
#else
    const auto replacement = Track::newTemporary(
            getTestDir().filePath(QStringLiteral("sine-30.wav")));
    const auto replacementChannels = mixxx::audio::ChannelCount::stereo();
#endif

    m_steadyDeckProgress = 0;
    m_steadyDeckAudible = 0;
    constexpr int kRounds = 8;
    for (int round = 0; round < kRounds; ++round) {
        SCOPED_TRACE(round);
        ControlObject::set(ConfigKey(m_sGroup1, "play"), 0);
        requestLoad(stereo);
        ASSERT_TRUE(waitForLoadWithoutGuiDelivery(stereo));
        m_pMixerDeck1->slotEjectTrack(1);
        requestLoad(stereo);
        requestLoad(stereo);
        requestLoad(replacement);
        process();
        m_pMixerDeck1->slotEjectTrack(1);
        const auto finalTrack = round % 2 == 0 ? replacement : stereo;
        const auto finalChannels = round % 2 == 0
                ? replacementChannels
                : mixxx::audio::ChannelCount::stereo();
        requestLoad(finalTrack);
        ASSERT_TRUE(waitForLoadWithoutGuiDelivery(finalTrack));
        ASSERT_TRUE(waitForPlayback(m_pChannel1, m_sGroup1, finalTrack, finalChannels));
        EXPECT_EQ(finalTrack, m_pMixerDeck1->getLoadedTrack());
        EXPECT_EQ(steady, m_pMixerDeck2->getLoadedTrack());
    }

    EXPECT_GE(m_steadyDeckProgress, kRounds * 4);
    EXPECT_GE(m_steadyDeckAudible, kRounds * 4);
    ControlObject::set(ConfigKey(m_sGroup1, "play"), 0);
    requestLoad(replacement);
    process();
    m_pMixerDeck1->slotEjectTrack(1);
    for (int callback = 0; callback < 32; ++callback) {
        process();
        QTest::qWait(1);
        EXPECT_FALSE(m_pChannel1->getEngineBuffer()->isTrackLoaded());
        EXPECT_FALSE(m_pChannel1->getEngineBuffer()->getLoadedTrack());
        EXPECT_FALSE(m_pMixerDeck1->getLoadedTrack());
    }
}

}
