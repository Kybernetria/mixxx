// Opt-in, reproducible software workload; not a sound-device or DJ-hardware test.
// This test also compiles against upstream headers for a matched baseline.
#include <QtGlobal>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#define MIXXX_HAS_GETRUSAGE 1
#else
#define MIXXX_HAS_GETRUSAGE 0
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "effects/backends/effectsbackendmanager.h"
#include "effects/effectchain.h"
#include "effects/effectslot.h"
#include "test/signalpathtest.h"
#include "util/fpclassify.h"
#include "util/time.h"

namespace {
using Clock = std::chrono::steady_clock;

struct SeekLatency {
    bool sourceProgress{false};
    bool audible{false};
    std::size_t sourceCallbacks{0};
    std::size_t audibleCallbacks{0};
    double sourceMs{0};
    double audibleMs{0};
    double targetFrame{0};
    std::size_t startedCallback{0};
    Clock::time_point started;
};

void reportLatencies(const char* name, const std::vector<double>& values) {
    if (values.empty()) {
        std::cout << ' ' << name << "_count=0";
        return;
    }
    auto sorted = values;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double p) {
        return sorted[static_cast<std::size_t>(p * (sorted.size() - 1))];
    };
    std::cout << ' ' << name << "_count=" << sorted.size()
              << ' ' << name << "_callbacks_p50=" << percentile(.50)
              << ' ' << name << "_callbacks_p95=" << percentile(.95)
              << ' ' << name << "_callbacks_p99=" << percentile(.99)
              << ' ' << name << "_callbacks_max=" << sorted.back();
}

void reportMilliseconds(const char* name, const std::vector<double>& values) {
    if (values.empty()) {
        std::cout << ' ' << name << "_ms=unavailable";
        return;
    }
    auto sorted = values;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double p) {
        return sorted[static_cast<std::size_t>(p * (sorted.size() - 1))];
    };
    std::cout << ' ' << name << "_ms_p50=" << percentile(.50)
              << ' ' << name << "_ms_p95=" << percentile(.95)
              << ' ' << name << "_ms_p99=" << percentile(.99)
              << ' ' << name << "_ms_max=" << sorted.back();
}

class StretchCallbackWorkload : public SignalPathTest {};

