#include "PluginProcessor.h"
#include "AudioImport.h"
#include "PluginEditor.h"
#include "RecordingCodec.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

namespace
{
constexpr int      kFirmwareBlock  = emu::Machine::kBlockSize;
constexpr size_t   kMaxPendingMidi = 4096;
constexpr uint32_t kIdleAfterMs    = 150; // host considered stopped after this long without a block
constexpr uint32_t kMaxIdleStepMs  = 100;

constexpr const char* kCardFiles = "*.wav;*.json";

// The unit's moves reach the host as gestures that end this long after the
// last one, so Touch automation writes for the length of a turn.
constexpr uint32_t kGestureMs = 300;

// TAPE's LooperEngine arms when both keys are down on an empty looper, and
// CheckReset() erases once they've been down kRecordClearTimeout (2000 ms).
constexpr int16_t kArmHoldMs   = 100;
constexpr int16_t kEraseHoldMs = 2200;

// Set while the processor reports a move to the host: not a host change.
thread_local bool reportingMove = false;

// A second of stereo: room for a recording to grow between sizing and copying.
constexpr size_t kRecordingSlack = 2 * emu::Machine::kSampleRate;

size_t hashOf(const std::vector<int16_t>& samples)
{
    return std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char*>(samples.data()), samples.size() * sizeof(int16_t)));
}

bool hasCardFiles(const juce::File& folder)
{
    return folder.isDirectory() && !folder.findChildFiles(juce::File::findFiles, false, kCardFiles).isEmpty();
}

/** Copies the factory card over `folder`, subfolders included (TEMPO). */
void copyFactoryCard(emu::Firmware firmware, const juce::File& folder)
{
    const auto factory = ChompiProcessor::factoryCardFolder(firmware);
    folder.createDirectory();
    for(const auto& file : factory.findChildFiles(juce::File::findFiles, true, kCardFiles))
    {
        const auto target = folder.getChildFile(file.getRelativePathFrom(factory));
        target.getParentDirectory().createDirectory();
        file.copyFileTo(target);
    }
}

const char* folderName(emu::Firmware firmware)
{
    switch(firmware)
    {
        case emu::Firmware::Tape: return "TAPE";
        case emu::Firmware::Wave: return "WAVE";
        case emu::Firmware::Tempo: return "TEMPO";
    }
    return "TAPE";
}

juce::Colour unscale(uint32_t rgb, int fullScale)
{
    auto channel = [fullScale](uint32_t v) { return uint8_t(juce::jmin(255u, v * 255u / uint32_t(fullScale))); };
    return juce::Colour(channel((rgb >> 16) & 0xff), channel((rgb >> 8) & 0xff), channel(rgb & 0xff));
}

emu::PanelEvent toggleEvent(bool play)
{
    return {emu::PanelEvent::Type::Toggle, 0, int16_t(play)};
}

/** Runs a machine nobody else holds through its boot animation, as fast as it
 *  computes (measured on an M2: 6 s of boot in 50-90 ms), so a restart for an
 *  import is over at once. Pending restores apply on the way. */
void bootThrough(emu::Machine& machine, const std::atomic<bool>& cancelled)
{
    constexpr int kMaxBlocks = 20 * emu::Machine::kSampleRate / emu::Machine::kBlockSize; // a card that never finishes

    juce::ScopedNoDenormals noDenormals;
    float        silence[emu::Machine::kNumInputs][emu::Machine::kBlockSize]  = {};
    float        scratch[emu::Machine::kNumOutputs][emu::Machine::kBlockSize] = {};
    const float* in[emu::Machine::kNumInputs]   = {silence[0], silence[1], silence[2], silence[3]};
    float*       out[emu::Machine::kNumOutputs] = {scratch[0], scratch[1], scratch[2], scratch[3]};
    uint8_t      msg[3];
    for(int block = 0; block < kMaxBlocks && !cancelled && !machine.IsReady(); block++)
    {
        machine.Process(in, out);
        while(machine.ReceiveMidi(msg))
        {
        }
    }
}

