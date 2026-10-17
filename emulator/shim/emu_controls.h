// Each firmware's automatable values (Machine::Controls()), shared by the
// machine that turns them and the table hosts see. Append only: hosts keep
// automation by ID, and order is the parameter order they show.
#pragma once
#include <cmath>
#include <cstdint>

#include "chompi/Machine.h"

namespace emu::controls
{
/** A control a knob sets: the knob (ui.h numbering, after encoder_map) on
 *  one of its pages, in NormalPage or the shift layer (MenuPage); the turns
 *  per click as UserInterface::GenerateEvents sends them (1 for knobs 0 and
 *  4, else 3); the value a click moves, or its first if `varies`. */
struct Knob
{
    bool    shift;
    uint8_t knob, page;
    int16_t turns;
    float   step;
    bool    varies = false; // the firmware sizes each click from the value
};

/** Clicks the knob toward `target` with its page on show, at most maxClicks:
 *  turn(turns) is one click into the page's handler (false: it takes no input
 *  now), value() reads the control. Clicks while more than half a click away,
 *  sizing a click by the last one's move; a knob whose clicks vary tries one
 *  more, once, and clicks back if that didn't get closer. Stops at a limit,
 *  where a click doesn't move it. False to carry on next block. */
template <typename Value, typename Turn>
bool ClickToward(const Knob& k, uint8_t* pages, float target, int maxClicks, Value value, Turn turn)
{
    const uint8_t shown = pages[k.knob];
    pages[k.knob]       = k.page;
    float step          = k.step;
    bool  tried         = !k.varies;
    int   result        = -1; // -1 more to do, 0 input not taken, 1 done
    for(int c = 0; c < maxClicks && result < 0; c++)
    {
        const float   before = value(), miss = std::fabs(target - before);
        const bool    close  = miss <= step * .5f;
        const int16_t turns  = target > before ? k.turns : int16_t(-k.turns);
        if(close && tried)
            result = 1;
        else if(!turn(turns))
            result = 0;
        else if(const float after = value(); after == before)
            result = 1;
        else
        {
            step = std::fabs(after - before);
            if(close) // the trial
            {
                tried = true;
                if(std::fabs(target - after) >= miss)
                {
                    turn(int16_t(-turns));
                    step = std::fabs(value() - after);
                }
            }
        }
    }
    pages[k.knob] = shown;
    if(result < 0 && tried && std::fabs(target - value()) <= step * .5f)
        result = 1;
    return result == 1;
}

enum Tape : int
{
    kTapeSpeed,
    kTapeGain,
    kTapeStart,
    kTapeAttack,
    kTapeEnd,
    kTapeRelease,
    kTapePan,
    kTapeMagic,
    kTapeLofi,
    kTapeFilter,
    kTapeDelayTime,
    kTapeWarble,
    kTapeResonance,
    kTapeLooperSpeed,
    kTapeVolume,
    kTapeInputGain,
    kTapeCompression,
    kNumTape
};

// Defaults: ui.h enc_defaults, MenuPage::Init, a slot's default pan.
inline const ControlInfo kTapeInfo[kNumTape] = {
    {"tape_speed", "TAPE Speed", .83f, ControlInfo::Unit::SampleSpeed},
    {"tape_gain", "TAPE Sample Gain", .704f, ControlInfo::Unit::Percent},
    {"tape_start", "TAPE Start", 0.f, ControlInfo::Unit::Percent},
    {"tape_attack", "TAPE Attack", 0.f, ControlInfo::Unit::Percent},
    {"tape_end", "TAPE End", 1.f, ControlInfo::Unit::Percent},
    {"tape_release", "TAPE Release", 0.f, ControlInfo::Unit::Percent},
    {"tape_pan", "TAPE Pan", .5f, ControlInfo::Unit::Pan},
    {"tape_magic", "TAPE Reverb/Delay", 0.f, ControlInfo::Unit::Percent},
    {"tape_lofi", "TAPE Lo-fi", 0.f, ControlInfo::Unit::Percent},
    {"tape_filter", "TAPE Filter", .5f, ControlInfo::Unit::Percent},
    {"tape_delay_time", "TAPE Delay Time", .5f, ControlInfo::Unit::Percent},
    {"tape_warble", "TAPE Warble", 0.f, ControlInfo::Unit::Percent},
    {"tape_resonance", "TAPE Resonance", 0.f, ControlInfo::Unit::Percent},
    {"tape_looper_speed", "TAPE Looper Speed", .75f, ControlInfo::Unit::LooperSpeed},
    {"tape_volume", "TAPE Volume", .84f, ControlInfo::Unit::Percent},
    {"tape_input_gain", "TAPE Input Gain", .75f, ControlInfo::Unit::Percent},
    {"tape_compression", "TAPE Compression", 0.f, ControlInfo::Unit::Percent},
};

enum Wave : int
{
    kWavePitch,
    kWaveFrame,
    kWaveAttack,
    kWavePitchLfoAmount,
    kWaveRelease,
    kWaveFilterLfoAmount,
    kWaveMagic,
    kWaveCutoff,
    kWaveTempo,
    kWaveVolume,
    kWavePan,
    kWavePitchLfoRate,
    kWaveFilterLfoRate,
    kWaveDelayTime,
    kWaveResonance,
    kWaveCompression,
    kNumWave
};

// Defaults: ui.h enc_defaults, MenuPage::Init, clockManager's 320.
inline const ControlInfo kWaveInfo[kNumWave] = {
    {"wave_pitch", "WAVE Pitch", .5f, ControlInfo::Unit::Semitones},
    {"wave_frame", "WAVE Wavetable Frame", 0.f, ControlInfo::Unit::Frame},
    {"wave_attack", "WAVE Attack", 0.f, ControlInfo::Unit::Percent},
    {"wave_pitch_lfo", "WAVE Pitch LFO Amount", 0.f, ControlInfo::Unit::Percent},
    {"wave_release", "WAVE Release", 0.f, ControlInfo::Unit::Percent},
    {"wave_filter_lfo", "WAVE Filter LFO Amount", 0.f, ControlInfo::Unit::Percent},
    {"wave_magic", "WAVE Delay/Reverb", .5f, ControlInfo::Unit::DelayReverb},
    {"wave_cutoff", "WAVE Filter Cutoff", .5f, ControlInfo::Unit::Percent},
    {"wave_tempo", "WAVE Tempo", .5f, ControlInfo::Unit::Tempo},
    {"wave_volume", "WAVE Volume", .84f, ControlInfo::Unit::Percent},
    {"wave_pan", "WAVE Pan", .5f, ControlInfo::Unit::Pan},
    {"wave_pitch_lfo_rate", "WAVE Pitch LFO Rate", .58f, ControlInfo::Unit::Percent},
    {"wave_filter_lfo_rate", "WAVE Filter LFO Rate", .58f, ControlInfo::Unit::Percent},
    {"wave_delay_time", "WAVE Delay Time", .4f, ControlInfo::Unit::Percent},
    {"wave_resonance", "WAVE Resonance", .63f, ControlInfo::Unit::Percent},
    {"wave_compression", "WAVE Compression", 0.f, ControlInfo::Unit::Percent},
};

enum Tempo : int
{
    kTempoPitch,
    kTempoSampleVolume,
    kTempoFilter,
    kTempoStart,
    kTempoAttack,
    kTempoEnd,
    kTempoRelease,
    kTempoDelay,
    kTempoDelayMix,
    kTempoTempo,
    kTempoVolume,
    kTempoInputGain,
    kTempoPan,
    kTempoRedux,
    kTempoLoop,
    kTempoSustain,
    kTempoDelayRandom,
    kTempoFeedback,
    kTempoPatternRandom,
    kTempoCompression,
    kNumTempo
};

// Defaults: ui.h enc_defaults, MenuPage::Init, clockManager's 320.
inline const ControlInfo kTempoInfo[kNumTempo] = {
    {"tempo_pitch", "TEMPO Pitch", .83f, ControlInfo::Unit::SampleSpeed},
    {"tempo_sample_volume", "TEMPO Sample Volume", .6f, ControlInfo::Unit::Percent},
    {"tempo_filter", "TEMPO Filter", .5f, ControlInfo::Unit::Percent},
    {"tempo_start", "TEMPO Start", 0.f, ControlInfo::Unit::Percent},
    {"tempo_attack", "TEMPO Attack", 0.f, ControlInfo::Unit::Percent},
    {"tempo_end", "TEMPO End", 1.f, ControlInfo::Unit::Percent},
    {"tempo_release", "TEMPO Release", 0.f, ControlInfo::Unit::Percent},
    {"tempo_delay", "TEMPO Delay", .5f, ControlInfo::Unit::Percent},
    {"tempo_delay_mix", "TEMPO Delay Mix", .5f, ControlInfo::Unit::Percent},
    {"tempo_tempo", "TEMPO Tempo", .5f, ControlInfo::Unit::Tempo},
    {"tempo_volume", "TEMPO Volume", .6f, ControlInfo::Unit::Percent},
    {"tempo_input_gain", "TEMPO Input Gain", .75f, ControlInfo::Unit::Percent},
    {"tempo_pan", "TEMPO Pan", .5f, ControlInfo::Unit::Pan},
    {"tempo_redux", "TEMPO Sample-Rate Reduction", 0.f, ControlInfo::Unit::Percent},
    {"tempo_loop", "TEMPO Loop", 1.f, ControlInfo::Unit::Percent},
    {"tempo_sustain", "TEMPO Sustain", 1.f, ControlInfo::Unit::Percent},
    {"tempo_delay_random", "TEMPO Delay Randomness", 0.f, ControlInfo::Unit::Percent},
    {"tempo_feedback", "TEMPO Delay Feedback", .3f, ControlInfo::Unit::Percent},
    {"tempo_pattern_random", "TEMPO Pattern Randomness", 0.f, ControlInfo::Unit::Percent},
    {"tempo_compression", "TEMPO Compression", 0.f, ControlInfo::Unit::Percent},
};
} // namespace emu::controls
