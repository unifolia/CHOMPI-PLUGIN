// Port of TAPE 2.0's chompi_main.cpp: globals become Firmware members,
// interrupt handlers methods that Process() schedules.
#include "chompi/Machine.h"

#include <algorithm>

#include "emu_context.h"
#include "emu_controls.h"
#include "emu_expose.h"
#include "emu_session.h"

#include "hardware.h"
#include "temp_led_stuff.h"
#include "ui.h"
#include "daisysp.h"
#include "fatfs.h"
#include "diskio.h"
#include "DSPEngine.h"
#include "FileCopier.h"
#include "RamBuffer.h"
#include "InterpolatedDelayLine.h"
#include "OptionsManager.h"

namespace emu
{
using daisy::System;

namespace
{
/** What a DAW session restores. Each slot's sample settings also live in
 *  presets.json on the card, but the firmware writes that file every 5 s at
 *  most, so the selected slot's settings are captured here too. */
struct SessionState
{
    uint8_t mode      = 0;      // 0 Jammi, 1 Cubbi
    uint8_t bank[2]   = {0, 0}; // bank shown in each mode
    uint8_t jammiBank = 0;      // bank of the selected Jammi slot
    uint8_t jammiSlot = 15;     // 1-14; 15 is the CHOMPI buffer
    float   knobs[3][6]{};      // [page][knob], the firmware's enc_values
    uint8_t pages[6]{};
    float   pan      = .5f; // selected slot's, with auto-loop and sustain
    bool    autoLoop = true;
    bool    sustain  = true;
    float   delayTime = .5f, warble = 0.f, resonance = 0.f, compression = 0.f;
    uint8_t monitor     = 0;
    uint8_t input       = 1;
    bool    fxPreLooper = true;
};

void ToSession(const SessionState& st, Session& s)
{
    using namespace session;
    s.clear();
    PutInt(s, "mode", st.mode);
    PutInt(s, "jammiBankShown", st.bank[0]);
    PutInt(s, "cubbiBankShown", st.bank[1]);
    PutInt(s, "jammiBank", st.jammiBank);
    PutInt(s, "jammiSlot", st.jammiSlot);
    PutFloats(s, "knobs", &st.knobs[0][0], 18, 4);
    const float pages[6] = {float(st.pages[0]), float(st.pages[1]), float(st.pages[2]),
                            float(st.pages[3]), float(st.pages[4]), float(st.pages[5])};
    PutFloats(s, "pages", pages, 6, 0);
    PutFloat(s, "pan", st.pan);
    PutBool(s, "autoLoop", st.autoLoop);
    PutBool(s, "sustain", st.sustain);
    PutFloat(s, "delayTime", st.delayTime);
    PutFloat(s, "warble", st.warble);
    PutFloat(s, "resonance", st.resonance);
    PutFloat(s, "compression", st.compression);
    PutInt(s, "monitor", st.monitor);
    PutInt(s, "input", st.input);
    PutBool(s, "fxPreLooper", st.fxPreLooper);
}

bool FromSession(const Session& s, SessionState& st)
{
    using namespace session;
    const auto knobs = GetFloats(s, "knobs");
    const auto pages = GetFloats(s, "pages");
    if(knobs.size() != 18 || pages.size() != 6)
        return false;

    st.mode      = uint8_t(GetInt(s, "mode"));
    st.bank[0]   = uint8_t(GetInt(s, "jammiBankShown"));
    st.bank[1]   = uint8_t(GetInt(s, "cubbiBankShown"));
    st.jammiBank = uint8_t(GetInt(s, "jammiBank"));
    st.jammiSlot = uint8_t(GetInt(s, "jammiSlot", 15));
    for(int i = 0; i < 18; i++)
        st.knobs[i / 6][i % 6] = knobs[size_t(i)];
    for(int i = 0; i < 6; i++)
        st.pages[i] = uint8_t(pages[size_t(i)]);
    st.pan         = GetFloat(s, "pan", .5f);
    st.autoLoop    = GetBool(s, "autoLoop", true);
    st.sustain     = GetBool(s, "sustain", true);
    st.delayTime   = GetFloat(s, "delayTime", .5f);
    st.warble      = GetFloat(s, "warble");
    st.resonance   = GetFloat(s, "resonance");
    st.compression = GetFloat(s, "compression");
    st.monitor     = uint8_t(GetInt(s, "monitor"));
    st.input       = uint8_t(GetInt(s, "input", 1));
    st.fxPreLooper = GetBool(s, "fxPreLooper", true);
    return true;
}

using EncValues = float[3][6];
using KnobPages = uint8_t[6];
using Banks     = int[int(daisy::VoiceMode::LAST)];
EMU_EXPOSE(UiEncValues, chompi::UserInterface, EncValues, enc_values);
EMU_EXPOSE(UiKnobPage, chompi::UserInterface, KnobPages, knob_page);
EMU_EXPOSE(MenuDelayTime, chompi::MenuPage, float, delay_time);
EMU_EXPOSE(MenuResonance, chompi::MenuPage, float, resonance);
EMU_EXPOSE(MenuWarble, chompi::MenuPage, float, warble);
EMU_EXPOSE(MenuFinalComp, chompi::MenuPage, float, final_comp);
EMU_EXPOSE(EngineBank, daisy::Engine, Banks, bank);
EMU_EXPOSE(EngineVoiceBank, daisy::Engine, size_t, voice_bank_);
EMU_EXPOSE(EngineVoiceSlot, daisy::Engine, size_t, voice_slot_);
EMU_EXPOSE(EngineLooper, daisy::Engine, daisy::LooperEngine, looper);
#undef EMU_EXPOSE

using namespace controls;

// NormalPage/MenuPage: kEncoderFineStep .003, kEncoderCoarseStep .01.
constexpr Knob kTapeKnobs[kNumTape] = {
    {false, 0, 0, 1, .003f}, // speed (see TurnControl)
    {false, 0, 1, 1, .01f},  // sample gain
    {false, 1, 0, 3, .009f}, // start
    {false, 1, 1, 3, .03f},  // attack
    {false, 2, 0, 3, .009f}, // end
    {false, 2, 1, 3, .03f},  // release
    {true, 0, 1, 1, .01f},   // pan
    {false, 3, 0, 3, .03f},  // reverb/delay
    {false, 3, 1, 3, .03f},  // lo-fi
    {false, 3, 2, 3, .03f},  // filter
    {true, 3, 0, 3, .03f},   // delay time
    {true, 3, 1, 3, .03f},   // warble
    {true, 3, 2, 3, .03f},   // resonance
    {false, 4, 0, 1, .003f}, // looper speed (see TurnControl)
    {false, 5, 0, 3, .03f},  // volume
    {false, 5, 1, 3, .03f},  // input gain
    {true, 5, 0, 3, .03f},   // compression (any page)
};

class TapeMachine final : public Machine
{
  public:
    explicit TapeMachine(std::shared_ptr<VirtualCard> card);
    ~TapeMachine() override;

