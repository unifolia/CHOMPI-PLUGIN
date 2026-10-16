// Makeup gain and a safety limiter on one stereo firmware output, at 48 kHz.
//
// The firmwares scale their outputs for CHOMPI's analog stage (TAPE's line out
// is 0.3 of full scale), so raw they sit 12-24 dB below a typical instrument
// plugin. The makeup restores a DAW level. The limiter catches what that pushes
// past kCeiling, with one gain for both channels: its gain is down to each
// peak's reduction kLookahead samples ahead of it, ramping there over that
// span, so peaks are turned down rather than clipped. Audio is delayed by
// kLookahead; below the ceiling (once released) it is the input times the
// makeup, exactly.
#pragma once
#include <array>
#include <cstdint>

class OutputStage
{
  public:
    static constexpr int   kLookahead = 48;          // 1 ms
    static constexpr float kCeiling   = 0.89125094f; // -1 dBFS
    static constexpr float kReleaseMs = 100.f;

    OutputStage() { reset(); }
    void reset();
    void setMakeup(float gain) { makeup_ = gain; }
    /** In place. */
    void process(float* l, float* r, int n);

  private:
    // Minimum gain over a peak and the kLookahead samples before it.
    static constexpr int kWindow = kLookahead + 1;

    float   makeup_ = 1.f;
    double  env_    = 1.0; // double: in float the release stalls 1.4e-4 short of 1
    double  sum_    = kLookahead; // of ramp_
    int64_t index_  = 0;
    int     pos_    = 0; // into the delay and ramp rings
    std::array<float, kLookahead>  delayL_{}, delayR_{};
    std::array<double, kLookahead> ramp_{};

    // Sliding-minimum queue (ring): gains strictly increase front to back.
    std::array<int64_t, kWindow> minIndex_{};
    std::array<float, kWindow>   minGain_{};
    int                          minFront_ = 0, minCount_ = 0;
};
