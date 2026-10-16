// One emulated CHOMPI running one of its firmwares.
//
// Time is virtual: each Process() call is one 24-sample, 48 kHz audio block
// (0.5 ms). Per block a firmware's port runs what the Daisy Seed runs in that
// time: the audio interrupt, the 1 kHz SD-card interrupt on every second block,
// and one pass of the main loop. Firmware time advances only through Process(),
// so a render depends on the blocks and inputs fed to it, not on wall-clock
// speed.
//
// The firmwares define the same class names differently, so each is compiled
// and prelinked on its own (CMakeLists.txt); only its factory crosses over.
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "chompi/Panel.h"

namespace emu
{
class VirtualCard;
struct Context;

enum class Firmware : uint8_t
{
    Tape,
    Wave,
    Tempo,
};

struct PanelEvent
{
    enum class Type : uint8_t
    {
        Button, // id: switch ID (Panel.h); value: 1 pressed, 0 released
        Turn,   // id: encoder (panel::Encoder); value: detents, + is clockwise
        Toggle, // value: 1 play position, 0 record position
    };
    Type    type;
    int16_t id;
    int16_t value;
};

/** Front-panel state a DAW session restores, as named values; the hardware
 *  forgets it at power off. Each firmware defines its own names. */
using Session = std::map<std::string, std::string>;

class Machine
{
  public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kBlockSize  = 24;
    static constexpr int kNumInputs  = 4; // mic, unused, line L, line R
    static constexpr int kNumOutputs = 4; // headphone L, R, line L, R

    /** Powers on a unit running `firmware`: runs its start-up up to the main
     *  loop; boot animations then play out over the first Process() calls.
     *  nullptr for a firmware that isn't built in. */
    static std::unique_ptr<Machine> Create(Firmware firmware, std::shared_ptr<VirtualCard> card);
    static bool                     IsBuiltIn(Firmware firmware);

    virtual ~Machine();

    Machine(const Machine&)            = delete;
    Machine& operator=(const Machine&) = delete;

    // ---- One thread at a time, never concurrently with Process() ----

    /** One block: in[kNumInputs], out[kNumOutputs], kBlockSize samples each. */
    virtual void Process(const float* const* in, float* const* out) = 0;

    /** A message arriving on the MIDI input: a channel message (1-3 bytes) or
     *  a system real-time byte. Dropped when the input queue is full. */
    void SendMidi(const uint8_t* bytes, int size);

    /** Next message the firmware sent to its MIDI output, into msg; its size,
     *  or 0 if there is none. */
    int ReceiveMidi(uint8_t msg[3]);

    /** The current session, or one restored but not yet applied. False while
     *  booting with nothing restored. */
    virtual bool CaptureSession(Session& out) = 0;

    /** Applied at the start of the first block after boot has finished. Names
     *  the firmware doesn't know, and missing ones, take defaults. */
    virtual void RestoreSession(const Session& session) = 0;

    /** Puts the firmware's clock in sync mode (follow MIDI clock and
     *  start/stop on the input) or back to its own tempo, as its panel would.
     *  Applied once boot has finished; firmwares without one ignore it. */
    virtual void SetClockSync(bool) {}

    // ---- Any thread ----

    /** Queues a panel interaction, applied at the start of the next block.
     *  False if the queue is full. */
    bool Post(const PanelEvent& event);

    /** LED colors as the firmware last sent them, 0x00RRGGBB, still dimmed
     *  (full scale is panel::kPthFullScale / kSmtFullScale). */
    uint32_t PthLed(int index) const;
    uint32_t SmtLed(int index) const;

    bool IsBooted() const { return booted_.load(std::memory_order_relaxed); }

    /** MIDI input channel (0-15) from the card's options.json. */
    int MidiInChannel() const { return midi_in_channel_; }

    Firmware GetFirmware() const { return firmware_; }

  protected:
    Machine(Firmware firmware, std::shared_ptr<VirtualCard> card);

    /** Next panel event Post() queued; false once drained. */
    bool PopPanelEvent(PanelEvent& event);

    std::shared_ptr<VirtualCard> card_;
    std::unique_ptr<Context>     ctx_;
    int                          midi_in_channel_ = 0;
    std::atomic<bool>            booted_{false};

  private:
    Firmware firmware_;

    static constexpr uint32_t kQueueSize = 512;
    PanelEvent                queue_[kQueueSize];
    std::atomic<uint32_t>     queue_write_{0};
    std::atomic<uint32_t>     queue_read_{0};
    std::atomic_flag          queue_producer_ = ATOMIC_FLAG_INIT;
};

} // namespace emu
