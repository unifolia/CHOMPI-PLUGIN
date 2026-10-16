// Port of WAVE 1.0's chompi_main.cpp: globals become Firmware members,
// interrupt handlers methods that Process() schedules.
#include "chompi/Machine.h"

#include "emu_context.h"
#include "emu_expose.h"
#include "emu_session.h"

#include "hardware.h"
#include "temp_led_stuff.h"
#include "ui.h"
#include "daisysp.h"
#include "fatfs.h"
#include "diskio.h"
#include "subtractiveEngine.h"
#include "Sequencer.h"
#include "InterpolatedDelayLine.h"
#include "OptionsManager.h"
#include "WavetableManager.h"
#include "clockManager.h"
#include "MidiManager.h"

#define MAX_CYCLES 256

namespace emu
{
using daisy::System;

namespace
{
/** What a DAW session restores: the sound as it stands (it differs from the
 *  selected preset until saved), tempo, and the sequence. The sequencer
 *  comes back stopped, as after power-on. */
struct SessionState
{
    uint8_t                 slot = 15; // selected preset; 15 is the defaults slot
    float                   knobs[3][6]{}; // [page][knob], the firmware's enc_values
    uint8_t                 pages[6]{};
    float                   pitchLfoRate = .58f, filterLfoRate = .58f, delayTime = .4f, resonance = .63f;
    float                   compression = 0.f;
    int                     table = 0, frame = 0, octave = 0;
    bool                    pitchLfo = true, filterLfo = true;
    int                     tempo = 320;
    uint8_t                 division = 2; // clockManager's freeDivs index
    int                     divisionCounter = 30;
    float                   gate = .5f;
    std::vector<KeyRequest> steps;
};

constexpr int kStepFields = 5; // KeyRequest: type, transpose, key, velocity, source

void ToSession(const SessionState& st, Session& s)
{
    using namespace session;
    s.clear();
    PutInt(s, "slot", st.slot);
    PutFloats(s, "knobs", &st.knobs[0][0], 18);
    const float pages[6] = {float(st.pages[0]), float(st.pages[1]), float(st.pages[2]),
                            float(st.pages[3]), float(st.pages[4]), float(st.pages[5])};
    PutFloats(s, "pages", pages, 6, 0);
    PutFloat(s, "pitchLfoRate", st.pitchLfoRate);
    PutFloat(s, "filterLfoRate", st.filterLfoRate);
    PutFloat(s, "delayTime", st.delayTime);
    PutFloat(s, "resonance", st.resonance);
    PutFloat(s, "compression", st.compression);
    PutInt(s, "wavetable", st.table);
    PutInt(s, "frame", st.frame);
    PutInt(s, "octave", st.octave);
    PutBool(s, "pitchLfo", st.pitchLfo);
    PutBool(s, "filterLfo", st.filterLfo);
    PutInt(s, "tempo", st.tempo);
    PutInt(s, "division", st.division);
    PutInt(s, "divisionCounter", st.divisionCounter);
    PutFloat(s, "gate", st.gate);
    std::vector<float> steps;
    for(const auto& k : st.steps)
        steps.insert(steps.end(), {float(k.type_), k.transpose_nn_, float(k.key_), k.vel_, float(k.src_)});
    PutFloats(s, "sequence", steps.data(), int(steps.size()));
}

bool FromSession(const Session& s, SessionState& st)
{
    using namespace session;
    const auto knobs = GetFloats(s, "knobs");
    const auto pages = GetFloats(s, "pages");
    const auto steps = GetFloats(s, "sequence");
    if(knobs.size() != 18 || pages.size() != 6 || steps.size() % kStepFields != 0
       || steps.size() > size_t(Sequencer::kMaxSeqLen * kStepFields))
        return false;

    st.slot = uint8_t(GetInt(s, "slot", 15));
    for(int i = 0; i < 18; i++)
        st.knobs[i / 6][i % 6] = knobs[size_t(i)];
    for(int i = 0; i < 6; i++)
        st.pages[i] = uint8_t(pages[size_t(i)]);
    st.pitchLfoRate    = GetFloat(s, "pitchLfoRate", .58f);
    st.filterLfoRate   = GetFloat(s, "filterLfoRate", .58f);
    st.delayTime       = GetFloat(s, "delayTime", .4f);
    st.resonance       = GetFloat(s, "resonance", .63f);
    st.compression     = GetFloat(s, "compression");
    st.table           = GetInt(s, "wavetable");
    st.frame           = GetInt(s, "frame");
    st.octave          = GetInt(s, "octave");
    st.pitchLfo        = GetBool(s, "pitchLfo", true);
    st.filterLfo       = GetBool(s, "filterLfo", true);
    st.tempo           = GetInt(s, "tempo", 320);
    st.division        = uint8_t(GetInt(s, "division", 2));
    st.divisionCounter = GetInt(s, "divisionCounter", 30);
    st.gate            = GetFloat(s, "gate", .5f);
    st.steps.clear();
    for(size_t i = 0; i < steps.size(); i += kStepFields)
        st.steps.emplace_back(KeyRequest::Type(int(steps[i])), steps[i + 1], int(steps[i + 2]), steps[i + 3],
                              KeyRequest::Source(int(steps[i + 4])));
    return true;
}

using EncValues = float[3][6];
using KnobPages = uint8_t[6];
using Steps     = KeyRequest[Sequencer::kMaxSeqLen];
EMU_EXPOSE(UiEncValues, chompi::UserInterface, EncValues, enc_values);
EMU_EXPOSE(UiKnobPage, chompi::UserInterface, KnobPages, knob_page);
EMU_EXPOSE(MenuResonance, chompi::MenuPage, float, res);
EMU_EXPOSE(MenuDelayTime, chompi::MenuPage, float, del_time);
EMU_EXPOSE(MenuPitchLfoRate, chompi::MenuPage, float, pitch_lfo_rate);
EMU_EXPOSE(MenuFilterLfoRate, chompi::MenuPage, float, filter_lfo_rate);
EMU_EXPOSE(MenuFinalComp, chompi::MenuPage, float, final_comp);
EMU_EXPOSE(MenuWavetableIdx, chompi::MenuPage, float, wavetable_idx);
EMU_EXPOSE(SeqSteps, Sequencer, Steps, mySequence);
EMU_EXPOSE(SeqLength, Sequencer, uint8_t, sequenceLength);
EMU_EXPOSE(ClockDivPos, clockManager, uint8_t, DivPos);
EMU_EXPOSE(ClockNewDivPos, clockManager, uint8_t, newDivPos);
EMU_EXPOSE(ClockDivCounter, clockManager, int, freeEncoderCounter);
EMU_EXPOSE(ClockDeferredDiv, clockManager, bool, deferredDivChange);

class WaveMachine final : public Machine
{
  public:
    explicit WaveMachine(std::shared_ptr<VirtualCard> card);
    ~WaveMachine() override;