juce::AudioParameterFloatAttributes controlAttributes(emu::ControlInfo::Unit unit)
{
    using Unit = emu::ControlInfo::Unit;
    // TAPE's speed knob (DSPEngine::SetGlobalPitchFree): 0.01x-0.5x-1x-2x over
    // each third of a half turn, reverse below the middle.
    auto speedOf = [](float v) {
        const float s = v < .5f ? (.5f - v) * -2.f : (v - .5f) * 2.f, a = std::abs(s), sign = s < 0 ? -1.f : 1.f;
        const float speed = a < .33f ? a * 1.484848f + .01f : a < .66f ? (a - .33f) * 1.515151f + .5f : (a - .66f) * 2.941176f + 1.f;
        return sign * speed;
    };
    auto knobOf = [](float speed) {
        const float p = std::abs(speed);
        const float a = p < .5f ? (p - .01f) / 1.484848f : p < 1.f ? (p - .5f) / 1.515151f + .33f : (p - 1.f) / 2.941176f + .66f;
        return juce::jlimit(0.f, 1.f, .5f + (speed < 0 ? -a : a) * .5f);
    };
    auto text = [unit, speedOf](float v, int) -> juce::String {
        switch(unit)
        {
            case Unit::Pan:
                return v < .495f ? "L" + juce::String(juce::roundToInt((.5f - v) * 200.f))
                     : v > .505f ? "R" + juce::String(juce::roundToInt((v - .5f) * 200.f))
                                 : juce::String("C");
            case Unit::SampleSpeed: return juce::String(speedOf(v), 2) + "x";
            case Unit::LooperSpeed: return juce::String(v * 4.f - 2.f, 2) + "x";
            case Unit::Semitones: return juce::String((v - .5f) * 24.f, 1) + " st";
            case Unit::Frame: return juce::String(juce::roundToInt(v * 32.f) + 1);
            case Unit::DelayReverb:
                return v < .495f ? "Delay " + juce::String(juce::roundToInt((.5f - v) * 200.f)) + "%"
                     : v > .505f ? "Reverb " + juce::String(juce::roundToInt((v - .5f) * 200.f)) + "%"
                                 : juce::String("Dry");
            case Unit::Tempo: return juce::String(juce::roundToInt(160.f + v * 320.f));
            case Unit::Percent: break;
        }
        return juce::String(juce::roundToInt(v * 100.f)) + "%";
    };
    auto value = [unit, knobOf](const juce::String& t) {
        const auto  s = t.trim();
        const float n = s.retainCharacters("-.0123456789").getFloatValue();
        switch(unit)
        {
            case Unit::Pan:
                return juce::jlimit(0.f, 1.f, .5f + (s.startsWithIgnoreCase("L") ? -n : s.startsWithIgnoreCase("R") ? n : 0.f) / 200.f);
            case Unit::SampleSpeed: return knobOf(n);
            case Unit::LooperSpeed: return juce::jlimit(0.f, 1.f, (n + 2.f) / 4.f);
            case Unit::Semitones: return juce::jlimit(0.f, 1.f, n / 24.f + .5f);
            case Unit::Frame: return juce::jlimit(0.f, 1.f, (n - 1.f) / 32.f);
            case Unit::DelayReverb:
                return juce::jlimit(0.f, 1.f, .5f + (s.startsWithIgnoreCase("D") ? -n : s.startsWithIgnoreCase("R") ? n : 0.f) / 200.f);
            case Unit::Tempo: return juce::jlimit(0.f, 1.f, (n - 160.f) / 320.f);
            case Unit::Percent: break;
        }
        return juce::jlimit(0.f, 1.f, n / 100.f);
    };
    return juce::AudioParameterFloatAttributes().withStringFromValueFunction(text).withValueFromStringFunction(value);
}

/** Puts a full-velocity four-note chord at the default volume near -4 dBFS
 *  peak on each factory card; raw it peaks at -22.2 (TAPE), -28.0 (WAVE) and
 *  -16.4 dBFS (TEMPO), measured with chompi_render. */
float outputMakeup(emu::Firmware firmware)
{
    static const float tape = juce::Decibels::decibelsToGain(18.f), wave = juce::Decibels::decibelsToGain(24.f),
                       tempo = juce::Decibels::decibelsToGain(12.f);
    switch(firmware)
    {
        case emu::Firmware::Tape: return tape;
        case emu::Firmware::Wave: return wave;
        case emu::Firmware::Tempo: return tempo;
    }
    return 1.f;
}
} // namespace

ChompiProcessor::ChompiProcessor()
: AudioProcessor(BusesProperties()
                     .withInput("Line In", juce::AudioChannelSet::stereo(), true)
                     .withOutput("Line Out", juce::AudioChannelSet::stereo(), true)
                     .withOutput("Headphones", juce::AudioChannelSet::stereo(), false))
{
    for(const auto firmware : {emu::Firmware::Tape, emu::Firmware::Wave, emu::Firmware::Tempo})
    {
        const auto& infos = emu::Machine::Controls(firmware);
        auto        group = std::make_unique<juce::AudioProcessorParameterGroup>(
            juce::String(folderName(firmware)).toLowerCase(), folderName(firmware), " | ");
        for(size_t i = 0; i < infos.size(); i++)
        {
            const auto& info  = infos[i];
            auto        param = std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{info.id, 1}, info.name, juce::NormalisableRange<float>(0.f, 1.f), info.defaultValue,
                controlAttributes(info.unit));
            auto control       = std::make_unique<HostControl>();
            control->param     = param.get();
            control->firmware  = firmware;
            control->index     = int(i);
            control->hostValue = info.defaultValue;
            controls_.push_back(std::move(control));
            group->addChild(std::move(param));
        }
        addParameterGroup(std::move(group));
    }
    jassert(getParameters().size() == int(controls_.size()));
    for(auto* param : getParameters())
        param->addListener(this);
    startTimerHz(30);

    pendingMidi_.resize(kMaxPendingMidi);
    adapter_.prepare(emu::Machine::kSampleRate, 512, OutputStage::kLookahead);
    startMachine(defaultCardFolder(emu::Firmware::Tape), false);
}

ChompiProcessor::~ChompiProcessor()
{
    stopTimer();
    for(auto* param : getParameters())
        param->removeListener(this);
    stopMachine();
}

void ChompiProcessor::parameterValueChanged(int index, float value)
{
    if(reportingMove || index < 0 || size_t(index) >= controls_.size())
        return;
    auto& control = *controls_[size_t(index)];
    control.hostValue.store(control.param->convertFrom0to1(value), std::memory_order_relaxed);
    control.hostChanged.store(true, std::memory_order_release);
    control.hostSerial.fetch_add(1, std::memory_order_acq_rel);
}

void ChompiProcessor::sendControls(emu::Machine& machine)
{
    const auto firmware = machine.GetFirmware();
    for(auto& control : controls_)
        if(control->firmware == firmware && control->hostChanged.exchange(false, std::memory_order_acquire))
            machine.SetControl(control->index, control->hostValue.load(std::memory_order_relaxed));
}

