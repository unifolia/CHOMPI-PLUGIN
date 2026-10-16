// Port of TEMPO 1.0's chompi_main.cpp. Its globals become Firmware members and
// its interrupt handlers become methods the scheduler in Process() calls; the
// logic is otherwise line for line. Divergences are marked EMULATOR.
#include "chompi/Machine.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>

#include "emu_context.h"
#include "emu_expose.h"
#include "emu_session.h"

#include "hardware.h"
#include "temp_led_stuff.h"
#include "ui.h"
#include "daisysp.h"
#include "fatfs.h"
#include "diskio.h"
#include "OptionsManager.h"
#include "EngineBase.h"
#include "SampleEngine.h"
#include "SliceEngine.h"
#include "SampleManager.h"
#include "ArpeggiatorSequencer.h"
#include "clockManager.h"
#include "MidiManager.h"
#include "FxEngine.h"
#include "granularDelay.h"
#include "StateSaver.h"
#include "reverb.h"

constexpr size_t kBufferSize = 480000; // 10 sec @ 48kHz mono

namespace emu
{
using daisy::System;

namespace
{
/** What a DAW session restores: the four engine states TEMPO keeps (chromatic
 *  and slice, A and B, sequences included) with the live one saved into its
 *  slot first, which engine and state are showing, tempo and clock mode.
 *  Patterns come back stopped, as after power-on. */
struct SessionState
{
    uint8_t    engine = CHROMATIC;
    uint8_t    ab     = StateSaver::A;
    SavedState slots[StateSaver::LAST]{};
    uint8_t    pages[6]{};
    int        tempo     = 320;
    int        clockMode = FREE;
    uint8_t    input     = uint8_t(InputSource::MIC);
};

constexpr int kNoteFields = 6; // NoteInfo: key, transpose, order, timestamp, key number, remove

void ToSession(const SessionState& st, Session& s)
{
    using namespace session;
    s.clear();
    PutInt(s, "engine", st.engine);
    PutInt(s, "ab", st.ab);
    const float pages[6] = {float(st.pages[0]), float(st.pages[1]), float(st.pages[2]),
                            float(st.pages[3]), float(st.pages[4]), float(st.pages[5])};
    PutFloats(s, "pages", pages, 6, 0);
    PutInt(s, "tempo", st.tempo);
    PutInt(s, "clockMode", st.clockMode);
    PutInt(s, "input", st.input);
    for(int i = 0; i < StateSaver::LAST; i++)
    {
        const auto&       slot   = st.slots[i];
        const std::string prefix = "s" + std::to_string(i) + "_";
        PutFloats(s, (prefix + "knobs").c_str(), &slot.enc_values[0][0], 8);
        const float params[] = {slot.filter,   slot.redux,    slot.pan,      slot.loop,
                                slot.sustain,  slot.randomness, slot.feedback, slot.out_gain,
                                slot.comp,     slot.in_gain,  slot.arp_randomness};
        PutFloats(s, (prefix + "params").c_str(), params, int(std::size(params)));
        const float ints[] = {float(slot.sample_slot), float(slot.monitor_mode), float(slot.pattern),
                              float(slot.rest_pattern), float(slot.clock_div), float(slot.latch_state)};
        PutFloats(s, (prefix + "ints").c_str(), ints, int(std::size(ints)), 0);
        std::vector<float> notes;
        for(const auto& n : slot.sequence)
            notes.insert(notes.end(), {float(n.key_), n.transpose_nn_, float(n.order_), float(n.timestamp_),
                                       float(n.key_number_), float(n.remove_)});
        PutFloats(s, (prefix + "sequence").c_str(), notes.data(), int(notes.size()));
    }
}

bool FromSession(const Session& s, SessionState& st)
{
    using namespace session;
    const auto pages = GetFloats(s, "pages");
    if(pages.size() != 6)
        return false;
    st.engine    = uint8_t(GetInt(s, "engine")) == SLICE ? SLICE : CHROMATIC;
    st.ab        = GetInt(s, "ab") == StateSaver::B ? StateSaver::B : StateSaver::A;
    for(int i = 0; i < 6; i++)
        st.pages[i] = uint8_t(pages[size_t(i)]);
    st.tempo     = GetInt(s, "tempo", 320);
    st.clockMode = GetInt(s, "clockMode") == SYNC ? SYNC : FREE;
    st.input     = uint8_t(GetInt(s, "input"));
    for(int i = 0; i < StateSaver::LAST; i++)
    {
        auto&             slot   = st.slots[i];
        const std::string prefix = "s" + std::to_string(i) + "_";
        const auto        knobs  = GetFloats(s, (prefix + "knobs").c_str());
        const auto        params = GetFloats(s, (prefix + "params").c_str());
        const auto        ints   = GetFloats(s, (prefix + "ints").c_str());
        const auto        notes  = GetFloats(s, (prefix + "sequence").c_str());
        if(knobs.size() != 8 || params.size() != 11 || ints.size() != 6 || notes.size() % kNoteFields != 0)
            return false;
        for(int k = 0; k < 8; k++)
            slot.enc_values[k / 4][k % 4] = knobs[size_t(k)];
        float* const fields[] = {&slot.filter,   &slot.redux,      &slot.pan,      &slot.loop,
                                 &slot.sustain,  &slot.randomness, &slot.feedback, &slot.out_gain,
                                 &slot.comp,     &slot.in_gain,    &slot.arp_randomness};
        for(size_t k = 0; k < std::size(fields); k++)
            *fields[k] = params[k];
        slot.sample_slot  = size_t(ints[0]);
        slot.monitor_mode = size_t(ints[1]);
        slot.pattern      = size_t(ints[2]);
        slot.rest_pattern = size_t(ints[3]);
        slot.clock_div    = size_t(ints[4]);
        slot.latch_state  = size_t(ints[5]);
        slot.play         = false;
        slot.sequence.clear();
        for(size_t k = 0; k < notes.size(); k += kNoteFields)
        {
            NoteInfo n{};
            n.key_        = int8_t(notes[k]);
            n.transpose_nn_ = notes[k + 1];
            n.order_      = uint8_t(notes[k + 2]);
            n.timestamp_  = uint32_t(notes[k + 3]);
            n.key_number_ = int8_t(notes[k + 4]);
            n.remove_     = notes[k + 5] != 0.f;
            slot.sequence.push_back(n);
        }
    }
    return true;
}

using KnobPages = uint8_t[6];
using EncValues = float[3][6];
using Slots     = SavedState[StateSaver::LAST];
EMU_EXPOSE(UiEncValues, chompi::UserInterface, EncValues, enc_values);
EMU_EXPOSE(UiKnobPage, chompi::UserInterface, KnobPages, knob_page);
EMU_EXPOSE(SaverSlots, StateSaver, Slots, states_);
EMU_EXPOSE(ClockFreeTempo, clockManager, uint32_t, current_tempo);

class TempoMachine final : public Machine
{
  public:
    explicit TempoMachine(std::shared_ptr<VirtualCard> card);
    ~TempoMachine() override;