    void Process(const float* const* in, float* const* out) override;
    bool CaptureSession(Session& out) override;
    void RestoreSession(const Session& session) override;
    bool IsReady() override;

  private:
    struct Firmware;

    void        Boot();
    void        InitMidiClockTimer();
    static void MidiClockCallback(void* data);
    void        AudioCallback(const float* const* in, float* const* out, size_t size);
    void        SDCallback();
    void        MainLoop();
    void        CheckSDCardMounted();
    void        DrainPanelEvents();
    void        ApplySession();

    std::unique_ptr<Firmware> fw_;
    uint64_t                  blocks_ = 0;

    SessionState session_;
    bool         session_pending_ = false;
};

/** Everything chompi_main.cpp keeps in globals. Allocated value-initialised,
 *  so it starts zeroed like .bss before constructors run. */
struct WaveMachine::Firmware
{
    chompi::Hardware      hw;
    chompi::UserInterface ui;

    myEngine               engine;
    chompi::PresetManager  presets;
    chompi::OptionsManager options;
    wavetableLoader        wtLoader;
    Sequencer              seq;
    clockManager           cManager;
    MidiManager            midi;

    bool testLoad; // To make sure wavetables don't get loaded until later

    daisysp::Reverb reverb;

    // SDRAM
    std::unique_ptr<chompi::InterpolatedDelayLine::AudioSample[]> del_mem;
    std::unique_ptr<float[][MAX_SAMPLES_PER_CYCLE]>                wavetableMemory;

    daisysp::Oscillator osc;

    uint32_t pret, sd_checkt;
    bool     booting;
    bool     rainbow_done;
    bool     testSDLoaded;
    bool     loading_screen;
    size_t   loading_screen_time;

    bool no_sd_card;

    uint32_t uit, now, pre_startt;
    uint32_t batt;

    daisy::TimerHandle midi_clock_timer;
    size_t             tim_base_freq;

