// State the firmware reaches through free functions and globals (System clock,
// LED buffers, FatFs, MIDI ports), redirected to the machine that is currently
// executing. Each Machine owns one Context and installs it around every entry
// into firmware code, so several plugin instances can run on parallel threads.
//
// Shared by every firmware build: its layout must not depend on one.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace emu
{
class VirtualCard;

// LED chain lengths including the 6-LED porch at each end (temp_led_stuff.h).
constexpr int kLedPorch      = 6;
constexpr int kPthLedsTotal  = 10 + 2 * kLedPorch;
constexpr int kSmtLedsTotal  = 25 + 2 * kLedPorch;
constexpr int kPthLedsActive = 10;
constexpr int kSmtLedsActive = 25;

/** Raw MIDI as it crosses the port: a channel message or a real-time byte. */
struct MidiMessage
{
    uint8_t bytes[3];
    uint8_t size;
};

/** Holds up to N messages, like libDaisy's FIFO<T, N>. Single-threaded: the
 *  machine's thread fills and drains both directions between blocks. */
template <size_t N>
struct MidiQueue
{
    bool Push(const MidiMessage& m)
    {
        if(count_ == N)
            return false;
        buf_[(head_ + count_) % N] = m;
        count_++;
        return true;
    }
    bool Pop(MidiMessage& m)
    {
        if(count_ == 0)
            return false;
        m     = buf_[head_];
        head_ = (head_ + 1) % N;
        count_--;
        return true;
    }
    bool Empty() const { return count_ == 0; }

  private:
    MidiMessage buf_[N];
    size_t      head_ = 0, count_ = 0;
};

struct Context
{
    uint64_t clock_us = 0;

    // led_pth_data / led_smt_data, after the firmware's /11 and /4 dimming.
    uint8_t led_pth[kPthLedsTotal][3]{};
    uint8_t led_smt[kSmtLedsTotal][3]{};

    // Published by fill_led_data() for the GUI thread: 0x00RRGGBB.
    std::atomic<uint32_t> pth_out[kPthLedsActive]{};
    std::atomic<uint32_t> smt_out[kSmtLedsActive]{};

    VirtualCard* card = nullptr;

    MidiQueue<128> midi_in;
    MidiQueue<256> midi_out;
};

// Defined once in the shared emulator library (Machine.cpp): every firmware
// object must see the same one.
extern thread_local Context* current;

struct ScopedContext
{
    explicit ScopedContext(Context& c) : prev_(current) { current = &c; }
    ~ScopedContext() { current = prev_; }
    ScopedContext(const ScopedContext&)            = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

  private:
    Context* prev_;
};

inline uint64_t NowUs() { return current ? current->clock_us : 0; }
inline void     AdvanceUs(uint64_t us)
{
    if(current)
        current->clock_us += us;
}

} // namespace emu