    void Process(const float* const* in, float* const* out) override;
    bool CaptureSession(Session& out) override;
    void RestoreSession(const Session& session) override;
    void SetClockSync(bool sync) override;

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
    int          sync_pending_    = -1; // -1 none, else the clockMode to set
};

/** The Daisy Seed's 64 MB SDRAM, zeroed (ZeroSDRAM()). calloc leaves pages
 *  the firmware never touches unmapped. */
struct Sdram
{
    static constexpr size_t kSize = 64 * 1024 * 1024;
    Sdram() : bytes(static_cast<uint8_t*>(std::calloc(kSize, 1))) {}
    ~Sdram() { std::free(bytes); }
    Sdram(const Sdram&)            = delete;
    Sdram& operator=(const Sdram&) = delete;
    uint8_t* bytes;
};

/** Everything chompi_main.cpp keeps in globals. Allocated value-initialised,
 *  so it starts zeroed like .bss before constructors run. */
struct TempoMachine::Firmware
{
    chompi::Hardware      hw;
    chompi::UserInterface ui;

    // .sdram_bss: the delay's two buffers at the start of SDRAM, then the
    // sample memory sampleManager places past them (sample_ram_start).
    Sdram  sdram;
    float* granularBuffer; // kBufferSize * 2: 10 sec stereo
    float* frozenBuffer;
    void*  sample_ram_start;