    FATFS fs; // fsi.GetSDFileSystem(); the loader only stores it
};

WaveMachine::WaveMachine(std::shared_ptr<VirtualCard> card)
: Machine(emu::Firmware::Wave, std::move(card)), fw_(new Firmware())
{
    // ZeroSDRAM(): value-initialised allocations.
    fw_->del_mem.reset(new chompi::InterpolatedDelayLine::AudioSample[kMaxDelayTime]());
    fw_->wavetableMemory.reset(new float[MAX_CYCLES][MAX_SAMPLES_PER_CYCLE]());

    fw_->booting        = true;
    fw_->loading_screen = true;

    ScopedContext scope(*ctx_);
    Boot();
}

WaveMachine::~WaveMachine() = default;

/** Queues a MIDI clock pulse. CHOMPI runs on 12 PPQN, not 24. 24 created too much traffic
 * on the MIDI out bus. */
void WaveMachine::MidiClockCallback(void* data)
{
    auto& f = *static_cast<Firmware*>(data);
    if(f.seq.getPlaying() && f.options.midi_clock_out)
        f.midi.QueueMidiClock();
}

void WaveMachine::InitMidiClockTimer()
{
    auto& f = *fw_;

    daisy::TimerHandle::Config tim_cfg;
    tim_cfg.periph     = daisy::TimerHandle::Config::Peripheral::TIM_16;
    tim_cfg.dir        = daisy::TimerHandle::Config::CounterDir::UP;
    tim_cfg.enable_irq = true;

    f.midi_clock_timer.Init(tim_cfg);
    f.midi_clock_timer.SetPrescaler(239);
    f.midi_clock_timer.SetPeriod(15624);
    f.midi_clock_timer.SetCallback(MidiClockCallback, &f);
    f.midi_clock_timer.Start();
}

/** main() up to its while(1). EMULATOR: the audio and SD interrupts start
 *  part-way through main() on hardware; while booting they only scan controls
 *  and find no work, so here they start once Boot() returns. */
void WaveMachine::Boot()
{
    auto& f = *fw_;

    f.hw.Init();

    f.midi.Init(&f.cManager, &f.seq, &f.ui, &f.engine, &f.hw);

    f.hw.MpWrite(0x0c, 0B01010001); // set BATT_LOW to 3V, turn on

    f.hw.MpReadAll();

    for(size_t i = 0; i < 10; i++)
    {
        f.hw.LowBatteryLockoutCheck();
        System::Delay(10);
    }

    // SDMMC init and f_mount: the VirtualCard is already mounted. The delays
    // around them are kept for boot timing.
    System::Delay(100);
    System::Delay(100);
    System::Delay(100);

    f.wtLoader.Init(&f.fs, f.wavetableMemory.get(), float(kSampleRate));

    // delete the battery log if it exists
    char filename[32];
    snprintf(filename, sizeof(filename), ".batt_log.txt");
    f_unlink(filename);

    // macos makes a copy
    snprintf(filename, sizeof(filename), "._.batt_log.txt");
    f_unlink(filename);

    f.options.Init();
    f.midi.setMidiOptions(f.options.midi_ch_in, f.options.midi_ch_out, f.options.midi_cc_in);
    midi_in_channel_ = f.options.midi_ch_in;
    f.hw.setMidiCCOut(f.options.midi_cc_out);

    chompi::LedSetup();
    f.ui.Init(&f.cManager, &f.engine, &f.seq, &f.hw, &f.presets, f.options.midi_ch_out);

    f.tim_base_freq = System::GetPClk2Freq();
    InitMidiClockTimer();
    f.cManager.Init(&f.midi_clock_timer, f.tim_base_freq);
    f.seq.Init(&f.engine, &f.cManager, &f.hw);

    f.testLoad     = false;
    f.testSDLoaded = false;

    f.engine.Init(float(kSampleRate), f.del_mem.get(), &f.reverb, &f.wtLoader);

    f.osc.Init(float(kSampleRate));
    f.osc.SetAmp(.2f);

    f.now        = System::GetNow();
    f.uit        = f.now;
    f.pret       = f.now;
    f.pre_startt = f.now;
    f.batt       = f.now;

    // get any junk out of the SRs, takes .5s
    uint32_t vol_state   = 0;
    uint32_t sleep_state = 0;

    for(int i = 0; i < 5000; i++)
    {
        f.hw.ProcessAllControls();
        vol_state += f.hw.button_sr.State(int(chompi::Hardware::SwId::ENC_6_SW));
        sleep_state += f.hw.button_sr.State(int(chompi::Hardware::SwId::KEY_26))
                       && f.hw.button_sr.State(int(chompi::Hardware::SwId::KEY_27))
                       && f.hw.button_sr.State(int(chompi::Hardware::SwId::KEY_28));

        System::DelayUs(100);
    }

    // EMULATOR: shipping mode (sleep_state) has no meaning without a battery.
    if(vol_state > 4000)
        f.ui.TestMode();

    // USB power handoff (MP2722 DPDM): not emulated.
}

/** breakdown:
 *  Inputs:  1 Microphone, 2 X, 3 Aux L, 4 Aux R
 *  Outputs: 1 Headphone L, 2 Headphone R, 3 Master L, 4 Master R
 */
void WaveMachine::AudioCallback(const float* const* in, float* const* out, size_t size)
{
    auto& f = *fw_;

    if((f.booting || f.loading_screen) && !f.ui.InTestMode())
    {
        f.hw.ProcessAllControls();
        f.ui.GenerateEvents();
        f.ui.DoEvents();

        for(size_t i = 0; i < size; i++)
            out[0][i] = out[1][i] = out[2][i] = out[3][i] = 0.f;

        return;
    }

    f.midi.ProcessMidiIn();

    f.hw.ProcessAllControls();
    f.ui.GenerateEvents();
    if(f.seq.getPlaying())
        f.seq.checkAndPop();
    f.engine.Prepare();

    if(f.ui.InTestMode() && f.ui.GetToggleState())
    {
        for(size_t i = 0; i < size; i++)
            out[0][i] = out[1][i] = out[2][i] = out[3][i] = f.osc.Process();
    }
    else
        f.engine.Process(in, const_cast<float**>(out), size);
}

void WaveMachine::CheckSDCardMounted()
{
    auto&         f   = *fw_;
    const DSTATUS res = disk_status(0);
    // lost the SD card, must reboot
    if(res != RES_OK)
    {
        f.no_sd_card = true;
        f.ui.NoSDCard();

        const uint32_t start_time = System::GetNow();
        while(System::GetNow() - start_time < 3000)
        {
            f.ui.NoSDCardAnimation(true);
            f.ui.DoEvents();
            System::Delay(1);
        }

        f.ui.NoSDCardAnimation(false);
    }
}

void WaveMachine::SDCallback()
{
    auto&          f   = *fw_;
    const uint32_t now = System::GetNow();
    if(now - f.sd_checkt > 1000 && !f.no_sd_card && !f.booting)
    {
        f.sd_checkt = now;
        CheckSDCardMounted();
    }
    else if(f.no_sd_card)
        return;

    f.engine.ProcessFileRequests();

    if(now - f.pret > 50)
    {
        f.pret = now;
        f.ui.WritePresets();
    }
}

/** One pass of MainLoop(). EMULATOR: the trailing DelayUs(10) only paced the
 *  busy loop, so MIDI out drains one message per block (0.5 ms) rather than
 *  per 10 us; the DIN port's 31250 baud allows about one per millisecond. */
void WaveMachine::MainLoop()
{
    auto& f = *fw_;

    if(f.booting)
    {
        f.hw.LowBatteryLockoutCheck();
        f.booting = false;
    }
    else if(!f.rainbow_done && !f.loading_screen)
    {
        f.ui.StopBootAnimation();
        f.ui.RainbowWave();
        f.rainbow_done = true;
        booted_.store(true, std::memory_order_relaxed);
    }

    f.now = System::GetNow();

    f.midi.ProcessMidiOut(); // We want midi checks to happen every 10uS for more precise timing

    if(f.now - f.uit > 1)
    {
        f.ui.DoEvents();
        f.uit = f.now;
    }

    if(f.now - f.pre_startt > 1000)
    {
        if(!f.testLoad)
        {
            f.wtLoader.loadAllToMemory();
            f.testLoad = true;
        }
    }

    // release the loading screen 250 ms after
    // the wavetable requests drain; if they never drain (no card), it never releases
    if(!f.testSDLoaded)
    {
        //numWavetables guard: with no card the loader finds zero tables
        //and queues nothing, so an empty request queue must not count as "loaded"
        if(f.testLoad && f.wtLoader.numWavetables > 0 && f.engine.checkLoaded())
        {
            f.testSDLoaded = true;
            f.ui.BootSelectDefaultSlot();
            f.loading_screen_time = System::GetNow();
        }
    }
    if(f.loading_screen_time)
    {
        if(System::GetNow() - f.loading_screen_time > 250)
        {
            f.loading_screen      = false;
            f.loading_screen_time = 0;
        }
    }

    if(f.now - f.pre_startt > 5000)
    {
        f.ui.TestPresets();
        f.pre_startt = f.now;
    }

    // update now to actually be now
    f.now = System::GetNow();

    if(f.ui.InRainbows())
    {
        f.batt = f.now;
    }
    else if(f.now - f.batt > 20)
    {
        f.hw.LowBatteryLockoutCheck();
        f.batt = f.now;
    }

    if(f.ui.InTestMode())
    {
        f.hw.MpReadAll();

        while(!f.hw.read_ready)
            System::Delay(1);
        f.ui.TestPowerCable(f.hw.mp_buff_[1] >> 5 & 1); //VIN_RDY

        // Normal NTC_MISSING, BATT_MISSING, NTC1_FAULT, and NTC2_FAULT
        f.ui.TestBMC(f.hw.mp_buff_[3] == 0);
    }
}

void WaveMachine::DrainPanelEvents()
{
    auto&      hw = fw_->hw;
    PanelEvent e;
    while(PopPanelEvent(e))
    {
        switch(e.type)
        {
            case PanelEvent::Type::Button: hw.SetButton(e.id, e.value != 0); break;
            case PanelEvent::Type::Turn:
                if(e.id >= 0 && e.id < panel::kNumEncoders)
                    hw.enc[e.id].AddDetents(e.value);
                break;
            case PanelEvent::Type::Toggle: hw.SetToggle(e.value != 0); break;
        }
    }
}

bool WaveMachine::CaptureSession(Session& out)
{
    // Not yet applied (the host hasn't run blocks since it was restored): it is
    // still the state the user last saw.
    if(session_pending_)
    {
        ToSession(session_, out);
        return true;
    }

    auto& f = *fw_;
    if(!f.rainbow_done)
        return false;

    ScopedContext scope(*ctx_);
    auto&         menu  = f.ui.menu_page_;
    auto&         clock = f.cManager;
    SessionState  s;

    s.slot = uint8_t(f.engine.getVoiceSlot());
    std::memcpy(s.knobs, f.ui.*Get(UiEncValues{}), sizeof(s.knobs));
    std::memcpy(s.pages, f.ui.*Get(UiKnobPage{}), sizeof(s.pages));
    s.pitchLfoRate    = menu.*Get(MenuPitchLfoRate{});
    s.filterLfoRate   = menu.*Get(MenuFilterLfoRate{});
    s.delayTime       = menu.*Get(MenuDelayTime{});
    s.resonance       = menu.*Get(MenuResonance{});
    s.compression     = menu.*Get(MenuFinalComp{});
    s.table           = f.engine.getTable();
    s.frame           = f.engine.getCycle();
    s.octave          = f.engine.getOctave();
    s.pitchLfo        = f.engine.getPitchLfoOn();
    s.filterLfo       = f.engine.getFilterLfoOn();
    s.tempo           = clock.getTempo();
    s.division        = clock.*Get(ClockNewDivPos{});
    s.divisionCounter = clock.*Get(ClockDivCounter{});
    s.gate            = f.seq.getGate();
    const auto& steps = f.seq.*Get(SeqSteps{});
    s.steps.assign(steps, steps + std::min<int>(f.seq.*Get(SeqLength{}), Sequencer::kMaxSeqLen));
    ToSession(s, out);
    return true;
}

void WaveMachine::RestoreSession(const Session& session)
{
    SessionState state;
    if(!FromSession(session, state))
        return;
    session_         = std::move(state);
    session_pending_ = true;
}

/** Re-creates the session through the calls a preset load makes
 *  (MenuPage::SetVoiceSlot), plus volume and pan, which NormalPage::Draw only
 *  applies for the page on show. */
void WaveMachine::ApplySession()
{
    auto&       f      = *fw_;
    auto&       engine = f.engine;
    auto&       menu   = f.ui.menu_page_;
    auto&       clock  = f.cManager;
    auto&       enc    = f.ui.*Get(UiEncValues{});
    auto&       pages  = f.ui.*Get(UiKnobPage{});
    const auto& s      = session_;
    using daisysp::fclamp;

    engine.setVoiceSlot(s.slot >= 1 && s.slot <= 15 ? s.slot : 15);
    for(int page = 0; page < 3; page++)
        for(int knob = 0; knob < 6; knob++)
            enc[page][knob] = fclamp(s.knobs[page][knob], 0.f, 1.f);
    for(int knob = 0; knob < 6; knob++)
        pages[knob] = s.pages[knob] < chompi::knob_num_pages[knob] ? s.pages[knob] : 0;

    auto& pitch_lfo_rate  = menu.*Get(MenuPitchLfoRate{});
    auto& filter_lfo_rate = menu.*Get(MenuFilterLfoRate{});
    auto& del_time        = menu.*Get(MenuDelayTime{});
    auto& res             = menu.*Get(MenuResonance{});
    auto& final_comp      = menu.*Get(MenuFinalComp{});
    pitch_lfo_rate        = fclamp(s.pitchLfoRate, 0.f, 1.f);
    filter_lfo_rate       = fclamp(s.filterLfoRate, 0.f, 1.f);
    del_time              = fclamp(s.delayTime, 0.f, 1.f);
    res                   = fclamp(s.resonance, 0.f, 1.f);
    final_comp            = fclamp(s.compression, 0.f, 1.f);

    engine.setCycle(int8_t(std::clamp(s.frame, 0, CYCLES - 1)), true);
    engine.nextTable(int8_t(s.table), true); // falls back to the first table if gone
    menu.*Get(MenuWavetableIdx{}) = engine.getTable() * chompi::kEncoderWtStep;
    engine.setOctave(std::clamp(s.octave, -1, 1) - engine.getOctave());
    engine.setPitchLfoOn(s.pitchLfo);
    engine.setFilterLfoOn(s.filterLfo);

    engine.setGlobalPitch(enc[0][0]);
    engine.setAttack(enc[0][1]);
    engine.setPitchLfoDepth(enc[1][1]);
    engine.setLfoDepth(enc[1][2]);
    engine.setPitchLfoRate(pitch_lfo_rate);
    engine.setLfoRate(filter_lfo_rate);
    engine.setRelease(enc[0][2]);
    engine.setMasterCutoff(enc[1][3]);
    engine.setDelayFeedback(enc[0][3]);
    engine.setMasterResonance(res);
    engine.setDelayTime(del_time);
    engine.setGain(enc[0][5]);
    engine.setPan(enc[1][5]);
    engine.setFinalComp(final_comp);

    // Division first: changeTempo() computes the step interval from it.
    clock.*Get(ClockDivPos{})      = std::min<uint8_t>(s.division, 4);
    clock.*Get(ClockNewDivPos{})   = std::min<uint8_t>(s.division, 4);
    clock.*Get(ClockDivCounter{})  = std::clamp(s.divisionCounter, 0, 60);
    clock.*Get(ClockDeferredDiv{}) = false;
    clock.changeTempo(std::clamp(s.tempo, 160, 480));

    auto& steps = f.seq.*Get(SeqSteps{});
    for(size_t i = 0; i < size_t(Sequencer::kMaxSeqLen); i++)
        steps[i] = i < s.steps.size() ? s.steps[i] : KeyRequest();
    f.seq.*Get(SeqLength{}) = uint8_t(std::min<size_t>(s.steps.size(), Sequencer::kMaxSeqLen));
    f.seq.setGate(fclamp(s.gate, 0.f, 1.f));
}

bool WaveMachine::IsReady()
{
    return fw_->rainbow_done && !fw_->loading_screen && !fw_->ui.InRainbows();
}

void WaveMachine::Process(const float* const* in, float* const* out)
{
    ScopedContext scope(*ctx_);

    if(session_pending_ && fw_->rainbow_done)
    {
        session_pending_ = false;
        ApplySession();
    }

    DrainPanelEvents();

    AudioCallback(in, out, kBlockSize);
    ctx_->clock_us += uint64_t(kBlockSize) * 1000000 / kSampleRate;
    fw_->midi_clock_timer.Advance(); // TIM16, MIDI clock out

    if(++blocks_ % 2 == 0) // TIM4 at 1 kHz
        SDCallback();

    MainLoop();
}

} // namespace
} // namespace emu

extern "C" emu::Machine* chompi_create_wave(const std::shared_ptr<emu::VirtualCard>* card)
{
    return new emu::WaveMachine(*card);
}
