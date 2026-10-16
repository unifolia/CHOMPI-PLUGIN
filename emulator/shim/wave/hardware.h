// Emulator stand-in for WAVE's hardware.h: the shared emulated hardware plus
// WAVE's outgoing MIDI queue, which its MidiManager drains (MIDI input reaches
// MidiManager through the libDaisy MIDI handlers, emu_peripherals.h).
#pragma once
#include "emu_hardware.h"

namespace chompi
{
class Hardware : public HardwareBase
{
  public:
    // Verbatim from WAVE's hardware.h, which leaves the fields MidiManager
    // doesn't read uninitialised.

    // This stuff lives here because its visible to both the UI and midi manager without
    // creating a circular dependency. Ideally it would go somewhere else
    void queueMidiNote(uint8_t channel, uint8_t note, uint8_t velocity, MidiMessageType type){
        MidiEvent event;
        event.type = type;
        event.channel = channel;
        event.data[0] = note;
        midi_out_queue.PushBack(event);
    }

    void setMidiCCOut(bool enable) {
        midi_cc_out = enable;
    }

    void queueMidiTransport(bool start)
    {
        MidiEvent event;
        event.type = MidiMessageType::SystemRealTime;
        event.srt_type = start ? SystemRealTimeType::Start : SystemRealTimeType::Stop;
        midi_out_queue.PushBack(event);
    }

    void SendCC(uint8_t channel, uint8_t control_number, uint8_t value)
    {
        if (!midi_cc_out) {
            return;
        }
        MidiEvent event;
        event.type = MidiMessageType::ControlChange;
        event.channel = channel;
        event.data[0] = control_number;
        event.data[1] = value;
        midi_out_queue.PushBack(event);
    }

    FIFO<MidiEvent, 64> midi_out_queue;
    bool midi_cc_out = true;
};

} // namespace chompi
