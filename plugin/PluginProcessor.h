#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <bitset>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "OutputStage.h"
#include "RateAdapter.h"
#include "chompi/Machine.h"
#include "chompi/VirtualCard.h"

class ChompiProcessor : public juce::AudioProcessor, private RateAdapter::BlockRenderer
{
  public:
    ChompiProcessor();
    ~ChompiProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool                        hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool               acceptsMidi() const override { return true; }
    bool               producesMidi() const override { return true; }
    // The unit sounds without input (looper, TEMPO's patterns, WAVE's
    // sequencer, delay feedback). VST3: kInfiniteTail, never suspend.
    double             getTailLengthSeconds() const override { return std::numeric_limits<double>::infinity(); }

    int                getNumPrograms() override { return 1; }
    int                getCurrentProgram() override { return 0; }
    void               setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void               changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ---- Any thread but the audio thread ----

    void post(const emu::PanelEvent& event);

    struct Leds
    {
        juce::Colour pth[emu::panel::kNumPthLeds];
        juce::Colour smt[emu::panel::kNumSmtLeds];
    };
    /** LED colors at full scale; false, and all black, while no machine runs. */
    bool readLeds(Leds& leds) const;

    bool isRunning() const;

    /** Changes whenever a new machine starts (card change, reload). */
    int machineGeneration() const { return generation_.load(); }

    /** Runs the machine on wall-clock time while the host isn't processing
     *  (e.g. a host that stops rendering idle instruments), so the panel stays
     *  live. Called by the editor's timer. */
    void idleTick();

    /** Switching power-cycles the unit on that firmware's default card: each
     *  firmware has its own card layout. */
    emu::Firmware getFirmware() const { return firmware_.load(); }
    void          setFirmware(emu::Firmware firmware);
    static juce::String firmwareName(emu::Firmware firmware); // "TAPE 2.0"

    /** The folder the project asks for, even while it's missing. */
    juce::File getCardFolder() const;
    /** True when that folder wasn't found at power-on and the unit runs on the
     *  default card instead. */
    bool       isCardMissing() const { return cardMissing_.load(); }
    juce::File getActiveCardFolder() const
    {
        return isCardMissing() ? defaultCardFolder(getFirmware()) : getCardFolder();
    }
    /** Power-cycles the unit on `folder` (empty: the current folder, re-read). */
    void insertCard(const juce::File& folder);
    /** Power-cycles the unit after copying the factory files over its card. */
    void restoreFactoryCard();
    static juce::File factoryCardFolder(emu::Firmware firmware);
    static juce::File defaultCardFolder(emu::Firmware firmware);

    /** Mode switch position; survives editor close and machine restarts. */
    bool getTogglePlay() const { return togglePlay_.load(); }
    void setTogglePlay(bool play);

    bool getMidiOmni() const { return midiOmni_.load(); }
    void setMidiOmni(bool omni) { midiOmni_.store(omni); }

    /** TEMPO follows host tempo and transport (followHost()); needs a host
     *  that reports a tempo. */
    bool getHostSync() const { return hostSync_.load(); }
    void setHostSync(bool sync) { hostSync_.store(sync); }

    struct ImportPlan
    {
        int               button;
        emu::ImportTarget target;
    };
    /** The keys that numFiles audio files dropped on `button` fill: it, then
     *  the white keys after it while they take card slots. Empty if it takes
     *  none. */
    std::vector<ImportPlan> planImport(int button, int numFiles);

    /** Converts the files and restarts the unit with them where planImport()
     *  puts them, keeping the session and recordings; the restart skips the
     *  boot animation. Returns what the panel says about it. */
    juce::String importAudio(const juce::StringArray& files, int button);

    juce::Point<int> getEditorSize() const { return {editorWidth_.load(), editorHeight_.load()}; }
    void             setEditorSize(juce::Point<int> size)
    {
        editorWidth_.store(size.x);
        editorHeight_.store(size.y);
    }

  private:
    void renderBlock(uint64_t index, const float* const* in, float* const* out) override;

