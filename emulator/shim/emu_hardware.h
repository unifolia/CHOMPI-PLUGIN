// What the firmwares' hardware.h files share, as emulated; each
// emulator/shim/<name>/hardware.h derives Hardware from it, adding that
// firmware's MIDI interface. The power-management chip (MP2722), USB switching
// and codec setup are inert.
#pragma once
#include <algorithm>
#include "daisy_seed.h"
#include "temp_led_stuff.h"

#define ENC_5_SW 4 // this one weird switch isn't on the SR

using namespace daisy;

namespace daisy
{
/** CD4021 chain with the debounce of the vendored CHOMPI driver
 *  (libDaisy dev/sr_4021.h). Inputs are active-low: a pressed key reads LOW.
 *  A counter (0..2*dbc_size) rises while HIGH and falls while LOW, one step per
 *  1 kHz update; State() is true below dbc_size, so a press from rest registers
 *  after dbc_size + 1 updates. */
template <size_t num_daisychained = 1, size_t num_parallel = 1>
class ShiftRegister4021
{
  public:
    struct Config
    {
        int dbc_size;
    };

    void Init(const Config& cfg)
    {
        config_ = cfg;
        for(size_t i = 0; i < kTotalStates; i++)
        {
            risen[i]        = false;
            states_[i]      = false;
            last_update_[i] = 0;
            updated_[i]     = false;
            dbc_state_[i]   = 0;
            inputs_[i]      = true;
        }
    }

    void Update()
    {
        const uint32_t now = System::GetNow();
        for(size_t idx = 0; idx < kTotalStates; idx++)
        {
            states_[idx]  = inputs_[idx];
            updated_[idx] = false;

            if(now - last_update_[idx] >= 1)
            {
                last_update_[idx] = now;
                updated_[idx]     = true;

                dbc_state_[idx] += states_[idx] * 2;

                if(dbc_state_[idx] > 0)
                    dbc_state_[idx] -= 1;
                if(dbc_state_[idx] > config_.dbc_size * 2)
                    dbc_state_[idx] = config_.dbc_size * 2;
            }
        }
    }

    inline bool State(int idx) const { return dbc_state_[idx] < config_.dbc_size; }
    inline bool RawState(int idx) const { return states_[idx]; }

    inline bool RisingEdge(uint32_t idx)
    {
        if(!risen[idx] && State(idx) && updated_[idx])
        {
            risen[idx] = true;
            return true;
        }
        return false;
    }

    inline bool FallingEdge(uint32_t idx)
    {
        if(risen[idx] && !State(idx) && updated_[idx])
        {
            risen[idx] = false;
            return true;
        }
        return false;
    }

    /** Emulator: electrical level presented on an input (true = HIGH). */
    void SetInput(size_t idx, bool high) { inputs_[idx] = high; }

  private:
    static constexpr size_t kTotalStates = 8 * num_daisychained * num_parallel;
    Config                  config_;
    bool                    states_[kTotalStates];
    bool                    inputs_[kTotalStates];
    bool                    risen[kTotalStates];
    uint32_t                last_update_[kTotalStates];
    bool                    updated_[kTotalStates];
    uint16_t                dbc_state_[kTotalStates];
};
} // namespace daisy

namespace chompi
{
/** Encoder reporting like the firmware's ChompiEncoder: at most one step per
 *  1 ms update (its quadrature decoding needs 2-3 updates per detent, so real
 *  turns are slower still). Detents from the emulator are released one per
 *  update; a backlog beyond kMaxPending is dropped, as steps are lost when a
 *  real encoder spins faster than it is sampled. The push switch debounces like
 *  libDaisy's Switch (7 stable updates). */
class ChompiEncoder
{
  public:
    void Init()
    {
        last_update_ = System::GetNow();
        updated_     = false;
        inc_         = 0;
        pending_     = 0;
        sw_state_    = 0;
        sw_pressed_  = false;
        rising_time_ = 0;
    }

    void Debounce()
    {
        DebounceSteps();
        if(updated_)
        {
            sw_state_ = (sw_state_ << 1) | (sw_pressed_ ? 1 : 0);
            if(sw_state_ == 0x7f)
                rising_time_ = System::GetNow();
        }
    }