    chompi::PresetManager        presets;
    chompi::OptionsManager       options;
    sampleEngine                 sEngine;
    sliceEngine                  slice;
    sampleManager                sManager;
    fxEngine                     fx;
    daisy::FileStreamingManager  file_manager;
    ArpeggiatorSequencer         arpSeq;
    clockManager                 cManager;
    MidiManager                  midi;
    granularDelay                delay;
    StateSaver                   stateSaver;
    BaseEngine*                  engines[2];
    daisysp::Reverb              reverb;
    daisy::TimerHandle           midi_clock_timer;

    size_t tim_base_freq;

    size_t testThingy;

    float chromaBuffer[2][48];
    float sliceBuffer[2][48];

    float* chromaPtr[2];
    float* slicePtr[2];

    uint32_t testTimer;

    bool testLoad; //To make sure wavetables don't get loaded until later
    bool testSDLoaded;

    daisysp::Oscillator osc;

    uint32_t pret, sd_checkt;
    bool     booting;
    bool     loading_screen;
    size_t   loading_screen_time;
    bool     rainbow_done;

    bool line_in_state;
    bool no_sd_card;

    uint32_t uit, now, pre_startt;
    uint32_t batt;
};

TempoMachine::TempoMachine(std::shared_ptr<VirtualCard> card)
: Machine(emu::Firmware::Tempo, std::move(card)), fw_(new Firmware())
{
    auto& f            = *fw_;
    f.granularBuffer   = reinterpret_cast<float*>(f.sdram.bytes);
    f.frozenBuffer     = f.granularBuffer + kBufferSize * 2;
    f.sample_ram_start = f.sdram.bytes;
    f.engines[0]       = &f.sEngine;
    f.engines[1]       = &f.slice;
    f.chromaPtr[0]     = f.chromaBuffer[0];
    f.chromaPtr[1]     = f.chromaBuffer[1];
    f.slicePtr[0]      = f.sliceBuffer[0];
    f.slicePtr[1]      = f.sliceBuffer[1];
    f.booting          = true;
    f.loading_screen   = true;

    ScopedContext scope(*ctx_);
    Boot();
}

TempoMachine::~TempoMachine() = default;

void TempoMachine::MidiClockCallback(void* data)
{
    auto& f = *static_cast<Firmware*>(data);
    // Called at the timer frequency
    if(f.arpSeq.getPlay(0) && f.options.midi_clock_out && f.cManager.getClockMode() == FREE)
        f.midi.QueueMidiClock();
    f.cManager.incrementCounters();
    f.delay.setClockPulse();
}

void TempoMachine::InitMidiClockTimer()
{
    auto& f = *fw_;

    daisy::TimerHandle::Config tim_cfg;

    // Pick a timer not used by libDaisy or audio
    tim_cfg.periph     = daisy::TimerHandle::Config::Peripheral::TIM_16; // better choice than TIM_2
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
void TempoMachine::Boot()
{
    auto&       f  = *fw_;
    const float sr = float(kSampleRate);

    f.hw.Init();
    f.midi.Init(&f.cManager, &f.ui, &f.hw, &f.fx, &f.arpSeq, f.engines);

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

    f.options.Init();
    f.tim_base_freq = System::GetPClk2Freq();

    f.reverb.Init(sr);
    f.fx.Init(&f.sManager, sr, &f.delay, &f.reverb, f.options.monitor_position);
    f.delay.Init(f.granularBuffer, f.frozenBuffer, kBufferSize, &f.cManager, f.options.delay_mute);
    f.sManager.Init(f.sample_ram_start, &f.file_manager, sr);
    for(size_t i = 0; i < kNumEngines; ++i)
        f.engines[i]->Init(&f.sManager, sr);

    // delete the battery log if it exists
    char filename[32];
    snprintf(filename, sizeof(filename), ".batt_log.txt");
    f_unlink(filename);

    // macos makes a copy
    snprintf(filename, sizeof(filename), "._.batt_log.txt");
    f_unlink(filename);

    InitMidiClockTimer();
    f.cManager.Init(&f.midi_clock_timer, f.tim_base_freq);
    chompi::LedSetup();
    f.ui.Init(&f.fx, &f.cManager, &f.arpSeq, &f.sManager, &f.hw, &f.presets, f.options.pitch_shift_quantization,
              f.engines, &f.stateSaver, &f.options);

    f.arpSeq.Init(f.engines, &f.cManager, &f.hw, &f.options);
    // EMULATOR: ArpeggiatorSequencer reads notes[i][curIdx[i]] without
    // checking for an empty vector (e.g. checkNotePlaying). On the STM32 that
    // reads near address 0, which is mapped, and the result goes unused; here
    // address 0 faults. A reserved buffer keeps data() non-null for good:
    // erase, clear and copy-assignment keep capacity.
    for(auto& notes : f.arpSeq.notes)
        notes.reserve(64);
    f.hw.setMidiCCOut(f.options.midi_cc_out);
    f.midi.setMidiOptions(f.options.midi_ch_in, f.options.midi_cc_in, f.options.transport_type);
    midi_in_channel_ = f.options.midi_ch_in;

    f.stateSaver.Init();

    // ZeroSDRAM(): calloc'd.

    f.testLoad     = false;
    f.testSDLoaded = false;

    f.osc.Init(sr);
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
void TempoMachine::AudioCallback(const float* const* in, float* const* out, size_t size)
{
    auto& f = *fw_;

    // meter.OnBlockStart();
    uint32_t point1 = System::GetUs();

    for(size_t i = 0; i < size; i++)
        out[0][i] = out[1][i] = out[2][i] = out[3][i] = 0.f;

    if((f.booting || f.loading_screen) && !f.ui.InTestMode())
    {
        f.hw.ProcessAllControls();
        f.ui.GenerateEvents();
        f.ui.DoEvents();

        return;
    }

    f.midi.ProcessMidiIn();
    f.cManager.averageMidiClock();

    f.hw.ProcessAllControls();
    f.ui.GenerateEvents();
    if(f.cManager.checkIntervalExpired(0))
    {
        f.arpSeq.setClockEdge(0);
        f.delay.setClockEdge();
    }
    if(f.cManager.checkIntervalExpired(1))
        f.arpSeq.setClockEdge(1);
    f.cManager.checkIntervalExpired(2);
    f.arpSeq.Prepare();
    for(size_t i = 0; i < kNumEngines; ++i)
        f.engines[i]->Prepare();

    if(f.hw.jack_detect.Read() != f.line_in_state)
    {
        if(f.hw.jack_detect.Read())
            f.fx.SetInputSource(InputSource::LINE_IN);
        else
            f.fx.SetInputSource(InputSource::MIC);
    }
    f.line_in_state = f.hw.jack_detect.Read();

    if(f.ui.InTestMode() && f.ui.GetToggleState())
    {
        for(size_t i = 0; i < size; i++)
            out[0][i] = out[1][i] = out[2][i] = out[3][i] = f.osc.Process();
    }

    auto** outs = const_cast<float**>(out);
    if(f.testSDLoaded)
    {
        f.engines[0]->Process(in, f.chromaPtr, size);
        f.engines[1]->Process(in, f.slicePtr, size);

        for(size_t i = 0; i < size; ++i)
        {
            out[0][i] = f.chromaBuffer[0][i] + f.sliceBuffer[0][i];
            out[1][i] = f.chromaBuffer[1][i] + f.sliceBuffer[1][i];
        }

        f.fx.Process(in, outs, size, f.chromaPtr, f.slicePtr);
        f.fx.ApplyOutputFX(in, outs, size);
    }
    else
    {
        for(size_t i = 0; i < size; i++)
            out[0][i] = out[1][i] = out[2][i] = out[3][i] = 0.f;
    }

    f.testTimer = System::GetUs() - point1;
}

void TempoMachine::CheckSDCardMounted()
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

void TempoMachine::SDCallback()
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

    f.file_manager.ProcessRequests();

    if(now - f.pret > 50)
    {
        f.pret = now;
        f.ui.WritePresets();
    }
}

/** One pass of MainLoop(). EMULATOR: the trailing DelayUs(10) only paced the
 *  busy loop, so MIDI out drains one message per block (0.5 ms) rather than
 *  per 10 us; the DIN port's 31250 baud allows about one per millisecond. */
void TempoMachine::MainLoop()
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

    f.midi.ProcessMidiOut(); // We want midi checks to happen every 10uS for more precise timing

    f.now        = System::GetNow();
    f.testThingy = f.now;

    if(f.now - f.uit > 1)
    {
        f.ui.DoEvents();
        f.uit = f.now;
    }

    if(f.now - f.pre_startt > 1000)
    {
        if(!f.testLoad)
        {
            f.sManager.loadFileData();
            f.testLoad = true;
        }
    }

    if(!f.testSDLoaded)
    {
        if(f.sManager.checkLoaded())
        {
            f.testSDLoaded = true;
            for(size_t i = 0; i < kNumEngines; ++i)
                f.engines[i]->setSample(14);
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

void TempoMachine::DrainPanelEvents()
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

bool TempoMachine::CaptureSession(Session& out)
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
    SessionState  s;
    s.engine = uint8_t(f.fx.getEngine());
    s.ab     = uint8_t(f.stateSaver.getState());
    // The live engine state into its slot, as switching A/B or engines does;
    // the other slots hold what was saved when they were left.
    f.ui.menu_page_.saveState(s.engine, StateSaver::State(s.ab));
    const auto& slots = f.stateSaver.*Get(SaverSlots{});
    for(int i = 0; i < StateSaver::LAST; i++)
        s.slots[i] = slots[i];
    std::memcpy(s.pages, f.ui.*Get(UiKnobPage{}), sizeof(s.pages));
    s.tempo     = int(f.cManager.*Get(ClockFreeTempo{}));
    s.clockMode = f.cManager.getClockMode();
    s.input     = f.fx.getInputSource();
    ToSession(s, out);
    return true;
}

void TempoMachine::RestoreSession(const Session& session)
{
    SessionState state;
    if(!FromSession(session, state))
        return;
    session_         = std::move(state);
    session_pending_ = true;
}

/** Re-creates the session through what the shift layer's engine and A/B
 *  keys do (MenuPage::recallState), with the saved slots in StateSaver. */
void TempoMachine::ApplySession()
{
    auto&       f      = *fw_;
    auto&       slots  = f.stateSaver.*Get(SaverSlots{});
    auto&       pages  = f.ui.*Get(UiKnobPage{});
    auto&       enc    = f.ui.*Get(UiEncValues{});
    const auto& s      = session_;

    // Sequences remember when each note was played (System::GetNow()); this
    // unit's clock started again from zero, so keep their order but end now.
    uint32_t newest = 0;
    for(const auto& slot : s.slots)
        for(const auto& n : slot.sequence)
            newest = std::max(newest, n.timestamp_);
    const uint32_t now = System::GetNow();
    for(int i = 0; i < StateSaver::LAST; i++)
    {
        slots[i] = s.slots[i];
        for(auto& n : slots[i].sequence)
            n.timestamp_ = now - std::min(now, newest - n.timestamp_);
        if(slots[i].sample_slot < 1 || slots[i].sample_slot > 15)
            slots[i].sample_slot = 15;
    }
    f.stateSaver.setState(s.ab == StateSaver::A);

    f.fx.setEngine(engineSelection(s.engine));
    f.arpSeq.setEngine(s.engine == CHROMATIC);
    f.ui.menu_page_.recallState(s.engine, StateSaver::State(s.ab), true);

    for(int knob = 0; knob < 6; knob++)
        pages[knob] = s.pages[knob] < chompi::knob_num_pages[knob] ? s.pages[knob] : 0;

    const int tempo = std::clamp(s.tempo, 160, 480);
    f.cManager.setTempo(size_t(tempo));
    enc[0][4]         = float(tempo - 160) / 320.f;
    f.cManager.mode_ = s.clockMode;
    if(s.input < uint8_t(InputSource::LAST))
        f.fx.SetInputSource(InputSource(s.input));
}

void TempoMachine::SetClockSync(bool sync)
{
    sync_pending_ = sync ? SYNC : FREE;
}

void TempoMachine::Process(const float* const* in, float* const* out)
{
    ScopedContext scope(*ctx_);

    if(fw_->rainbow_done)
    {
        if(session_pending_)
        {
            session_pending_ = false;
            ApplySession();
        }
        if(sync_pending_ >= 0) // after the session, which has a clock mode too
        {
            fw_->cManager.mode_ = sync_pending_;
            sync_pending_       = -1;
        }
    }

    DrainPanelEvents();

    AudioCallback(in, out, kBlockSize);
    ctx_->clock_us += uint64_t(kBlockSize) * 1000000 / kSampleRate;
    fw_->midi_clock_timer.Advance(); // TIM16: clock counters, delay pulses, MIDI clock out

    if(++blocks_ % 2 == 0) // TIM4 at 1 kHz
        SDCallback();

    MainLoop();
}

} // namespace
} // namespace emu

extern "C" emu::Machine* chompi_create_tempo(const std::shared_ptr<emu::VirtualCard>* card)
{
    return new emu::TempoMachine(*card);
}
