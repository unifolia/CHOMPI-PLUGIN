#include "PluginEditor.h"

#include "AudioImport.h"

using namespace juce;
using namespace panel_ui;
namespace pnl = emu::panel;

ChompiEditor::ChompiEditor(ChompiProcessor& p)
: AudioProcessorEditor(p), processor_(p), input_(p), audioExtensions_(audio_import::wildcard().removeCharacters("*"))
{
    const auto savedSize = processor_.getEditorSize(); // resized() overwrites it below

    canvas_.setSize(kWidth, kHeight);
    addAndMakeVisible(canvas_);

    background_.setBounds(0, 0, kWidth, kHeight);
    canvas_.addAndMakeVisible(background_);

    auto place = [this](Component& c, Point<float> centre) {
        c.setCentrePosition(centre.roundToInt());
        canvas_.addAndMakeVisible(c);
    };

    for(int i = 0; i < pnl::kNumPthLeds; i++)
    {
        const auto style = i == pnl::kLedLooperRev || i == pnl::kLedLooperFwd ? Led::Style::Small
                           : i == pnl::kLedPlay || i == pnl::kLedLoop         ? Led::Style::Reel
                                                                              : Led::Style::Large;
        leds_.push_back(std::make_unique<Led>(i, style));
        place(*leds_.back(), Layout::pthLed(i));
    }

    auto addKey = [&](int button, Key::Style style, int smt, Point<float> centre) {
        keys_.push_back(std::make_unique<Key>(input_, button, style, smt));
        keyByButton_[size_t(button)] = keys_.back().get();
        place(*keys_.back(), centre);
    };
    for(int i = 0; i < 10; i++)
        addKey(pnl::kBlackKeys[i], Key::Style::Black, pnl::SmtLedForBlackKey(i), Layout::blackKey(i));
    for(int i = 0; i < 15; i++)
        addKey(pnl::kWhiteKeys[i], Key::Style::White, pnl::SmtLedForWhiteKey(i), Layout::whiteKey(i));
    addKey(pnl::kChompiKey, Key::Style::Chompi, -1, Layout::kChompiKey);
    addKey(pnl::kPlayKey, Key::Style::Play, -1, Layout::kPlayKey);
    addKey(pnl::kLoopKey, Key::Style::Loop, -1, Layout::kLoopKey);

    // Pixels per detent suit TAPE's step sizes; the others' are similar.
    const std::pair<int, float> encoders[] = {{pnl::kEncSpeed, 1.f}, {pnl::kEncStart, 2.f}, {pnl::kEncEnd, 2.f},
                                              {pnl::kEncMagic, 4.f}, {pnl::kEncLooper, 1.f}, {pnl::kEncVolume, 4.f}};
    for(const auto& [encoder, pixelsPerDetent] : encoders)
    {
        const auto style = encoder == pnl::kEncLooper ? Knob::Style::Purple : Knob::Style::Small;
        knobs_.push_back(std::make_unique<Knob>(input_, encoder, style, pixelsPerDetent));
        place(*knobs_.back(), Layout::encoder(encoder));
    }

    toggle_ = std::make_unique<Toggle>(input_);
    place(*toggle_, Layout::kToggle);
    shownTogglePlay_ = input_.togglePlay();

    applyTooltips(processor_.getFirmware());

    input_.onButtonChanged = [this](int button) {
        if(auto* key = keyByButton_[size_t(button)])
            key->repaint();
    };

    firmwareButton_.setTooltip("Firmware: TAPE (sampler and looper), WAVE (wavetable synth) or TEMPO (pattern generator)");
    firmwareButton_.onClick = [this] { showFirmwareMenu(); };
    firmwareButton_.setTopLeftPosition(20, 5);
    canvas_.addAndMakeVisible(firmwareButton_);

    // TAPE's looper gestures, which take both play and loop at once.
    armButton_.setTooltip("Arms the looper: recording starts with the next note, from a key or MIDI. Click again to "
                          "disarm.\nOn the unit: press play and loop together on an empty looper.");
    armButton_.setSize(70, 20);
    armButton_.onClick = [this] {
        gesture_ = true;
        processor_.armLooper();
    };
    eraseButton_.setTooltip("Erases the looper.\nOn the unit: hold play and loop together for 2 s.");
    eraseButton_.setSize(80, 20);
    eraseButton_.onClick = [this] {
        gesture_ = erasing_ = true;
        processor_.eraseLooper();
    };
    canvas_.addChildComponent(armButton_);
    canvas_.addChildComponent(eraseButton_);

    settings_.setTooltip("SD card and MIDI settings");
    settings_.onClick = [this] { showSettingsMenu(); };
    settings_.setBounds(kWidth - 56, 6, 34, 20);
    canvas_.addAndMakeVisible(settings_);

    // Right of the carry handle, up to the card menu.
    cardWarning_.setFont(FontOptions(12.f));
    cardWarning_.setColour(Label::textColourId, Colour(0xffb3302a));
    cardWarning_.setJustificationType(Justification::centredRight);
    cardWarning_.setMinimumHorizontalScale(0.8f);
    cardWarning_.setBounds(780, 4, settings_.getX() - 4 - 780, 22);
    canvas_.addChildComponent(cardWarning_);

    status_.setFont(FontOptions(12.f));
    status_.setColour(Label::textColourId, Colour(0xff6c6a69));
    status_.setJustificationType(Justification::centredRight);
    status_.setMinimumHorizontalScale(0.7f);
    status_.setBounds(cardWarning_.getBounds());
    canvas_.addChildComponent(status_);

    setResizable(true, true);
    setResizeLimits(kWidth * 6 / 10, kHeight * 6 / 10, kWidth * 2, kHeight * 2);
    getConstrainer()->setFixedAspectRatio(double(kWidth) / double(kHeight));
    if(savedSize.x > 0 && savedSize.y > 0)
        setSize(savedSize.x, savedSize.y);
    else
        setSize(kWidth, kHeight);

    updateStatus();
    startTimerHz(60);
}

