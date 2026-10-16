#include "RateAdapter.h"

#include <algorithm>
#include <cmath>

#include "chompi/Machine.h"

namespace
{
constexpr int    kBlock        = emu::Machine::kBlockSize;
constexpr double kFirmwareRate = emu::Machine::kSampleRate;
constexpr double kPi           = 3.14159265358979323846;
constexpr double kPassband     = 20000; // Hz, capped at 0.45 x the lower rate
constexpr double kStopbandDb   = 90;

double besselI0(double x)
{
    double sum = 1, term = 1;
    for(int k = 1; term > 1e-12 * sum; k++)
    {
        term *= (0.5 * x / k) * (0.5 * x / k);
        sum += term;
    }
    return sum;
}
} // namespace

template <int Channels>
void RateAdapter::Resampler<Channels>::reset(double inRate, double outRate)
{
    // Kaiser design (Oppenheim & Schafer): the stopband starts at the lower
    // rate minus the passband edge, so anything that folds lands above it.
    const double lower      = std::min(inRate, outRate);
    const double pass       = std::min(kPassband, 0.45 * lower);
    const double transition = (lower - 2 * pass) / inRate; // cycles per input sample
    const double length     = (kStopbandDb - 7.95) / (2.285 * 2 * kPi * transition);
    const double beta       = 0.1102 * (kStopbandDb - 8.7);
    const double cutoff     = 0.5 * lower / inRate;

    step  = inRate / outRate;
    pos   = 0;
    half  = 2 * int(std::ceil(length / 4)); // even: taps splits into push()'s four sums
    taps  = 2 * half;
    write = 0;
    hist.assign(size_t(2 * taps) * Channels, 0.f);

    // Phase p, tap k: the kernel at (half - 1 + p / kPhases) - k, tap k being
    // the k-th oldest of the newest taps inputs. Each phase is normalised to
    // unity gain at DC.
    phases.resize(size_t(kPhases + 1) * size_t(taps));
    const double i0Beta = besselI0(beta);
    std::vector<double> h(static_cast<size_t>(taps));
    for(int p = 0; p <= kPhases; p++)
    {
        double sum = 0;
        for(int k = 0; k < taps; k++)
        {
            const double d = half - 1 + double(p) / kPhases - k;
            const double x = d / half;
            const double w = x * x < 1 ? besselI0(beta * std::sqrt(1 - x * x)) / i0Beta : 0;
            const double a = 2 * kPi * cutoff * d;
            h[size_t(k)]   = w * (std::abs(a) < 1e-9 ? 1 : std::sin(a) / a);
            sum += h[size_t(k)];
        }
        for(int k = 0; k < taps; k++)
            phases[size_t(p) * size_t(taps) + size_t(k)] = float(h[size_t(k)] / sum);
    }
}

template <int Channels>
template <typename Emit>
void RateAdapter::Resampler<Channels>::push(const float* frame, Emit&& emit)
{
    std::copy(frame, frame + Channels, &hist[size_t(write) * Channels]);
    std::copy(frame, frame + Channels, &hist[size_t(write + taps) * Channels]);
    if(++write == taps)
        write = 0;
    const float* x = &hist[size_t(write) * Channels];

    while(pos < 1.0)
    {
        const double phase = pos * kPhases;
        const int    p     = int(phase);
        const float  f     = float(phase - p);
        const float* a     = &phases[size_t(p) * size_t(taps)];
        const float* b     = a + taps;
        // Both neighbouring phases, then interpolate. Each sum is split four
        // ways: as one dependent chain it was FMA-latency-bound, 2x slower.
        float sa[4][Channels] = {}, sb[4][Channels] = {};
        for(int k = 0; k < taps; k += 4)
            for(int j = 0; j < 4; j++)
                for(int ch = 0; ch < Channels; ch++)
                {
                    sa[j][ch] += a[k + j] * x[(k + j) * Channels + ch];
                    sb[j][ch] += b[k + j] * x[(k + j) * Channels + ch];
                }
        float out[Channels];
        for(int ch = 0; ch < Channels; ch++)
        {
            const float va = (sa[0][ch] + sa[1][ch]) + (sa[2][ch] + sa[3][ch]);
            const float vb = (sb[0][ch] + sb[1][ch]) + (sb[2][ch] + sb[3][ch]);
            out[ch]        = va + f * (vb - va);
        }
        emit(out);
        pos += step;
    }
    pos -= 1.0;
}

void RateAdapter::Ring::reset(int numChannels, int frames)
{
    channels = numChannels;
    capacity = frames;
    head = size = 0;
    data.assign(size_t(numChannels) * size_t(frames), 0.f);
}

void RateAdapter::Ring::pushFrame(const float* frame)
{
    if(size == capacity) // cannot happen with prepare()'s sizing; drop oldest
        drop(1);
    int tail = head + size;
    if(tail >= capacity)
        tail -= capacity;
    std::copy(frame, frame + channels, &data[size_t(tail) * size_t(channels)]);
    size++;
}

const float* RateAdapter::Ring::frame(int i) const
{
    int at = head + i;
    if(at >= capacity)
        at -= capacity;
    return &data[size_t(at) * size_t(channels)];
}

void RateAdapter::Ring::drop(int n)
{
    n = std::min(n, size);
    head += n;
    if(head >= capacity)
        head -= capacity;
    size -= n;
}

