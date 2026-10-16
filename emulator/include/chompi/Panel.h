// Physical controls and LEDs of the CHOMPI panel, by the firmware's indices.
#pragma once
#include <cstdint>

namespace emu::panel
{
// Button IDs are positions in the firmware's switch chain (Hardware::SwId),
// except kEncoderSwitch[kEncLooper], which the firmware reports as ID 4.

// White keys 1-15 left to right (KEY_1..KEY_15). Key 15 is the CHOMPI
// recording buffer slot.
inline constexpr int kWhiteKeys[15] = {15, 8, 9, 10, 11, 16, 17, 18, 19, 20, 24, 25, 26, 27, 28};
// Black keys left to right (KEY_16..KEY_25), piano layout over the white keys.
inline constexpr int kBlackKeys[10] = {7, 12, 13, 14, 21, 22, 23, 29, 30, 31};
// Each black key sits between white key n and n+1 (0-based n).
inline constexpr int kBlackKeyAfterWhite[10] = {0, 1, 3, 4, 5, 7, 8, 10, 11, 12};

inline constexpr int kChompiKey = 5;  // KEY_26: shift layer / record
inline constexpr int kPlayKey   = 33; // KEY_27: looper play
inline constexpr int kLoopKey   = 34; // KEY_28: looper record
inline constexpr int kToggle    = 6;  // SW_TOG: play / record mode

// Encoders by hardware index (Hardware::EncoderId).
enum Encoder : int
{
    kEncStart  = 0,
    kEncEnd    = 1,
    kEncMagic  = 2,
    kEncSpeed  = 3,
    kEncLooper = 4, // big purple knob
    kEncVolume = 5,
    kNumEncoders
};
inline constexpr int kEncoderSwitch[kNumEncoders] = {0, 1, 2, 3, 4, 32};

// Through-hole LEDs (above the controls), indices into the firmware's PTH chain.
enum PthLed : int
{
    kLedChompi = 0,
    kLedSpeed,
    kLedStart,
    kLedEnd,
    kLedMagic,
    kLedLooperRev, // either side of the "<<|>>" above the purple knob
    kLedLooperFwd,
    kLedPlay, // tape reels above play / loop
    kLedLoop,
    kLedVolume,
    kNumPthLeds
};

// Key backlights: black keys are SMT LEDs 0-9 left to right; white key n (1-15)
// is SMT LED 25 - n.
inline constexpr int kNumSmtLeds = 25;
inline constexpr int SmtLedForWhiteKey(int n0) { return 24 - n0; }
inline constexpr int SmtLedForBlackKey(int n0) { return n0; }

// The firmware dims LEDs before sending them (PTH r/11, SMT r/4); these undo it.
inline constexpr int kPthFullScale = 255 / 11;
inline constexpr int kSmtFullScale = 255 / 4;

} // namespace emu::panel