void ChompiProcessor::readControls(emu::Machine& machine)
{
    const auto firmware = machine.GetFirmware();
    for(auto& control : controls_)
    {
        // A host value still on its way supersedes the unit's: serial first,
        // so one arriving after this check is caught by timerCallback().
        const uint32_t serial = control->hostSerial.load(std::memory_order_acquire);
        float          value;
        if(control->firmware == firmware && !control->hostChanged.load(std::memory_order_acquire)
           && machine.ReadControl(control->index, value))
        {
            control->unitValue.store(value, std::memory_order_relaxed);
            control->unitSerial.store(serial, std::memory_order_relaxed);
            control->unitChanged.store(true, std::memory_order_release);
        }
    }
}

void ChompiProcessor::timerCallback()
{
    if(const auto machine = currentMachine())
        machine->FreeSpent();

    const uint32_t now = juce::Time::getMillisecondCounter();
    for(auto& control : controls_)
    {
        auto& param = *control->param;
        if(control->unitChanged.exchange(false, std::memory_order_acquire))
        {
            const float value = control->unitValue.load(std::memory_order_relaxed);
            if(control->unitSerial.load(std::memory_order_relaxed) != control->hostSerial.load(std::memory_order_acquire)
               || juce::exactlyEqual(value, param.get()))
                continue;
            if(!control->gesture)
                param.beginChangeGesture();
            control->gesture    = true;
            control->lastMoveMs = now;
            const juce::ScopedValueSetter<bool> reporting(reportingMove, true);
            param.setValueNotifyingHost(param.convertTo0to1(value));
        }
        else if(control->gesture && now - control->lastMoveMs > kGestureMs)
        {
            param.endChangeGesture();
            control->gesture = false;
        }
    }
}

juce::String ChompiProcessor::firmwareName(emu::Firmware firmware)
{
    return juce::String(folderName(firmware)) + (firmware == emu::Firmware::Tape ? " 2.0" : " 1.0");
}

juce::File ChompiProcessor::factoryCardFolder(emu::Firmware firmware)
{
    // The build copies them into the plugin bundle, whose binary is
    // Contents/<MacOS, x86_64-win, x86_64-linux>/ on every OS. The source
    // tree's cards/ serves what runs outside a bundle (the harness).
    using juce::File;
    const auto bundled = File::getSpecialLocation(File::currentExecutableFile)
                             .getParentDirectory()
                             .getSiblingFile("Resources")
                             .getChildFile("Factory Cards")
                             .getChildFile(folderName(firmware));
    if(hasCardFiles(bundled))
        return bundled;
    return File(CHIMPO_CARDS).getChildFile(juce::String(folderName(firmware)).toLowerCase());
}

juce::File ChompiProcessor::defaultCardFolder(emu::Firmware firmware)
{
    const auto variable = "CHIMPO_" + juce::String(folderName(firmware)) + "_CARD";
    if(const auto* override = std::getenv(variable.toRawUTF8()))
        return juce::File(override);
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory)
        .getChildFile("CHIMPO")
        .getChildFile(folderName(firmware) + juce::String(" SD Card"));
}

juce::File ChompiProcessor::getCardFolder() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return cardFolder_;
}

std::shared_ptr<emu::Machine> ChompiProcessor::currentMachine() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return live_;
}

void ChompiProcessor::stopMachine()
{
    const std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    loaderCancelled_ = true;
    if(loader_.joinable())
        loader_.join();

    std::shared_ptr<emu::Machine> old;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        old = std::move(live_);
        const juce::SpinLock::ScopedLockType audio(machineLock_);
        machine_.reset();
    }
    // Destroyed here (or by a GUI thread still holding it); destroying the last
    // machine on a card writes the card's pending changes.
}

void ChompiProcessor::clearPending()
{
    hasPendingSession_ = hasPendingRecordings_ = false;
    pendingRecordings_.clear();
    pendingWrites_.clear();
    pendingImports_.clear();
}

void ChompiProcessor::applyPending(emu::Machine& machine)
{
    machine.Post(toggleEvent(togglePlay_.load()));
    if(hasPendingSession_)
    {
        machine.RestoreSession(pendingSession_);
        hasPendingSession_ = false;
    }
    if(hasPendingRecordings_)
    {
        machine.RestoreRecordings(std::exchange(pendingRecordings_, {}));
        hasPendingRecordings_ = false;
    }
    if(!pendingImports_.empty())
        machine.CompleteImports(std::exchange(pendingImports_, {}));
}

void ChompiProcessor::startMachine(const juce::File& folder, bool restoreFactory, bool fastBoot)
{
    const std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    stopMachine();
    const auto firmware = firmware_.load();
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        cardFolder_   = folder;
        cardFirmware_ = firmware;
    }
    loaderCancelled_ = false;

    // Copying and loading the card and booting take a moment; the audio thread
    // outputs silence until the machine is published.
    loader_ = std::thread([this, requested = folder, restoreFactory, fastBoot, firmware] {
        // A folder that's gone (project from another Mac, renamed, drive not
        // mounted) would otherwise be recreated as a blank card: run on the
        // default card instead, while the project keeps asking for it.
        // Checked here because a dead network volume can stall the check.
        const auto defaultFolder = defaultCardFolder(firmware);
        const bool missing       = requested != defaultFolder && !requested.isDirectory();
        const auto active        = missing ? defaultFolder : requested;

        // Only the default folder is seeded, like a unit's factory card; a
        // folder the user picks is used as found (empty = blank card).
        if(restoreFactory || (active == defaultFolder && !hasCardFiles(active)))
            copyFactoryCard(firmware, active);
        auto card = emu::VirtualCard::Open(std::filesystem::u8path(active.getFullPathName().toStdString()));
        {
            std::vector<CardWrite> writes;
            {
                const std::lock_guard<std::mutex> lock(stateMutex_);
                writes = std::exchange(pendingWrites_, {});
            }
            for(const auto& write : writes)
                writeCardFile(*card, write);
        }
        if(loaderCancelled_)
            return;
        std::shared_ptr<emu::Machine> machine = emu::Machine::Create(firmware, std::move(card));
        if(machine == nullptr) // not built in: the unit stays off
            return;

        if(fastBoot)
        {
            {
                const std::lock_guard<std::mutex> lock(stateMutex_);
                applyPending(*machine);
            }
            bootThrough(*machine, loaderCancelled_);
        }

        // Under stateMutex_, so the toggle and a restored session can't slip
        // between this machine's creation and its publication.
        const std::lock_guard<std::mutex> lock(stateMutex_);
        if(loaderCancelled_)
            return;
        applyPending(*machine);
        {
            const juce::SpinLock::ScopedLockType audio(machineLock_);
            machine_ = machine;
        }
        live_ = std::move(machine);
        cardMissing_ = missing;
        generation_++;
    });
}

