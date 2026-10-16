// Stand-ins for the libDaisy peripherals WAVE and TEMPO drive directly: MIDI
// over UART (DMA transmit) and USB, the MIDI-clock timer, and the Cortex-M
// cache and interrupt calls around them.
//
// The UART is the emulated unit's one MIDI port (Context::midi_in/out). The
// firmwares send every message on both UART and USB, so USB MIDI is inert:
// the plugin has one MIDI port, and each message must leave it once.
#pragma once
#include <cstddef>
#include <cstdint>

#include "emu_context.h"
#include "hid/midi_parser.h"
#include "util/FIFO.h"

inline void SCB_CleanDCache_by_Addr(uint32_t*, int32_t) {}
inline void __disable_irq() {}
inline void __enable_irq() {}

namespace daisy
{
class UartHandler
{
  public:
    enum class Result
    {
        OK,
        ERR
    };
    typedef void (*StartCallbackFunctionPtr)(void* context);
    typedef void (*EndCallbackFunctionPtr)(void* context, Result result);

    /** The transfer completes at once, onto the MIDI output; the firmware's
     *  queue then sends its next message from the end callback. */
    Result DmaTransmit(uint8_t*                 buff,
                       size_t                   size,
                       StartCallbackFunctionPtr start_callback,
                       EndCallbackFunctionPtr   end_callback,
                       void*                    callback_context)
    {
        if(start_callback != nullptr)
            start_callback(callback_context);
        if(size >= 1 && size <= 3)
        {
            emu::MidiMessage m{};
            for(size_t i = 0; i < size; i++)
                m.bytes[i] = buff[i];
            m.size = uint8_t(size);
            emu::current->midi_out.Push(m);
        }
        if(end_callback != nullptr)
            end_callback(callback_context, Result::OK);
        return Result::OK;
    }
};

class MidiUartTransport
{
  public:
    struct Config
    {
    };
    UartHandler& GetUartHandle() { return uart_; }

  private:
    UartHandler uart_;
};

class MidiUsbTransport
{
  public:
    struct Config
    {
        enum Periph
        {
            INTERNAL = 0,
            EXTERNAL,
            HOST
        };
        Periph  periph         = INTERNAL;
        uint8_t tx_retry_count = 3;
    };
};

/** libDaisy's MidiHandler<Transport> interface. Received bytes are parsed by
 *  libDaisy's own parser into the same 256-event queue. */
template <typename Transport, bool kReceives>
class EmuMidiHandler
{
  public:
    struct Config
    {
        typename Transport::Config transport_config;
    };

    void Init(Config) { parser_.Init(); }
    void ResetTransport() {}
    void StartReceive() {}

    /** On hardware the UART interrupt parses as bytes arrive; here, whatever
     *  arrived since the firmware last listened. */
    void Listen()
    {
        if(!kReceives)
            return;
        emu::MidiMessage m;
        while(emu::current->midi_in.Pop(m))
            for(uint8_t i = 0; i < m.size; i++)
            {
                MidiEvent event;
                if(parser_.Parse(m.bytes[i], &event))
                    event_q_.PushBack(event);
            }
    }

    bool      HasEvents() const { return event_q_.GetNumElements() > 0; }
    MidiEvent PopEvent() { return event_q_.PopFront(); }

    // Direct sends (the firmware uses DMA on the UART): USB is inert.
    void SendMessage(uint8_t*, size_t) {}
    bool SendNoteOn(uint8_t, uint8_t, uint8_t) { return true; }
    bool SendNoteOff(uint8_t, uint8_t, uint8_t) { return true; }
    bool SendCC(uint8_t, uint8_t, uint8_t) { return true; }

    Transport& GetMutableTransport() { return transport_; }

  private:
    Transport            transport_;
    MidiParser           parser_;
    FIFO<MidiEvent, 256> event_q_;
};

using MidiUartHandler = EmuMidiHandler<MidiUartTransport, true>;
using MidiUsbHandler  = EmuMidiHandler<MidiUsbTransport, false>;

/** A hardware timer on the virtual clock. The kernel clock is 240 MHz, as on
 *  the Daisy Seed's APB1/APB2 timers (PCLK x2); the period elapses every
 *  (prescaler + 1) * (period + 1) cycles. A new period takes effect from the
 *  next update. */
class TimerHandle
{
  public:
    struct Config
    {
        enum class Peripheral
        {
            TIM_2,
            TIM_3,
            TIM_4,
            TIM_5,
            TIM_16,
            TIM_17,
        };
        enum class CounterDir
        {
            UP,
            DOWN,
        };
        Peripheral periph     = Peripheral::TIM_2;
        CounterDir dir        = CounterDir::UP;
        bool       enable_irq = false;
    };
    enum class Result
    {
        OK,
        ERR,
    };
    typedef void (*PeriodElapsedCallback)(void* data);

    static constexpr uint64_t kKernelHz = 240000000;

    Result Init(const Config& config)
    {
        config_ = config;
        return Result::OK;
    }
    Result Start()
    {
        running_     = true;
        next_cycles_ = Now() + Interval();
        return Result::OK;
    }
    Result Stop()
    {
        running_ = false;
        return Result::OK;
    }
    Result SetPeriod(uint32_t ticks)
    {
        period_ = ticks;
        return Result::OK;
    }
    Result SetPrescaler(uint32_t val)
    {
        prescaler_ = val;
        return Result::OK;
    }
    void SetCallback(PeriodElapsedCallback cb, void* data = nullptr)
    {
        callback_ = cb;
        data_     = data;
    }

    /** Emulator: runs the callback for every period elapsed by now. */
    void Advance()
    {
        while(running_ && Now() >= next_cycles_)
        {
            next_cycles_ += Interval();
            if(callback_ != nullptr && config_.enable_irq)
                callback_(data_);
        }
    }

  private:
    static uint64_t Now() { return emu::NowUs() * (kKernelHz / 1000000); }
    uint64_t        Interval() const { return uint64_t(prescaler_ + 1) * (uint64_t(period_) + 1); }

    Config                config_;
    bool                  running_   = false;
    uint32_t              period_    = 0xffff;
    uint32_t              prescaler_ = 0;
    uint64_t              next_cycles_ = 0;
    PeriodElapsedCallback callback_  = nullptr;
    void*                 data_      = nullptr;
};
} // namespace daisy
