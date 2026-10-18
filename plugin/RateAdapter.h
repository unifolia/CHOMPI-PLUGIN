// Bridges host audio (any rate, any block size) to the machine's fixed
// 48 kHz / 24-sample blocks.
//
// Input is queued at 48 kHz behind a short prefill, so a firmware block can
// always be rendered when the output needs one; output is the 48 kHz stream
// converted to the host rate. Latency is therefore constant. At 48 kHz no
// resampling happens and the latency is one firmware block plus the
// renderer's output delay.
//
// Rate conversion is a Kaiser-windowed sinc cut off at the lower rate's Nyquist,
// like a codec's filter: flat to 20 kHz (0.45 x the lower rate below 44.4 kHz),
// >= 90 dB down from the lower rate minus that, so aliases and images can only
// land above the passband. Each way adds half the filter's length to the
// latency: ~0.7 ms at 44.1 kHz, ~0.4 ms from 88.2 kHz up.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

class RateAdapter
{
  public:
    static constexpr int kInputs  = 4; // firmware inputs: mic, unused, line L, line R
    static constexpr int kOutputs = 4; // firmware outputs: headphone L, R, line L, R

    struct BlockRenderer
    {
        virtual ~BlockRenderer() = default;
        /** Render firmware block `index` (covering firmware input samples
         *  [index * 24, index * 24 + 24)). */
        virtual void renderBlock(uint64_t index, const float* const* in, float* const* out) = 0;
    };

    /** Allocates; not on the audio thread. outputDelay: firmware samples the
     *  renderer delays its output by, counted in the latency. */
    void prepare(double hostRate, int maxHostBlock, int outputDelay);

    /** Host-rate latency from input / MIDI to output. */
    int latencySamples() const { return latency_; }

    /** Firmware input index of host sample `offset` of the next process() call. */
    double firmwareTimeAt(int offset) const;

    /** Host offset, relative to the start of the current process() call, at
     *  which firmware output index `firmwareTime` is heard. */
    int hostOffsetOf(double firmwareTime) const;

    /** in: host stereo input (may be null for silence). out: kOutputs host-rate channels (null to discard). */
    void process(const float* inL, const float* inR, float* const* out, int numSamples, BlockRenderer& renderer);

  private:
    void processChunk(const float* inL, const float* inR, float* const* out, int n, BlockRenderer& renderer);

    // Windowed-sinc resampler over a continuous stream. Output m is input time
    // m * step - half; one is emitted per push while pos < 1, so after k
    // pushes ceil(k / step) have been, independent of the filter.
    template <int Channels>
    struct Resampler
    {
        static constexpr int kPhases = 256; // kernel table steps per input sample, linearly interpolated

        double             step = 1; // input samples per output sample
        double             pos  = 0;
        int                half = 0, taps = 0, write = 0;
        int                silent = 0; // newest frames all zero, up to taps: then outputs are zero, not computed
        std::vector<float> phases; // (kPhases + 1) x taps
        std::vector<float> hist;   // 2 x taps frames, each written twice so the newest taps are contiguous

        void reset(double inRate, double outRate);
        template <typename Emit>
        void push(const float* frame, Emit&& emit);
    };

    // Interleaved frame FIFO.
    struct Ring
    {
        std::vector<float> data;
        int                channels = 0, capacity = 0, head = 0, size = 0;

        void         reset(int numChannels, int frames);
        void         pushFrame(const float* frame);
        const float* frame(int i) const; // i-th oldest
        void         drop(int n);
        void         popFrames(int n, float* const* dst); // dst entries may be null
    };

    bool                resampling_ = false;
    double              ratio_      = 1; // firmware samples per host sample
    int                 prefill_    = 0;
    double              inputLag_   = 0; // input resampler lag, in firmware samples
    double              outputLag_  = 0; // output resampler lag, in firmware samples
    int                 latency_    = 0;
    int                 maxChunk_   = 1;
    uint64_t            blocks_     = 0;
    int64_t             hostIn_     = 0; // host samples consumed so far
    int64_t             hostOut_    = 0; // host samples emitted so far
    int64_t             callStart_  = 0; // hostOut_ when the current process() call began
    Ring                inQueue_;        // 48 kHz input
    Ring                outQueue_;       // host-rate output
    Resampler<2>        inResampler_;
    Resampler<kOutputs> outResampler_;

    std::vector<float> blockIn_, blockOut_;

  public:
    /** The same filter offline: interleaved stereo at fromRate to 48 kHz, its
     *  delay removed. */
    class ToFirmwareRate
    {
      public:
        explicit ToFirmwareRate(double fromRate);
        /** Appends the output for these frames to out. */
        void push(const float* lr, size_t frames, std::vector<float>& out);
        /** Flushes the filter: out then holds ceil(input frames * 48000 /
         *  fromRate) frames. */
        void finish(std::vector<float>& out);

      private:
        bool         resampling_;
        double       ratio_;
        Resampler<2> resampler_;
        size_t       inFrames_ = 0, skip_ = 0, emitted_ = 0, kept_ = 0;
    };
};