void ChompiProcessor::writeCardFile(emu::VirtualCard& card, const CardWrite& write)
{
    if(const auto slash = write.path.rfind('/'); slash != std::string::npos)
        card.MakeDir(write.path.substr(0, slash).c_str());
    for(const auto& name : write.obsolete)
        card.Unlink(name.c_str());
    uint32_t  size   = 0;
    const int handle = card.OpenFile(write.path.c_str(), true, true, &size);
    if(handle != 0)
        card.Write(handle, 0, write.bytes.data(), uint32_t(write.bytes.size()));
}

std::vector<ChompiProcessor::ImportPlan> ChompiProcessor::planImport(int button, int numFiles)
{
    std::vector<ImportPlan>           plan;
    const std::lock_guard<std::mutex> lock(stateMutex_);
    if(live_ == nullptr || numFiles < 1)
        return plan;
    const juce::SpinLock::ScopedLockType audio(machineLock_);
    emu::ImportTarget                    target;
    if(!live_->PlanImport(button, target))
        return plan;
    plan.push_back({button, target});

    const auto* keys  = std::begin(emu::panel::kWhiteKeys);
    const auto* end   = std::end(emu::panel::kWhiteKeys);
    auto        isSlot = [](const emu::ImportTarget& t) { return !t.file.empty() && t.slot >= 1 && t.slot <= 14; };
    if(!isSlot(target))
        return plan;
    for(const auto* key = std::find(keys, end, button) + 1; key < end && int(plan.size()) < numFiles; key++)
    {
        if(!live_->PlanImport(*key, target) || !isSlot(target))
            break;
        plan.push_back({*key, target});
    }
    return plan;
}

juce::String ChompiProcessor::importAudio(const juce::StringArray& files, int button)
{
    juce::StringArray sorted(files);
    sorted.sortNatural();
    const auto plan = planImport(button, sorted.size());
    if(plan.empty())
        return "The unit can't take audio there now.";

    // Converted on the calling (message) thread: a 165 s 24-bit 44.1 kHz file
    // takes ~240 ms on an M2 (2026-10-17), short samples a few ms.
    std::vector<CardWrite>         writes;
    std::vector<emu::ImportTarget> imported;
    emu::Recordings                recordings;
    juce::StringArray              unreadable;
    bool                           cut = false;
    for(size_t i = 0; i < plan.size(); i++)
    {
        const juce::File     file(sorted[int(i)]);
        const auto&          target = plan[i].target;
        std::vector<int16_t> samples;
        bool                 longer = false;
        if(!audio_import::read(file, target.maxSeconds, samples, longer))
        {
            unreadable.add(file.getFileName());
            continue;
        }
        cut = cut || longer;
        if(!target.recording.empty())
            recordings[target.recording] = std::move(samples);
        else
            writes.push_back({target.file, audio_import::toWav(samples), target.obsolete});
        imported.push_back(target);
    }

    juce::String message;
    if(!imported.empty())
    {
        const std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
        {
            const std::lock_guard<std::mutex> lock(stateMutex_);
            if(live_ != nullptr)
            {
                pendingRecordings_ = {};
                hasPendingSession_ = captureLive(pendingSession_, pendingRecordings_);
            }
            for(auto& [name, samples] : recordings)
                pendingRecordings_[name] = std::move(samples);
            hasPendingRecordings_ = true;
            pendingWrites_        = std::move(writes);
            pendingImports_       = imported;
        }
        startMachine(getCardFolder(), false, true);

        const juce::String first(imported.front().label), last(imported.back().label);
        message = (imported.size() == 1 ? "Loaded " + juce::File(sorted[0]).getFileName()
                                        : "Loaded " + juce::String(int(imported.size())) + " files")
                  + " into " + first + (imported.size() > 1 ? " to " + last : juce::String());
        if(cut)
            message << " (cut to " << juce::String(plan.front().target.maxSeconds, 0) << " s)";
    }
    if(const int left = sorted.size() - int(plan.size()); left > 0)
        message << (message.isEmpty() ? "" : "; ") << left << " more didn't fit";
    if(!unreadable.isEmpty())
        message << (message.isEmpty() ? "" : "; ") << "can't read " << unreadable.joinIntoString(", ");
    return message;
}

void ChompiProcessor::setFirmware(emu::Firmware firmware)
{
    const std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    if(firmware == firmware_.load() || !emu::Machine::IsBuiltIn(firmware))
        return;
    firmware_.store(firmware);
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        clearPending(); // the other firmware's
    }
    startMachine(defaultCardFolder(firmware), false);
}

void ChompiProcessor::insertCard(const juce::File& folder)
{
    startMachine(folder == juce::File() ? getCardFolder() : folder, false);
}

