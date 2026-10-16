#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

#include "PanelComponents.h"
#include "PluginProcessor.h"

class ChompiEditor : public juce::AudioProcessorEditor, private juce::Timer
{
  public:
    explicit ChompiEditor(ChompiProcessor&);
    ~ChompiEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

  private:
    void timerCallback() override;
    void showSettingsMenu();
    void showFirmwareMenu();
    void updateStatus();
    void applyTooltips(emu::Firmware firmware);

    struct SettingsButton : juce::Component, juce::SettableTooltipClient
    {
        std::function<void()> onClick;
        void                  paint(juce::Graphics&) override;
        void                  mouseUp(const juce::MouseEvent&) override;
    };

    /** "TAPE 2.0 firmware" with a chevron; opens the firmware menu. */
    struct FirmwareButton : juce::Component, juce::SettableTooltipClient
    {
        std::function<void()> onClick;
        void                  setText(const juce::String& text);
        void                  paint(juce::Graphics&) override;
        void                  mouseUp(const juce::MouseEvent&) override;
        void                  mouseEnter(const juce::MouseEvent&) override { repaint(); }
        void                  mouseExit(const juce::MouseEvent&) override { repaint(); }

      private:
        juce::String text_;
    };

    ChompiProcessor& processor_;
    panel_ui::Input  input_;

    juce::Component                               canvas_; // fixed-size panel, scaled as a whole
    panel_ui::Background                          background_;
    std::vector<std::unique_ptr<panel_ui::Led>>   leds_;
    std::vector<std::unique_ptr<panel_ui::Key>>   keys_;
    std::vector<std::unique_ptr<panel_ui::Knob>>  knobs_;
    std::unique_ptr<panel_ui::Toggle>             toggle_;
    std::array<panel_ui::Key*, 40>                keyByButton_{};
    FirmwareButton                                firmwareButton_;
    juce::Label                                   cardWarning_; // beside the card menu
    SettingsButton                                settings_;
    juce::TooltipWindow                           tooltips_{this, 700};
    std::unique_ptr<juce::FileChooser>            chooser_;

    bool shiftHeld_    = false;
    int  generation_   = -1;
    bool wasRunning_   = false;
    bool wasMissing_   = false;

    emu::Firmware shownFirmware_ = emu::Firmware::Tape; // whose tooltips are up

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChompiEditor)
};
