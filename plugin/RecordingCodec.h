// Lossless FLAC for emu::Recordings in the plugin state.
#pragma once
#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

namespace recording_codec
{
/** Interleaved stereo 16-bit at 48 kHz. Empty on failure. */
juce::MemoryBlock encode(const std::vector<int16_t>& samples);
bool              decode(const void* data, size_t size, std::vector<int16_t>& samples);
} // namespace recording_codec
