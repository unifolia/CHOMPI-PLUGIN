// OutputStage and RateAdapter against what their headers promise. No JUCE.
// Thresholds sit a little outside what was measured (2026-10-17): passband
// within 0.001 dB, spurious content -91 dB at worst, stopband -90 dB at its
// edge, reported latency within 0.34 samples of the measured delay.
#include "OutputStage.h"
#include "RateAdapter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
int failures = 0;
#define CHECK(cond, ...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if(!(cond))                                                                                                    \
        {                                                                                                              \
            failures++;                                                                                                \
            std::printf("FAIL %s:%d ", __FILE__, __LINE__);                                                            \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while(0)

constexpr double kPi = 3.14159265358979323846;
constexpr double kFw = 48000;

double db(double x) { return 20 * std::log10(x + 1e-30); }

// ---- OutputStage ----------------------------------------------------------

constexpr int L = OutputStage::kLookahead;

// In 24-sample blocks, as the processor runs it.
void run(OutputStage& s, std::vector<float>& l, std::vector<float>& r)
{
    for(size_t i = 0; i < l.size(); i += 24)
        s.process(&l[i], &r[i], int(std::min<size_t>(24, l.size() - i)));
}

void testOutputStage()
{
    std::mt19937                          rng(1);
    std::uniform_real_distribution<float> uni(-1.f, 1.f);
    const float                           makeup = 7.9432823f; // +18 dB

    // Below the ceiling: exactly the makeup, delayed by L.
    {
        OutputStage s;
        s.setMakeup(makeup);
        const int          n = 48000;
        std::vector<float> l(n), r(n);
        for(int i = 0; i < n; i++)
        {
            l[size_t(i)] = uni(rng) * 0.1f; // x 7.94 = 0.794 < 0.891
            r[size_t(i)] = uni(rng) * 0.1f;
        }
        const auto il = l, ir = r;
        run(s, l, r);
        int mismatches = 0;
        for(size_t i = 0; i < size_t(n); i++)
        {
            const float el = i >= L ? il[i - L] * makeup : 0.f;
            const float er = i >= L ? ir[i - L] * makeup : 0.f;
            mismatches += (l[i] != el) + (r[i] != er);
        }
        CHECK(mismatches == 0, "output stage: %d samples not makeup x input below the ceiling", mismatches);
    }

    // Never over the ceiling.
    {
        OutputStage s;
        s.setMakeup(makeup);
        const int          n = 24000 * 9;
        std::vector<float> l(n), r(n);
        for(int i = 0; i < n; i++)
        {
            const double t   = i / kFw;
            const int    seg = i / 24000;
            float        x   = 0;
            switch(seg)
            {
                case 0: x = float(std::sin(2 * kPi * 50 * t)); break;           // +18 dB over
                case 1: x = 0.05f * float(std::sin(2 * kPi * 1000 * t)); break; // under
                case 2: x = uni(rng) * 3.f; break;
                case 3: x = (i % 4800 == 0) ? 10.f : 0.f; break; // impulses
                case 4: x = ((i / 480) % 2) ? 0.5f * float(std::sin(2 * kPi * 220 * t)) : 0.01f; break;
                case 5: x = 1.f; break;
                case 6: x = uni(rng) * 100.f; break;
                case 7: x = 0.f; break;
                case 8: x = OutputStage::kCeiling * 1.03f / makeup * float(std::sin(2 * kPi * 3000 * t)); break; // just over
            }
            l[size_t(i)] = x;
            r[size_t(i)] = (seg % 2) ? x * 0.1f : -x;
        }
        run(s, l, r);
        float worst = 0;
        for(size_t i = 0; i < size_t(n); i++)
            worst = std::max({worst, std::abs(l[i]), std::abs(r[i])});
        CHECK(worst <= OutputStage::kCeiling * (1 + 1e-6f), "output stage: peak %.9f over the ceiling", worst);
    }

    // Linked: the quiet channel takes the loud one's gain.
    {
        OutputStage s;
        std::vector<float> l(4800, 2.f), r(4800, 0.5f);
        run(s, l, r);
        CHECK(std::abs(r[4000] / l[4000] - 0.25f) < 1e-5f, "output stage: stereo ratio %.6f, not 0.25", r[4000] / l[4000]);
    }

    // Attack ramps over L, no step bigger than 1/L of the drop; release ends at exactly unity.
    {
        OutputStage        s;
        const int          n = 48000 * 3, b0 = 4800, b1 = 9600;
        std::vector<float> l(n, 0.25f), r(n, 0.25f);
        for(size_t i = b0; i < b1; i++)
            l[i] = r[i] = 4.f;
        const auto il = l;
        run(s, l, r);
        int back = -1;
        for(int i = n - 1; i >= b1 + L; i--)
            if(l[size_t(i)] != il[size_t(i - L)])
            {
                back = i + 1;
                break;
            }
        CHECK(back > 0 && back < n - 1000, "output stage: never released to unity");
        float maxStep = 0, prevGain = 1.f;
        for(size_t i = b0 - 2 * L; i < b1; i++)
        {
            const float g = l[i] / il[i - L];
            maxStep       = std::max(maxStep, prevGain - g);
            prevGain      = g;
        }
        const float drop = 1.f - OutputStage::kCeiling / 4.f;
        CHECK(maxStep <= drop / L * 1.001f, "output stage: attack step %.6f > %.6f", maxStep, drop / L);
    }

    // A NaN or infinity doesn't stick.
    {
        OutputStage s;
        s.setMakeup(makeup);
        std::vector<float> l(4800, 0.01f), r(4800, 0.01f);
        l[100] = NAN;
        r[200] = INFINITY;
        run(s, l, r);
        bool finite = true;
        for(size_t i = 0; i < l.size(); i++)
            finite = finite && std::isfinite(l[i]) && std::isfinite(r[i]);
        CHECK(finite && l[4000] == 0.01f * makeup, "output stage: non-finite input stuck (output %g)", double(l[4000]));
    }
}

// ---- RateAdapter ----------------------------------------------------------

struct Fit
{
    double amp, phase, residualDb; // y = amp sin(w n + phase); residual relative to the sine
};

// Least-squares sine at freq (cycles per sample) plus DC over y[from..].
Fit fitSine(const std::vector<float>& y, size_t from, double freq)
{
    double M[3][4] = {};
    for(size_t i = from; i < y.size(); i++)
    {
        const double w = 2 * kPi * freq * double(i), b[3] = {std::sin(w), std::cos(w), 1};
        for(int r = 0; r < 3; r++)
        {
            for(int c = 0; c < 3; c++)
                M[r][c] += b[r] * b[c];
            M[r][3] += b[r] * y[i];
        }
    }
    for(int i = 0; i < 3; i++)
        for(int j = i + 1; j < 3; j++)
        {
            const double f = M[j][i] / M[i][i];
            for(int k = i; k < 4; k++)
                M[j][k] -= f * M[i][k];
        }
    double x[3];
    for(int i = 2; i >= 0; i--)
    {
        x[i] = M[i][3];
        for(int k = i + 1; k < 3; k++)
            x[i] -= M[i][k] * x[k];
        x[i] /= M[i][i];
    }
    double res = 0;
    for(size_t i = from; i < y.size(); i++)
    {
        const double w = 2 * kPi * freq * double(i);
        const double e = y[i] - (x[0] * std::sin(w) + x[1] * std::cos(w) + x[2]);
        res += e * e;
    }
    const double amp = std::hypot(x[0], x[1]);
    return {amp, std::atan2(x[1], x[0]), 10 * std::log10(res / double(y.size() - from) / (amp * amp / 2) + 1e-30)};
}

// A firmware that outputs a 0.5 sine (freq > 0) or its line input, and keeps the line input it gets.
struct ToneFirmware : RateAdapter::BlockRenderer
{
    double             freq = 0;
    std::vector<float> input;
    void               renderBlock(uint64_t index, const float* const* in, float* const* out) override
    {
        for(int i = 0; i < 24; i++)
        {
            input.push_back(in[2][i]);
            const float v = float(0.5 * std::sin(2 * kPi * freq / kFw * double(index * 24 + uint64_t(i))));
            for(int c = 0; c < 4; c++)
                out[c][i] = freq > 0 ? v : in[c < 2 ? 2 : c][i];
        }
    }
};

/** Host line out L over `seconds` with a 0.5 sine of inFreq (0: silence) on the input. */
std::vector<float> runAdapter(double rate, int block, ToneFirmware& fw, double inFreq, double seconds, int* latency = nullptr)
{
    RateAdapter a;
    a.prepare(rate, block, 0);
    if(latency != nullptr)
        *latency = a.latencySamples();
    const size_t       n = size_t(rate * seconds);
    std::vector<float> in(static_cast<size_t>(block)), out(n), scratch(static_cast<size_t>(block) * 3);
    for(size_t done = 0; done < n; done += size_t(block))
    {
        const int m = int(std::min(size_t(block), n - done));
        for(int i = 0; i < m; i++)
            in[size_t(i)] = float(0.5 * std::sin(2 * kPi * inFreq / rate * double(done + size_t(i))));
        float* outs[4] = {scratch.data(), scratch.data() + block, out.data() + done, scratch.data() + 2 * block};
        a.process(in.data(), in.data(), outs, m, fw);
    }
    return out;
}

double passbandEdge(double rate) { return std::min(20000.0, 0.45 * std::min(rate, kFw)); }

void testRateAdapter()
{
    const double rates[] = {32000, 44100, 88200, 96000, 192000};
    const double tones[] = {100, 1000, 5000, 10000, 15000, 19000};

    for(double rate : rates)
        for(double f : tones)
        {
            if(f > passbandEdge(rate))
                continue;
            ToneFirmware fw;
            fw.freq          = f;
            const auto out   = runAdapter(rate, 512, fw, 0, 1);
            const Fit  toOut = fitSine(out, out.size() / 4, f / rate);
            CHECK(std::abs(db(toOut.amp / 0.5)) < 0.01 && toOut.residualDb < -85,
                  "rate adapter out @%.0f, %.0f Hz: gain %+.4f dB, spurious %.1f dB", rate, f, db(toOut.amp / 0.5),
                  toOut.residualDb);

            ToneFirmware in;
            runAdapter(rate, 512, in, f, 1);
            const Fit toIn = fitSine(in.input, in.input.size() / 4, f / kFw);
            CHECK(std::abs(db(toIn.amp / 0.5)) < 0.01 && toIn.residualDb < -85,
                  "rate adapter in @%.0f, %.0f Hz: gain %+.4f dB, spurious %.1f dB", rate, f, db(toIn.amp / 0.5),
                  toIn.residualDb);
        }

    // Content from the lower rate minus the passband edge up is in the
    // stopband: what folds from there is >= 90 dB down. Below 44.4 kHz that
    // reaches the firmware's output; above 48 kHz, the host's input.
    for(double f : {20000.0, 23000.0, 23900.0})
    {
        const double rate = 32000;
        ToneFirmware fw;
        fw.freq        = f;
        const auto out = runAdapter(rate, 512, fw, 0, 1);
        const double level = db(fitSine(out, out.size() / 4, (rate - f) / rate).amp / 0.5);
        CHECK(level < -85, "rate adapter out @%.0f: %.0f Hz folds to %.0f Hz at %.1f dB", rate, f, rate - f, level);
    }
    for(double rate : {88200.0, 96000.0, 192000.0})
        for(double f : {28000.0, 30000.0, 40000.0})
        {
            ToneFirmware fw;
            runAdapter(rate, 512, fw, f, 1);
            const double folded = kFw - std::fmod(f, kFw) < kFw / 2 ? kFw - std::fmod(f, kFw) : std::fmod(f, kFw);
            const double level  = db(fitSine(fw.input, fw.input.size() / 4, folded / kFw).amp / 0.5);
            CHECK(level < -85, "rate adapter in @%.0f: %.0f Hz folds to %.0f Hz at %.1f dB", rate, f, folded, level);
        }

    // Reported latency is the delay through a pass-through firmware, any block size.
    for(double rate : {32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0})
        for(int block : {512, 64, 7})
        {
            ToneFirmware fw;
            int          reported = 0;
            const auto   out      = runAdapter(rate, block, fw, 50, 2, &reported);
            const Fit    fit      = fitSine(out, out.size() / 3, 50 / rate);
            double       delay    = -fit.phase / (2 * kPi * 50 / rate); // input phase 0 at sample 0
            while(delay < 0)
                delay += rate / 50;
            CHECK(std::abs(delay - reported) <= 0.5 && std::abs(db(fit.amp / 0.5)) < 0.01,
                  "rate adapter @%.0f, block %d: delay %.3f, reported %d, gain %+.4f dB", rate, block, delay, reported,
                  db(fit.amp / 0.5));
        }

    // Random host blocks never leave the firmware short of input (it would see zeros).
    for(double rate : rates)
    {
        struct DcCheck : RateAdapter::BlockRenderer
        {
            uint64_t bad = 0;
            void     renderBlock(uint64_t index, const float* const* in, float* const* out) override
            {
                for(int i = 0; i < 24; i++)
                {
                    bad += index > 20 && std::abs(in[2][i] - 0.5f) > 1e-3f;
                    for(int c = 0; c < 4; c++)
                        out[c][i] = 0;
                }
            }
        } check;
        const int    maxBlock = 1024;
        std::mt19937 rng(1);
        RateAdapter  a;
        a.prepare(rate, maxBlock, 0);
        std::vector<float> in(size_t(maxBlock), 0.5f), o(size_t(maxBlock) * 4);
        float*             outs[4] = {o.data(), o.data() + maxBlock, o.data() + 2 * maxBlock, o.data() + 3 * maxBlock};
        for(int64_t done = 0; done < int64_t(rate * 20);)
        {
            const int m = std::uniform_int_distribution<int>(1, maxBlock)(rng);
            a.process(in.data(), in.data(), outs, m, check);
            done += m;
        }
        CHECK(check.bad == 0, "rate adapter @%.0f: %llu firmware input samples short", rate, (unsigned long long)check.bad);
    }

    // Offline (audio import): delay removed to the nearest sample, ceil(frames
    // x 48k / rate) frames out.
    for(double rate : {44100.0, 48000.0, 96000.0})
    {
        const size_t       frames = size_t(rate * 0.5) + 7;
        std::vector<float> lr(frames * 2), out;
        for(size_t i = 0; i < frames; i++)
            lr[2 * i] = lr[2 * i + 1] = float(0.5 * std::sin(2 * kPi * 1000 / rate * double(i)));
        RateAdapter::ToFirmwareRate converter(rate);
        converter.push(lr.data(), frames / 2, out);
        converter.push(lr.data() + frames / 2 * 2, frames - frames / 2, out);
        converter.finish(out);
        const size_t expected = size_t(std::ceil(double(frames) * kFw / rate));
        std::vector<float> left;
        for(size_t i = 0; i + 1 < out.size(); i += 2)
            left.push_back(out[i]);
        left.resize(left.size() - std::min<size_t>(left.size(), 200)); // the filter's ring-out at the end
        const Fit    fit   = fitSine(left, 200, 1000 / kFw);
        const double delay = -fit.phase / (2 * kPi * 1000 / kFw);
        CHECK(out.size() == expected * 2 && std::abs(db(fit.amp / 0.5)) < 0.01 && std::abs(delay) <= 0.5,
              "to firmware rate from %.0f: %zu frames (want %zu), gain %+.4f dB, delay %.3f", rate, out.size() / 2,
              expected, db(fit.amp / 0.5), delay);
    }
}
} // namespace

int main()
{
    testOutputStage();
    testRateAdapter();
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