TEST_F(StretchCallbackWorkload, ThreeDecks) {
    const auto* backendText = std::getenv("MIXXX_STRETCH_BENCHMARK_BACKEND");
    if (!backendText) {
        GTEST_SKIP() << "Set MIXXX_STRETCH_BENCHMARK_BACKEND (0, 1, or 5) to measure";
    }
    const int backend = std::atoi(backendText);
    const auto* samplesText = std::getenv("MIXXX_STRETCH_BENCHMARK_SAMPLES");
    const int samples = samplesText ? std::atoi(samplesText) : kProcessBufferSize;
    ASSERT_TRUE(samples == 128 || samples == 512 || samples == 1024);
    const auto* seekText = std::getenv("MIXXX_STRETCH_BENCHMARK_SEEK_PERIOD");
    const int seekPeriod = seekText ? std::atoi(seekText) : 0;
    const auto* seekDeckText = std::getenv("MIXXX_STRETCH_BENCHMARK_SEEK_DECK");
    const int seekDeck = seekDeckText ? std::atoi(seekDeckText) : 0; // 0 means all decks.
    ASSERT_TRUE(seekDeck >= 0 && seekDeck <= 3);
    const auto* pacingText = std::getenv("MIXXX_STRETCH_BENCHMARK_PACING");
    const bool deadlinePacing = pacingText && std::string(pacingText) == "deadline";
    const auto* repeatText = std::getenv("MIXXX_STRETCH_BENCHMARK_REPEAT");
    const int repeats = repeatText ? std::atoi(repeatText) : 1;
    ASSERT_GT(repeats, 0);

    ControlObject::set(ConfigKey("[App]", "keylock_engine"), backend);
    const std::array channels{m_pChannel1, m_pChannel2, m_pChannel3};
    const std::array groups{m_sGroup1, m_sGroup2, m_sGroup3};
    for (const auto& group : groups) {
        ControlObject::set(ConfigKey(group, "quantize"), 0);
        ControlObject::set(ConfigKey(group, "keylock"), 1);
        ControlObject::set(ConfigKey(group, "rate"), 0.25);
        ControlObject::set(ConfigKey(group, "repeat"),
                1); // Repeated runs may exceed the fixture duration.
        ControlObject::set(ConfigKey(group, "play"), 1);
    }
    const auto* effectsText = std::getenv("MIXXX_STRETCH_BENCHMARK_EFFECTS");
    const bool effectsEnabled = effectsText && std::string(effectsText) == "1";
    if (effectsEnabled) {
        mixxx::Time::start();
        m_pEffectsManager->setup();
        const auto chain = m_pEffectsManager->getStandardEffectChain(0);
        ASSERT_TRUE(chain);
        for (const auto& [slotIndex, id] :
                {std::pair{0u, QStringLiteral("org.mixxx.effects.echo")},
                        std::pair{1u, QStringLiteral("org.mixxx.effects.reverb")}}) {
            const auto manifest = m_pEffectsManager->getBackendManager()->getManifest(
                    id, EffectBackendType::BuiltIn);
            ASSERT_TRUE(manifest);
            const auto slot = chain->getEffectSlot(slotIndex);
            slot->loadEffectWithDefaults(manifest);
            slot->setEnabled(true);
        }
        ControlObject::set(ConfigKey(chain->getGroup(), "enabled"), 1);
        ControlObject::set(ConfigKey(chain->getGroup(), "mix"), 0.5);
        ControlObject::set(ConfigKey(chain->getGroup(), "group_[Channel1]_enable"), 1);
        ControlObject::set(ConfigKey(chain->getGroup(), "group_[Headphone]_enable"), 1);
        ControlObject::set(ConfigKey(m_sGroup1, "pfl"), 1);
    }
    // Preparation and initial cache fill are outside the measured region.
    QTest::qWait(100);
    for (int i = 0; i < 16; ++i) {
        m_pEngineMixer->process(samples);
        QTest::qSleep(1);
    }

    constexpr int callbacksPerRepeat = 1000;
    const auto callbackTotal = static_cast<std::size_t>(callbacksPerRepeat) * repeats;
    std::vector<double> micros;
    std::array<std::size_t, 3> stalls{};
    std::array<std::size_t, 3> silentBuffers{};
    std::array<std::size_t, 3> stalledCallbacks{};
    std::array<std::size_t, 3> mutedCallbacks{};
    std::array<SeekLatency, 3> pending{};
    std::array<std::size_t, 3> canceled{};
    std::array<std::size_t, 3> unfinished{};
    std::array<std::size_t, 3> seekHeldSourceCallbacks{}, seekHeldAudibleCallbacks{};
    std::array<std::vector<double>, 3> sourceCallbackLatencies, audibleCallbackLatencies;
    std::array<std::vector<double>, 3> sourceMsLatencies, audibleMsLatencies;
    std::size_t globalCallback = 0;
    micros.reserve(callbackTotal);

    for (int repeat = 0; repeat < repeats; ++repeat) {
        for (int i = 0; i < callbacksPerRepeat; ++i, ++globalCallback) {
            if (seekPeriod > 0 && i % seekPeriod == 0) {
                const mixxx::audio::FramePos position(1000 + (i / seekPeriod % 20) * 2000);
                for (std::size_t deck = 0; deck < channels.size(); ++deck) {
                    if (seekDeck != 0 && seekDeck != static_cast<int>(deck + 1)) {
                        continue;
                    }
                    if (!pending[deck].sourceProgress || !pending[deck].audible) {
                        if (pending[deck].started != Clock::time_point{}) {
                            ++canceled[deck];
                        }
                    }
                    pending[deck] = SeekLatency{};
                    pending[deck].started = Clock::now(); // Include seekExact dispatch time.
                    pending[deck].targetFrame = position.value();
                    pending[deck].startedCallback = globalCallback;
                    channels[deck]->getEngineBuffer()->seekExact(position);
                }
            }
            const std::array before{channels[0]->getEngineBuffer()->getExactPlayPos(),
                    channels[1]->getEngineBuffer()->getExactPlayPos(),
                    channels[2]->getEngineBuffer()->getExactPlayPos()};
            const auto begin = Clock::now();
            m_pEngineMixer->process(samples);
            const auto end = Clock::now();
            micros.push_back(std::chrono::duration<double, std::micro>(end - begin).count());

            for (std::size_t deck = 0; deck < channels.size(); ++deck) {
                const auto after = channels[deck]->getEngineBuffer()->getExactPlayPos();
                const bool advanced = after != before[deck];
                if (!advanced) {
                    ++stalls[deck];
                    ++stalledCallbacks[deck];
                }
                double energy = 0;
                for (const auto sample :
                        m_pEngineMixer->getChannelBuffer(groups[deck])
                                .first(samples)) {
                    ASSERT_TRUE(util_isfinite(static_cast<double>(sample)));
                    energy += sample * sample;
                }
                const bool silent = energy < 1e-8;
                if (silent) {
                    ++silentBuffers[deck];
                    ++mutedCallbacks[deck];
                }

                auto& seek = pending[deck];
                if (seek.started == Clock::time_point{}) {
                    continue;
                }
                if (!seek.sourceProgress && after.value() > seek.targetFrame) {
                    // A frame position beyond the seek origin is required; an old crossfade
                    // alone cannot count as source progress or as audible seek completion.
                    seek.sourceProgress = true;
                    seek.sourceCallbacks = globalCallback - seek.startedCallback + 1;
                    seek.sourceMs = std::chrono::duration<double, std::milli>(
                            end - seek.started)
                                            .count();
                    sourceCallbackLatencies[deck].push_back(seek.sourceCallbacks);
                    sourceMsLatencies[deck].push_back(seek.sourceMs);
                }
                if (!seek.audible && seek.sourceProgress && !silent) {
                    seek.audible = true;
                    seek.audibleCallbacks = globalCallback - seek.startedCallback + 1;
                    seek.audibleMs = std::chrono::duration<double, std::milli>(
                            end - seek.started)
                                             .count();
                    audibleCallbackLatencies[deck].push_back(seek.audibleCallbacks);
                    audibleMsLatencies[deck].push_back(seek.audibleMs);
                }
                // Count the first held callback too: a seek-position jump or old
                // audio fade is not newly delivered source content.
                if (!seek.sourceProgress)
                    ++seekHeldSourceCallbacks[deck];
                if (!seek.audible)
                    ++seekHeldAudibleCallbacks[deck];
                if (seek.sourceProgress && seek.audible) {
                    seek.started = Clock::time_point{};
                }
            }
            // Fixed sleep remains the default for baseline comparability.
            // Optional pacing is outside the timed mixer call and waits only
            // for the remainder of the callback deadline.
            if (deadlinePacing) {
                const double deadlineMicros = samples / 2.0 / 44100 * 1e6;
                std::this_thread::sleep_until(begin +
                        std::chrono::duration_cast<Clock::duration>(
                                std::chrono::duration<double, std::micro>(deadlineMicros)));
            } else {
                QTest::qSleep(1);
            }
        }
    }
    for (std::size_t deck = 0; deck < channels.size(); ++deck) {
        if (pending[deck].started != Clock::time_point{}) {
            ++unfinished[deck];
        }
    }

    std::sort(micros.begin(), micros.end());
    const double deadlineMicros = samples / 2.0 / 44100 * 1e6;
    const auto over = std::count_if(micros.begin(), micros.end(), [deadlineMicros](double elapsed) {
        return elapsed > deadlineMicros;
    });
#if MIXXX_HAS_GETRUSAGE
    rusage usage{};
    const bool hasRss = getrusage(RUSAGE_SELF, &usage) == 0;
#endif
    std::cout << "STRETCH_WORKLOAD backend=" << backend << " decks=3 samples=" << samples
              << " seek_period=" << seekPeriod << " seek_deck=" << seekDeck
              << " repeats=" << repeats << " effects=" << effectsEnabled
              << " pacing=" << (deadlinePacing ? "deadline" : "fixed")
              << " callbacks=" << micros.size() << " deadline_us=" << deadlineMicros
              << " p50_us=" << micros[micros.size() / 2]
              << " p95_us=" << micros[(micros.size() - 1) * 95 / 100]
              << " p99_us=" << micros[(micros.size() - 1) * 99 / 100]
              << " max_us=" << micros.back() << " overruns=" << over
              << " peak_rss_kib=";
#if MIXXX_HAS_GETRUSAGE
    if (hasRss) {
#if defined(__APPLE__)
        std::cout << usage.ru_maxrss / 1024; // Darwin reports bytes, Linux reports KiB.
#else
        std::cout << usage.ru_maxrss;
#endif
    } else {
        std::cout << "unavailable(getrusage_failed)";
    }
#else
    std::cout << "unavailable(platform_unsupported)";
#endif
    for (std::size_t deck = 0; deck < channels.size(); ++deck) {
        std::cout << " deck" << deck + 1 << "_source_stalls=" << stalls[deck]
                  << " deck" << deck + 1
                  << "_silent_buffers=" << silentBuffers[deck] << " deck"
                  << deck + 1 << "_stalled_callbacks=" << stalledCallbacks[deck]
                  << " deck" << deck + 1
                  << "_muted_callbacks=" << mutedCallbacks[deck] << " deck"
                  << deck + 1 << "_seek_canceled=" << canceled[deck] << " deck"
                  << deck + 1 << "_seek_unfinished=" << unfinished[deck]
                  << " deck" << deck + 1 << "_seek_held_source_callbacks="
                  << seekHeldSourceCallbacks[deck] << " deck" << deck + 1
                  << "_seek_held_audible_callbacks="
                  << seekHeldAudibleCallbacks[deck];
        const std::string suffix = "_deck" + std::to_string(deck + 1);
        reportLatencies(("seek_source" + suffix).c_str(), sourceCallbackLatencies[deck]);
        reportMilliseconds(("seek_source" + suffix).c_str(), sourceMsLatencies[deck]);
        reportLatencies(("seek_audible" + suffix).c_str(), audibleCallbackLatencies[deck]);
        reportMilliseconds(("seek_audible" + suffix).c_str(), audibleMsLatencies[deck]);
    }
    std::cout << " audible_metric=postseek_source_progress_plus_nonzero_output_proxy"
              << " true_audio_impulse_integration=not_measured" << std::endl;
    for (std::size_t deck = 0; deck < channels.size(); ++deck) {
        EXPECT_LT(stalls[deck], callbackTotal) << "deck " << deck + 1 << " stalled throughout";
        EXPECT_LT(silentBuffers[deck], callbackTotal)
                << "deck " << deck + 1 << " was silent throughout";
    }
}
} // namespace
