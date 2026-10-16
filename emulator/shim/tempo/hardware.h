// Emulator stand-in for TEMPO's hardware.h: the shared emulated hardware plus
// TEMPO's outgoing MIDI queue, which its MidiManager drains (MIDI input reaches
// MidiManager through the libDaisy MIDI handlers, emu_peripherals.h).
#pragma once
#include "emu_hardware.h"

namespace chompi
{
class Hardware : public HardwareBase
{
  public:
    // Verbatim from TEMPO's hardware.h, which leaves the fields MidiManager
    // doesn't read uninitialised.

    void setMidiCCOut(bool enable) {
        midi_cc_out = enable;
    }

    void queueMidiNote(uint8_t channel, uint8_t note, uint8_t velocity, MidiMessageType type){
        MidiEvent event;
        event.type = type;
        event.channel = channel;
        event.data[0] = note;
        midi_out_queue.PushBack(event);
    }

    // turn off CC sending for initial FW
    void queueMidiCC(uint8_t channel, uint8_t control_number, uint8_t value)
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

    void queueMidiTransport(uint8_t channel, bool start) {
        MidiEvent event;
        event.type = MidiMessageType::SystemRealTime;
        event.srt_type = start ? SystemRealTimeType::Start : SystemRealTimeType::Stop;
        event.channel = channel;
        midi_out_queue.PushBack(event);
    }

    void queueMidiClock(uint8_t channel) {
        MidiEvent event;
        event.type = MidiMessageType::SystemRealTime;
        event.srt_type = SystemRealTimeType::TimingClock;
        event.channel = channel;
        midi_out_queue.PushBack(event);
    }

    FIFO<MidiEvent, 64> midi_out_queue;

    bool midi_cc_out = true;
};

} // namespace chompi