    struct SyncEvent
    {
        int     offset; // host sample in this block
        uint8_t status; // clock, start or stop
    };
    static constexpr int kMaxSyncEvents = 256;
    /** TEMPO's clock and transport for this block, in time order; 0 when not
     *  following the host. Audio thread. */
    int followHost(int numSamples, SyncEvent* events);
    struct CardWrite
    {
        std::string              path;
        std::vector<uint8_t>     bytes;
        std::vector<std::string> obsolete; // deleted first
    };
    static void writeCardFile(emu::VirtualCard& card, const CardWrite& write);

    void startMachine(const juce::File& folder, bool restoreFactory, bool fastBoot = false);
    void stopMachine();
    /** The pending toggle, session, recordings and imports; under stateMutex_. */
    void applyPending(emu::Machine& machine);
    /** Under stateMutex_: false when the live machine has no session yet. */
    bool captureLive(emu::Session& session, emu::Recordings& recordings);
    std::shared_ptr<emu::Machine> currentMachine() const;
    /** The recordings a state carries, decoded; primes encoded_ with them. */
    emu::Recordings readRecordings(const juce::XmlElement& xml, const void* data, int sizeInBytes);

    // Locking: the audio thread only ever try-locks machineLock_, which guards
    // machine_. Every other thread reaches the machine through live_ under
    // stateMutex_ (taken before machineLock_ when both are needed), so the GUI
    // never waits on a whole audio block. Machines are shared, so the last
    // holder destroys one, and the audio thread never holds a reference.
    juce::SpinLock                machineLock_;
    std::shared_ptr<emu::Machine> machine_;

    mutable std::mutex            stateMutex_;
    std::shared_ptr<emu::Machine> live_;
    juce::File                    cardFolder_;
    emu::Session                  pendingSession_; // from setStateInformation, for the next machine
    bool                          hasPendingSession_ = false;
    emu::Recordings               pendingRecordings_;
    bool                          hasPendingRecordings_ = false;
    std::vector<CardWrite>        pendingWrites_; // made by the next loader before boot
    std::vector<emu::ImportTarget> pendingImports_;

    // FLAC of the recordings last saved or loaded, by name: a save re-encodes
    // only what changed.
    struct EncodedRecording
    {
        size_t            size = 0, hash = 0;
        juce::MemoryBlock flac;
    };
    std::mutex                              encodedMutex_;
    std::map<std::string, EncodedRecording> encoded_;

    // Starting and stopping machines (and the loader thread) are serialised.
    std::recursive_mutex lifecycleMutex_;
    std::thread          loader_;
    std::atomic<bool>    loaderCancelled_{false};

    RateAdapter adapter_;
    OutputStage headphoneStage_, lineStage_; // audio thread

    struct PendingMidi
    {
        double  time; // firmware input index
        uint8_t bytes[3];
        uint8_t size;
    };
    // Time-ordered queue [head, tail); sized in the constructor, never grows.
    std::vector<PendingMidi> pendingMidi_;
    size_t                   pendingHead_  = 0;
    size_t                   pendingTail_  = 0;
    juce::MidiBuffer*        midiOut_      = nullptr;
    int                      midiOutLimit_ = 0;

    // Notes held by MIDI input, by channel as sent to the firmware, so All
    // Notes Off can release them (the firmware ignores it).
    std::array<std::bitset<128>, 16> heldNotes_;

    // Host sync (audio thread): the clock's position in pulses (24 per
    // beat), and whether TEMPO has been sent a start.
    double  clockPhase_       = 0;
    int64_t nextPulse_        = 0;
    bool   tempoStarted_      = false;
    int    syncedGeneration_  = -1;
    bool   syncFollowing_     = false;

    std::atomic<emu::Firmware> firmware_{emu::Firmware::Tape};
    std::atomic<bool>     cardMissing_{false};
    std::atomic<bool>     midiOmni_{true};
    std::atomic<bool>     hostSync_{true};
    std::atomic<bool>     togglePlay_{true};
    std::atomic<int>      generation_{0};
    std::atomic<uint32_t> lastProcessMs_{0};
    uint32_t              lastIdleMs_ = 0;
    std::atomic<int>      editorWidth_{0}, editorHeight_{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChompiProcessor)
};