ChompiEditor::~ChompiEditor()
{
    // Don't leave keys held in the machine once nobody can release them.
    input_.onButtonChanged = nullptr;
    input_.releaseAll();
}

void ChompiEditor::paint(Graphics& g)
{
    g.fillAll(Colour(0xffcac8c7));
}

void ChompiEditor::resized()
{
    const float scale = float(getWidth()) / float(kWidth);
    canvas_.setTransform(AffineTransform::scale(scale));
    processor_.setEditorSize({getWidth(), getHeight()});
}

void ChompiEditor::timerCallback()
{
    processor_.idleTick();

    if(processor_.machineGeneration() != generation_)
    {
        generation_ = processor_.machineGeneration();
        input_.releaseAll(); // the new machine starts with every key up
        shiftHeld_ = false;
    }
    if(processor_.getFirmware() != shownFirmware_)
        applyTooltips(processor_.getFirmware());
    if(input_.togglePlay() != shownTogglePlay_) // a project load moves it too
    {
        shownTogglePlay_ = !shownTogglePlay_;
        toggle_->repaint();
    }
    if(processor_.isRunning() != wasRunning_ || processor_.isCardMissing() != wasMissing_)
        updateStatus();
    if(statusUntil_ != 0 && Time::getMillisecondCounter() > statusUntil_)
    {
        statusUntil_ = 0;
        status_.setVisible(false);
        updateStatus();
    }

    ChompiProcessor::Leds leds;
    processor_.readLeds(leds);
    for(auto& led : leds_)
        led->setColour(leds.pth[led->index()]);
    for(auto& key : keys_)
        if(key->smtLed() >= 0)
            key->setBacklight(leds.smt[key->smtLed()]);
    updateLooperButtons();

    // Shift on the computer keyboard holds the CHOMPI key for the shift layer,
    // while the pointer is over the panel and the host is the active app. Not
    // in record mode, where the CHOMPI key records.
    const bool shift = ModifierKeys::getCurrentModifiersRealtime().isShiftDown() && input_.togglePlay()
                       && isMouseOverOrDragging(true) && Process::isForegroundProcess();
    if(shift != shiftHeld_)
    {
        shiftHeld_ = shift;
        input_.set(pnl::kChompiKey, Input::Shift, shift);
    }
}

void ChompiEditor::updateStatus()
{
    wasRunning_ = processor_.isRunning();
    wasMissing_ = processor_.isCardMissing();
    firmwareButton_.setText(wasRunning_ ? ChompiProcessor::firmwareName(processor_.getFirmware()) + " firmware"
                                        : String("Inserting SD card..."));

    const bool warn = wasRunning_ && wasMissing_;
    cardWarning_.setVisible(warn && !status_.isVisible());
    cardWarning_.setText(warn ? "SD card folder not found: using the default card" : "", dontSendNotification);
    cardWarning_.setTooltip(warn ? processor_.getCardFolder().getFullPathName()
                                       + " wasn't found. Reconnect it and choose Reload SD Card, or choose another folder."
                                 : String());
}

