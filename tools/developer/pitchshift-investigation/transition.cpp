#define main timing_benchmark_unused
#include "/tmp/mixxx-pitchshift-investigation/timing.cpp"
#undef main

#include <cstdio>
#include <limits>

namespace {
constexpr int kRate = 44100;
constexpr int kFrames = 64;
constexpr int kSeconds = 3;
constexpr int kTotal = kRate * kSeconds;
constexpr double kPi = 3.14159265358979323846;

double q(double pitch, double range, bool semitones) {
    double x = pitch * range;
    if (semitones) {
        double whole;
        const double frac = std::modf(x, &whole);
        x = whole + std::round(frac * 12) / 12;
    }
    return x;
}

struct Ring {
    std::vector<float> data;
    std::size_t pos = 0;
    explicit Ring(std::size_t n)
            : data(n, 0.0f) {
    }
    float push(float x) {
        float y = data[pos];
        data[pos] = x;
        pos = (pos + 1) % data.size();
        return y;
    }
};

float fixture(int n) {
    const double t = double(n) / kRate;
    double x = .23 * sin(2 * kPi * 70 * t) + .14 * sin(2 * kPi * 440 * t) +
            .06 * sin(2 * kPi * 880 * t);
    // Deterministic transient once per second; the test is intentionally synthetic.
    if (n % kRate < 32)
        x += (n % 2 ? -0.65 : 0.65) * (1.0 - double(n % kRate) / 32);
    return float(x);
}
} // namespace

