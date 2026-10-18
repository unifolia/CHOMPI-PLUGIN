// The processor as a host drives it: each firmware boots and sounds,
// processBlock() never allocates (or frees), MIDI in a block that renders
// nothing still acts, a saved project (TAPE recording and knob values
// included) comes back byte for byte in a new instance at another host rate,
// restored while loading or live, and damaged state or values are survived;
// TAPE's looper buttons arm, disarm and erase. Runs on the card folders in
// CHIMPO_TAPE_CARD / _WAVE_ / _TEMPO_, which must not exist yet (ctest sets
// them in the build folder and clears them first).
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <string_view>
#include <thread>

#include "../plugin/PluginProcessor.h"

// operator new/delete on a thread inside processBlock(); malloc isn't seen.
namespace
{
thread_local bool inProcessBlock = false;
std::atomic<int>  audioAllocations{0};
} // namespace

void* operator new(std::size_t size)
{
    if(inProcessBlock)
        audioAllocations++;
    if(void* p = std::malloc(size != 0 ? size : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void  operator delete(void* p) noexcept
{
    if(inProcessBlock && p != nullptr)
        audioAllocations++;
    std::free(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

namespace
{
int failures = 0;
#define CHECK(cond, ...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if(!(cond))                                                                                                    \
        {                                                                                                              \
            failures++;                                                                                                \
            std::printf("FAIL %s:%d ", __FILE__, __LINE__);                                                            \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while(0)

/** A host transport for TEMPO's sync: 120 bpm, playing from beat 0. */
struct PlayHead : juce::AudioPlayHead
{
    double ppq = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm(120);
        info.setPpqPosition(ppq);
        info.setIsPlaying(true);
        return info;
    }
};

struct Host
{
    std::unique_ptr<ChompiProcessor> plugin = std::make_unique<ChompiProcessor>();
    double                           rate;
    int                              block;
    juce::AudioBuffer<float>         buffer;
    juce::MidiBuffer                 midi;
    PlayHead                         playHead;
    double                           inputHz = 0, phase = 0;
    float                            peak = 0;
    bool                             finite      = true; // over the last run()
    int                              allocations = 0;    // in processBlock() over the last run()

    Host(double sampleRate, int blockSize)
    : rate(sampleRate), block(blockSize), buffer(4, blockSize)
    {
        midi.ensureSize(4096); // a host's buffer, reused
        plugin->setPlayHead(&playHead);
        plugin->setRateAndBufferSizeDetails(rate, block); // as host wrappers do; TEMPO's sync reads it
        plugin->prepareToPlay(rate, block);
    }

    /** Waits for the loader thread to publish a machine after generation
     *  `before`, then runs it through its boot (which only advances by
     *  processing); true if it booted. */
    bool start(int before = 0)
    {
        for(int i = 0; i < 1200 && plugin->machineGeneration() == before; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        run(kBootSeconds);
        return plugin->isRunning();
    }

    /** `seconds` of audio as fast as it computes; `note` (if > 0) on at the start. */
    void run(double seconds, int note = 0)
    {
        peak                  = 0;
        finite                = true;
        const int allocBefore = audioAllocations.load();
        for(int b = 0; b < int(seconds * rate / block); b++)
        {
            buffer.clear();
            for(int i = 0; i < block; i++, phase += 2 * juce::MathConstants<double>::pi * inputHz / rate)
                buffer.setSample(0, i, inputHz > 0 ? float(0.3 * std::sin(phase)) : 0.f);
            buffer.copyFrom(1, 0, buffer, 0, 0, block);
            midi.clear();
            if(b == 0 && note > 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, note, juce::uint8(110)), 0);
            if(b == int(seconds * rate / block) / 2 && note > 0)
                midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
            inProcessBlock = true;
            plugin->processBlock(buffer, midi);
            inProcessBlock = false;
            playHead.ppq += block / rate * 2; // 120 bpm
            for(int ch = 0; ch < 2; ch++)
            {
                peak = std::max(peak, buffer.getMagnitude(ch, 0, block));
                for(int i = 0; i < block; i++)
                    finite = finite && std::isfinite(buffer.getSample(ch, i));
            }
        }
        allocations = audioAllocations.load() - allocBefore;
    }

    juce::AudioParameterFloat* param(const char* id)
    {
        for(auto* p : plugin->getParameters())
            if(auto* f = dynamic_cast<juce::AudioParameterFloat*>(p); f != nullptr && f->getParameterID() == id)
                return f;
        return nullptr;
    }

    juce::MemoryBlock state()
    {
        juce::MemoryBlock data;
        plugin->getStateInformation(data);
        return data;
    }

    static constexpr double kBootSeconds = 7; // TAPE's rainbow; WAVE is silent for its first 5 s
};

/** A project on a firmware's default card, as hostcheck sends it. */
juce::MemoryBlock projectOn(const char* firmware)
{
    juce::XmlElement xml("CHOMPI");
    xml.setAttribute("firmware", firmware);
    xml.setAttribute("defaultCard", true);
    juce::MemoryBlock data;
    juce::AudioProcessor::copyXmlToBinary(xml, data);
    return data;
}

void testTape(juce::MemoryBlock& saved)
{
    Host host(44100, 512);
    CHECK(host.start(), "TAPE never booted");
    CHECK(host.allocations == 0, "TAPE boot: %d allocations in processBlock", host.allocations); // on the audio thread

    host.run(1, 60);
    CHECK(host.finite && host.peak > 0.01f, "TAPE note: peak %.4f, finite %d", host.peak, int(host.finite));
    CHECK(host.allocations == 0, "TAPE note: %d allocations in processBlock", host.allocations);

    // MIDI in a block without audio (or one that can't reach the unit, which
    // takes the same path) acts from the next block: lost, a note-off hangs.
    auto midiOnly = [&host](const juce::MidiMessage& message) {
        juce::AudioBuffer<float> none(4, 0);
        juce::MidiBuffer         events;
        events.addEvent(message, 0);
        host.plugin->processBlock(none, events);
    };
    host.run(1);
    midiOnly(juce::MidiMessage::noteOn(1, 64, juce::uint8(110)));
    host.run(0.5);
    CHECK(host.peak > 0.01f, "note-on in a 0-sample block: peak %.4f", host.peak);
    midiOnly(juce::MidiMessage::noteOff(1, 64));
    host.run(0.5); // the release
    host.run(0.5);
    CHECK(host.peak < 0.001f, "note-off in a 0-sample block: still sounding, peak %.4f", host.peak);

    // A knob from the host, then the line input recorded into the CHIMPO buffer.
    auto* gain = host.param("tape_gain");
    CHECK(gain != nullptr, "no tape_gain parameter");
    if(gain != nullptr)
        gain->setValueNotifyingHost(0.3f);
    host.run(0.2);
    host.plugin->setTogglePlay(false);
    host.run(0.4); // switch debounce
    host.inputHz = 440;
    host.plugin->post({emu::PanelEvent::Type::Button, emu::panel::kChompiKey, 1});
    host.run(1);
    host.plugin->post({emu::PanelEvent::Type::Button, emu::panel::kChompiKey, 0});
    host.inputHz = 0;
    host.run(0.2);
    host.plugin->setTogglePlay(true);
    host.run(0.5);
    CHECK(host.allocations == 0, "TAPE recording: %d allocations in processBlock", host.allocations);

    saved = host.state();
    const auto  xml        = juce::AudioProcessor::getXmlFromBinary(saved.getData(), int(saved.getSize()));
    const auto* recordings = xml != nullptr ? xml->getChildByName("Recordings") : nullptr;
    CHECK(recordings != nullptr && recordings->getChildByAttribute("name", "buffer") != nullptr,
          "TAPE state has no CHIMPO buffer recording");
    std::printf("TAPE state: %zu bytes\n", saved.getSize());
}

void testRecall(const juce::MemoryBlock& saved)
{
    // The first instance is gone, as when a project is reopened. Restored
    // while the machine loads, then again into the running one.
    Host host(96000, 256);
    host.plugin->setStateInformation(saved.getData(), int(saved.getSize()));
    CHECK(host.start(), "TAPE never booted after recall");
    auto again = host.state();
    CHECK(again == saved, "TAPE state recalled at load differs: %zu bytes, was %zu", again.getSize(), saved.getSize());
    host.plugin->setStateInformation(saved.getData(), int(saved.getSize()));
    host.run(0.5);
    CHECK(host.allocations == 0, "TAPE live recall: %d allocations in processBlock", host.allocations);
    again = host.state();
    CHECK(again == saved, "TAPE state recalled live differs: %zu bytes, was %zu", again.getSize(), saved.getSize());

    // Damaged state: cut short at a few points, or noise. Whatever survives
    // of it, the unit keeps running and its output stays finite.
    std::mt19937 rng(1);
    for(const size_t cut : {saved.getSize() - 1, saved.getSize() / 2, size_t(200), size_t(9)})
    {
        host.plugin->setStateInformation(saved.getData(), int(cut));
        host.run(0.5, 60);
        CHECK(host.finite, "state cut to %zu bytes: output not finite", cut);
    }
    juce::MemoryBlock noise(4096);
    for(size_t i = 0; i < noise.getSize(); i++)
        noise[i] = char(rng());
    host.plugin->setStateInformation(noise.getData(), int(noise.getSize()));
    host.plugin->setStateInformation(saved.getData(), 0);
    host.run(0.5, 60);
    CHECK(host.finite && host.plugin->isRunning(), "after noise as state: finite %d, running %d", int(host.finite),
          int(host.plugin->isRunning()));
}

/** TAPE's looper buttons, through the firmware's own gestures. */
void testLooper()
{
    using Looper = ChompiProcessor::Looper;
    Host  host(48000, 480);
    auto& plugin = *host.plugin;
    CHECK(host.start(), "TAPE never booted");
    CHECK(plugin.looperState() == Looper::Empty, "looper at power-on: %d", int(plugin.looperState()));

    plugin.armLooper();
    host.run(0.3);
    CHECK(plugin.looperState() == Looper::Armed, "after arm: %d", int(plugin.looperState()));
    plugin.armLooper(); // disarm: an erase, busy for 2.8 s measured
    host.run(0.5);
    CHECK(plugin.looperState() == Looper::Busy, "disarming: %d", int(plugin.looperState()));
    host.run(3);
    CHECK(plugin.looperState() == Looper::Empty, "after disarm: %d", int(plugin.looperState()));

    plugin.armLooper();
    host.run(0.3);
    host.run(1, 60); // starts the recording
    CHECK(plugin.looperState() == Looper::Recorded, "armed, then a note: %d", int(plugin.looperState()));
    plugin.armLooper(); // would pause it on the unit: not taken
    CHECK(plugin.looperState() == Looper::Recorded, "arm while recording: %d", int(plugin.looperState()));
    plugin.eraseLooper();
    host.run(3.5);
    CHECK(plugin.looperState() == Looper::Empty, "after erase: %d", int(plugin.looperState()));
    CHECK(host.allocations == 0, "looper gestures: %d allocations in processBlock", host.allocations);
}

void testFirmware(const char* name)
{
    Host host(48000, 480);
    CHECK(host.start(), "TAPE never booted");
    const int  generation = host.plugin->machineGeneration();
    const auto project    = projectOn(name);
    host.plugin->setStateInformation(project.getData(), int(project.getSize()));
    CHECK(host.start(generation), "%s never booted", name);
    // TEMPO's boot opens and lists its samples from the SD interrupt (the
    // audio thread), by paths past the small-string size.
    CHECK(host.allocations == 0, "%s boot: %d allocations in processBlock", name, host.allocations);
    host.run(2, 60); // TEMPO: arpeggiates in sync with the play head
    CHECK(host.finite && host.peak > 0.01f, "%s note: peak %.4f, finite %d", name, host.peak, int(host.finite));
    CHECK(host.allocations == 0, "%s note: %d allocations in processBlock", name, host.allocations);

    // Session values no save writes, as a damaged project could hold. Run under
    // UBSan and ASan: no undefined conversion, no table indexed past its end.
    if(std::string_view(name) == "tempo")
    {
        const auto state   = host.state();
        auto       xml     = juce::AudioProcessor::getXmlFromBinary(state.getData(), int(state.getSize()));
        auto*      session = xml != nullptr ? xml->getChildByName("Session") : nullptr;
        CHECK(session != nullptr, "TEMPO state has no session");
        if(session == nullptr)
            return;
        session->setAttribute("pages", "nan,-1,1e9,3,300,0");
        session->setAttribute("s0_ints", "nan,1e30,-5,5,1e9,-1"); // the chromatic engine's state A, on show
        session->setAttribute("s0_sequence", "1e9,0,300,-7,nan,1,-200,0,1,4e9,3,0");
        juce::MemoryBlock damaged;
        juce::AudioProcessor::copyXmlToBinary(*xml, damaged);
        host.plugin->setStateInformation(damaged.getData(), int(damaged.getSize()));
        host.playHead.ppq = 0; // a locate: the pattern, stopped by the restore, starts with the transport
        host.run(1, 60);
        CHECK(host.finite && host.plugin->isRunning(), "TEMPO after damaged values: finite %d, running %d",
              int(host.finite), int(host.plugin->isRunning()));
    }
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // all of it if ctest kills a hang
    const juce::ScopedJuceInitialiser_GUI juce;
    for(const char* variable : {"CHIMPO_TAPE_CARD", "CHIMPO_WAVE_CARD", "CHIMPO_TEMPO_CARD"})
        if(std::getenv(variable) == nullptr)
        {
            std::fprintf(stderr, "%s isn't set\n", variable);
            return 2;
        }

    juce::MemoryBlock saved;
    testTape(saved);
    testRecall(saved);
    testLooper();
    testFirmware("wave");
    testFirmware("tempo");
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
