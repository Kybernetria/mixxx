#include <rubberband/RubberBandStretcher.h>
#include <signalsmith-stretch.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <new>
#include <string>
#include <vector>

namespace {
thread_local bool countAllocations = false;
thread_local std::size_t allocations = 0;
thread_local std::size_t deallocations = 0;

struct Config {
    const char* name;
    int blockMs;
    int intervalMs;
    bool preset;
    bool cheaper;
};
constexpr Config configs[] = {
        {"default120_30", 120, 30, true, false},
        {"cheaper100_40", 100, 40, true, true},
        {"manual64_16", 64, 16, false, false},
        {"manual32_8", 32, 8, false, false},
        {"manual16_4", 16, 4, false, false},
        {"manual32_4", 32, 4, false, false},
};
using Clock = std::chrono::steady_clock;

struct Counts {
    std::size_t alloc;
    std::size_t free;
};
struct Measurement {
    double us;
    Counts counts;
};

void countStart() {
    allocations = deallocations = 0;
    countAllocations = true;
}
Counts countStop() {
    countAllocations = false;
    return {allocations, deallocations};
}

template<class Callback>
Measurement measured(Callback&& callback) {
    countStart();
    const auto begin = Clock::now();
    callback();
    const auto end = Clock::now();
    const auto counts = countStop();
    return {std::chrono::duration<double, std::micro>(end - begin).count(), counts};
}

void setup(signalsmith::stretch::SignalsmithStretch<float>& stretch,
        const Config& config,
        int sampleRate) {
    if (config.preset) {
        if (config.cheaper) {
            stretch.presetCheaper(2, float(sampleRate), true);
        } else {
            stretch.presetDefault(2, float(sampleRate), true);
        }
    } else {
        stretch.configure(2,
                sampleRate * config.blockMs / 1000,
                sampleRate * config.intervalMs / 1000,
                true);
    }
}

void makeInput(std::vector<float>& interleaved, int frames, int sampleRate, int offset) {
    // Fixed, nonsilent harmonics with a slow deterministic envelope; no fixture I/O.
    for (int i = 0; i < frames; ++i) {
        const double t = double(offset + i) / sampleRate;
        const double envelope = 0.65 + 0.25 * std::sin(2.0 * 3.141592653589793 * 0.37 * t);
        const float left = float(envelope *
                (0.31 * std::sin(2.0 * 3.141592653589793 * 173.0 * t) +
                        0.17 * std::sin(2.0 * 3.141592653589793 * 347.0 * t) +
                        0.09 * std::sin(2.0 * 3.141592653589793 * 691.0 * t)));
        const float right = float(envelope *
                (0.29 * std::sin(2.0 * 3.141592653589793 * 181.0 * t) +
                        0.13 * std::sin(2.0 * 3.141592653589793 * 367.0 * t) +
                        0.07 * std::sin(2.0 * 3.141592653589793 * 733.0 * t)));
        interleaved[2 * i] = left;
        interleaved[2 * i + 1] = right;
    }
}

// Approximate the production continuous-range mapping on every callback:
// pitch knob alternates -1/+1, range is 2 octaves, and pitch ratio is pow(2, product).
double pitchRatio(int callbackIndex) {
    const double pitchControl = (callbackIndex & 1) ? 1.0 : -1.0;
    constexpr double range = 2.0;
    const double pitchParameter = pitchControl * range;
    return std::pow(2.0, pitchParameter);
}

void writeRow(std::ofstream& csv,
        int sr,
        const std::string& backend,
        const std::string& config,
        int samples,
        int rep,
        const Measurement& first,
        const std::vector<double>& steady,
        Counts steadyAlloc,
        Counts steadyFree) {
    auto sorted = steady;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) { return sorted[std::size_t(p * (sorted.size() - 1))]; };
    csv << sr << ',' << backend << ',' << config << ',' << samples << ',' << samples / 2 << ','
        << rep << ',' << std::setprecision(9) << first.us << ',' << pct(.50) << ','
        << pct(.95) << ',' << pct(.99) << ',' << sorted.back() << ','
        << first.counts.alloc << ',' << first.counts.free << ','
        << steadyAlloc.alloc << ',' << steadyFree.free << ',' << steady.size() << ','
        << (samples / 2.0 * 1e6 / sr) << ','
        << (std::count_if(sorted.begin(), sorted.end(), [&](double us) {
               return us > samples / 2.0 * 1e6 / sr;
           }) + (first.us > samples / 2.0 * 1e6 / sr))
        << '\n';
}

