#include "RecordingCodec.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include "chompi/Machine.h"

namespace recording_codec
{
namespace
{
constexpr int kChunk = 4096;
// JUCE passes integer samples left-justified in 32 bits.
constexpr int kJustify = 1 << 16;
// Larger than a TAPE buffer (31.7 MB), so a corrupt header can't demand gigabytes.
constexpr juce::int64 kMaxFrames = 1 << 24;
// Measured on an M2, 165 s of factory audio: level 5 is 34.6% of raw, 133 ms to
// encode, 144 ms to decode; level 1 41.4% in 110 ms, level 8 34.0% in 239 ms.
constexpr int kQuality = 5;
} // namespace

juce::MemoryBlock encode(const std::vector<int16_t>& samples)
{
    juce::MemoryBlock block;
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream>(block, false);
        juce::FlacAudioFormat               flac;
        auto writer = flac.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                       .withSampleRate(emu::Machine::kSampleRate)
                                                       .withNumChannels(2)
                                                       .withBitsPerSample(16)
                                                       .withQualityOptionIndex(kQuality));
        if(writer == nullptr)
            return {};

        std::vector<int> l(kChunk), r(kChunk);
        const int*       channels[] = {l.data(), r.data()};
        const size_t     frames     = samples.size() / 2;
        for(size_t start = 0; start < frames; start += kChunk)
        {
            const int n = int(std::min<size_t>(kChunk, frames - start));
            for(int i = 0; i < n; i++)
            {
                l[size_t(i)] = samples[2 * (start + size_t(i))] * kJustify;
                r[size_t(i)] = samples[2 * (start + size_t(i)) + 1] * kJustify;
            }
            if(!writer->write(channels, n))
                return {};
        }
    } // the writer finishes the stream (and its header) on destruction
    return block;
}

bool decode(const void* data, size_t size, std::vector<int16_t>& samples)
{
    juce::FlacAudioFormat                    flac;
    std::unique_ptr<juce::AudioFormatReader> reader(flac.createReaderFor(new juce::MemoryInputStream(data, size, false), true));
    if(reader == nullptr || reader->numChannels != 2 || reader->bitsPerSample != 16 || reader->lengthInSamples < 0
       || reader->lengthInSamples > kMaxFrames)
        return false;

    const auto frames = reader->lengthInSamples;
    samples.resize(size_t(frames) * 2);
    std::vector<int> l(kChunk), r(kChunk);
    int*             channels[] = {l.data(), r.data()};
    for(juce::int64 start = 0; start < frames; start += kChunk)
    {
        const int n = int(std::min<juce::int64>(kChunk, frames - start));
        if(!reader->read(channels, 2, start, n, false))
            return false;
        for(int i = 0; i < n; i++)
        {
            samples[2 * size_t(start + i)]     = int16_t(l[size_t(i)] / kJustify);
            samples[2 * size_t(start + i) + 1] = int16_t(r[size_t(i)] / kJustify);
        }
    }
    return true;
}
} // namespace recording_codec