    /** Encoders 1-4 have their switches on the button SR. */
    void Debounce(bool, bool) { DebounceSteps(); }

    inline int32_t Increment() const { return updated_ ? inc_ : 0; }
    inline bool    RisingEdge() const { return updated_ ? sw_state_ == 0x7f : false; }
    inline bool    FallingEdge() const { return updated_ ? sw_state_ == 0x80 : false; }
    inline bool    Pressed() const { return sw_state_ == 0xff; }
    inline float   TimeHeldMs() const
    {
        return Pressed() ? float(System::GetNow() - rising_time_) : 0.f;
    }

    /** Emulator inputs. */
    void AddDetents(int detents)
    {
        pending_ = std::clamp(pending_ + detents, -kMaxPending, kMaxPending);
    }
    void SetSwitch(bool pressed) { sw_pressed_ = pressed; }

  private:
    static constexpr int32_t kMaxPending = 64;

    void DebounceSteps()
    {
        const uint32_t now = System::GetNow();
        updated_           = false;

        if(now - last_update_ >= 1)
        {
            last_update_ = now;
            updated_     = true;

            inc_ = 0;
            if(pending_ > 0)
            {
                inc_ = 1;
                pending_--;
            }
            else if(pending_ < 0)
            {
                inc_ = -1;
                pending_++;
            }
        }
    }

    uint32_t last_update_;
    bool     updated_;
    int32_t  inc_;
    int32_t  pending_;
    uint8_t  sw_state_;
    bool     sw_pressed_;
    uint32_t rising_time_;
};

/** A GPIO line the emulator sets (jack detect) or the firmware drives (USB switch). */
struct EmuPin
{
    bool level = false;
    bool Read() const { return level; }
    void Write(bool l) { level = l; }
};

class HardwareBase
{
  public:
    enum class EncoderId : int
    {
        SW1,
        SW2,
        SW3,
        SW4,
        SW5,
        SW6,
        ENC_LAST,
    };

    enum class EncoderSrId : int
    {
        ENC_1_A,
        ENC_1_B,
        ENC_2_A,
        ENC_2_B,
        ENC_3_A,
        ENC_3_B,
        ENC_4_A,
        ENC_4_B,
    };

    /** Switch reference for the 4021 chain */
    enum class SwId
    {
        /** SR 1 (U5) */
        ENC_1_SW,
        ENC_2_SW,
        ENC_3_SW,
        ENC_4_SW,
        NC_6,
        KEY_26,
        SW_TOG,
        KEY_16,
        /** SR 2 (U7) */
        KEY_2,
        KEY_3,
        KEY_4,
        KEY_5,
        KEY_17,
        KEY_18,
        KEY_19,
        KEY_1,
        /** SR 3 (U8) */
        KEY_6,
        KEY_7,
        KEY_8,
        KEY_9,
        KEY_10,
        KEY_20,
        KEY_21,
        KEY_22,
        /** SR 4 (U9) */
        KEY_11,
        KEY_12,
        KEY_13,
        KEY_14,
        KEY_15,
        KEY_23,
        KEY_24,
        KEY_25,
        /** SR 5 (U10) */
        ENC_6_SW,
        KEY_27,
        KEY_28,
        NC_1,
        NC_2,
        NC_3,
        NC_4,
        NC_5,
        SR_LAST,
    };

    void Init()
    {
        for(size_t i = 0; i < 6; i++)
            enc[i].Init();

        ShiftRegister4021<5, 1>::Config button_sr_cfg;
        button_sr_cfg.dbc_size = 7;
        button_sr.Init(button_sr_cfg);

        ShiftRegister4021<1, 1>::Config encoder_sr_cfg;
        encoder_sr_cfg.dbc_size = 50;
        encoder_sr.Init(encoder_sr_cfg);

        // Toggle starts in play position (closed = LOW), see SetToggle().
        button_sr.SetInput(static_cast<int>(SwId::SW_TOG), false);
        jack_detect.Write(true);

        for(size_t i = 0; i < 6; i++)
            enc_trackers[i] = 0;
    }

    enum BatteryLevel
    {
        FULL,
        HIGH,
        MEDIUM,
        LOW,
        LAST,
    };