void ChompiEditor::updateLooperButtons()
{
    using Looper     = ChompiProcessor::Looper;
    const auto state = processor_.looperState();
    if(state != Looper::Busy) // a click's key press is Busy for a block or so: keep the last state
    {
        looper_  = state;
        gesture_ = erasing_ = false;
    }
    // The gestures need play and loop up and the shift layer closed.
    const bool ready = !gesture_ && !input_.isDown(pnl::kPlayKey) && !input_.isDown(pnl::kLoopKey)
                    && !input_.isDown(pnl::kChompiKey);

    armButton_.setVisible(looper_ != Looper::Off);
    eraseButton_.setVisible(looper_ != Looper::Off);
    armButton_.setText(looper_ == Looper::Armed ? "Disarm" : "Arm loop", looper_ == Looper::Armed);
    armButton_.setEnabled(ready && (looper_ == Looper::Empty || looper_ == Looper::Armed));
    eraseButton_.setText(erasing_ ? "Erasing..." : "Erase loop", false);
    eraseButton_.setEnabled(ready && looper_ == Looper::Recorded);
    armButton_.setTopLeftPosition(firmwareButton_.getRight() + 12, firmwareButton_.getY());
    eraseButton_.setTopLeftPosition(armButton_.getRight() + 6, firmwareButton_.getY());
}

int ChompiEditor::keyAt(int x, int y) const
{
    const auto p = canvas_.getLocalPoint(this, Point<int>(x, y));
    for(const auto& key : keys_)
        if(key->getBounds().contains(p) && key->hitTest(p.x - key->getX(), p.y - key->getY()))
            return key->button();
    return -1;
}

StringArray ChompiEditor::audioFiles(const StringArray& files) const
{
    StringArray audio;
    for(const auto& file : files)
        if(File(file).existsAsFile() && File(file).hasFileExtension(audioExtensions_))
            audio.add(file);
    return audio;
}

void ChompiEditor::clearDropTargets()
{
    for(auto& key : keys_)
        key->setDropTarget(false);
    dropButton_ = kNoDrag;
}

void ChompiEditor::showStatus(const String& text, double seconds)
{
    status_.setText(text, dontSendNotification);
    status_.setVisible(text.isNotEmpty());
    statusUntil_ = seconds > 0 ? Time::getMillisecondCounter() + uint32_t(seconds * 1000) : 0;
    updateStatus();
}

bool ChompiEditor::isInterestedInFileDrag(const StringArray& files)
{
    return !audioFiles(files).isEmpty() && !processor_.planImport(pnl::kWhiteKeys[0], 1).empty();
}

void ChompiEditor::fileDragEnter(const StringArray& files, int x, int y)
{
    fileDragMove(files, x, y);
}

void ChompiEditor::fileDragMove(const StringArray& files, int x, int y)
{
    const int button = keyAt(x, y);
    if(button == dropButton_)
        return;
    clearDropTargets();
    dropButton_ = button;
    const auto plan = button >= 0 ? processor_.planImport(button, audioFiles(files).size())
                                  : std::vector<ChompiProcessor::ImportPlan>();
    if(plan.empty())
    {
        showStatus(button >= 0 ? "That key takes no audio" : "Drop on a key to load into its slot", 0);
        return;
    }
    for(const auto& [key, target] : plan)
        keyByButton_[size_t(key)]->setDropTarget(true);
    const String first(plan.front().target.label);
    showStatus(plan.size() == 1 ? "Drop to load into " + first
                                : "Drop to load " + String(int(plan.size())) + " files into " + first + " to "
                                      + String(plan.back().target.label),
               0);
}

void ChompiEditor::fileDragExit(const StringArray&)
{
    clearDropTargets();
    showStatus({}, 0);
}

void ChompiEditor::filesDropped(const StringArray& files, int x, int y)
{
    const int button = keyAt(x, y);
    clearDropTargets();
    showStatus(button >= 0 ? processor_.importAudio(audioFiles(files), button) : String("Drop on a key to load into its slot"), 6);
}