void ChompiProcessor::restoreFactoryCard()
{
    // The old machine is destroyed (flushing its card) before the copy. Another
    // instance sharing this card keeps its in-memory copy and may later write
    // its own presets.json back.
    startMachine(getCardFolder(), true);
}

bool ChompiProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if(layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    const auto in = layouts.getMainInputChannelSet();
    if(!in.isDisabled() && in != juce::AudioChannelSet::stereo() && in != juce::AudioChannelSet::mono())
        return false;

    if(layouts.outputBuses.size() > 1)
    {
        const auto hp = layouts.getChannelSet(false, 1);
        if(!hp.isDisabled() && hp != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void ChompiProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    adapter_.prepare(sampleRate, samplesPerBlock, OutputStage::kLookahead);
    headphoneStage_.reset();
    lineStage_.reset();
    pendingHead_ = pendingTail_ = 0;
    for(auto& held : heldNotes_)
        held.reset();
    // tempoStarted_ stays: TEMPO is still playing if the host started it, and
    // must hear the stop.
    clockPhase_ = 0;
    nextPulse_  = 0;
    setLatencySamples(adapter_.latencySamples());
}

void ChompiProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int               numSamples = buffer.getNumSamples();
    lastProcessMs_.store(juce::Time::getMillisecondCounter(), std::memory_order_relaxed);

    const juce::SpinLock::ScopedTryLockType lock(machineLock_);
    if(lock.isLocked() && machine_ == nullptr) // no unit to play to
    {
        pendingHead_ = pendingTail_ = 0;
        for(auto& held : heldNotes_)
            held.reset();
        buffer.clear();
        midi.clear();
        return;
    }
    // Without the unit this block (a save or restore holds it) or with no
    // audio to render, the MIDI waits for the next block, stamped at its
    // start: dropped, a note-off would leave its note hanging.
    const bool render = lock.isLocked() && numSamples > 0;
    if(render)
        midiInChannel_ = uint8_t(machine_->MidiInChannel());

    // MIDI in, stamped on the firmware timeline. Host buffers are time-ordered
    // and stamps grow across calls, so the queue stays sorted.
    auto enqueue = [this](double time, uint8_t status, uint8_t data1, uint8_t data2, uint8_t size) {
        if(pendingTail_ == pendingMidi_.size())
        {
            if(pendingHead_ == 0)
                return; // full: drop
            std::move(pendingMidi_.begin() + long(pendingHead_), pendingMidi_.begin() + long(pendingTail_), pendingMidi_.begin());
            pendingTail_ -= pendingHead_;
            pendingHead_ = 0;
        }
        pendingMidi_[pendingTail_++] = {time, {status, data1, data2}, size};
    };

    // Host sync: clock and transport for TEMPO, merged into the host's MIDI
    // by sample position below.
    SyncEvent  sync[kMaxSyncEvents];
    const int  numSync   = render ? followHost(numSamples, sync) : 0;
    int        nextSync  = 0;
    auto       flushSync = [&](int upTo) {
        for(; nextSync < numSync && sync[nextSync].offset <= upTo; nextSync++)
            enqueue(adapter_.firmwareTimeAt(sync[nextSync].offset), sync[nextSync].status, 0, 0, 1);
    };

    const bool    omni      = midiOmni_.load(std::memory_order_relaxed);
    const uint8_t inChannel = midiInChannel_;
    for(const auto meta : midi)
    {
        flushSync(meta.samplePosition);
        const auto* raw = meta.data;
        // Channel messages, and real-time ones (clock, start/stop) unless the
        // plugin is sending TEMPO its own.
        const bool realtime = meta.numBytes == 1 && raw[0] >= 0xf8 && raw[0] <= 0xfc && !syncFollowing_;
        if(meta.numBytes < 1 || meta.numBytes > 3 || raw[0] < 0x80 || (raw[0] >= 0xf0 && !realtime))
            continue;
        const double  time   = adapter_.firmwareTimeAt(render ? meta.samplePosition : 0);
        const uint8_t status = omni && !realtime ? uint8_t((raw[0] & 0xf0) | inChannel) : raw[0];
        const uint8_t data1  = meta.numBytes > 1 ? raw[1] & 0x7f : 0;
        const uint8_t data2  = meta.numBytes > 2 ? raw[2] & 0x7f : 0;

        auto& held = heldNotes_[status & 0x0f];
        if((status & 0xf0) == 0x90)
            held.set(data1, data2 > 0);
        else if((status & 0xf0) == 0x80)
            held.reset(data1);
        else if((status & 0xf0) == 0xb0 && (data1 == 120 || data1 == 123)) // All Sound / Notes Off
        {
            // Only the held notes: one note-off per key would overflow the
            // firmware's 32-entry key request queue.
            for(uint8_t note = 0; note < 128; note++)
                if(held.test(note))
                    enqueue(time, uint8_t(0x80 | (status & 0x0f)), note, 0, 3);
            held.reset();
        }
        enqueue(time, status, data1, data2, uint8_t(meta.numBytes));
    }
    flushSync(numSamples);
    midi.clear();
    if(!render)
    {
        buffer.clear();
        return;
    }

    // Buses share channel memory with the input; the adapter reads each chunk's
    // input before writing its output.
    const auto   in      = getBusBuffer(buffer, true, 0);
    auto         lineOut = getBusBuffer(buffer, false, 0);
    const float* inL     = in.getNumChannels() > 0 ? in.getReadPointer(0) : nullptr;
    const float* inR     = in.getNumChannels() > 1 ? in.getReadPointer(1) : inL;

    float* outs[RateAdapter::kOutputs] = {nullptr, nullptr, lineOut.getWritePointer(0), lineOut.getWritePointer(1)};
    if(getBusCount(false) > 1 && getBus(false, 1)->isEnabled())
    {
        auto hp = getBusBuffer(buffer, false, 1);
        outs[0] = hp.getWritePointer(0);
        outs[1] = hp.getWritePointer(1);
    }

    midiOut_      = &midi;
    midiOutLimit_ = numSamples;
    sendControls(*machine_);
    adapter_.process(inL, inR, outs, numSamples, *this);
    readControls(*machine_);
    midiOut_ = nullptr;

    for(int ch = getTotalNumOutputChannels(); ch < buffer.getNumChannels(); ch++)
        buffer.clear(ch, 0, numSamples);
}

int ChompiProcessor::followHost(int numSamples, SyncEvent* events)
{
    const bool tempo = machine_->GetFirmware() == emu::Firmware::Tempo;
    std::optional<double> bpm, ppq;
    bool                  playing = false;
    if(tempo && hostSync_.load(std::memory_order_relaxed))
        if(auto* head = getPlayHead())
            if(const auto pos = head->getPosition())
            {
                if(const auto b = pos->getBpm())
                    bpm = *b;
                if(const auto p = pos->getPpqPosition())
                    ppq = *p;
                playing = pos->getIsPlaying();
            }
    const bool follow = bpm.has_value() && *bpm > 0;

    // A new machine starts in its own clock mode: put it in sync mode if
    // following; otherwise only act when following stops.
    const int generation = generation_.load();
    if(generation != syncedGeneration_ || follow != syncFollowing_)
    {
        if(follow || generation == syncedGeneration_)
            machine_->SetClockSync(follow);
        syncedGeneration_ = generation;
        syncFollowing_    = follow;
        tempoStarted_     = false;
    }
    if(!follow)
        return 0;

    int  count = 0;
    auto add   = [&](double offset, uint8_t status) {
        if(count < kMaxSyncEvents)
            events[count++] = {juce::jlimit(0, numSamples - 1, int(offset)), status};
    };

    // Pulses (24 per beat) on the host's beat grid while it plays; at its
    // tempo from wherever they were while it's stopped, so TEMPO's patterns
    // still run at the project tempo.
    const double perSample = *bpm * 24.0 / (60.0 * getSampleRate());
    const bool   rolling   = playing && ppq.has_value();
    const double start     = rolling ? *ppq * 24.0 : clockPhase_;
    const double end       = start + numSamples * perSample;

    // Start when the transport starts, at beat 0 if it is in a pre-roll, and
    // again when it jumps (cycle, locate) so the pattern restarts with it.
    const bool jumped  = std::abs(start - clockPhase_) > 1.0;
    double     startAt = -1;
    if(rolling && end > 0 && (!tempoStarted_ || jumped))
        startAt = std::max(start, 0.0);
    else if(!rolling && tempoStarted_)
        add(0, 0xfc);
    if(startAt >= 0)
        tempoStarted_ = true;
    else if(!rolling)
        tempoStarted_ = false;

    // Pulses are counted, not re-derived per block: a pulse on a block
    // boundary would otherwise round into both blocks or neither, and TEMPO
    // counts pulses to its steps.
    if(jumped || startAt >= 0)
        nextPulse_ = int64_t(std::ceil(startAt >= 0 ? startAt : start));
    for(; double(nextPulse_) < end; nextPulse_++)
    {
        const double n = double(nextPulse_);
        if(startAt >= 0 && n >= startAt)
        {
            add((startAt - start) / perSample, 0xfa);
            startAt = -1;
        }
        add((n - start) / perSample, 0xf8);
    }
    if(startAt >= 0)
        add((startAt - start) / perSample, 0xfa);

    clockPhase_ = end;
    return count;
}

void ChompiProcessor::renderBlock(uint64_t index, const float* const* in, float* const* out)
{
    auto&        machine = *machine_;
    const double end     = double(index + 1) * kFirmwareBlock;

    for(; pendingHead_ < pendingTail_ && pendingMidi_[pendingHead_].time < end; pendingHead_++)
    {
        const auto& p = pendingMidi_[pendingHead_];
        machine.SendMidi(p.bytes, p.size);
    }
    if(pendingHead_ == pendingTail_)
        pendingHead_ = pendingTail_ = 0;

    machine.Process(in, out);
    const float makeup = outputMakeup(machine.GetFirmware());
    headphoneStage_.setMakeup(makeup);
    lineStage_.setMakeup(makeup);
    headphoneStage_.process(out[0], out[1], kFirmwareBlock);
    lineStage_.process(out[2], out[3], kFirmwareBlock);

    // Heard with the block's audio, which the output stage delays. Clamped to
    // this call, so MIDI from a block heard in the next call leads it slightly.
    uint8_t   msg[3];
    const int offset = juce::jlimit(0, midiOutLimit_ - 1,
                                    adapter_.hostOffsetOf(double(index) * kFirmwareBlock + OutputStage::kLookahead));
    while(const int size = machine.ReceiveMidi(msg))
        if(midiOut_ != nullptr)
            midiOut_->addEvent(msg, size, offset);
}

void ChompiProcessor::idleTick()
{
    const uint32_t now     = juce::Time::getMillisecondCounter();
    const uint32_t elapsed = juce::jmin(kMaxIdleStepMs, now - lastIdleMs_);
    lastIdleMs_            = now;
    if(now - lastProcessMs_.load(std::memory_order_relaxed) < kIdleAfterMs)
        return;

    juce::ScopedNoDenormals              noDenormals;
    const juce::SpinLock::ScopedLockType lock(machineLock_);
    if(machine_ == nullptr)
        return;

    float        silence[emu::Machine::kNumInputs][kFirmwareBlock]  = {};
    float        scratch[emu::Machine::kNumOutputs][kFirmwareBlock] = {};
    const float* in[emu::Machine::kNumInputs]   = {silence[0], silence[1], silence[2], silence[3]};
    float*       out[emu::Machine::kNumOutputs] = {scratch[0], scratch[1], scratch[2], scratch[3]};

    const uint32_t blocks = elapsed * emu::Machine::kSampleRate / 1000 / kFirmwareBlock;
    uint8_t        msg[3];
    sendControls(*machine_);
    for(uint32_t b = 0; b < blocks; b++)
    {
        machine_->Process(in, out);
        while(machine_->ReceiveMidi(msg))
        {
        }
    }
    readControls(*machine_);
}

void ChompiProcessor::setTogglePlay(bool play)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    togglePlay_.store(play);
    if(live_ != nullptr)
        live_->Post(toggleEvent(play));
}