    void Process(const float* const* in, float* const* out) override;
    bool CaptureSession(Session& out) override;
    void RestoreSession(const Session& session) override;
    void RecordingSizes(std::map<std::string, size_t>& out) override;
    void CaptureRecordings(Recordings& out) override;
    void RestoreRecordings(Recordings recordings) override;
    bool PlanImport(int button, ImportTarget& out) override;
    void CompleteImports(std::vector<ImportTarget> imports) override;
    bool IsReady() override;

  protected:
    float ControlValue(int index) override;
    bool  TurnControl(int index, float target, int maxClicks) override;

  private:
    struct Firmware;

    void Boot();
    void AudioCallback(const float* const* in, float* const* out, size_t size);
    void SDCallback();
    void MainLoop();
    void CheckSDCardMounted();
    void DrainPanelEvents();
    void ApplySession();
    void ApplyRecordings();
    void ApplyImports();
    void ApplyKnobPage(int knob, int page);

    std::unique_ptr<Firmware> fw_;
    uint64_t                  blocks_ = 0;

    SessionState session_;
    bool         session_pending_ = false;

    std::vector<int16_t> default_buffer_; // the CHIMPO buffer at power-on: FillDefaultSample()'s tone
    Recordings           recordings_;
    bool                 recordings_pending_ = false;