void ChompiEditor::showFirmwareMenu()
{
    PopupMenu menu;
    const auto current = processor_.getFirmware();
    for(const auto firmware : {emu::Firmware::Tape, emu::Firmware::Tempo, emu::Firmware::Wave})
    {
        const bool built = emu::Machine::IsBuiltIn(firmware);
        menu.addItem(ChompiProcessor::firmwareName(firmware) + (built ? "" : " (coming soon)"), built, firmware == current,
                     [this, firmware] {
                         processor_.setFirmware(firmware);
                         updateStatus();
                     });
    }
    menu.addSeparator();
    menu.addItem("Switching restarts the unit on that firmware's own SD card.", false, false, nullptr);
    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&firmwareButton_));
}

void ChompiEditor::showSettingsMenu()
{
    PopupMenu menu;
    menu.addSectionHeader("SD card: " + processor_.getActiveCardFolder().getFullPathName());
    menu.addItem("Show SD Card in Finder", [this] { processor_.getActiveCardFolder().revealToUser(); });
    menu.addItem("Choose SD Card Folder...", [this] {
        chooser_ = std::make_unique<FileChooser>("Choose a folder to use as the SD card", processor_.getActiveCardFolder(), "");
        chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                              [this](const FileChooser& fc) {
                                  const auto folder = fc.getResult();
                                  if(folder.isDirectory())
                                  {
                                      processor_.insertCard(folder);
                                      updateStatus();
                                  }
                              });
    });
    menu.addItem("Reload SD Card (Power Cycle)", [this] {
        processor_.insertCard({});
        updateStatus();
    });
    menu.addItem("Restore Factory Samples...", ChompiProcessor::factoryCardFolder(processor_.getFirmware()).isDirectory(), false, [this] {
        const auto options = MessageBoxOptions::makeOptionsOkCancel(
            MessageBoxIconType::WarningIcon,
            "Restore factory samples?",
            "The factory samples or wavetables, options.json and presets.json are copied over the files on this "
            "card. Your other files stay. The unit restarts afterwards.",
            "Restore",
            "Cancel",
            this);
        // The box can outlive the editor, so the processor is reached through it.
        AlertWindow::showAsync(options, [editor = SafePointer<ChompiEditor>(this)](int result) {
            if(result != 0 && editor != nullptr)
                editor->processor_.restoreFactoryCard();
        });
    });
    menu.addSeparator();
    menu.addItem("Respond to MIDI on All Channels", true, processor_.getMidiOmni(), [this] {
        processor_.setMidiOmni(!processor_.getMidiOmni());
    });
    if(processor_.getFirmware() == emu::Firmware::Tempo)
        menu.addItem("Follow Host Tempo and Transport", true, processor_.getHostSync(), [this] {
            processor_.setHostSync(!processor_.getHostSync());
        });
    menu.addItem("Release Latched Keys", [this] { input_.releaseAll(); });

    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&settings_));
}

void ChompiEditor::SettingsButton::paint(Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced(1.f);
    g.setColour(Colour(isMouseOver() ? 0xffbdbbba : 0xffd1cfce));
    g.fillRoundedRectangle(r, r.getHeight() * 0.5f);
    g.setColour(Colour(0xff6c6a69));
    for(int i = -1; i <= 1; i++)
        g.fillEllipse(Rectangle<float>(3.6f, 3.6f).withCentre(r.getCentre().translated(float(i) * 7.f, 0)));
}

void ChompiEditor::SettingsButton::mouseUp(const MouseEvent& e)
{
    if(e.mouseWasClicked() && onClick)
        onClick();
}

void ChompiEditor::BarButton::setText(const String& text, bool lit)
{
    if(text == text_ && lit == lit_)
        return;
    text_ = text;
    lit_  = lit;
    repaint();
}

void ChompiEditor::BarButton::paint(Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced(1.f);
    g.setColour(Colour(isEnabled() && isMouseOver() ? 0xffbdbbba : 0xffd1cfce));
    g.fillRoundedRectangle(r, r.getHeight() * 0.5f);
    g.setColour(Colour(lit_ ? 0xffb3302a : 0xff6c6a69).withMultipliedAlpha(isEnabled() ? 1.f : 0.45f));
    g.setFont(FontOptions(12.f));
    g.drawText(text_, r, Justification::centred, false);
}

void ChompiEditor::BarButton::mouseUp(const MouseEvent& e)
{
    if(e.mouseWasClicked() && onClick)
        onClick();
}