void ChompiProcessor::post(const emu::PanelEvent& event)
{
    if(const auto machine = currentMachine())
        machine->Post(event);
}

ChompiProcessor::Looper ChompiProcessor::looperState() const
{
    const auto machine = currentMachine();
    if(machine == nullptr || !machine->IsBooted() || machine->GetFirmware() != emu::Firmware::Tape)
        return Looper::Off;
    using State = emu::Machine::LooperState;
    const auto state = machine->GetLooperState();
    if(machine->HasPendingEvents() || state == State::Erasing)
        return Looper::Busy;
    return state == State::Armed ? Looper::Armed : state == State::Empty ? Looper::Empty : Looper::Recorded;
}

void ChompiProcessor::armLooper()
{
    const auto state = looperState();
    if(state == Looper::Empty)
        holdPlayAndLoop(kArmHoldMs);
    else if(state == Looper::Armed) // the firmware's only way back
        holdPlayAndLoop(kEraseHoldMs);
}

void ChompiProcessor::eraseLooper()
{
    if(looperState() == Looper::Recorded)
        holdPlayAndLoop(kEraseHoldMs);
}

void ChompiProcessor::holdPlayAndLoop(int16_t ms)
{
    using Type = emu::PanelEvent::Type;
    namespace pnl = emu::panel;
    if(const auto machine = currentMachine())
        for(const emu::PanelEvent event : {emu::PanelEvent{Type::Button, pnl::kPlayKey, 1},
                                           emu::PanelEvent{Type::Button, pnl::kLoopKey, 1},
                                           emu::PanelEvent{Type::Wait, 0, ms},
                                           emu::PanelEvent{Type::Button, pnl::kLoopKey, 0},
                                           emu::PanelEvent{Type::Button, pnl::kPlayKey, 0}})
            machine->Post(event);
}

