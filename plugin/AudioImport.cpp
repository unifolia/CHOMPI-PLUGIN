#include "AudioImport.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include "RateAdapter.h"
#include "chompi/Machine.h"

namespace audio_import
{
namespace
{
constexpr int kChunk = 65536;

void putLE(std::vector<uint8_t>& out, uint32_t value, int bytes)
{
    for(int i = 0; i < bytes; i++)
        out.push_back(uint8_t(value >> (8 * i)));
}
} // namespace

bool read(const juce::File& file, double maxSeconds, std::vector<int16_t>& out, bool& cut)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if(reader == nullptr || reader->sampleRate <= 0 || reader->numChannels < 1 || reader->lengthInSamples <= 0)
        return false;

    const double rate   = reader->sampleRate;
    const auto   limit  = juce::int64(std::ceil(maxSeconds * rate));
    const auto   frames = std::min(reader->lengthInSamples, limit);
    const bool   exact  = std::abs(rate - emu::Machine::kSampleRate) < 0.5 && reader->bitsPerSample == 16
                       && !reader->usesFloatingPointData;
    cut = reader->lengthInSamples > limit;

    RateAdapter::ToFirmwareRate converter(rate);
    std::vector<float>          lr;
    lr.reserve(size_t(std::ceil(double(frames) * emu::Machine::kSampleRate / rate)) * 2 + 2);
    juce::AudioBuffer<float> chunk(2, kChunk);
    std::vector<float>       interleaved(2 * kChunk);
    for(juce::int64 pos = 0; pos < frames; pos += kChunk)
    {
        const int n = int(std::min<juce::int64>(kChunk, frames - pos));
        if(!reader->read(&chunk, 0, n, pos, true, true))
            return false;
        for(int i = 0; i < n; i++)
        {
            interleaved[2 * size_t(i)]     = chunk.getSample(0, i);
            interleaved[2 * size_t(i) + 1] = chunk.getSample(1, i);
        }
        converter.push(interleaved.data(), size_t(n), lr);
    }
    converter.finish(lr);

    // JUCE reads 16-bit as value / 32768, so that scale makes the exact case
    // bit-exact. Seeded per call: the same file always imports the same.
    out.resize(lr.size());
    juce::Random random(1);
    for(size_t i = 0; i < lr.size(); i++)
    {
        const float dither = exact ? 0.f : random.nextFloat() - random.nextFloat();
        out[i] = int16_t(juce::jlimit(-32768.f, 32767.f, std::round(lr[i] * 32768.f + dither)));
    }
    return true;
}

std::vector<uint8_t> toWav(const std::vector<int16_t>& samples)
{
    const auto           dataBytes = uint32_t(samples.size() * sizeof(int16_t));
    std::vector<uint8_t> wav;
    wav.reserve(44 + dataBytes);
    for(const char c : std::string("RIFF"))
        wav.push_back(uint8_t(c));
    putLE(wav, 36 + dataBytes, 4);
    for(const char c : std::string("WAVEfmt "))
        wav.push_back(uint8_t(c));
    putLE(wav, 16, 4);
    putLE(wav, 1, 2); // PCM
    putLE(wav, 2, 2);
    putLE(wav, emu::Machine::kSampleRate, 4);
    putLE(wav, emu::Machine::kSampleRate * 4, 4);
    putLE(wav, 4, 2);
    putLE(wav, 16, 2);
    for(const char c : std::string("data"))
        wav.push_back(uint8_t(c));
    putLE(wav, dataBytes, 4);
    const auto* bytes = reinterpret_cast<const uint8_t*>(samples.data());
    wav.insert(wav.end(), bytes, bytes + dataBytes); // little-endian host
    return wav;
}

juce::String wildcard()
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    return formats.getWildcardForAllFormats();
}
} // namespace audio_import