void ChompiEditor::FirmwareButton::setText(const String& text)
{
    if(text == text_)
        return;
    text_ = text;
    // Text, 6 px each side, and the chevron; rounded up, or drawText elides.
    setSize(int(std::ceil(GlyphArrangement::getStringWidth(FontOptions(12.f), text_))) + 32, 20);
    repaint();
}

void ChompiEditor::FirmwareButton::paint(Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    if(isMouseOver())
    {
        g.setColour(Colour(0xffd1cfce));
        g.fillRoundedRectangle(r, r.getHeight() * 0.5f);
    }
    g.setColour(Colour(0xff6c6a69));
    g.setFont(FontOptions(12.f));
    g.drawText(text_, r.withTrimmedLeft(6.f).withTrimmedRight(20.f), Justification::centredLeft, false);

    Path chevron;
    const auto c = Point<float>(r.getRight() - 12.f, r.getCentreY());
    chevron.startNewSubPath(c.translated(-3.5f, -1.5f));
    chevron.lineTo(c.translated(0.f, 2.f));
    chevron.lineTo(c.translated(3.5f, -1.5f));
    g.strokePath(chevron, PathStrokeType(1.4f, PathStrokeType::curved, PathStrokeType::rounded));
}

void ChompiEditor::FirmwareButton::mouseUp(const MouseEvent& e)
{
    if(e.mouseWasClicked() && onClick)
        onClick();
}

/** From each firmware's NormalPage.h and MenuPage.h, WAVE's Sequencer.h and
 *  TEMPO's ArpeggiatorSequencer.h. */
