// Geometry is in a fixed 1100 x 470 canvas (measured from a top-down product
// photo); the editor scales the whole canvas.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

#include "chompi/Machine.h"

class ChompiProcessor;

namespace panel_ui
{
constexpr int kWidth  = 1100;
constexpr int kHeight = 470;

/** Combines the ways a button can be held (mouse, latch, Shift key) and sends
 *  press/release to the machine on transitions. */
class Input
{
  public:
    enum Source : uint8_t
    {
        Mouse = 1,
        Latch = 2,
        Shift = 4
    };

    explicit Input(ChompiProcessor& processor) : processor_(processor) {}

    void set(int button, Source source, bool held);
    bool isDown(int button) const { return sources_[size_t(button)] != 0; }
    bool isLatched(int button) const { return (sources_[size_t(button)] & Latch) != 0; }
    void releaseAll();

    void turn(int encoder, int detents);

    void setTogglePlay(bool play);
    bool togglePlay() const;

    std::function<void(int button)> onButtonChanged;

  private:
    ChompiProcessor&         processor_;
    std::array<uint8_t, 40> sources_{};
};

class Background : public juce::Component
{
  public:
    Background();
    void paint(juce::Graphics&) override;
};

/** Through-hole LED. */
class Led : public juce::Component
{
  public:
    enum class Style
    {
        Large, // 8 mm with gold bezel
        Small, // 5 mm
        Reel,  // 5 mm inside a gold tape-reel ring
    };

    Led(int index, Style style);
    void setColour(juce::Colour c);
    int  index() const { return index_; }
    void paint(juce::Graphics&) override;

    static constexpr float kGlowScale = 2.4f; // component size relative to the bezel

  private:
    int          index_;
    Style        style_;
    juce::Colour colour_{juce::Colours::black};
};

class Key : public juce::Component, public juce::SettableTooltipClient
{
  public:
    enum class Style
    {
        White,
        Black,
        Chompi,
        Play,
        Loop
    };

    Key(Input& input, int button, Style style, int smtLed);
    void setBacklight(juce::Colour c);
    void setDropTarget(bool target);
    int  smtLed() const { return smtLed_; }
    int  button() const { return button_; }

    void paint(juce::Graphics&) override;
    bool hitTest(int x, int y) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

  private:
    /** The keycap; the component is larger to hold the backlight halo, and the
     *  halos of neighbouring keys overlap. */
    juce::Rectangle<float> capBounds() const;

    Input&       input_;
    int          button_;
    Style        style_;
    int          smtLed_;
    juce::Colour backlight_{juce::Colours::black};
    bool         dropTarget_ = false;
};

class Knob : public juce::Component, public juce::SettableTooltipClient
{
  public:
    enum class Style
    {
        Small,
        Purple
    };

    Knob(Input& input, int encoder, Style style, float pixelsPerDetent);
    int encoder() const { return encoder_; }

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

  private:
    void emit(float detents);

    Input&           input_;
    int              encoder_;
    Style            style_;
    float            pixelsPerDetent_;
    float            pending_   = 0;
    float            angle_     = 0;
    bool             dragged_   = false;
    juce::Point<int> lastDrag_;
};

class Toggle : public juce::Component, public juce::SettableTooltipClient
{
  public:
    explicit Toggle(Input& input);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

  private:
    Input& input_;
};

struct Layout
{
    static juce::Point<float> whiteKey(int i) { return {92.f + 65.29f * float(i), 382.f}; }
    static juce::Point<float> blackKey(int i);
    static juce::Point<float> pthLed(int i);
    static juce::Point<float> encoder(int hwIndex);

    static constexpr float kWhiteKeySize = 58.f;
    static constexpr float kBlackKeySize = 54.f;
    static constexpr float kFunctionKey  = 60.f; // CHOMPI, play, loop
    static constexpr float kKnobSkirt    = 30.f; // radius
    static constexpr float kPurpleCap    = 53.f; // radius

    static inline const juce::Point<float> kChompiKey{159.f, 220.f};
    static inline const juce::Point<float> kPlayKey{810.f, 220.f};
    static inline const juce::Point<float> kLoopKey{875.f, 220.f};
    static inline const juce::Point<float> kToggle{87.f, 220.f}; // level with the CHOMPI key
    static inline const juce::Rectangle<float> kPanel{23.f, 112.f, 1054.f, 338.f};
};

} // namespace panel_ui
