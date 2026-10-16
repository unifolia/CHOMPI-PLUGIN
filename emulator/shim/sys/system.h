// EMULATOR STAND-IN, not upstream libDaisy.
// The hardware System configures clocks/caches and reads SysTick. Here time is
// the executing machine's virtual clock (emu_context.h), advanced by the
// emulator's scheduler. Delay*() advance that clock instead of spinning: they
// only run in boot-time loops, which must see time pass to debounce controls.
// Only the calls the firmwares make are provided.
#pragma once
#ifndef DSY_SYSTEM_H
#define DSY_SYSTEM_H

#include <cstdint>
#include "emu_context.h"

namespace daisy
{
class System
{
  public:
    static uint32_t GetNow() { return static_cast<uint32_t>(emu::NowUs() / 1000); }
    static uint32_t GetUs() { return static_cast<uint32_t>(emu::NowUs()); }

    /** The free-running tick, at the Seed's 240 MHz timer clock. 64-bit, unlike
     *  libDaisy's: TEMPO subtracts ticks in size_t, which wraps correctly only
     *  where size_t is 32 bits. */
    static uint64_t GetTick() { return emu::NowUs() * (GetTickFreq() / 1000000); }
    static uint32_t GetTickFreq() { return 240000000; }

    /** APB2 clock with the core at 480 MHz. */
    static uint32_t GetPClk2Freq() { return 120000000; }
    static void     Delay(uint32_t ms) { emu::AdvanceUs(uint64_t(ms) * 1000); }
    static void     DelayUs(uint32_t us) { emu::AdvanceUs(us); }
};
} // namespace daisy

#endif
