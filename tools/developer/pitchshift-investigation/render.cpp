#include <rubberband/RubberBandStretcher.h>
#include <signalsmith-stretch.h>
#include <sndfile.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

struct Config {
    const char* name;
    int blockMs, intervalMs;
};
constexpr Config configs[] = {{"default120_30", 120, 30},
        {"cheaper100_40", 100, 40},
        {"manual64_16", 64, 16},
        {"manual32_8", 32, 8},
        {"manual16_4", 16, 4},
        {"manual32_4", 32, 4}};
int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    SF_INFO info{};
    SNDFILE* file = sf_open(argv[1], SFM_READ, &info);
    if (!file || info.channels != 2) {
        std::cerr << "stereo input required\n";
        return 3;
    }
    std::vector<float> input(info.frames * 2);
    if (sf_readf_float(file, input.data(), info.frames) != info.frames)
        return 4;
    sf_close(file);
    std::cout << "config,octaves,formants,latency_frames,returned_frames,finite\n";
    constexpr int block = 256;
    const int total = int(info.frames) + info.samplerate;
    std::array<std::vector<float>, 2> in{std::vector<float>(block), std::vector<float>(block)},
            out{std::vector<float>(block), std::vector<float>(block)};
    for (int ci = 0; ci < 8; ++ci)
        for (double octaves : {0.0, 0.5, -0.5, 1.0, -1.0, 2.0, -2.0})
            for (bool preserve : {false, true}) {
                std::string name = ci < 6
                        ? configs[ci].name
                        : (ci == 6 ? "rubberband_actual_rate"
                                   : "rubberband_placeholder96k");
                signalsmith::stretch::SignalsmithStretch<float> s{0};
                std::unique_ptr<RubberBand::RubberBandStretcher> rb;
                int latency = 0;
                if (ci < 6) {
                    const auto& c = configs[ci];
                    s.configure(2,
                            info.samplerate * c.blockMs / 1000,
                            info.samplerate * c.intervalMs / 1000,
                            true);
                    s.setTransposeFactor(float(std::pow(2.0, octaves)));
                    s.setFormantFactor(1, preserve);
                    s.setFormantBase(0);
                    latency = s.inputLatency() + s.outputLatency();
                } else {
                    rb = std::make_unique<RubberBand::RubberBandStretcher>(
                            ci == 6 ? info.samplerate : 96000,
                            2,
                            RubberBand::RubberBandStretcher::
                                    OptionProcessRealTime);
                    rb->setMaxProcessSize(8192);
                    rb->setTimeRatio(1);
                    rb->setFormantOption(preserve
                                    ? RubberBand::RubberBandStretcher::
                                              OptionFormantPreserved
                                    : RubberBand::RubberBandStretcher::
                                              OptionFormantShifted);
                    rb->setPitchScale(std::pow(2.0, octaves));
                    latency = int(rb->getLatency());
                }
                std::vector<float> rendered(total * 2, 0);
                long received = 0;
                bool finite = true;
                for (int pos = 0; pos < total; pos += block) {
                    int n = std::min(block, total - pos);
                    for (int i = 0; i < n; ++i)
                        for (int ch = 0; ch < 2; ++ch)
                            in[ch][i] = pos + i < info.frames ? input[(pos + i) * 2 + ch] : 0;
                    for (auto& c : out)
                        std::fill(c.begin(), c.end(), 0);
                    int count = n;
                    float* ip[] = {in[0].data(), in[1].data()};
                    float* op[] = {out[0].data(), out[1].data()};
                    if (ci < 6)
                        s.process(ip, n, op, n);
                    else {
                        rb->process(ip, n, false);
                        count = int(rb->retrieve(op,
                                std::min<size_t>(
                                        n, std::max(0, rb->available()))));
                    }
                    received += count;
                    for (int i = 0; i < n; ++i)
                        for (int ch = 0; ch < 2; ++ch) {
                            rendered[(pos + i) * 2 + ch] = out[ch][i];
                            finite &= std::isfinite(out[ch][i]);
                        }
                }
                const std::string path = std::string(argv[2]) + "/" + name +
                        "_q" + std::to_string(octaves) + "_f" +
                        std::to_string(preserve) + ".wav";
                SF_INFO outputInfo{};
                outputInfo.samplerate = info.samplerate;
                outputInfo.channels = 2;
                outputInfo.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
                SNDFILE* output = sf_open(path.c_str(), SFM_WRITE, &outputInfo);
                if (!output || sf_writef_float(output, rendered.data(), total) != total)
                    return 5;
                sf_close(output);
                std::cout << name << ',' << octaves << ',' << preserve << ','
                          << latency << ',' << received << ',' << finite
                          << '\n';
            }
}