void runSignalsmith(std::ofstream& csv, const Config& config, int sr, int samples, int rep) {
    signalsmith::stretch::SignalsmithStretch<float> stretch{0};
    setup(stretch, config, sr);
    std::array<std::vector<float>, 2> planarIn{
            std::vector<float>(samples / 2), std::vector<float>(samples / 2)};
    std::array<std::vector<float>, 2> planarOut{
            std::vector<float>(samples / 2), std::vector<float>(samples / 2)};
    std::vector<float> input(samples), output(samples, 0.0f);
    std::vector<double> steady;
    steady.reserve(999);
    Counts allocSum{}, freeSum{};
    auto callback = [&](int index) {
        const int frames = samples / 2;
        const double ratio = pitchRatio(index);
        const bool preserveFormants = (index & 1) != 0;
        for (int i = 0; i < frames; ++i) {
            planarIn[0][i] = input[2 * i];
            planarIn[1][i] = input[2 * i + 1];
        }
        stretch.setTransposeFactor(ratio);
        stretch.setFormantFactor(1.0, preserveFormants);
        stretch.process(planarIn.data(), frames, planarOut.data(), frames);
        for (int i = 0; i < frames; ++i) {
            output[2 * i] = planarOut[0][i];
            output[2 * i + 1] = planarOut[1][i];
        }
    };
    makeInput(input, samples / 2, sr, 0);
    std::fill(output.begin(), output.end(), 0.0f);
    const auto first = measured([&] { callback(0); });
    for (int i = 1; i < 1000; ++i) {
        makeInput(input, samples / 2, sr, i * (samples / 2));
        std::fill(output.begin(), output.end(), 0.0f);
        const auto result = measured([&] { callback(i); });
        steady.push_back(result.us);
        allocSum.alloc += result.counts.alloc;
        freeSum.free += result.counts.free;
    }
    writeRow(csv, sr, "signalsmith", config.name, samples, rep, first, steady, allocSum, freeSum);
}

