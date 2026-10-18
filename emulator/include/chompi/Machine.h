// One emulated CHOMPI running one of its firmwares.
//
// Time is virtual: each Process() call is one 24-sample, 48 kHz audio block
// (0.5 ms). Per block a firmware's port runs what the Daisy Seed runs in that
// time: the audio interrupt, the 1 kHz SD-card interrupt on every second block,
// and one pass of the main loop. Firmware time advances only through Process(),
// so a render depends on the blocks and inputs fed to it, not on wall-clock
// speed.
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

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
        Wait,   // value: ms of the unit's time the events queued after it wait (gestures held
                // for a time); taken by the queue, firmwares never see it
    };
    Type    type;
    int16_t id;
    int16_t value;
};

/** Front-panel state a DAW session restores, as named values; the hardware
 *  forgets it at power off. Each firmware defines its own names. */
using Session = std::map<std::string, std::string>;

/** Audio the unit keeps only in RAM, by the firmware's names (TAPE: "buffer",
 *  the CHIMPO buffer, and "looper"): interleaved stereo 16-bit at 48 kHz. */
using Recordings = std::map<std::string, std::vector<int16_t>>;

/** Where audio dropped on a panel key goes: a card file or a recording, as
 *  48 kHz 16-bit stereo. */
struct ImportTarget
{
    std::string              file;      // card path, or
    std::string              recording; // a Recordings name
    std::vector<std::string> obsolete;  // card files to delete with it
    double                   maxSeconds = 0;
    std::string              label; // e.g. "Jammi a3"
    int                      mode = 0, bank = 0, slot = 0; // the firmware's, for CompleteImports()
};

/** A panel value a host can automate: what one knob sets on one of its pages
 *  (or in the shift layer), 0-1 as the firmware keeps it. */
struct ControlInfo
{
    enum class Unit : uint8_t
    {
        Percent,
        Pan,
        SampleSpeed, // TAPE's sample speed knob: -2x (reverse) to 2x
        LooperSpeed, // -2x to 2x, linear
        Semitones,   // -12 to 12, linear
        Frame,       // a wavetable's 33 frames
        DelayReverb, // delay below the middle, reverb above
        Tempo,       // 160-480, the firmware's tempo number
    };
    const char* id; // saved in projects with their automation: never change or reuse one
    const char* name;
    float       defaultValue; // at power-on with the factory card
    Unit        unit;
};

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

    /** Sizes (int16 values) of what CaptureRecordings() would copy now: the
     *  recordings that differ from power-on. Cheap. */
    virtual void RecordingSizes(std::map<std::string, size_t>& out) { out.clear(); }

    /** Copies them into out, reusing its vectors' capacity, so a caller that
     *  pre-sizes them from RecordingSizes() keeps allocation out of the copy.
     *  While a restore is pending, the restored ones. */
    virtual void CaptureRecordings(Recordings& out) { out.clear(); }

    /** Applied with the session (before it); a recording missing here returns
     *  to its power-on state. */
    virtual void RestoreRecordings(Recordings) {}

    /** Frees restored recordings that Process() has applied, so the audio
     *  thread never frees them (up to 63 MB). Lock-free against Process(),
     *  but from one thread only. */
    virtual void FreeSpent() {}

    /** Where audio dropped on `button` (Panel.h) goes, given what the panel
     *  shows; false if that key takes none. */
    virtual bool PlanImport(int, ImportTarget&) { return false; }

    /** After a restart with imported audio in place, applied after the
     *  session: what the firmware does with a new sample in a slot (default
     *  settings), selecting the first. */
    virtual void CompleteImports(std::vector<ImportTarget>) {}

    /** Booted and past the boot animation: takes input as after a power-on wait. */
    virtual bool IsReady() { return IsBooted(); }

    /** Puts the firmware's clock in sync mode (follow MIDI clock and
     *  start/stop on the input) or back to its own tempo, as its panel would.
     *  Applied once boot has finished; firmwares without one ignore it. */
    virtual void SetClockSync(bool) {}

    /** The values a host can automate on `firmware`; SetControl() and
     *  ReadControl() take indices into it. */
    static const std::vector<ControlInfo>& Controls(Firmware firmware);

    /** Turns the knob that sets control `index` toward `value` from the next
     *  block on which the unit takes input: clicks through the firmware's own
     *  handler for that knob and page (or shift layer), until the value is
     *  within half a click or stops moving. The page on show stays. */
    void SetControl(int index, float value);

    /** The control's value now; true if it moved other than by SetControl()
     *  since the last call (panel, MIDI, slot or session change). */
    bool ReadControl(int index, float& value);

    // ---- Any thread ----

    /** Queues a panel interaction, applied at the start of the next block.
     *  False if the queue is full. */
    bool Post(const PanelEvent& event);

    /** Posted events no block has taken yet: waiting behind a Wait, or the
     *  unit isn't running. */
    bool HasPendingEvents() const
    {
        return queue_read_.load(std::memory_order_acquire) != queue_write_.load(std::memory_order_acquire);
    }

    /** TAPE's looper as of the last block; Empty on the others. */
    enum class LooperState : uint8_t
    {
        Empty,
        Armed,    // to record from the next note
        Recorded, // or recording
        Erasing,  // until done and play and loop are up: takes no arming
    };
    LooperState GetLooperState() const { return looper_state_.load(std::memory_order_relaxed); }

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

    /** Turns the knobs SetControl() asked for; call where the unit takes input. */
    void ChaseControls();
    virtual float ControlValue(int) { return 0.f; }
    /** At most maxClicks clicks of control `index` toward `target`. False to
     *  carry on next block: more clicks needed, or the firmware takes none now. */
    virtual bool TurnControl(int, float, int) { return true; }

    std::shared_ptr<VirtualCard> card_;
    std::unique_ptr<Context>     ctx_;
    int                          midi_in_channel_ = 0;
    std::atomic<bool>            booted_{false};
    std::atomic<LooperState>     looper_state_{LooperState::Empty};

  private:
    Firmware firmware_;

    // Sized at construction; the audio thread never allocates.
    std::vector<float>   control_target_;
    std::vector<float>   control_seen_; // as last read, or as a chase left it
    std::vector<uint8_t> control_pending_;
    int                  controls_pending_ = 0;

    static constexpr uint32_t kQueueSize = 512;
    PanelEvent                queue_[kQueueSize];
    std::atomic<uint32_t>     queue_write_{0};
    std::atomic<uint32_t>     queue_read_{0};
    std::atomic_flag          queue_producer_ = ATOMIC_FLAG_INIT;
    bool                      waiting_       = false; // on the Wait at the queue's front, until:
    uint64_t                  wait_until_us_ = 0;
};

} // namespace emu