    // Emulated unit is always on USB power with a charged battery.
    BatteryLevel        batt_level = FULL;
    inline BatteryLevel GetBatteryLevel() { return batt_level; }

    void BMCPerformCheck() {}
    void BMCMediumBattCheck() {}
    void LowBatteryLockoutCheck() {}
    void MpWrite(uint8_t, uint8_t) {}
    void MpReadAll()
    {
        mp_buff_[1] = 1 << 5; // VIN_RDY
        mp_buff_[3] = 0;      // no NTC / battery faults
        read_ready  = true;
    }

    void LedsOff()
    {
        for(size_t i = 0; i < kNumPthLeds; i++)
            SetPthLed(i, 0, 0, 0);
        for(size_t i = 0; i < kNumSmtLeds; i++)
            SetSmtLed(i, 0, 0, 0);
        fill_led_data();
    }

    bool GetToggleState() { return tog_state < 100; }

    uint32_t tog_state = 0;
    void     ProcessAllControls()
    {
        ApplyMinimumHolds();

        button_sr.Update();
        encoder_sr.Update();

        if(!button_sr.State(static_cast<int>(SwId::SW_TOG)))
            tog_state++;
        else if(tog_state != 0)
            tog_state--;

        tog_state = tog_state > 200 ? 200 : tog_state;

        for(size_t i = 0; i < 4; i++)
        {
            const bool a_state = encoder_sr.RawState(i * 2);
            const bool b_state = encoder_sr.RawState(i * 2 + 1);
            enc[i].Debounce(a_state, b_state);
        }

        enc[4].Debounce();
        enc[5].Debounce();

        for(int i = 0; i < 6; i++)
        {
            enc_trackers[i] += enc[i].Increment();
            if(enc[i].RisingEdge())
                enc_trackers[i] = 0;
        }
    }

    void PrintAllControls() {}

    // ======== emulator inputs ========

    /** A key or encoder push. A press shorter than the debounce window would be
     *  lost on hardware too; GUI clicks can be that short when several arrive in
     *  one host block, so releases are held back until kMinHoldMs has passed. */
    void SetButton(int idx, bool pressed)
    {
        if(idx < 0 || idx >= static_cast<int>(SwId::SR_LAST))
            return;

        if(pressed)
        {
            press_time_[idx] = System::GetNow();
            SetReleasePending(idx, false);
            SetLevel(idx, false);
        }
        else if(System::GetNow() - press_time_[idx] >= kMinHoldMs)
            SetLevel(idx, true);
        else
            SetReleasePending(idx, true);
    }

    /** Play position closes the switch (LOW), which GetToggleState() reads as true. */
    void SetToggle(bool play) { button_sr.SetInput(static_cast<int>(SwId::SW_TOG), !play); }

    int enc_trackers[6];

    uint8_t mp_buff_[6];
    bool    read_ready = false;

    ChompiEncoder           enc[6];
    ShiftRegister4021<5, 1> button_sr;
    ShiftRegister4021<1, 1> encoder_sr;
    EmuPin                  usb_sw, mpc_int, jack_detect;

  private:
    static constexpr uint32_t kMinHoldMs = 20;

    void SetLevel(int idx, bool high)
    {
        if(idx == ENC_5_SW)
            enc[4].SetSwitch(!high);
        else
            button_sr.SetInput(idx, high);
    }

    void SetReleasePending(int idx, bool pending)
    {
        if(release_pending_[idx] != pending)
            releases_pending_ += pending ? 1 : -1;
        release_pending_[idx] = pending;
    }

    void ApplyMinimumHolds()
    {
        if(releases_pending_ == 0)
            return;
        const uint32_t now = System::GetNow();
        for(int i = 0; i < static_cast<int>(SwId::SR_LAST); i++)
        {
            if(release_pending_[i] && now - press_time_[i] >= kMinHoldMs)
            {
                SetReleasePending(i, false);
                SetLevel(i, true);
            }
        }
    }

    uint32_t press_time_[static_cast<int>(SwId::SR_LAST)]      = {};
    bool     release_pending_[static_cast<int>(SwId::SR_LAST)] = {};
    int      releases_pending_                                 = 0;
};

} // namespace chompi