int main() {
    std::ofstream controls("/tmp/mixxx-pitchshift-investigation/control-results.csv");
    controls << "pitch,range,mode,q,ratio,expected_q,pass\n";
    int controlCases = 0, controlFailures = 0;
    for (double pitch : {-1., -.75, -.5, -1. / 24., 0., 1. / 24., .5, .75, 1.}) {
        for (double range : {0., .5, 1., 2.}) {
            for (bool semis : {false, true}) {
                const double actual = q(pitch, range, semis);
                // Compute expected using the production modf/round rule.
                double whole = 0;
                const double frac = std::modf(pitch * range, &whole);
                const double exact = semis ? whole + std::round(frac * 12) / 12 : pitch * range;
                const bool pass = std::abs(actual - exact) < 1e-12 &&
                        std::isfinite(std::pow(2., actual));
                controlFailures += !pass;
                ++controlCases;
                controls << pitch << ',' << range << ','
                         << (semis ? "semitones" : "continuous") << ','
                         << std::setprecision(12) << actual << ','
                         << std::pow(2., actual) << ',' << exact << ',' << pass
                         << '\n';
            }
        }
    }
    controls.close();

    signalsmith::stretch::SignalsmithStretch<float> toggle{0}, other{0}, reference{0};
    const Config isolationConfig = {"manual16_4", 16, 4, false, false};
    setup(toggle, isolationConfig, kRate);
    setup(other, isolationConfig, kRate);
    setup(reference, isolationConfig, kRate);
    std::array<std::vector<float>, 2> ti{
            std::vector<float>(kFrames), std::vector<float>(kFrames)},
            oi{std::vector<float>(kFrames), std::vector<float>(kFrames)},
            ri{std::vector<float>(kFrames), std::vector<float>(kFrames)},
            to{std::vector<float>(kFrames), std::vector<float>(kFrames)},
            oo{std::vector<float>(kFrames), std::vector<float>(kFrames)},
            ro{std::vector<float>(kFrames), std::vector<float>(kFrames)};
    bool formantIsolation = true;
    for (int cb = 0; cb < 200; ++cb) {
        for (int i = 0; i < kFrames; ++i)
            ti[0][i] = ti[1][i] = oi[0][i] = oi[1][i] = ri[0][i] = ri[1][i] =
                    fixture(cb * kFrames + i);
        float *ta[2] = {ti[0].data(), ti[1].data()},
              *oa[2] = {oi[0].data(), oi[1].data()},
              *ra[2] = {ri[0].data(), ri[1].data()};
        float *tb[2] = {to[0].data(), to[1].data()},
              *ob[2] = {oo[0].data(), oo[1].data()},
              *rb[2] = {ro[0].data(), ro[1].data()};
        toggle.setTransposeFactor(std::sqrt(2.0));
        toggle.setFormantFactor(1.0, (cb & 1) != 0);
        other.setTransposeFactor(std::sqrt(2.0));
        other.setFormantFactor(1.0, false);
        reference.setTransposeFactor(std::sqrt(2.0));
        toggle.process(ta, kFrames, tb, kFrames);
        other.process(oa, kFrames, ob, kFrames);
        reference.process(ra, kFrames, rb, kFrames);
        for (int i = 0; i < kFrames; ++i)
            formantIsolation &= oo[0][i] == ro[0][i] && oo[1][i] == ro[1][i];
    }

    std::ofstream csv("/tmp/mixxx-pitchshift-investigation/transition-results.csv");
    csv << "config,latency_frames,reset_us,reset_new,reset_delete,stress_"
           "callbacks,stress_us,stress_new,stress_delete,finite,initialized,"
           "step_max,fade_peak_diff,fade_energy_min_ratio,fade_energy_median_"
           "ratio\n";
    std::ofstream wavlog("/tmp/mixxx-pitchshift-investigation/transition.wav.log");
    const Config selected[] = {{"manual16_4", 16, 4, false, false},
            {"manual32_4", 32, 4, false, false},
            {"default120_30", 120, 30, true, false}};
    int finiteAll = 1;
    for (const auto& cfg : selected) {
        signalsmith::stretch::SignalsmithStretch<float> stretch{0};
        setup(stretch, cfg, kRate);
        const int latency = int(stretch.inputLatency() + stretch.outputLatency());
        std::array<std::vector<float>, 2> in{
                std::vector<float>(kFrames), std::vector<float>(kFrames)};
        std::array<std::vector<float>, 2> out{
                std::vector<float>(kFrames), std::vector<float>(kFrames)};
        // The object and all callback buffers are created before measured reset/work.
        auto reset = measured([&] { stretch.reset(); });
        Ring dry(latency + 1);
        std::vector<float> rendered(kTotal);
        std::vector<float> dryNow(kTotal), wet(kTotal);
        std::vector<double> energy;
        energy.reserve(20);
        bool initialized = true, finite = true;
        const int enableAt = kRate, disableAt = 2 * kRate, fadeFrames = kRate / 50;
        double stepMax = 0, peakDiff = 0;
        std::vector<double> ratios;
        ratios.reserve(20);
        std::size_t stressNew = 0, stressDelete = 0;
        const auto begin = Clock::now();
        for (int base = 0, callback = 0; base < kTotal; base += kFrames, ++callback) {
            const int frames = std::min(kFrames, kTotal - base);
            for (int i = 0; i < frames; ++i)
                in[0][i] = in[1][i] = fixture(base + i);
            // No allocating APIs beyond this point; stress control/formant changes each callback.
            if (callback < 2000)
                countStart();
            stretch.setTransposeFactor(std::pow(2.,
                    q((callback & 1) ? 1. : -1.,
                            (callback % 3 == 0)
                                    ? 0.
                                    : ((callback % 3 == 1) ? .5 : 2.),
                            false)));
            stretch.setFormantFactor(1.0, (callback & 1) != 0);
            float* input[2] = {in[0].data(), in[1].data()};
            float* output[2] = {out[0].data(), out[1].data()};
            stretch.process(input, frames, output, frames);
            if (callback < 2000) {
                auto c = countStop();
                stressNew += c.alloc;
                stressDelete += c.free;
            }
            for (int i = 0; i < frames; ++i) {
                const int n = base + i;
                const float x = fixture(n), w = out[0][i], d = dry.push(x);
                wet[n] = w;
                dryNow[n] = x;
                // Full bypass is the exact current input, with no delayed ring tail.
                double gain = 0;
                if (n >= enableAt && n < disableAt)
                    gain = std::min(1.0, double(n - enableAt) / fadeFrames);
                else if (n >= disableAt && n < disableAt + fadeFrames)
                    gain = 1.0 - double(n - disableAt) / fadeFrames;
                else if (n >= disableAt + fadeFrames)
                    gain = 0;
                const float y = float((1. - gain) * x + gain * w);
                rendered[n] = y;
                if (!std::isfinite(w) || !std::isfinite(y))
                    finite = false;
                // At bypass points this compares bit-exactly after float assignment.
                if ((n < enableAt || n >= disableAt + fadeFrames) && y != x)
                    initialized = false;
                if (n > 0)
                    stepMax = std::max(stepMax, std::abs(double(rendered[n] - rendered[n - 1])));
                if (n >= enableAt && n < disableAt + fadeFrames)
                    peakDiff = std::max(peakDiff, std::abs(double(y - x)));
            }
        }
        const auto elapsed =
                std::chrono::duration<double, std::micro>(Clock::now() - begin)
                        .count();
        // Quantify output energy against source in 20ms windows around both toggles.
        for (int center : {enableAt, disableAt})
            for (int b = center - fadeFrames / 2; b < center + fadeFrames / 2; b += kRate / 100) {
                double ey = 0, ex = 0;
                for (int n = b; n < std::min(b + kRate / 100, kTotal); ++n) {
                    ey += rendered[n] * rendered[n];
                    ex += dryNow[n] * dryNow[n];
                }
                ratios.push_back(ex > 0 ? ey / ex : 0);
            }
        std::sort(ratios.begin(), ratios.end());
        const double med = ratios[ratios.size() / 2];
        const double min = *std::min_element(ratios.begin(), ratios.end());
        csv << cfg.name << ',' << latency << ',' << reset.us << ','
            << reset.counts.alloc << ',' << reset.counts.free << ",2000,"
            << elapsed << ',' << stressNew << ',' << stressDelete << ','
            << finite << ',' << initialized << ',' << stepMax << ',' << peakDiff
            << ',' << min << ',' << med << '\n';
        finiteAll &= finite && initialized;
        wavlog << cfg.name << " latency_frames=" << latency
               << " latency_ms=" << 1000. * latency / kRate
               << " finite=" << finite << " initialized=" << initialized
               << "\n";
    }
    // 16-bit PCM stereo WAV for human inspection; fixed candidate configuration
    // manual32_4. Regenerate with the focused chosen config so candidate
    // transition has a single listening artifact.
    signalsmith::stretch::SignalsmithStretch<float> s{0};
    setup(s, selected[1], kRate);
    std::vector<float> left(kTotal), right(kTotal);
    std::array<std::vector<float>, 2> pi{
            std::vector<float>(kFrames), std::vector<float>(kFrames)},
            po{std::vector<float>(kFrames), std::vector<float>(kFrames)};
    for (int base = 0, cb = 0; base < kTotal; base += kFrames, ++cb) {
        int nframes = std::min(kFrames, kTotal - base);
        for (int i = 0; i < nframes; ++i)
            pi[0][i] = pi[1][i] = fixture(base + i);
        s.setTransposeFactor(1.25);
        s.setFormantFactor(1., (cb & 1) != 0);
        float* a[2] = {pi[0].data(), pi[1].data()};
        float* b[2] = {po[0].data(), po[1].data()};
        s.process(a, nframes, b, nframes);
        for (int i = 0; i < nframes; ++i) {
            int n = base + i;
            double g = n >= kRate && n < 2 * kRate
                    ? std::min(1., double(n - kRate) / (kRate / 50))
                    : n >= 2 * kRate && n < 2 * kRate + kRate / 50
                    ? 1. - double(n - 2 * kRate) / (kRate / 50)
                    : n >= 2 * kRate + kRate / 50 ? 0.
                                                  : 0.;
            left[n] = float((1 - g) * fixture(n) + g * po[0][i]);
            right[n] = left[n];
        }
    }
    std::ofstream wav(
            "/tmp/mixxx-pitchshift-investigation/transition-manual32_4.wav",
            std::ios::binary);
    const std::uint32_t dataBytes = kTotal * 4;
    auto u16 = [&](std::uint16_t v) {wav.put(char(v));wav.put(char(v>>8)); };
    auto u32 = [&](std::uint32_t v) {u16(v&65535);u16(v>>16); };
    wav.write("RIFF", 4);
    u32(36 + dataBytes);
    wav.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(kRate);
    u32(kRate * 4);
    u16(4);
    u16(16);
    wav.write("data", 4);
    u32(dataBytes);
    for (int i = 0; i < kTotal; ++i)
        for (float x : {left[i], right[i]}) {
            int v = std::clamp(int(std::lrint(std::clamp(x, -1.f, 1.f) * 32767)), -32768, 32767);
            u16(std::uint16_t(std::int16_t(v)));
        }
    wav.close();
    wavlog << "wav=transition-manual32_4.wav stereo synthetic "
              "70/440/880Hz+transients, wet ratio 1.25; transition model is "
              "dry->wet blend (not latency aligned)\n";
    std::ofstream report("/tmp/mixxx-pitchshift-investigation/report-transitions.md");
    report << "# Focused PitchShift scalar/control and bypass-transition "
              "probe\n\n"
           << "Disposable standalone probe only; no repository source/build or "
              "host engine integration was changed. Signalsmith API is "
              "exercised directly. `timing.cpp` is included with its main "
              "renamed for the existing thread-local C++ allocation counter "
              "and setup helper.\n\n"
           << "## Controls and processing\n\n"
           << controlCases
           << " control cases (9 pitch positions including +/- half-semitone "
              "boundaries, 4 ranges 0/0.5/1/2, continuous and semitone modes): "
           << (controlFailures ? "FAIL" : "PASS")
           << " against `modf` + `round(fraction*12)` / `pow(2,q)` semantics. "
              "Automation stress called transpose/formant setters per callback "
              "for 2,000 calls; measurement row is in CSV. A three-instance "
              "isolation check toggled formants on one instance while a second "
              "stayed false; its output matched an untouched reference "
              "bit-for-bit: "
           << (formantIsolation ? "yes" : "no")
           << ". All scratch storage was allocated before callback loops. "
              "Output finite and bypass samples initialized/exact in tested "
              "renderer: "
           << (finiteAll ? "yes" : "no") << ".\n\n"
           << "Reset is measured separately after object construction; reset "
              "cost/allocation fields are reported per config. C++ new/delete "
              "counters do not cover C allocation, internal synchronization, "
              "or host effects.\n\n"
           << "## Transition experiment\n\nManual 16/4, 32/4, and default "
              "120/30 stereo configurations at 44.1 kHz; reported latency is "
              "input+output latency. Synthetic 70/440/880 Hz plus transients; "
              "20 ms linear blend from current dry to currently emitted wet, "
              "then back to exact immediate dry. This intentionally exposes "
              "whether an unaligned wet blend is a plausible transition, not a "
              "proposed/adopted production policy. WAV is "
              "`transition-manual32_4.wav`. CSV reports transition peak "
              "deviation from source and 10 ms energy ratios, alongside max "
              "adjacent-sample delta.\n\n"
           << "A delayed wet timeline and current immediate dry timeline "
              "cannot generally be phase-aligned; full immediate bypass cannot "
              "retain delayed wet output. A fade can reduce a hard step but "
              "may create cancellation/combing or an audible gap. A candidate "
              "delayed-dry alignment would need a fixed-latency ring "
              "initialized off callback and deliberate enable/disable timeline "
              "semantics; it was not silently adopted here. Warm-up/history "
              "and stale-state re-enable are not validated.\n\n"
           << "## Limits\n\nNo actual `PitchShiftEffect`/`EngineEffectChain` "
              "integration, no shared delay-history or effect-enable "
              "transition behavior, no real program material/listening "
              "validation, no production callback deadline test. Direct API "
              "probe cannot establish existing host correctness or approve "
              "implementation. Values are host-session observations.\n";
    return (controlFailures || !finiteAll) ? 1 : 0;
}
