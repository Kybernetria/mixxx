#include <signalsmith-stretch.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
struct Config {
    const char* name;
    int blockMs;
    int intervalMs;
    bool preset;
    bool cheaper;
};
static const Config configs[] = {{"default120_30", 120, 30, true, false},
        {"cheaper100_40", 100, 40, true, true},
        {"manual64_16", 64, 16, false, false},
        {"manual32_8", 32, 8, false, false},
        {"manual16_4", 16, 4, false, false},
        {"manual32_4", 32, 4, false, false}};
template<class Stretch>
void setup(Stretch& s, const Config& c, int sr) {
    if (c.preset) {
        if (c.cheaper)
            s.presetCheaper(2, float(sr), true);
        else
            s.presetDefault(2, float(sr), true);
    } else
        s.configure(2, sr * c.blockMs / 1000, sr * c.intervalMs / 1000, true);
}
int main() {
    std::cout << "sample_rate,config,block_samples,interval_samples,input_"
                 "latency,output_latency,frames,p50_us,p95_us,p99_us,max_us,"
                 "overruns,deadline_us,impulse_peak_frame,peak_abs\n";
    constexpr int block = 128, duration = 3;
    for (int sr : {44100, 48000, 96000})
        for (const auto& c : configs) {
            signalsmith::stretch::SignalsmithStretch<float> s{0};
            setup(s, c, sr);
            s.setTransposeFactor(1);
            s.setFormantFactor(1, true);
            int total = sr * duration, impulse = sr / 2;
            std::array<std::vector<float>, 2> in{
                    std::vector<float>(block), std::vector<float>(block)};
            std::array<std::vector<float>, 2> out{
                    std::vector<float>(block), std::vector<float>(block)};
            std::vector<float> rendered(total * 2, 0);
            std::vector<double> costs;
            int over = 0;
            double deadline = block * 1e6 / sr;
            for (int pos = 0; pos < total; pos += block) {
                int n = std::min(block, total - pos);
                for (int i = 0; i < n; ++i) {
                    float x = (pos + i == impulse) ? 1.f : 0.f;
                    in[0][i] = x;
                    in[1][i] = x;
                }
                std::fill(out[0].begin(), out[0].end(), 0);
                std::fill(out[1].begin(), out[1].end(), 0);
                auto start = Clock::now();
                s.process(in, n, out, n);
                auto end = Clock::now();
                double us = std::chrono::duration<double, std::micro>(end - start).count();
                if (pos > sr / 2)
                    costs.push_back(us);
                if (us > deadline)
                    ++over;
                for (int i = 0; i < n; ++i) {
                    rendered[(pos + i) * 2] = out[0][i];
                    rendered[(pos + i) * 2 + 1] = out[1][i];
                }
            }
            auto percentile = [&](double p) {
                auto v = costs;
                std::sort(v.begin(), v.end());
                return v[std::min(v.size() - 1, size_t(p * (v.size() - 1)))];
            };
            int peak = 0;
            float peakv = 0;
            for (int i = 0; i < total; ++i)
                if (std::abs(rendered[i * 2]) > peakv) {
                    peakv = std::abs(rendered[i * 2]);
                    peak = i;
                }
            std::string name = std::string(c.name) + "_" + std::to_string(sr);
            std::ofstream wav(
                    std::string("/tmp/mixxx-pitchshift-investigation/") + name +
                            ".f32",
                    std::ios::binary);
            wav.write(reinterpret_cast<const char*>(rendered.data()),
                    rendered.size() * sizeof(float));
            std::cout << sr << ',' << c.name << ',' << s.blockSamples() << ','
                      << s.intervalSamples() << ',' << s.inputLatency() << ','
                      << s.outputLatency() << ',' << total << ','
                      << percentile(.50) << ',' << percentile(.95) << ','
                      << percentile(.99) << ','
                      << *std::max_element(costs.begin(), costs.end()) << ','
                      << over << ',' << deadline << ',' << peak << ',' << peakv
                      << '\n';
        }
}
