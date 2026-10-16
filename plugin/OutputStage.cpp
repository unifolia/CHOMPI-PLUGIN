#include "OutputStage.h"

#include <algorithm>
#include <cmath>

#include "chompi/Machine.h"

namespace
{
const double kReleaseKeep = std::exp(-1000.0 / (OutputStage::kReleaseMs * emu::Machine::kSampleRate));
} // namespace

void OutputStage::reset()
{
    env_   = 1.0;
    sum_   = kLookahead;
    index_ = 0;
    pos_   = 0;
    delayL_.fill(0.f);
    delayR_.fill(0.f);
    ramp_.fill(1.0);
    minFront_ = minCount_ = 0;
}

void OutputStage::process(float* l, float* r, int n)
{
    for(int i = 0; i < n; i++, index_++)
    {
        // A non-finite sample would stick in the limiter's state for good.
        const float xl   = std::isfinite(l[i]) ? l[i] * makeup_ : 0.f;
        const float xr   = std::isfinite(r[i]) ? r[i] * makeup_ : 0.f;
        const float peak = std::max(std::abs(xl), std::abs(xr));
        const float want = peak > kCeiling ? kCeiling / peak : 1.f;

        // held = min(want) over the last kWindow samples.
        if(minCount_ > 0 && minIndex_[size_t(minFront_)] <= index_ - kWindow)
        {
            minFront_ = (minFront_ + 1) % kWindow;
            minCount_--;
        }
        while(minCount_ > 0 && minGain_[size_t((minFront_ + minCount_ - 1) % kWindow)] >= want)
            minCount_--;
        const auto back = size_t((minFront_ + minCount_) % kWindow);
        minIndex_[back] = index_;
        minGain_[back]  = want;
        minCount_++;
        const double held = minGain_[size_t(minFront_)];

        // Instant down, exponential release; env_ <= held throughout.
        if(held < env_)
            env_ = held;
        else
        {
            env_ = held - (held - env_) * kReleaseKeep;
            if(held - env_ < 1e-9) // reach held exactly
                env_ = held;
        }

        // Averaging env_ over kLookahead samples, all <= the gain the delayed
        // sample's peak wants, ramps the attack and keeps the output at or
        // under the ceiling.
        sum_ += env_ - ramp_[size_t(pos_)];
        ramp_[size_t(pos_)] = env_;
        const float gain    = float(sum_ / kLookahead);

        l[i]                  = delayL_[size_t(pos_)] * gain;
        r[i]                  = delayR_[size_t(pos_)] * gain;
        delayL_[size_t(pos_)] = xl;
        delayR_[size_t(pos_)] = xr;
        pos_                  = pos_ + 1 == kLookahead ? 0 : pos_ + 1;
    }
}
