// What every firmware's machine shares: panel and MIDI queues, LED readout,
// and the factory. Built once, outside the firmware objects.
#include "chompi/Machine.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "emu_context.h"
#include "emu_controls.h"

// Each firmware object exports only its factory, with C linkage so the export
// list (CMakeLists.txt) needn't spell a mangled name. CHOMPI_HAS_<NAME> says
// which are built.
using Factory = emu::Machine* (*)(const std::shared_ptr<emu::VirtualCard>* card);
#define CHOMPI_FACTORY(name) extern "C" emu::Machine* chompi_create_##name(const std::shared_ptr<emu::VirtualCard>* card);
CHOMPI_FACTORY(tape)
CHOMPI_FACTORY(wave)
CHOMPI_FACTORY(tempo)
#undef CHOMPI_FACTORY

namespace emu
{
thread_local Context* current = nullptr;

namespace
{
Factory FactoryFor(Firmware firmware)
{
    Factory create = nullptr;
    switch(firmware)
    {
#if CHOMPI_HAS_TAPE
        case Firmware::Tape: create = &chompi_create_tape; break;
#endif
#if CHOMPI_HAS_WAVE
        case Firmware::Wave: create = &chompi_create_wave; break;
#endif
#if CHOMPI_HAS_TEMPO
        case Firmware::Tempo: create = &chompi_create_tempo; break;
#endif
        default: break;
    }
    return create;
}
} // namespace

std::unique_ptr<Machine> Machine::Create(Firmware firmware, std::shared_ptr<VirtualCard> card)
{
    const Factory create = FactoryFor(firmware);
    return create != nullptr ? std::unique_ptr<Machine>(create(&card)) : nullptr;
}

bool Machine::IsBuiltIn(Firmware firmware)
{
    return FactoryFor(firmware) != nullptr;
}

Machine::Machine(Firmware firmware, std::shared_ptr<VirtualCard> card)
: card_(std::move(card)), ctx_(std::make_unique<Context>()), firmware_(firmware)
{
    ctx_->card = card_.get();
    const size_t controls = Controls(firmware).size();
    control_target_.assign(controls, 0.f);
    control_seen_.assign(controls, NAN); // the first read reports the power-on values
    control_pending_.assign(controls, 0);
}

const std::vector<ControlInfo>& Machine::Controls(Firmware firmware)
{
    static const std::vector<ControlInfo> tape(std::begin(controls::kTapeInfo), std::end(controls::kTapeInfo));
    static const std::vector<ControlInfo> wave(std::begin(controls::kWaveInfo), std::end(controls::kWaveInfo));
    static const std::vector<ControlInfo> tempo(std::begin(controls::kTempoInfo), std::end(controls::kTempoInfo));
    switch(firmware)
    {
        case Firmware::Tape: return tape;
        case Firmware::Wave: return wave;
        case Firmware::Tempo: return tempo;
    }
    return tape;
}

void Machine::SetControl(int index, float value)
{
    if(index < 0 || size_t(index) >= control_target_.size() || !std::isfinite(value))
        return;
    control_target_[size_t(index)] = std::clamp(value, 0.f, 1.f);
    if(!control_pending_[size_t(index)])
    {
        control_pending_[size_t(index)] = 1;
        controls_pending_++;
    }
}

bool Machine::ReadControl(int index, float& value)
{
    if(index < 0 || size_t(index) >= control_seen_.size())
        return false;
    value = ControlValue(index);
    if(control_pending_[size_t(index)]) // the host's value is on its way
        return false;
    float&     seen    = control_seen_[size_t(index)];
    const bool changed = !(value == seen); // NaN: never read
    seen               = value;
    return changed;
}

void Machine::ChaseControls()
{
    // Bounds the work, and the burst of MIDI CC the firmware sends per click,
    // in one block: a full sweep of a fine-step knob (333 clicks) takes 21
    // blocks, 10 ms.
    constexpr int kMaxClicksPerBlock = 16;

    for(size_t i = 0; i < control_pending_.size() && controls_pending_ > 0; i++)
    {
        if(!control_pending_[i])
            continue;
        const float before = ControlValue(int(i));
        const bool  done   = TurnControl(int(i), control_target_[i], kMaxClicksPerBlock);
        if(const float after = ControlValue(int(i)); done || after != before)
            control_seen_[i] = after; // not reported: the host asked for it
        if(done)
        {
            control_pending_[i] = 0;
            controls_pending_--;
        }
    }
}

Machine::~Machine() = default;

void Machine::SendMidi(const uint8_t* bytes, int size)
{
    if(size < 1 || size > 3)
        return;
    MidiMessage m{};
    for(int i = 0; i < size; i++)
        m.bytes[i] = bytes[i];
    m.size = uint8_t(size);
    ctx_->midi_in.Push(m);
}

int Machine::ReceiveMidi(uint8_t msg[3])
{
    MidiMessage m;
    if(!ctx_->midi_out.Pop(m))
        return 0;
    for(int i = 0; i < 3; i++)
        msg[i] = m.bytes[i];
    return m.size;
}

bool Machine::Post(const PanelEvent& event)
{
    // Producers serialise on a spin flag; the consumer (Process) stays lock-free.
    while(queue_producer_.test_and_set(std::memory_order_acquire))
    {
    }
    const uint32_t write = queue_write_.load(std::memory_order_relaxed);
    const uint32_t next  = (write + 1) % kQueueSize;
    const bool     room  = next != queue_read_.load(std::memory_order_acquire);
    if(room)
    {
        queue_[write] = event;
        queue_write_.store(next, std::memory_order_release);
    }
    queue_producer_.clear(std::memory_order_release);
    return room;
}

bool Machine::PopPanelEvent(PanelEvent& event)
{
    for(;;)
    {
        const uint32_t read = queue_read_.load(std::memory_order_relaxed);
        if(read == queue_write_.load(std::memory_order_acquire))
            return false;
        event = queue_[read];
        if(event.type == PanelEvent::Type::Wait)
        {
            if(!waiting_)
            {
                waiting_       = true;
                wait_until_us_ = ctx_->clock_us + uint64_t(std::max<int16_t>(event.value, 0)) * 1000;
            }
            if(ctx_->clock_us < wait_until_us_)
                return false;
            waiting_ = false;
        }
        queue_read_.store((read + 1) % kQueueSize, std::memory_order_release);
        if(event.type != PanelEvent::Type::Wait)
            return true;
    }
}

uint32_t Machine::PthLed(int index) const
{
    return ctx_->pth_out[index].load(std::memory_order_relaxed);
}

uint32_t Machine::SmtLed(int index) const
{
    return ctx_->smt_out[index].load(std::memory_order_relaxed);
}

} // namespace emu
