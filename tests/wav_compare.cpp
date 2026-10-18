// Compares two 16-bit PCM WAVs (as chompi_render writes them) sample by sample.
//
//   wav_compare <actual.wav> <reference.wav> <tolerance in LSB>
//
// Exits 1 if their formats or lengths differ, or a sample differs by more.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
struct Wav
{
    int                  channels = 0, rate = 0, bits = 0;
    std::vector<int16_t> samples;
};

uint32_t le(const unsigned char* p, int n)
{
    uint32_t v = 0;
    for(int i = n - 1; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

bool read(const char* path, Wav& wav)
{
    std::ifstream                    file(path, std::ios::binary);
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), {}};
    if(bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return false;
    for(size_t pos = 12; pos + 8 <= bytes.size();)
    {
        const unsigned char* chunk = bytes.data() + pos;
        const size_t         size  = std::min<size_t>(le(chunk + 4, 4), bytes.size() - pos - 8);
        if(std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16)
        {
            if(le(chunk + 8, 2) != 1) // PCM
                return false;
            wav.channels = int(le(chunk + 10, 2));
            wav.rate     = int(le(chunk + 12, 4));
            wav.bits     = int(le(chunk + 22, 2));
        }
        else if(std::memcmp(chunk, "data", 4) == 0)
        {
            wav.samples.resize(size / 2);
            for(size_t i = 0; i < wav.samples.size(); i++)
                wav.samples[i] = int16_t(le(chunk + 8 + 2 * i, 2));
        }
        pos += 8 + size + (size & 1);
    }
    return wav.bits == 16 && wav.channels > 0;
}
} // namespace

int main(int argc, char** argv)
{
    if(argc != 4)
    {
        std::fprintf(stderr, "usage: %s <actual.wav> <reference.wav> <tolerance in LSB>\n", argv[0]);
        return 2;
    }
    Wav actual, reference;
    if(!read(argv[1], actual) || !read(argv[2], reference))
    {
        std::printf("can't read %s as 16-bit PCM WAV\n", read(argv[1], actual) ? argv[2] : argv[1]);
        return 1;
    }
    if(actual.channels != reference.channels || actual.rate != reference.rate
       || actual.samples.size() != reference.samples.size())
    {
        std::printf("format differs: %d ch, %d Hz, %zu frames; reference %d ch, %d Hz, %zu frames\n", actual.channels,
                    actual.rate, actual.samples.size() / size_t(actual.channels), reference.channels, reference.rate,
                    reference.samples.size() / size_t(reference.channels));
        return 1;
    }

    const int tolerance = std::atoi(argv[3]);
    int       worst     = 0;
    size_t    differing = 0, first = 0, worstAt = 0;
    for(size_t i = 0; i < actual.samples.size(); i++)
        if(const int d = std::abs(int(actual.samples[i]) - int(reference.samples[i])))
        {
            if(differing++ == 0)
                first = i;
            if(d > worst)
            {
                worst   = d;
                worstAt = i;
            }
        }
    const double frame = double(actual.channels) * actual.rate;
    if(differing == 0)
        std::printf("identical\n");
    else
        std::printf("%zu samples differ, from %.4f s; at most %d LSB (%.4f s), tolerance %d\n", differing,
                    double(first) / frame, worst, double(worstAt) / frame, tolerance);
    return worst > tolerance;
}
