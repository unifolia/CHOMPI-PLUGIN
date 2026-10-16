// Emulator stand-in for TAPE's hardware.h: the shared emulated hardware plus
// TAPE's MIDI interface (it reads and sends MIDI through Hardware directly),
// on the machine's MIDI queues.
#pragma once
#include "emu_hardware.h"

namespace chompi
{
class Hardware : public HardwareBase
{
  public:
    /** The next channel message from the emulated MIDI input. TAPE ignores
     *  system messages, so they are skipped. */
    bool GetMidi(MidiEvent& event)
    {
        emu::MidiMessage m;
        while(emu::current->midi_in.Pop(m))
        {
            const uint8_t status = m.bytes[0];
            if(status < 0x80 || status >= 0xf0)
                continue;

            event         = MidiEvent{};
            event.channel = status & 0x0f;
            event.data[0] = m.size > 1 ? m.bytes[1] & 0x7f : 0;
            event.data[1] = m.size > 2 ? m.bytes[2] & 0x7f : 0;
            switch(status & 0xf0)
            {
                case 0x80: event.type = NoteOff; break;
                // libDaisy's parser turns velocity-0 note-ons into note-offs.
                case 0x90: event.type = event.data[1] == 0 ? NoteOff : NoteOn; break;
                case 0xa0: event.type = PolyphonicKeyPressure; break;
                case 0xb0: event.type = ControlChange; break;
                case 0xc0: event.type = ProgramChange; break;
                case 0xd0: event.type = ChannelPressure; break;
                default: event.type = PitchBend; break;
            }
            return true;
        }
        return false;
    }

    void SendMessage(uint8_t*, size_t) {}

    bool usb_midi_active = true;
    void ResetUSBMidi() {}
    void USBMidiActive(bool a) { usb_midi_active = a; }

    void SendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity)
    {
        SendMidi(0x90 | (channel & 0x0f), note, velocity);
    }
    void SendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity)
    {
        SendMidi(0x80 | (channel & 0x0f), note, velocity);
    }
    void SendCC(uint8_t channel, uint8_t control_number, uint8_t value)
    {
        SendMidi(0xb0 | (channel & 0x0f), control_number, value);
    }

  private:
    void SendMidi(uint8_t status, uint8_t d1, uint8_t d2)
    {
        emu::current->midi_out.Push({{status, uint8_t(d1 & 0x7f), uint8_t(d2 & 0x7f)}, 3});
    }
};

} // namespace chompi