void ChompiEditor::applyTooltips(emu::Firmware firmware)
{
    shownFirmware_ = firmware;

    struct Tips
    {
        const char* black;
        const char* blackShift[10]; // left to right
        const char* white;
        const char* whiteShift;     // "%d" is the key's number
        const char* whiteShift15;
        const char* chompi;
        const char* play;
        const char* loop;
        const char* knobs[pnl::kNumEncoders]; // by encoder
        const char* toggle;
    };

    static const Tips tape = {
        "Jammi mode: plays the sample chromatically (Cubbi: unused).",
        {"Jammi mode; again: next bank", "Cubbi mode; again: next bank", "Input: mic", "Input: line",
         "Input: resample", "Effects before the looper", "Effects after the looper",
         "Erase: pick a slot, then press CHIMPO", "Copy: pick source and destination, then press CHIMPO",
         "Save the CHIMPO buffer: pick a slot, then press CHIMPO"},
        "Plays the sample chromatically (Jammi) or this slot's sample (Cubbi).",
        "select slot %d",
        "select the CHIMPO buffer (your recording)",
        "CHIMPO key. Play mode: hold for the shift layer (or hold Shift with the pointer over the panel). "
        "Record mode: hold to sample the input (press to start and stop with Record Latch on).",
        "Looper play / pause. Hold 2 s while paused: back to the start. Hold with loop: on an empty looper, "
        "arms recording to start with the next note; held 2 s, clears the looper.\nShift: lower the overdub level.",
        "Looper record, then overdub.\nShift: raise the overdub level.",
        {// start, end, magic, speed, looper, volume
         "Start point. Push: attack page.\nShift: slide the whole window (attack page: attack and release); "
         "push toggles auto-loop.",
         "End point. Push: release page.\nShift: slide the whole window (release page: attack and release); "
         "push toggles sustain.",
         "Magic: reverb and delay. Push: lo-fi page, then filter page.\nShift: delay time / warble / resonance "
         "(per page); push resets the effects.",
         "Speed and direction. Push: sample gain page.\nShift: pitch in fifths and octaves (gain page: pan); "
         "push resets.\noptions.json can swap which layer steps.",
         "Looper speed and direction; scrubs while paused. Push: back to 1x.\nShift: speed in fifths and "
         "octaves.\noptions.json can swap which layer steps.",
         "Volume. Push: input gain page.\nShift: output compression; push cycles input monitoring "
         "(headphones, both, send/return)."},
        "Mode switch. Up: play. Down: record, where the CHIMPO key samples the input and the input is monitored.",
    };

    static const Tips wave = {
        "Plays notes.",
        {"octave down", "octave up", "sequencer gate: short", "sequencer gate: half", "sequencer gate: full",
         "pitch LFO on / off", "filter LFO on / off", "erase a preset: pick its slot, then press CHIMPO",
         "copy a preset: pick source and destination, then press CHIMPO",
         "save the sound as a preset: pick a slot, then press CHIMPO"},
        "Plays notes.",
        "load preset %d",
        "load the default sound",
        "CHIMPO key. Switch up: hold for the shift layer (or hold Shift with the pointer over the panel); "
        "confirms save, copy and erase. Switch down: mutes while held, or adds a rest while the sequencer records.",
        "Sequencer play / stop. Hold with loop for 1.25 s: clear the sequence.",
        "Sequencer step recording on / off: each key played adds a step. Hold 1.25 s: delete the last step.",
        {"Attack. Push: pitch-LFO amount page.\nShift: coarse attack (LFO page: pitch-LFO rate); push resets.",
         "Release. Push: filter-LFO amount page.\nShift: coarse release (LFO page: filter-LFO rate); push resets.",
         "Magic: delay (left) or reverb (right). Push: filter cutoff page.\nShift: delay time (cutoff page: "
         "resonance); push resets the effects.",
         "Pitch, fine. Push: wavetable frame page.\nShift: pitch in semitones (frame page: choose the wavetable); "
         "push resets.",
         "Sequencer tempo. Push: tap tempo.\nShift: clock division; push resets tempo and division.",
         "Volume. Push: pan page; hold: battery level.\nShift: output compression; push resets."},
        "Mode switch. Up: the CHIMPO key opens the shift layer. Down: the CHIMPO key mutes, or adds rests while "
        "the sequencer records.",
    };

    static const Tips tempo = {
        "Plays the engine on show: the sample chromatically, or one slice per key.",
        {"chromatic engine", "slice engine", "input: mic", "input: line", "input: resample",
         "state A; hold while on A: copy it to B", "state B; hold while on B: copy it to A",
         "erase a sample slot: pick it, then press CHIMPO",
         "copy a sample slot: pick source and destination, then press CHIMPO",
         "save the recording to a slot: pick it, then press CHIMPO"},
        "Plays the engine on show: the sample chromatically, or one slice per key.",
        "select sample slot %d",
        "select the recording",
        "CHIMPO key. Switch up: hold for the shift layer (or hold Shift with the pointer over the panel). "
        "Switch down: hold to record the input (press to start and stop with Record Latch on).",
        "Pattern play / stop. Following the host (••• menu), the host's transport starts and stops it too.\n"
        "Shift: next pattern.",
        "Latch: notes keep playing after you let go.\nShift: next rest pattern.",
        {"Sample start. Push: attack page.\nShift: move the window (attack page: loop); push resets.",
         "Sample end. Push: release page.\nShift: halve or double the window (release page: sustain); push resets.",
         "Magic: delay. Push: delay mix page.\nShift: delay randomness (mix page: feedback); push resets the effects.",
         "Pitch. Push: sample volume page, then filter page.\nShift: pitch in steps (volume page: pan; filter "
         "page: sample-rate reduction); push resets.",
         "Tempo; with the push held, clock division (following MIDI clock: division only). Push: tap tempo.\n"
         "Shift: pattern randomness; hold the push 1 s: follow MIDI clock / own tempo.",
         "Volume. Push: input gain page; hold: battery level.\nShift: output compression; push cycles input "
         "monitoring."},
        "Mode switch. Up: play; the CHIMPO key opens the shift layer. Down: record; the CHIMPO key samples the input.",
    };

    const Tips& t = firmware == emu::Firmware::Wave ? wave : firmware == emu::Firmware::Tempo ? tempo : tape;
    const auto latchTip = String("\nOption-click any key to latch it.");

    for(int i = 0; i < 10; i++)
        keyByButton_[size_t(pnl::kBlackKeys[i])]->setTooltip(String(t.black) + "\nShift: " + t.blackShift[i]);
    for(int i = 0; i < 15; i++)
        keyByButton_[size_t(pnl::kWhiteKeys[i])]->setTooltip(
            String(t.white) + "\nShift: " + (i == 14 ? String(t.whiteShift15) : String(t.whiteShift).replace("%d", String(i + 1))));
    keyByButton_[pnl::kChompiKey]->setTooltip(t.chompi + latchTip);
    keyByButton_[pnl::kPlayKey]->setTooltip(t.play);
    keyByButton_[pnl::kLoopKey]->setTooltip(t.loop);
    for(auto& knob : knobs_)
        knob->setTooltip(t.knobs[knob->encoder()]);
    toggle_->setTooltip(t.toggle);
}
