// Loads a built CHIMPO.vst3 as a host does and plays each firmware in it: the
// check for a packaged plugin on its own OS. Each firmware's factory card must
// be in the bundle and seed its default card folder, and the firmware must
// boot and sound.
//
//   CHIMPO_TAPE_CARD=<dir> CHIMPO_WAVE_CARD=<dir> CHIMPO_TEMPO_CARD=<dir> chompi_hostcheck <CHIMPO.vst3>
//
// The card folders must not exist yet. Move the source tree's cards/ away
// first: otherwise the plugin's fallback to it (factoryCardFolder()) can seed
// them while the bundle's copy goes unused. Exits 1 if any check fails.

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace
{
constexpr double kRate  = 48000;
constexpr int    kBlock = 512;

int countCardFiles(const juce::File& folder)
{
    return folder.findChildFiles(juce::File::findFiles, true, "*.wav;*.json").size();
}

/** Runs `seconds` of audio as fast as it computes; returns the output's peak. */
float run(juce::AudioPluginInstance& plugin, double seconds, const juce::MidiBuffer& first = {})
{
    juce::AudioBuffer<float> buffer(std::max(plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels()),
                                    kBlock);
    float peak = 0;
    for(int b = 0; b < int(seconds * kRate / kBlock); b++)
    {
        buffer.clear();
        juce::MidiBuffer midi;
        if(b == 0)
            midi = first;
        plugin.processBlock(buffer, midi);
        peak = std::max(peak, buffer.getMagnitude(0, kBlock));
    }
    return peak;
}
} // namespace

int main(int argc, char* argv[])
{
    if(argc != 2)
    {
        std::fprintf(stderr, "usage: %s <CHIMPO.vst3>\n", argv[0]);
        return 2;
    }
    const juce::ScopedJuceInitialiser_GUI juce; // the format and the plugin post to the message thread
    const auto bundle = juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]);

    juce::VST3PluginFormatHeadless            format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile(types, bundle.getFullPathName());
    if(types.isEmpty())
    {
        std::fprintf(stderr, "no VST3 plugin in %s\n", bundle.getFullPathName().toRawUTF8());
        return 1;
    }
    juce::String error;
    auto         plugin = format.createInstanceFromDescription(*types[0], kRate, kBlock, error);
    if(plugin == nullptr)
    {
        std::fprintf(stderr, "can't instantiate it: %s\n", error.toRawUTF8());
        return 1;
    }
    plugin->prepareToPlay(kRate, kBlock);

    int failures = 0;
    for(const juce::String name : {"TAPE", "WAVE", "TEMPO"})
    {
        const auto variable = "CHIMPO_" + name + "_CARD";
        const auto path     = juce::SystemStats::getEnvironmentVariable(variable, {});
        if(path.isEmpty())
        {
            std::fprintf(stderr, "%s isn't set\n", variable.toRawUTF8());
            return 2;
        }
        const juce::File card(path);
        const int factory = countCardFiles(bundle.getChildFile("Contents/Resources/Factory Cards").getChildFile(name));

        // A project on this firmware's default card; the plugin switches to it
        // (TAPE's is already running since the plugin was created). JUCE's
        // VST3 host takes the plugin's state base64-wrapped, as it saves it.
        juce::XmlElement state("CHOMPI");
        state.setAttribute("firmware", name.toLowerCase());
        state.setAttribute("defaultCard", true);
        juce::MemoryBlock pluginState;
        juce::AudioProcessor::copyXmlToBinary(state, pluginState);
        juce::XmlElement hostState("VST3PluginState");
        hostState.createNewChildElement("IComponent")->addTextElement(pluginState.toBase64Encoding());
        juce::MemoryBlock data;
        juce::AudioProcessor::copyXmlToBinary(hostState, data);
        plugin->setStateInformation(data.getData(), int(data.getSize()));

        // A loader thread seeds the card, loads it and publishes the machine;
        // until then blocks come out silent without running it.
        for(int i = 0; i < 600 && countCardFiles(card) < std::max(factory, 1); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::this_thread::sleep_for(std::chrono::seconds(2));

        run(*plugin, 7.0); // boot (TAPE's rainbow, WAVE's silent first 5 s)
        juce::MidiBuffer note;
        note.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)), 0);
        const float peak = run(*plugin, 1.0, note);

        const int  seeded = countCardFiles(card);
        const bool ok     = factory > 0 && seeded == factory && peak > 0;
        std::printf("%-5s bundle %3d files, card %3d, peak %.4f: %s\n", name.toRawUTF8(), factory, seeded, peak,
                    ok ? "ok" : "FAILED");
        failures += ok ? 0 : 1;
    }

    plugin->releaseResources();
    plugin.reset();
    return failures == 0 ? 0 : 1;
}