    std::vector<ImportTarget> imports_; // pending until the first block after boot
};

/** Everything chompi_main.cpp keeps in globals. Allocated value-initialised,
 *  so it starts zeroed like .bss before constructors run, which the firmware
 *  relies on (e.g. Engine fields read before Init). */
struct TapeMachine::Firmware
{
    chompi::Hardware       hw;
    chompi::UserInterface  ui;
    daisy::Engine          engine;
    chompi::PresetManager  presets;
    chompi::OptionsManager options;

    daisysp::Reverb reverb;

    // SDRAM
    std::unique_ptr<chompi::InterpolatedDelayLine::AudioSample[]> del_mem;
    daisy::RamBufferMemory                                        loop_buff;
    std::unique_ptr<int16_t[]>                                    loop_mem;
    daisy::RamBufferMemory                                        chompi_buff;
    std::unique_ptr<int16_t[]>                                    chompi_mem;

    daisysp::Oscillator osc;

    uint32_t pret, sd_checkt;
    bool     booting;
    bool     rainbow_done;

    daisy::FileCopier copier;

    bool line_in_state;
    bool no_sd_card;

    uint32_t uit, now, pre_startt;

    uint8_t preset;
    uint8_t bank;
    uint8_t mode;
};

TapeMachine::TapeMachine(std::shared_ptr<VirtualCard> card)
: Machine(emu::Firmware::Tape, std::move(card)), fw_(new Firmware())
{
    // ZeroSDRAM(): value-initialised allocations.
    fw_->del_mem.reset(new chompi::InterpolatedDelayLine::AudioSample[kMaxDelayTime]());
    fw_->loop_mem.reset(new int16_t[daisy::kMaxRamBuffSize]());
    fw_->chompi_mem.reset(new int16_t[daisy::kMaxRamBuffSize]());

    fw_->booting = true;

    ScopedContext scope(*ctx_);
    Boot();
    default_buffer_.assign(fw_->chompi_mem.get(), fw_->chompi_mem.get() + fw_->chompi_buff.length);
}

TapeMachine::~TapeMachine() = default;

/** main() up to its while(1). EMULATOR: the audio and SD interrupts start
 *  part-way through main() on hardware; while booting they only scan controls
 *  and find no work, so here they start once Boot() returns. */
void TapeMachine::Boot()
{
    auto& f = *fw_;

    f.hw.Init();

    f.hw.MpWrite(0x0c, 0B01010001); // set BATT_LOW to 3V, turn on
    f.hw.MpReadAll();

    for(size_t i = 0; i < 10; i++)
    {
        f.hw.LowBatteryLockoutCheck();
        System::Delay(10);
    }

    // SDMMC init and f_mount: the VirtualCard is already mounted.

    // delete the battery log if it exists
    char filename[32];
    snprintf(filename, sizeof(filename), ".batt_log.txt");
    f_unlink(filename);

    // macos makes a copy
    snprintf(filename, sizeof(filename), "._.batt_log.txt");
    f_unlink(filename);

    f.options.Init();
    midi_in_channel_ = f.options.midi_ch_in;

    chompi::LedSetup();
    f.ui.Init(&f.hw,
              &f.engine,
              &f.copier,
              &f.presets,
              f.options.midi_ch_in,
              f.options.midi_ch_out,
              f.options.pitch_shift_quantization,
              f.options.delay_split);

    f.loop_buff.Init(f.loop_mem.get());
    f.chompi_buff.Init(f.chompi_mem.get());
    f.engine.Init(float(kSampleRate),
                  &f.reverb,
                  f.del_mem.get(),
                  &f.loop_buff,
                  &f.chompi_buff,
                  f.options.record_latch,
                  f.options.tape_slew_on,
                  daisy::MonitorMode(f.options.monitor_position));

    f.osc.Init(float(kSampleRate));
    f.osc.SetAmp(.2f);

    f.now        = System::GetNow();
    f.uit        = f.now;
    f.pret       = f.now;
    f.pre_startt = f.now;

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

    f.copier.Init(float(kSampleRate), &f.engine, &f.chompi_buff, &f.loop_buff);
    f.engine.FillDefaultSample(float(kSampleRate));
}

/** breakdown:
 *  Inputs:  1 Microphone, 2 X, 3 Aux L, 4 Aux R
 *  Outputs: 1 Headphone L, 2 Headphone R, 3 Master L, 4 Master R
 */
void TapeMachine::AudioCallback(const float* const* in, float* const* out, size_t size)
{
    auto& f = *fw_;

    if(f.booting && !f.ui.InTestMode())
    {
        f.hw.ProcessAllControls();
        f.ui.GenerateEvents();
        f.ui.DoEvents();

        for(size_t i = 0; i < size; i++)
            out[0][i] = out[1][i] = out[2][i] = out[3][i] = 0.f;

        return;
    }

    f.hw.ProcessAllControls();
    f.ui.GenerateEvents();
    f.engine.Prepare();

    if(f.hw.jack_detect.Read() != f.line_in_state)
    {
        if(f.hw.jack_detect.Read())
            f.engine.SetInputSource(daisy::InputSource::LINE_IN);
        else
            f.engine.SetInputSource(daisy::InputSource::MIC);
    }
    f.line_in_state = f.hw.jack_detect.Read();

    if(f.ui.InTestMode() && f.ui.GetToggleState())
    {
        for(size_t i = 0; i < size; i++)
            out[0][i] = out[1][i] = out[2][i] = out[3][i] = f.osc.Process();
    }
    else
        f.engine.Process(in, const_cast<float**>(out), size);
}

void TapeMachine::CheckSDCardMounted()
{
    auto&         f   = *fw_;
    const DSTATUS res = disk_status(0);

    // lost the SD card, must reboot
    if(res != RES_OK)
    {
        f.no_sd_card = true;
        f.ui.NoSDCard();

        f.engine.SetVoiceMode(daisy::VoiceMode::JAMMI);
        f.engine.SetVoiceSlot(15, true);

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

void TapeMachine::SDCallback()
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

    if(f.copier.CopyProcess())
    {
        f.ui.DoEvents();
        return;
    }

    f.engine.ProcessFileRequests();

    if(now - f.pret > 50 && !f.engine.AnyVoicesPlaying())
    {
        f.pret = now;
        f.ui.WritePresets();
    }
}

/** One pass of MainLoop(). EMULATOR: the trailing DelayUs(10) only paced the
 *  busy loop, and the battery/USB-power section has no hardware to talk to. */
void TapeMachine::MainLoop()
{
    auto& f = *fw_;

    if(f.booting)
    {
        f.hw.LowBatteryLockoutCheck();

        if(!f.copier.IsCopying())
        {
            if(f.copier.NeedsOverwrite(f.preset, daisy::VoiceMode(f.mode), f.bank))
            {
                daisy::FileCopier::CopyRequest req(f.preset + 1,
                                                   f.bank,
                                                   daisy::VoiceMode(f.mode),
                                                   f.preset + 1,
                                                   f.bank,
                                                   daisy::VoiceMode(f.mode),
                                                   false,
                                                   daisy::FileCopier::CopyRequest::RamDir::NONE,
                                                   daisy::FileCopier::CopyRequest::RamDir::NONE);

                f.copier.req_fifo.PushBack(req);

                System::Delay(5); // fixes data race
            }

            f.preset++;
            if(f.preset >= 14)
            {
                f.preset = 0;
                f.bank++;

                if(f.bank >= 5)
                {
                    f.bank = 0;
                    f.mode++;

                    if(f.mode >= 2)
                    {
                        f.engine.UpdateFileExists();
                        f.booting = false;
                    }
                }
            }
        }

        return;
    }
    else if(!f.rainbow_done)
    {
        f.ui.StopBootAnimation();
        f.ui.RainbowWave();
        f.rainbow_done = true;
        booted_.store(true, std::memory_order_relaxed);
    }

    f.now = System::GetNow();

    if(f.now - f.uit > 1)
    {
        f.ui.DoEvents();
        f.ui.ProcessMidi();
        f.uit = f.now;
    }

    if(f.now - f.pre_startt > 5000)
    {
        f.ui.TestPresets();
        f.pre_startt = f.now;
    }

    if(f.ui.InTestMode())
    {
        f.hw.MpReadAll();
        f.ui.TestPowerCable(f.hw.mp_buff_[1] >> 5 & 1); // VIN_RDY
        f.ui.TestBMC(f.hw.mp_buff_[3] == 0);
    }
}

void TapeMachine::DrainPanelEvents()
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

bool TapeMachine::CaptureSession(Session& out)
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

    SessionState  s;
    ScopedContext scope(*ctx_);
    auto&         engine = f.engine;
    auto&         ui     = f.ui;
    auto&         menu   = ui.menu_page_;

    s.mode      = uint8_t(engine.GetVoiceMode());
    s.bank[0]   = uint8_t((engine.*Get(EngineBank{}))[0]);
    s.bank[1]   = uint8_t((engine.*Get(EngineBank{}))[1]);
    s.jammiBank = uint8_t(engine.*Get(EngineVoiceBank{}));
    s.jammiSlot = uint8_t(engine.*Get(EngineVoiceSlot{}));
    std::memcpy(s.knobs, ui.*Get(UiEncValues{}), sizeof(s.knobs));
    std::memcpy(s.pages, ui.*Get(UiKnobPage{}), sizeof(s.pages));
    s.pan         = engine.GetPan();
    s.autoLoop    = engine.GetAutoLoop();
    s.sustain     = engine.GetSustainActive();
    s.delayTime   = menu.*Get(MenuDelayTime{});
    s.warble      = menu.*Get(MenuWarble{});
    s.resonance   = menu.*Get(MenuResonance{});
    s.compression = menu.*Get(MenuFinalComp{});
    s.monitor     = uint8_t(engine.GetMonitorMode());
    s.input       = uint8_t(engine.GetInputSource());
    s.fxPreLooper = engine.GetFxPreLooper();
    ToSession(s, out);
    return true;
}

void TapeMachine::RestoreSession(const Session& session)
{
    SessionState state;
    if(!FromSession(session, state))
        return;
    session_         = state;
    session_pending_ = true;
}

/** Re-creates the session through the same calls the panel pages make. */
void TapeMachine::ApplySession()
{
    auto&       f      = *fw_;
    auto&       engine = f.engine;
    auto&       menu   = f.ui.menu_page_;
    auto&       enc    = f.ui.*Get(UiEncValues{});
    auto&       pages  = f.ui.*Get(UiKnobPage{});
    const auto& s      = session_;
    using daisy::VoiceMode;

    // Slot first: selecting it loads the slot's presets into the knob values
    // and the voices (as the shift layer's slot keys do). A slot whose sample
    // is gone falls back to the CHOMPI buffer rather than creating an empty
    // file.
    engine.SetVoiceMode(VoiceMode::JAMMI);
    const uint8_t jammi_bank = s.jammiBank % 5;
    engine.SetBank(jammi_bank);
    size_t slot = s.jammiSlot >= 1 && s.jammiSlot <= 15 ? s.jammiSlot : 15;
    if(!engine.GetFileExists(slot - 1))
        slot = 15;

    // The captured knobs belong to the Jammi slot only if Jammi was showing (in
    // Cubbi they are the last Cubbi slot's, reloaded when its key plays). Hand
    // them to the slot's presets, in PresetManager's control order, so
    // SetVoiceSlot applies them exactly as the firmware would.
    if(s.mode == uint8_t(VoiceMode::JAMMI))
    {
        const float controls[chompi::PresetManager::kMaxControls] = {
            s.knobs[0][0], s.knobs[0][1], s.knobs[0][2], s.knobs[1][1], s.knobs[1][2],
            float(s.autoLoop), float(s.sustain), s.knobs[1][0], s.pan};
        for(int c = 0; c < chompi::PresetManager::kMaxControls; c++)
            f.presets.SetValue(daisysp::fclamp(controls[c], 0.f, 1.f), size_t(VoiceMode::JAMMI), jammi_bank, slot, size_t(c));
    }
    menu.SetVoiceSlot(slot);
    engine.SetBank(s.bank[0] % 5);
    if(s.mode == uint8_t(VoiceMode::CUBBI))
    {
        engine.SetVoiceMode(VoiceMode::CUBBI);
        engine.SetBank(s.bank[1] % 5);
        engine.StopAllVoices();
    }

    // Knobs that aren't per-slot: magic (3 pages), volume and input gain.
    for(int page = 0; page < 3; page++)
        enc[page][3] = daisysp::fclamp(s.knobs[page][3], 0.f, 1.f);
    enc[0][5] = daisysp::fclamp(s.knobs[0][5], 0.f, 1.f);
    enc[1][5] = daisysp::fclamp(s.knobs[1][5], 0.f, 1.f);
    for(int knob = 0; knob < 6; knob++)
        pages[knob] = s.pages[knob] < chompi::knob_num_pages[knob] ? s.pages[knob] : 0;

    // NormalPage::Draw re-applies only the page on show; apply every page once.
    for(int page = 0; page < 3; page++)
        ApplyKnobPage(3, page);
    ApplyKnobPage(5, 0);
    ApplyKnobPage(5, 1);

    menu.*Get(MenuDelayTime{}) = daisysp::fclamp(s.delayTime, 0.f, 1.f);
    menu.*Get(MenuWarble{})    = daisysp::fclamp(s.warble, 0.f, 1.f);
    menu.*Get(MenuResonance{}) = daisysp::fclamp(s.resonance, 0.f, 1.f);
    menu.*Get(MenuFinalComp{}) = daisysp::fclamp(s.compression, 0.f, 1.f);
    engine.SetDelayTime(menu.*Get(MenuDelayTime{}));
    engine.SetWarble(menu.*Get(MenuWarble{}));
    engine.SetFilterResonance(menu.*Get(MenuResonance{}));
    engine.SetFinalComp(menu.*Get(MenuFinalComp{}));

    for(int i = 0; i < int(daisy::MonitorMode::LAST) && uint8_t(engine.GetMonitorMode()) != s.monitor; i++)
        engine.IncrementMonitorMode();
    if(s.input < uint8_t(daisy::InputSource::LAST))
        engine.SetInputSource(daisy::InputSource(s.input));
    engine.SetFxPreLooper(s.fxPreLooper);
}

/** NormalPage::Draw's setters, which apply a knob's value while its page
 *  shows (and NormalPage is on top), as Draw would with `page` on show (but
 *  for the volume knob's while the battery level shows). */
void TapeMachine::ApplyKnobPage(int knob, int page)
{
    auto&       f      = *fw_;
    auto&       engine = f.engine;
    const float value  = (f.ui.*Get(UiEncValues{}))[page][knob];
    if(knob == 0 && page == 1)
    {
        if(f.ui.GetToggleState()) // Draw skips it in record mode
            engine.SetGain(value);
    }
    else if(knob == 1 && page == 1)
        engine.SetAttack(value);
    else if(knob == 2 && page == 1)
        engine.SetDecay(value);
    else if(knob == 3 && page == 0)
    {
        if(f.options.delay_split)
        {
            engine.SetReverb(value < .5f ? 0.f : (value - .5f) * 2.f);
            engine.SetDelayFeedback(value < .5f ? (.5f - value) * 2.f : 0.f);
        }
        else
        {
            engine.SetReverb(value);
            engine.SetDelayFeedback(value);
        }
    }
    else if(knob == 3 && page == 1)
        engine.SetSaturate(value);
    else if(knob == 3 && page == 2)
        engine.SetFilter(value);
    else if(knob == 5 && page == 0)
        engine.SetMainGain(value);
    else if(knob == 5 && page == 1)
        engine.SetInputGain(value);
}

float TapeMachine::ControlValue(int index)
{
    auto& f    = *fw_;
    auto& menu = f.ui.menu_page_;
    switch(index)
    {
        case kTapePan: return f.engine.GetPan();
        case kTapeDelayTime: return menu.*Get(MenuDelayTime{});
        case kTapeWarble: return menu.*Get(MenuWarble{});
        case kTapeResonance: return menu.*Get(MenuResonance{});
        case kTapeCompression: return menu.*Get(MenuFinalComp{});
        default:
        {
            const auto& k = kTapeKnobs[index];
            return (f.ui.*Get(UiEncValues{}))[k.page][k.knob];
        }
    }
}

/** Clicks as the knob would send them with its page on show (or the shift
 *  layer held), one handler call per click, so the firmware's own steps,
 *  limits, per-slot presets and MIDI CC out apply. */
bool TapeMachine::TurnControl(int index, float target, int maxClicks)
{
    auto& f    = *fw_;
    auto  knob = kTapeKnobs[index];
    // Speed and looper speed step in fifths and octaves on one layer
    // (options.json "Pitch Quantize In Shift Menu" picks which) and move
    // freely on the other: clicks go to the free one.
    if(index == kTapeSpeed || index == kTapeLooperSpeed)
        knob.shift = !f.options.pitch_shift_quantization;
    // Unshifted, the looper knob scrubs while the looper is paused.
    if(index == kTapeLooperSpeed && !knob.shift && !f.engine.IsLooperPlaying())
        return false;

    const bool done = ClickToward(
        knob, f.ui.*Get(UiKnobPage{}), target, maxClicks, [&] { return ControlValue(index); },
        [&](int16_t turns) {
            return knob.shift ? f.ui.menu_page_.OnEncoderTurned(knob.knob, turns, 0)
                              : f.ui.normal_page_.OnEncoderTurned(knob.knob, turns, 0);
        });
    if(!knob.shift)
        ApplyKnobPage(knob.knob, knob.page);
    return done;
}

void TapeMachine::RecordingSizes(std::map<std::string, size_t>& out)
{
    out.clear();
    if(recordings_pending_)
    {
        for(const auto& [name, samples] : recordings_)
            out[name] = samples.size();
        return;
    }
    auto&       f      = *fw_;
    const auto& chompi = f.chompi_buff;
    if(chompi.length != default_buffer_.size() || !std::equal(default_buffer_.begin(), default_buffer_.end(), chompi.mem))
        out["buffer"] = chompi.length;
    if(!f.engine.GetLooperIsEmpty() && f.loop_buff.length > 0)
        out["looper"] = f.loop_buff.length;
}

void TapeMachine::CaptureRecordings(Recordings& out)
{
    std::map<std::string, size_t> sizes;
    RecordingSizes(sizes);
    for(auto it = out.begin(); it != out.end();)
        it = sizes.count(it->first) != 0 ? std::next(it) : out.erase(it);
    for(const auto& [name, size] : sizes)
    {
        const int16_t* samples = recordings_pending_ ? recordings_.at(name).data()
                                 : name == "buffer"  ? fw_->chompi_mem.get()
                                                     : fw_->loop_mem.get();
        out[name].assign(samples, samples + size);
    }
}

void TapeMachine::RestoreRecordings(Recordings recordings)
{
    recordings_         = std::move(recordings);
    recordings_pending_ = true;
}

/** As the firmware loads a buffer from a slot (MenuPage: StopAllVoices(), the
 *  copy, then LooperOpenFile() for the looper, which comes back paused at its
 *  start). No looper clears it as holding play + loop does. */
void TapeMachine::ApplyRecordings()
{
    auto& f      = *fw_;
    auto& engine = f.engine;
    auto& enc    = f.ui.*Get(UiEncValues{});
    auto  put    = [](daisy::RamBufferMemory& ram, const std::vector<int16_t>& samples) {
        const size_t n = std::min(samples.size(), daisy::kMaxRamBuffSize) & ~size_t(1);
        std::copy(samples.begin(), samples.begin() + long(n), ram.mem);
        ram.length = n;
    };
    const auto buffer = recordings_.find("buffer");
    const auto looper = recordings_.find("looper");

    engine.StopAllVoices();
    if(engine.Recording())
        engine.StopRecording();
    put(f.chompi_buff, buffer != recordings_.end() ? buffer->second : default_buffer_);

    if(looper != recordings_.end() && looper->second.size() >= 2)
    {
        put(f.loop_buff, looper->second);
        engine.LooperOpenFile();
        enc[0][4] = chompi::enc_defaults[0][4];
        engine.ResetLooperPitchQuant();
    }
    else if(!engine.GetLooperIsEmpty())
    {
        (engine.*Get(EngineLooper{})).Reset();
        engine.looper_reset = true; // NormalPage::Draw resets the looper knob
    }

    recordings_.clear();
    recordings_pending_ = false;
}

bool TapeMachine::PlanImport(int button, ImportTarget& out)
{
    const auto* white = std::find(std::begin(panel::kWhiteKeys), std::end(panel::kWhiteKeys), button);
    const int   key   = white != std::end(panel::kWhiteKeys) ? int(white - std::begin(panel::kWhiteKeys)) + 1 : 0;
    out               = {};
    out.maxSeconds    = double(daisy::kMaxRamBuffSize / 2) / kSampleRate; // the CHIMPO buffer's 165 s, for slots too
    if(button == panel::kPlayKey || button == panel::kLoopKey)
    {
        out.recording = "looper";
        out.label     = "the looper";
        return true;
    }
    if(key == 15)
    {
        out.recording = "buffer";
        out.label     = "the CHIMPO buffer";
        return true;
    }
    if(key < 1)
        return false;

    auto&      engine = fw_->engine;
    const auto mode   = engine.GetVoiceMode();
    const int  bank   = (engine.*Get(EngineBank{}))[int(mode)];
    char       name[32];
    daisy::Engine::GetFileNameForSlot(key, bank, mode, name, false);
    out.file = name;
    daisy::Engine::GetFileNameForSlot(key, bank, mode, name, true);
    out.obsolete = {name}; // rebuilt at boot (FileCopier::NeedsOverwrite)
    out.label    = std::string(mode == daisy::VoiceMode::JAMMI ? "Jammi " : "Cubbi ") + char('a' + bank) + std::to_string(key);
    out.mode     = int(mode);
    out.bank     = bank;
    out.slot     = key;
    return true;
}

void TapeMachine::CompleteImports(std::vector<ImportTarget> imports)
{
    imports_ = std::move(imports);
}

/** A slot gets default settings, as erasing one does; the first is selected
 *  if Jammi shows. New CHIMPO buffer audio ends as a recording does. */
void TapeMachine::ApplyImports()
{
    auto& f = *fw_;
    for(size_t i = 0; i < imports_.size(); i++)
    {
        const auto& t = imports_[i];
        if(t.recording == "buffer")
            f.ui.normal_page_.StopVoiceRecording();
        else if(!t.file.empty())
        {
            f.presets.Invalidate(uint8_t(t.mode), uint8_t(t.bank), uint8_t(t.slot));
            if(i == 0 && t.mode == int(daisy::VoiceMode::JAMMI))
            {
                f.engine.SetBank(size_t(t.bank));
                f.ui.menu_page_.SetVoiceSlot(size_t(t.slot));
            }
        }
    }
    imports_.clear();
}

bool TapeMachine::IsReady()
{
    return fw_->rainbow_done && !fw_->ui.InRainbows();
}

void TapeMachine::Process(const float* const* in, float* const* out)
{
    ScopedContext scope(*ctx_);

    if(fw_->rainbow_done)
    {
        if(recordings_pending_)
            ApplyRecordings();
        if(session_pending_)
        {
            session_pending_ = false;
            ApplySession();
        }
        if(!imports_.empty())
            ApplyImports();
        ChaseControls();
    }

    DrainPanelEvents();

    AudioCallback(in, out, kBlockSize);
    ctx_->clock_us += uint64_t(kBlockSize) * 1000000 / kSampleRate;

    if(++blocks_ % 2 == 0) // TIM4 at 1 kHz
        SDCallback();

    MainLoop();
}

} // namespace
} // namespace emu

extern "C" emu::Machine* chompi_create_tape(const std::shared_ptr<emu::VirtualCard>* card)
{
    return new emu::TapeMachine(*card);
}