void runRubberBand(std::ofstream& csv, int sr, int samples, int rep, int constructorRate) {
    // Mirrors PitchShiftEffect's realtime constructor, maximum process size,
    // per-callback pow pitch mapping, formant option transition, planar conversion,
    // process/available/retrieve/interleave. Output tail is explicitly zeroed; the
    // production retrieve buffer itself is not initialized before partial retrieval.
    RubberBand::RubberBandStretcher stretcher(constructorRate,
            2,
            RubberBand::RubberBandStretcher::OptionProcessRealTime);
    // Match the real EngineEffect constructor's kMaxEngineFrames capacity.
    stretcher.setMaxProcessSize(8192);
    stretcher.setTimeRatio(1.0);
    std::array<std::vector<float>, 2> planar{std::vector<float>(samples / 2, 0.0f),
            std::vector<float>(samples / 2, 0.0f)};
    std::array<float*, 2> pointers{planar[0].data(), planar[1].data()};
    std::vector<float> input(samples), output(samples, 0.0f);
    std::vector<double> steady;
    steady.reserve(999);
    Counts allocSum{}, freeSum{};
    bool currentFormant = false;
    auto callback = [&](int index) {
        const int frames = samples / 2;
        const double ratio = pitchRatio(index);
        const bool preserveFormants = (index & 1) != 0;
        if (currentFormant != preserveFormants) {
            currentFormant = preserveFormants;
            stretcher.setFormantOption(currentFormant
                            ? RubberBand::RubberBandStretcher::OptionFormantPreserved
                            : RubberBand::RubberBandStretcher::OptionFormantShifted);
        }
        stretcher.setPitchScale(ratio);
        for (int i = 0; i < frames; ++i) {
            planar[0][i] = input[2 * i];
            planar[1][i] = input[2 * i + 1];
        }
        stretcher.process(pointers.data(), frames, false);
        const int available = stretcher.available();
        const int requested = std::min(available, frames);
        const int received = stretcher.retrieve(pointers.data(), requested);
        for (int i = 0; i < received; ++i) {
            output[2 * i] = planar[0][i];
            output[2 * i + 1] = planar[1][i];
        }
    };
    makeInput(input, samples / 2, sr, 0);
    std::fill(output.begin(), output.end(), 0.0f);
    const auto first = measured([&] { callback(0); });
    for (int i = 1; i < 1000; ++i) {
        makeInput(input, samples / 2, sr, i * (samples / 2));
        std::fill(output.begin(), output.end(), 0.0f);
        const auto result = measured([&] { callback(i); });
        steady.push_back(result.us);
        allocSum.alloc += result.counts.alloc;
        freeSum.free += result.counts.free;
    }
    writeRow(csv,
            sr,
            "rubberband",
            constructorRate == sr ? "actual-rate" : "96000-placeholder",
            samples,
            rep,
            first,
            steady,
            allocSum,
            freeSum);
}
} // namespace

void* operator new(std::size_t size) {
    if (countAllocations)
        ++allocations;
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}
void operator delete(void* p) noexcept {
    if (p && countAllocations)
        ++deallocations;
    std::free(p);
}
void operator delete[](void* p) noexcept {
    ::operator delete(p);
}
void operator delete(void* p, std::size_t) noexcept {
    ::operator delete(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    ::operator delete(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    ::operator delete(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    ::operator delete(p);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    if (countAllocations)
        ++allocations;
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(alignment), size ? size : 1) != 0)
        p = nullptr;
    if (p)
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size,
        std::align_val_t alignment,
        const std::nothrow_t& tag) noexcept {
    return ::operator new(size, alignment, tag);
}
void operator delete(void* p, std::align_val_t) noexcept {
    if (p && countAllocations)
        ++deallocations;
    std::free(p);
}
void operator delete[](void* p, std::align_val_t alignment) noexcept {
    ::operator delete(p, alignment);
}
void operator delete(void* p, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(p, alignment);
}
void operator delete[](void* p, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(p, alignment);
}
void operator delete(void* p, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(p, alignment);
}
void operator delete[](void* p, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(p, alignment);
}

int main() {
    const char* outputPath = "/tmp/mixxx-pitchshift-investigation/results-timing.csv";
    std::ofstream csv(outputPath);
    if (!csv)
        return 2;
    csv << "sample_rate,backend,config,interleaved_samples,frames,repetition,"
           "first_us,steady_p50_us,steady_p95_us,steady_p99_us,steady_max_us,"
           "first_cpp_new,first_cpp_delete,steady_cpp_new_total,steady_cpp_"
           "delete_total,steady_callbacks,deadline_us,overruns\n";
    for (const int sr : {44100, 48000, 96000}) {
        for (const int samples : {128, 512, 1024}) {
            for (int rep = 1; rep <= 3; ++rep) {
                for (const auto& config : configs)
                    runSignalsmith(csv, config, sr, samples, rep);
                runRubberBand(csv, sr, samples, rep, sr);
                if (sr != 96000)
                    runRubberBand(csv, sr, samples, rep, 96000);
                csv.flush();
            }
        }
    }
    return csv ? 0 : 3;
}
