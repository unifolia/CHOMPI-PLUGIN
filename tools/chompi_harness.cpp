// Drives the whole plugin without a host: renders the editor offscreen to PNG
// while scripting the panel, and runs processBlock() at arbitrary host rates.
// Between renders the machine runs on wall-clock time through the editor's
// idle driver, as when a host isn't processing.
//
//   CHIMPO_TAPE_CARD=<card copy> chompi_harness "<script>"
//
// Commands (';'-separated): wait <ms> | press <id> | release <id> |
// turn <encoder> <detents> | toggle <1|0> | shot <file.png> [scale] |
// render <file.wav> <host rate> <block> <seconds> <midi note> [off|cc123|none]
//   (note on at 0.1 s unless the note is -1, released at 60% of the length by
//   note-off, All Notes Off, or not at all; line out only) |
// input <hz> <gain>   sine on the line input during renders (0 0: off) |
// codectest <file.wav> <seconds>   recording FLAC round trip on that audio, tiled |
// import <button> <file>...   as if dropped on that key (Panel.h ID) |
// drag <x> <y> <file>... | drop <x> <y> <file>... | dragexit   the editor's file drag, at canvas coordinates |
// save <file> | load <file>   plugin state | reopen   new plugin instance |
// reload   power-cycle on the project's card folder (menu: Reload SD Card) |
// firmware <tape|wave|tempo>   switch firmware (on its default card) |
// host <bpm> <playing 1|0>   the host's playhead during renders (playing
//   starts at beat 0); without it there is none | hostsync <1|0>
// Renders print the spacing of the plugin's MIDI note-ons (TEMPO's steps).
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <sstream>

#include "../plugin/PluginEditor.h"
#include "../plugin/PluginProcessor.h"
#include "../plugin/RecordingCodec.h"

namespace
{
/** A host transport: tempo, and a beat position that runs while playing. */
struct HostPlayHead : juce::AudioPlayHead
{
    double bpm = 120, ppq = 0;
    bool   playing = false;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm(bpm);
        info.setPpqPosition(ppq);
        info.setIsPlaying(playing);
        return info;
    }
};
} // namespace

