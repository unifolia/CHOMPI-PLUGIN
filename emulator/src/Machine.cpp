// What every firmware's machine shares: panel and MIDI queues, LED readout,
// and the factory. Built once, outside the firmware objects.
#include "chompi/Machine.h"

#include "emu_context.h"

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
    const uint32_t read = queue_read_.load(std::memory_order_relaxed);
    if(read == queue_write_.load(std::memory_order_acquire))
        return false;
    event = queue_[read];
    queue_read_.store((read + 1) % kQueueSize, std::memory_order_release);
    return true;
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