bool ChompiProcessor::readLeds(Leds& leds) const
{
    const auto machine = currentMachine();
    for(int i = 0; i < emu::panel::kNumPthLeds; i++)
        leds.pth[i] = machine != nullptr ? unscale(machine->PthLed(i), emu::panel::kPthFullScale) : juce::Colours::black;
    for(int i = 0; i < emu::panel::kNumSmtLeds; i++)
        leds.smt[i] = machine != nullptr ? unscale(machine->SmtLed(i), emu::panel::kSmtFullScale) : juce::Colours::black;
    return machine != nullptr;
}

bool ChompiProcessor::isRunning() const
{
    const auto machine = currentMachine();
    return machine != nullptr && machine->IsBooted();
}

juce::AudioProcessorEditor* ChompiProcessor::createEditor()
{
    return new ChompiEditor(*this);
}

bool ChompiProcessor::captureLive(emu::Session& session, emu::Recordings& recordings)
{
    std::map<std::string, size_t> sizes;
    bool                          haveSession;
    {
        // Brief: copies a few dozen values while the audio thread waits.
        const juce::SpinLock::ScopedLockType audio(machineLock_);
        haveSession = live_->CaptureSession(session);
        live_->RecordingSizes(sizes);
    }
    // While the recordings copy, a block the audio thread tries to render is
    // skipped, so their pages are touched beforehand: on an M2 a full TAPE
    // buffer (31.7 MB) copies in ~1.4 ms into touched memory, 3-5 ms into fresh.
    for(const auto& [name, size] : sizes)
        recordings[name].assign(size + kRecordingSlack, 0);
    const juce::SpinLock::ScopedLockType audio(machineLock_);
    live_->CaptureRecordings(recordings);
    return haveSession;
}

void ChompiProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::File      folder;
    emu::Firmware   firmware;
    emu::Session    session;
    emu::Recordings recordings;
    bool            haveSession = false;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        folder   = cardFolder_;
        firmware = cardFirmware_; // a firmware switch under way changes both at once
        if(live_ != nullptr)
            haveSession = captureLive(session, recordings);
        else if(hasPendingRecordings_)
            recordings = pendingRecordings_;
        if(!haveSession && hasPendingSession_) // restored, machine still loading
        {
            session     = pendingSession_;
            haveSession = true;
        }
    }

    juce::XmlElement xml("CHOMPI");
    xml.setAttribute("firmware", juce::String(folderName(firmware)).toLowerCase());
    xml.setAttribute("cardFolder", folder.getFullPathName());
    xml.setAttribute("defaultCard", folder == defaultCardFolder(firmware));
    xml.setAttribute("midiOmni", getMidiOmni());
    xml.setAttribute("hostSync", getHostSync());
    xml.setAttribute("togglePlay", getTogglePlay());
    if(haveSession)
    {
        // The firmware's own names (emu::Session); TAPE's predate this and
        // must stay as they are for older projects.
        auto* element = xml.createNewChildElement("Session");
        for(const auto& [name, value] : session)
            element->setAttribute(juce::Identifier(name), juce::String(value));
    }
    if(const auto size = getEditorSize(); size.x > 0)
    {
        xml.setAttribute("editorWidth", size.x);
        xml.setAttribute("editorHeight", size.y);
    }

    // Each recording's FLAC follows the XML, in <Recordings> order, ending the
    // state: readers before this format see only the XML.
    const std::lock_guard<std::mutex> lock(encodedMutex_);
    for(auto it = encoded_.begin(); it != encoded_.end();)
        it = recordings.count(it->first) != 0 ? std::next(it) : encoded_.erase(it);
    std::vector<const juce::MemoryBlock*> flacs;
    if(!recordings.empty())
    {
        auto*       element = xml.createNewChildElement("Recordings");
        juce::int64 total   = 0;
        for(const auto& [name, samples] : recordings)
        {
            auto&      encoded = encoded_[name];
            const auto hash    = hashOf(samples);
            if(encoded.flac.isEmpty() || encoded.size != samples.size() || encoded.hash != hash)
                encoded = {samples.size(), hash, recording_codec::encode(samples)};
            if(encoded.flac.isEmpty())
                continue;
            auto* child = element->createNewChildElement("Recording");
            child->setAttribute("name", juce::String(name));
            child->setAttribute("bytes", juce::String(juce::int64(encoded.flac.getSize())));
            total += juce::int64(encoded.flac.getSize());
            flacs.push_back(&encoded.flac);
        }
        element->setAttribute("bytes", juce::String(total));
    }
    copyXmlToBinary(xml, destData);
    for(const auto* flac : flacs)
        destData.append(flac->getData(), flac->getSize());
}

emu::Recordings ChompiProcessor::readRecordings(const juce::XmlElement& xml, const void* data, int sizeInBytes)
{
    emu::Recordings recordings;
    const auto*     element = xml.getChildByName("Recordings");
    if(element == nullptr)
        return recordings;
    const auto total = element->getStringAttribute("bytes").getLargeIntValue();
    if(total <= 0 || total > sizeInBytes)
        return recordings;

    const auto*                       next = static_cast<const char*>(data) + (sizeInBytes - total);
    const auto*                       end  = static_cast<const char*>(data) + sizeInBytes;
    const std::lock_guard<std::mutex> lock(encodedMutex_);
    encoded_.clear();
    for(const auto* child : element->getChildWithTagNameIterator("Recording"))
    {
        const auto name  = child->getStringAttribute("name").toStdString();
        const auto bytes = child->getStringAttribute("bytes").getLargeIntValue();
        if(bytes <= 0 || bytes > end - next)
            break;
        std::vector<int16_t> samples;
        if(recording_codec::decode(next, size_t(bytes), samples))
        {
            encoded_[name]    = {samples.size(), hashOf(samples), juce::MemoryBlock(next, size_t(bytes))};
            recordings[name] = std::move(samples);
        }
        next += bytes;
    }
    return recordings;
}

void ChompiProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if(xml == nullptr || !xml->hasTagName("CHOMPI"))
        return;

    const std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    setMidiOmni(xml->getBoolAttribute("midiOmni", true));
    setHostSync(xml->getBoolAttribute("hostSync", true));
    setTogglePlay(xml->getBoolAttribute("togglePlay", true));
    setEditorSize({xml->getIntAttribute("editorWidth", 0), xml->getIntAttribute("editorHeight", 0)});

    // Projects from before there were other firmwares are TAPE's.
    const auto name     = xml->getStringAttribute("firmware", "tape");
    auto       firmware = name == "wave" ? emu::Firmware::Wave : name == "tempo" ? emu::Firmware::Tempo : emu::Firmware::Tape;
    if(!emu::Machine::IsBuiltIn(firmware))
        firmware = emu::Firmware::Tape;

    // On the default card, the project follows this machine's default folder
    // (whose path differs per user), so moving it between Macs isn't a missing card.
    const auto folder = xml->getBoolAttribute("defaultCard") ? defaultCardFolder(firmware)
                                                             : juce::File(xml->getStringAttribute("cardFolder"));
    const bool switching = firmware != getFirmware();
    if(switching || (folder != juce::File() && folder != getCardFolder()))
    {
        // This project replaces whatever waited for the old machine, an
        // import included.
        firmware_.store(firmware);
        {
            const std::lock_guard<std::mutex> lock(stateMutex_);
            clearPending();
        }
        startMachine(folder != juce::File() ? folder : defaultCardFolder(firmware), false); // before the session, which goes to the new machine
    }

    std::optional<emu::Session> session;
    if(const auto* element = xml->getChildByName("Session"))
    {
        session.emplace();
        for(int i = 0; i < element->getNumAttributes(); i++)
            (*session)[element->getAttributeName(i).toStdString()] = element->getAttributeValue(i).toStdString();
    }
    auto recordings = readRecordings(*xml, data, sizeInBytes);

    const std::lock_guard<std::mutex> lock(stateMutex_);
    if(live_ != nullptr)
    {
        const juce::SpinLock::ScopedLockType audio(machineLock_);
        if(session)
            live_->RestoreSession(*session);
        live_->RestoreRecordings(std::move(recordings));
    }
    else
    {
        // A project without a session replaces one restored earlier too.
        hasPendingSession_ = session.has_value();
        if(session)
            pendingSession_ = std::move(*session);
        pendingRecordings_    = std::move(recordings);
        hasPendingRecordings_ = true;
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ChompiProcessor();
}
