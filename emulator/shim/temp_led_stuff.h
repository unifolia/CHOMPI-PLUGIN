// Emulator stand-in for the firmware's LED driver (temp_led_stuff.h). The
// hardware streams two addressable-LED chains over timer PWM + DMA; here the RGB
// buffers live in the executing machine's Context and fill_led_data() publishes
// them for the GUI. Setters and color helpers are kept verbatim, including the
// /11 (PTH) and /4 (SMT) dimming the firmware applies.
#pragma once
#include <algorithm>
#include "daisy_seed.h"
#include "emu_context.h"

namespace chompi
{
const int kNumPthLeds = emu::kPthLedsTotal;
const int kNumSmtLeds = emu::kSmtLedsTotal;

// Firmware code indexes these like the original globals (led_pth_data[i][0],
// &led_pth_data[0][0]); the proxies resolve to the executing machine's buffers.
struct PthLedBuffer
{
    uint8_t (&operator[](size_t i) const)[3] { return emu::current->led_pth[i]; }
};
struct SmtLedBuffer
{
    uint8_t (&operator[](size_t i) const)[3] { return emu::current->led_smt[i]; }
};
inline constexpr PthLedBuffer led_pth_data{};
inline constexpr SmtLedBuffer led_smt_data{};

inline void LedSetup()
{
    std::fill(&led_pth_data[0][0], &led_pth_data[0][0] + kNumPthLeds * 3, 0);
    std::fill(&led_smt_data[0][0], &led_smt_data[0][0] + kNumSmtLeds * 3, 0);
}

inline void fill_led_data()
{
    auto* ctx  = emu::current;
    auto  pack = [](const uint8_t* c) {
        return (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | uint32_t(c[2]);
    };
    for(int i = 0; i < emu::kPthLedsActive; i++)
        ctx->pth_out[i].store(pack(ctx->led_pth[i]), std::memory_order_relaxed);
    for(int i = 0; i < emu::kSmtLedsActive; i++)
        ctx->smt_out[i].store(pack(ctx->led_smt[i]), std::memory_order_relaxed);
}

inline void SetPthLed(int index, uint8_t r, uint8_t g, uint8_t b)
{
    led_pth_data[index][0] = r / 11;
    led_pth_data[index][1] = g / 11;
    led_pth_data[index][2] = b / 11;
}
inline void SetSmtLed(int index, uint8_t r, uint8_t g, uint8_t b)
{
    led_smt_data[index][0] = r / 4;
    led_smt_data[index][1] = g / 4;
    led_smt_data[index][2] = b / 4;
}

inline void SetPthLedFloat(int index, float r, float g, float b)
{
    SetPthLed(index, r * 255, g * 255, b * 255);
}
inline void SetSmtLedFloat(int index, float r, float g, float b)
{
    SetSmtLed(index, r * 255, g * 255, b * 255);
}

// ======== helper functions for color crossfading ========
inline float color_xfade(float start, float end, float idx)
{
    return (1.f - idx) * start + idx * end;
}

inline float color_triple_xfade(float start, float mid, float end, float idx)
{
    if(idx < .5f)
    {
        idx *= 2.f;
        return color_xfade(start, mid, idx);
    }
    else
    {
        idx = (idx - .5f) * 2.f;
        return color_xfade(mid, end, idx);
    }
}

inline float color_quad_xfade(float start, float mid1, float mid2, float end, float idx)
{
    if(idx < .33f)
    {
        idx *= 3.f;
        return color_xfade(start, mid1, idx);
    }
    else if(idx < .66f)
    {
        idx = (idx - .33f) * 3.f;
        return color_xfade(mid1, mid2, idx);
    }
    else
    {
        idx = (idx - .66f) * 3.f;
        return color_xfade(mid2, end, idx);
    }
}
} // namespace chompi