int main(int argc, char** argv)
{
    if(argc < 2)
    {
        std::fprintf(stderr, "usage: CHIMPO_TAPE_CARD=<folder> %s \"<script>\"\n", argv[0]);
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI juce;
    auto                            processor = std::make_unique<ChompiProcessor>();
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditor());

    auto pump = [](int ms) {
        const auto end = juce::Time::getMillisecondCounter() + uint32_t(ms);
        while(juce::Time::getMillisecondCounter() < end)
        {
            juce::Timer::callPendingTimersSynchronously();
            juce::Thread::sleep(5);
        }
    };

    HostPlayHead host;
    bool         hasHost = false;
    double       inputHz = 0, inputGain = 0, inputPhase = 0;

    std::stringstream script(argv[1]);
    std::string       command;
    while(std::getline(script, command, ';'))
    {
        std::istringstream in(command);
        std::string        op;
        if(!(in >> op))
            continue;

        if(op == "wait")
        {
            int ms;
            in >> ms;
            pump(ms);
        }
        else if(op == "press" || op == "release")
        {
            int id;
            in >> id;
            processor->post({emu::PanelEvent::Type::Button, int16_t(id), int16_t(op == "press")});
        }
        else if(op == "turn")
        {
            int enc, detents;
            in >> enc >> detents;
            processor->post({emu::PanelEvent::Type::Turn, int16_t(enc), int16_t(detents)});
        }
        else if(op == "toggle")
        {
            int play;
            in >> play;
            processor->setTogglePlay(play != 0);
        }
        else if(op == "shot")
        {
            std::string file;
            float       scale = 1.f;
            in >> file >> scale;
            pump(50);
            const auto           image = editor->createComponentSnapshot(editor->getLocalBounds(), true, scale);
            juce::PNGImageFormat png;
            juce::File(file).deleteFile();
            juce::FileOutputStream out{juce::File(file)};
            png.writeImageToStream(image, out);
            std::printf("wrote %s (%dx%d)\n", file.c_str(), image.getWidth(), image.getHeight());
        }
        else if(op == "save")
        {
            std::string file;
            in >> file;
            juce::MemoryBlock state;
            const auto        t0 = juce::Time::getMillisecondCounterHiRes();
            processor->getStateInformation(state);
            std::printf("saved %zu bytes in %.1f ms\n", state.getSize(), juce::Time::getMillisecondCounterHiRes() - t0);
            juce::File(file).replaceWithData(state.getData(), state.getSize());
        }
        else if(op == "load")
        {
            std::string       file;
            juce::MemoryBlock state;
            in >> file;
            juce::File(file).loadFileAsData(state);
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            processor->setStateInformation(state.getData(), int(state.getSize()));
            std::printf("loaded in %.1f ms\n", juce::Time::getMillisecondCounterHiRes() - t0);
        }
        else if(op == "reload")
            processor->insertCard({});
        else if(op == "firmware")
        {
            std::string name;
            in >> name;
            processor->setFirmware(name == "wave" ? emu::Firmware::Wave : name == "tempo" ? emu::Firmware::Tempo : emu::Firmware::Tape);
        }
        else if(op == "host")
        {
            int playing;
            in >> host.bpm >> playing;
            host.playing = playing != 0;
            host.ppq     = 0;
            hasHost      = true;
        }
        else if(op == "input")
            in >> inputHz >> inputGain;
        else if(op == "hostsync")
        {
            int on;
            in >> on;
            processor->setHostSync(on != 0);
        }
        else if(op == "import")
        {
            int               button;
            juce::StringArray files;
            std::string       file;
            in >> button;
            while(in >> file)
                files.add(file);
            const auto t0      = juce::Time::getMillisecondCounterHiRes();
            const auto message = processor->importAudio(files, button);
            std::printf("import: %s (%.0f ms)\n", message.toRawUTF8(), juce::Time::getMillisecondCounterHiRes() - t0);
        }
        else if(op == "drag" || op == "drop")
        {
            int               x, y;
            juce::StringArray files;
            std::string       file;
            in >> x >> y;
            while(in >> file)
                files.add(file);
            auto& target = dynamic_cast<juce::FileDragAndDropTarget&>(*editor);
            if(op == "drag")
                std::printf("interested: %d\n", int(target.isInterestedInFileDrag(files)));
            if(op == "drag")
                target.fileDragMove(files, x, y);
            else
                target.filesDropped(files, x, y);
        }
        else if(op == "dragexit")
            dynamic_cast<juce::FileDragAndDropTarget&>(*editor).fileDragExit({});
        else if(op == "codectest")
        {
            std::string file;
            double      seconds;
            in >> file >> seconds;
            juce::WavAudioFormat                     wav;
            std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(new juce::FileInputStream(juce::File(file)), true));
            juce::AudioBuffer<float>                 source(2, int(reader->lengthInSamples));
            reader->read(&source, 0, source.getNumSamples(), 0, true, true);
            std::vector<int16_t> samples(size_t(seconds * 48000) * 2);
            for(size_t i = 0; i < samples.size() / 2; i++)
                for(int ch = 0; ch < 2; ch++)
                    samples[2 * i + size_t(ch)] = int16_t(std::lround(32767.f * source.getSample(ch, int(i % size_t(source.getNumSamples())))));
            const auto           t0   = juce::Time::getMillisecondCounterHiRes();
            const auto           flac = recording_codec::encode(samples);
            const auto           t1   = juce::Time::getMillisecondCounterHiRes();
            std::vector<int16_t> back;
            const bool           ok = recording_codec::decode(flac.getData(), flac.getSize(), back);
            const auto           t2 = juce::Time::getMillisecondCounterHiRes();
            std::printf("flac: %.1f%% of raw, encode %.0f ms, decode %.0f ms, lossless %s\n",
                        100.0 * double(flac.getSize()) / double(samples.size() * 2), t1 - t0, t2 - t1,
                        ok && back == samples ? "yes" : "NO");
        }
        else if(op == "reopen")
        {
            editor.reset();
            processor = std::make_unique<ChompiProcessor>();
            editor.reset(processor->createEditor());
        }
        else if(op == "render")
        {
            std::string file;
            double      rate, seconds;
            int         block, note;
            std::string release = "off";
            in >> file >> rate >> block >> seconds >> note >> release;

            processor->setPlayConfigDetails(2, 2, rate, block);
            processor->prepareToPlay(rate, block);
            processor->setPlayHead(hasHost ? &host : nullptr);
            std::vector<double> noteOns;
            const int                total = int(rate * seconds);
            juce::AudioBuffer<float> out(2, total);
            juce::AudioBuffer<float> io(2, block);
            juce::MidiBuffer         midi;
            const int                on = int(rate * 0.1), off = int(total * 0.6);
            for(int pos = 0; pos < total; pos += block)
            {
                const int n = std::min(block, total - pos);
                io.setSize(2, n, false, false, true);
                io.clear();
                for(int i = 0; i < n && inputHz > 0; i++, inputPhase += 2 * M_PI * inputHz / rate)
                    for(int ch = 0; ch < 2; ch++)
                        io.setSample(ch, i, float(inputGain * std::sin(inputPhase)));
                midi.clear();
                if(note >= 0 && on >= pos && on < pos + n)
                    midi.addEvent(juce::MidiMessage::noteOn(1, note, uint8_t(127)), on - pos);
                if(note >= 0 && off >= pos && off < pos + n && release != "none")
                    midi.addEvent(release == "cc123" ? juce::MidiMessage::allNotesOff(1) : juce::MidiMessage::noteOff(1, note), off - pos);
                processor->processBlock(io, midi);
                for(int ch = 0; ch < 2; ch++)
                    out.copyFrom(ch, pos, io, ch, 0, n);
                for(const auto meta : midi)
                    if(meta.getMessage().isNoteOn())
                        noteOns.push_back((pos + meta.samplePosition) / rate);
                if(host.playing)
                    host.ppq += n * host.bpm / 60.0 / rate;
            }
            processor->setPlayHead(nullptr);
            std::printf("midi note-ons: %zu, spacing ms:", noteOns.size());
            for(size_t i = 1; i < noteOns.size() && i < 13; i++)
                std::printf(" %.1f", (noteOns[i] - noteOns[i - 1]) * 1000.0);
            if(!noteOns.empty())
                std::printf(" (first at %.4f s)", noteOns[0]);
            std::printf("\n");

            juce::File(file).deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream(new juce::FileOutputStream(juce::File(file)));
            auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(2).withBitsPerSample(24));
            if(writer == nullptr)
            {
                std::fprintf(stderr, "cannot write %s\n", file.c_str());
                return 1;
            }
            writer->writeFromAudioSampleBuffer(out, 0, total);
            std::printf("rendered %s at %.0f Hz, block %d, latency %d\n", file.c_str(), rate, block, processor->getLatencySamples());
        }
        else
            std::fprintf(stderr, "unknown command: %s\n", op.c_str());
    }

    editor.reset();
    processor.reset();
    return 0;
}