void RateAdapter::Ring::popFrames(int n, float* const* dst)
{
    n = std::min(n, size);
    int at = head;
    for(int i = 0; i < n; i++)
    {
        const float* src = &data[size_t(at) * size_t(channels)];
        for(int c = 0; c < channels; c++)
            if(dst[c] != nullptr)
                dst[c][i] = src[c];
        if(++at == capacity)
            at = 0;
    }
    drop(n);
}

void RateAdapter::prepare(double hostRate, int maxHostBlock, int outputDelay)
{
    ratio_      = kFirmwareRate / hostRate;
    resampling_ = std::abs(hostRate - kFirmwareRate) > 0.5;
    // Enough queued input that a block can be rendered whenever output needs
    // one. A chunk queues ceil(hostIn * ratio) firmware inputs and renders
    // until ceil(24 * blocks / ratio) >= hostOut (== hostIn), which needs at
    // most one block beyond them; the 2 covers each count's rounding of pos.
    prefill_ = resampling_ ? kBlock + 2 : kBlock;

    maxChunk_ = std::max(1, maxHostBlock);
    inQueue_.reset(2, prefill_ + int(std::ceil(maxChunk_ * ratio_)) + 2 * kBlock + 8);
    outQueue_.reset(kOutputs, maxChunk_ + int(std::ceil(kBlock / ratio_)) + 8);
    if(resampling_)
    {
        inResampler_.reset(hostRate, kFirmwareRate);
        outResampler_.reset(kFirmwareRate, hostRate);
    }
    inputLag_  = resampling_ ? inResampler_.half * ratio_ : 0.0;
    outputLag_ = resampling_ ? outResampler_.half : 0.0;

    const float silence[2] = {0.f, 0.f};
    for(int i = 0; i < prefill_; i++)
        inQueue_.pushFrame(silence);

    // Host input at t reaches firmware index t * ratio + inputLag + prefill;
    // firmware output j is heard at host (j + outputLag) / ratio.
    latency_ = int(std::lround((prefill_ + inputLag_ + outputLag_ + outputDelay) / ratio_));

    blocks_    = 0;
    hostIn_    = 0;
    hostOut_   = 0;
    callStart_ = 0;
    blockIn_.assign(size_t(kInputs) * kBlock, 0.f);
    blockOut_.assign(size_t(kOutputs) * kBlock, 0.f);
}

double RateAdapter::firmwareTimeAt(int offset) const
{
    return double(hostIn_ + offset) * ratio_ + inputLag_ + prefill_;
}

int RateAdapter::hostOffsetOf(double firmwareTime) const
{
    return int(std::floor((firmwareTime + outputLag_) / ratio_ - double(callStart_)));
}

void RateAdapter::process(const float* inL, const float* inR, float* const* out, int numSamples, BlockRenderer& renderer)
{
    callStart_ = hostOut_;
    for(int done = 0; done < numSamples;)
    {
        const int n = std::min(numSamples - done, maxChunk_);
        float*    chunkOut[kOutputs];
        for(int c = 0; c < kOutputs; c++)
            chunkOut[c] = out[c] != nullptr ? out[c] + done : nullptr;
        processChunk(inL != nullptr ? inL + done : nullptr, inR != nullptr ? inR + done : nullptr, chunkOut, n, renderer);
        done += n;
    }
}

void RateAdapter::processChunk(const float* inL, const float* inR, float* const* out, int n, BlockRenderer& renderer)
{
    auto toInput = [this](const float* frame) { inQueue_.pushFrame(frame); };
    for(int i = 0; i < n; i++)
    {
        const float frame[2] = {inL != nullptr ? inL[i] : 0.f, inR != nullptr ? inR[i] : 0.f};
        if(resampling_)
            inResampler_.push(frame, toInput);
        else
            inQueue_.pushFrame(frame);
    }
    hostIn_ += n;

    float* in[kInputs];
    float* blockOut[kOutputs];
    for(int c = 0; c < kInputs; c++)
        in[c] = &blockIn_[size_t(c) * kBlock];
    for(int c = 0; c < kOutputs; c++)
        blockOut[c] = &blockOut_[size_t(c) * kBlock];

    auto toOutput = [this](const float* frame) { outQueue_.pushFrame(frame); };
    while(outQueue_.size < n)
    {
        const int available = std::min(kBlock, inQueue_.size);
        for(int i = 0; i < kBlock; i++)
        {
            const float* lr = i < available ? inQueue_.frame(i) : nullptr;
            const float  l  = lr != nullptr ? lr[0] : 0.f;
            const float  r  = lr != nullptr ? lr[1] : 0.f;
            in[0][i]        = 0.5f * (l + r); // mic
            in[1][i]        = 0.f;
            in[2][i]        = l; // line
            in[3][i]        = r;
        }
        inQueue_.drop(available);

        renderer.renderBlock(blocks_++, in, blockOut);

        for(int i = 0; i < kBlock; i++)
        {
            const float frame[kOutputs] = {blockOut[0][i], blockOut[1][i], blockOut[2][i], blockOut[3][i]};
            if(resampling_)
                outResampler_.push(frame, toOutput);
            else
                outQueue_.pushFrame(frame);
        }
    }

    outQueue_.popFrames(n, out);
    hostOut_ += n;
}
