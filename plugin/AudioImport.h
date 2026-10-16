// Audio files dropped on the panel, as the firmwares' 48 kHz 16-bit stereo.
#pragma once
#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

namespace audio_import
{
/** Interleaved stereo 16-bit at 48 kHz from any file JUCE reads: mono doubled,
 *  channels past two dropped, other rates through the host-rate filter,
 *  TPDF-dithered unless the file already is 16-bit 48 kHz. At most maxSeconds;
 *  cut says whether it was longer. */
bool read(const juce::File& file, double maxSeconds, std::vector<int16_t>& out, bool& cut);

/** As a WAV with the 44-byte header the firmwares write themselves. */
std::vector<uint8_t> toWav(const std::vector<int16_t>& samples);

/** The file patterns read() takes, ";"-separated ("*.wav;*.aif;..."). */
juce::String wildcard();
} // namespace audio_import
