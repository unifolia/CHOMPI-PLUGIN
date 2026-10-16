#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
constexpr int      kFirmwareBlock  = emu::Machine::kBlockSize;
constexpr size_t   kMaxPendingMidi = 4096;
constexpr uint32_t kIdleAfterMs    = 150; // host considered stopped after this long without a block
constexpr uint32_t kMaxIdleStepMs  = 100;

constexpr const char* kCardFiles = "*.wav;*.json";

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

/** Puts a full-velocity four-note chord at the default volume near -4 dBFS
 *  peak on each factory card; raw it peaks at -22.2 (TAPE), -28.0 (WAVE) and
 *  -16.4 dBFS (TEMPO), measured with chompi_render. */
float outputMakeup(emu::Firmware firmware)
{
    switch(firmware)
    {
        case emu::Firmware::Tape: return juce::Decibels::decibelsToGain(18.f);
        case emu::Firmware::Wave: return juce::Decibels::decibelsToGain(24.f);
        case emu::Firmware::Tempo: return juce::Decibels::decibelsToGain(12.f);
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
    pendingMidi_.resize(kMaxPendingMidi);
    adapter_.prepare(emu::Machine::kSampleRate, 512, OutputStage::kLookahead);
    startMachine(defaultCardFolder(emu::Firmware::Tape), false);
}

ChompiProcessor::~ChompiProcessor()
{
    stopMachine();
}

juce::String ChompiProcessor::firmwareName(emu::Firmware firmware)
{
    return juce::String(folderName(firmware)) + (firmware == emu::Firmware::Tape ? " 2.0" : " 1.0");
}

juce::File ChompiProcessor::factoryCardFolder(emu::Firmware firmware)
{
    // Installed per user by the build, or system-wide by an installer; the
    // source tree's cards/ serves builds that skipped installing.
    using juce::File;
    const auto sub = juce::String("Application Support/CHIMPO/Factory Cards/") + folderName(firmware);
    for(const auto& folder : {File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile(sub),
                              File::getSpecialLocation(File::commonApplicationDataDirectory).getChildFile(sub)})
        if(hasCardFiles(folder))
            return folder;
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

void ChompiProcessor::startMachine(const juce::File& folder, bool restoreFactory)
{
    const std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    stopMachine();
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        cardFolder_ = folder;
    }
    loaderCancelled_ = false;

    // Copying and loading the card and booting take a moment; the audio thread
    // outputs silence until the machine is published.
    loader_ = std::thread([this, requested = folder, restoreFactory, firmware = firmware_.load()] {
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
        auto card = emu::VirtualCard::Open(active.getFullPathName().toStdString());
        if(loaderCancelled_)
            return;
        std::shared_ptr<emu::Machine> machine = emu::Machine::Create(firmware, std::move(card));
        if(machine == nullptr) // not built in: the unit stays off
            return;

        // Under stateMutex_, so the toggle and a restored session can't slip
        // between this machine's creation and its publication.
        const std::lock_guard<std::mutex> lock(stateMutex_);
        if(loaderCancelled_)
            return;
        machine->Post(toggleEvent(togglePlay_.load()));
        if(hasPendingSession_)
        {
            machine->RestoreSession(pendingSession_);
            hasPendingSession_ = false;
        }
        {
            const juce::SpinLock::ScopedLockType audio(machineLock_);
            machine_ = machine;
        }
        live_ = std::move(machine);
        cardMissing_ = missing;
        generation_++;
    });
}

void ChompiProcessor::setFirmware(emu::Firmware firmware)
{
    const std::lock_guard<std::recursive_mutex> lifecycle(lifecycleMutex_);
    if(firmware == firmware_.load() || !emu::Machine::IsBuiltIn(firmware))
        return;
    firmware_.store(firmware);
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        hasPendingSession_ = false; // the other firmware's
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
    if(!lock.isLocked() || machine_ == nullptr)
    {
        buffer.clear();
        midi.clear();
        return;
    }

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
    const int  numSync   = followHost(numSamples, sync);
    int        nextSync  = 0;
    auto       flushSync = [&](int upTo) {
        for(; nextSync < numSync && sync[nextSync].offset <= upTo; nextSync++)
            enqueue(adapter_.firmwareTimeAt(sync[nextSync].offset), sync[nextSync].status, 0, 0, 1);
    };

    const bool    omni      = midiOmni_.load(std::memory_order_relaxed);
    const uint8_t inChannel = uint8_t(machine_->MidiInChannel());
    for(const auto meta : midi)
    {
        flushSync(meta.samplePosition);
        const auto* raw = meta.data;
        // Channel messages, and real-time ones (clock, start/stop) unless the
        // plugin is sending TEMPO its own.
        const bool realtime = meta.numBytes == 1 && raw[0] >= 0xf8 && raw[0] <= 0xfc && !syncFollowing_;
        if(meta.numBytes < 1 || meta.numBytes > 3 || raw[0] < 0x80 || (raw[0] >= 0xf0 && !realtime))
            continue;
        const double  time   = adapter_.firmwareTimeAt(meta.samplePosition);
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
    adapter_.process(inL, inR, outs, numSamples, *this);
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
    for(uint32_t b = 0; b < blocks; b++)
    {
        machine_->Process(in, out);
        while(machine_->ReceiveMidi(msg))
        {
        }
    }
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

void ChompiProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::File   folder;
    emu::Session session;
    bool         haveSession = false;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        folder = cardFolder_;
        if(live_ != nullptr)
        {
            // Brief: copies a few dozen values while the audio thread waits.
            const juce::SpinLock::ScopedLockType audio(machineLock_);
            haveSession = live_->CaptureSession(session);
        }
        if(!haveSession && hasPendingSession_) // restored, machine still loading
        {
            session     = pendingSession_;
            haveSession = true;
        }
    }

    const auto       firmware = getFirmware();
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
    copyXmlToBinary(xml, destData);
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
    if(switching)
    {
        firmware_.store(firmware);
        const std::lock_guard<std::mutex> lock(stateMutex_);
        hasPendingSession_ = false;
    }
    if(switching || (folder != juce::File() && folder != getCardFolder()))
        startMachine(folder != juce::File() ? folder : defaultCardFolder(firmware), false); // before the session, which goes to the new machine

    if(const auto* element = xml->getChildByName("Session"))
    {
        emu::Session session;
        for(int i = 0; i < element->getNumAttributes(); i++)
            session[element->getAttributeName(i).toStdString()] = element->getAttributeValue(i).toStdString();
        const std::lock_guard<std::mutex> lock(stateMutex_);
        if(live_ != nullptr)
        {
            const juce::SpinLock::ScopedLockType audio(machineLock_);
            live_->RestoreSession(session);
        }
        else
        {
            pendingSession_    = session;
            hasPendingSession_ = true;
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ChompiProcessor();
}
