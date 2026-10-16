#include "PanelComponents.h"
#include "PluginProcessor.h"

#include <cmath>

using namespace juce;

namespace panel_ui
{
namespace
{
const Colour kGold{0xffc9a24c};
const Colour kSilk{0xffe9e9e6};
const Colour kRedKey{0xffe8574e};
const Colour kTealKey{0xff3dc3c8};
const Colour kYellowKey{0xfff2c12e};
const Colour kPurple{0xff8d58d6};

constexpr float kSilkStroke = 1.7f;

/** LED drive to perceived brightness: dim PWM levels look brighter than linear. */
Colour displayColour(Colour c)
{
    const float peak = jmax(c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue());
    if(peak <= 0.f)
        return Colours::black;
    const float gain = std::pow(peak, 0.5f) / peak;
    return Colour::fromFloatRGBA(jmin(1.f, c.getFloatRed() * gain),
                                 jmin(1.f, c.getFloatGreen() * gain),
                                 jmin(1.f, c.getFloatBlue() * gain),
                                 1.f);
}

float brightness(Colour c)
{
    return jmax(c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue());
}

void glow(Graphics& g, Point<float> centre, float radius, Colour c, float alpha)
{
    ColourGradient grad(c.withAlpha(alpha), centre, c.withAlpha(0.f), centre.translated(radius, 0), true);
    grad.addColour(0.35, c.withAlpha(alpha * 0.45f));
    g.setGradientFill(grad);
    g.fillEllipse(Rectangle<float>(radius * 2, radius * 2).withCentre(centre));
}

/** Arc around `centre` from `from` to `to` (degrees, 0 = up, clockwise), arrowhead at `to`. */
void arcArrow(Graphics& g, Point<float> centre, float radius, float from, float to, bool arrow = true)
{
    Path arc;
    arc.addCentredArc(centre.x, centre.y, radius, radius, 0.f, degreesToRadians(from), degreesToRadians(to), true);
    g.setColour(kSilk);
    g.strokePath(arc, PathStrokeType(kSilkStroke, PathStrokeType::curved, PathStrokeType::rounded));
    if(!arrow)
        return;

    const float a   = degreesToRadians(to);
    const float dir = to > from ? 1.f : -1.f;
    const auto  tip = centre + Point<float>(std::sin(a), -std::cos(a)) * radius;
    // Tangent in the direction of travel.
    const Point<float> t(std::cos(a) * dir, std::sin(a) * dir);
    const Point<float> n(-t.y, t.x);
    Path head;
    head.addTriangle(tip + t * 5.f, tip - t * 1.5f + n * 4.f, tip - t * 1.5f - n * 4.f);
    g.fillPath(head);
}

void screw(Graphics& g, Point<float> c)
{
    const float r = 7.5f;
    g.setGradientFill(ColourGradient(Colour(0xff3a3a3a), c.translated(-r, -r), Colour(0xff141414), c.translated(r, r), false));
    g.fillEllipse(Rectangle<float>(r * 2, r * 2).withCentre(c));
    g.setColour(Colour(0xff0a0a0a));
    g.drawEllipse(Rectangle<float>(r * 2, r * 2).withCentre(c), 1.f);
    g.setColour(Colour(0xff090909));
    const float s = r * 0.55f;
    g.drawLine(c.x - s, c.y - s, c.x + s, c.y + s, 1.8f);
    g.drawLine(c.x - s, c.y + s, c.x + s, c.y - s, 1.8f);
}

void iconBox(Graphics& g, Rectangle<float> r, bool filled)
{
    g.setColour(kSilk);
    if(filled)
        g.fillRoundedRectangle(r, 3.f);
    else
        g.drawRoundedRectangle(r, 3.5f, kSilkStroke);
}

void speedometer(Graphics& g, Point<float> c)
{
    Path p;
    p.addCentredArc(c.x, c.y + 3.f, 7.f, 7.f, 0.f, -MathConstants<float>::halfPi, MathConstants<float>::halfPi, true);
    g.setColour(Colours::black);
    g.strokePath(p, PathStrokeType(1.6f));
    g.drawLine(c.x, c.y + 3.f, c.x + 4.f, c.y - 2.f, 1.6f);
    g.fillEllipse(c.x - 1.6f, c.y + 1.4f, 3.2f, 3.2f);
}

void wand(Graphics& g, Point<float> c)
{
    g.setColour(Colours::black);
    g.drawLine(c.x - 6.f, c.y + 6.f, c.x + 3.f, c.y - 3.f, 2.f);
    auto sparkle = [&g](float x, float y, float s) {
        g.drawLine(x - s, y, x + s, y, 1.2f);
        g.drawLine(x, y - s, x, y + s, 1.2f);
    };
    sparkle(c.x + 5.f, c.y - 5.f, 2.6f);
    sparkle(c.x - 2.f, c.y - 5.f, 1.6f);
    sparkle(c.x + 6.f, c.y + 2.f, 1.6f);
}

/** Two overlapping half-moons: the panel's sample/buffer glyph. */
void moons(Graphics& g, Point<float> c, float s)
{
    g.setColour(kSilk);
    Path p;
    p.addCentredArc(c.x - s * 0.35f, c.y, s, s, 0.f, MathConstants<float>::pi, MathConstants<float>::twoPi, true);
    p.addCentredArc(c.x + s * 0.35f, c.y, s, s, 0.f, 0.f, MathConstants<float>::pi, true);
    g.strokePath(p, PathStrokeType(1.4f));
}

void plusCircle(Graphics& g, Point<float> c, float r)
{
    g.setColour(kSilk);
    g.drawEllipse(Rectangle<float>(r * 2, r * 2).withCentre(c), 1.3f);
    g.drawLine(c.x - r * 0.55f, c.y, c.x + r * 0.55f, c.y, 1.3f);
    g.drawLine(c.x, c.y - r * 0.55f, c.x, c.y + r * 0.55f, 1.3f);
}

void starGlyph(Graphics& g, Point<float> c, float r)
{
    g.setColour(kSilk);
    for(int i = 0; i < 4; i++)
    {
        const float a = MathConstants<float>::pi * float(i) / 4.f;
        g.drawLine(c.x - std::cos(a) * r, c.y - std::sin(a) * r, c.x + std::cos(a) * r, c.y + std::sin(a) * r, 1.3f);
    }
}

void waveformStrip(Graphics& g, float x0, float x1, float y)
{
    g.setColour(kSilk);
    g.drawLine(x0, y, x1, y, 1.3f);

    // Burst in the middle.
    const float mid = (x0 + x1) * 0.5f;
    for(int i = -14; i <= 14; i++)
    {
        const float x = mid + float(i) * 2.2f;
        const float h = 9.f * std::exp(-float(i * i) / 70.f) * (0.55f + 0.45f * std::abs(std::sin(float(i) * 1.7f)));
        g.drawLine(x, y - h, x, y + h * 0.6f, 1.f);
    }

    // Start and end flags.
    auto flag = [&g, y](float x, float dir) {
        g.drawLine(x, y, x, y - 13.f, 1.3f);
        Path f;
        f.addTriangle(x, y - 13.f, x, y - 7.f, x + dir * 7.f, y - 10.f);
        g.fillPath(f);
    };
    flag(x0 + 2.f, 1.f);
    flag(x1 - 2.f, -1.f);
}

void micGlyph(Graphics& g, Point<float> c)
{
    g.setColour(kSilk);
    g.drawRoundedRectangle(Rectangle<float>(5.f, 9.f).withCentre(c.translated(0, -2.f)), 2.5f, 1.4f);
    Path stand;
    stand.addCentredArc(c.x, c.y, 5.f, 5.f, 0.f, MathConstants<float>::halfPi, MathConstants<float>::halfPi * 3.f, true);
    g.strokePath(stand, PathStrokeType(1.3f));
    g.drawLine(c.x, c.y + 5.f, c.x, c.y + 8.f, 1.3f);
}

void boltGlyph(Graphics& g, Point<float> c)
{
    g.setColour(kSilk);
    g.drawEllipse(Rectangle<float>(18.f, 18.f).withCentre(c), 1.4f);
    Path bolt;
    bolt.startNewSubPath(c.x + 1.5f, c.y - 6.f);
    bolt.lineTo(c.x - 3.f, c.y + 1.f);
    bolt.lineTo(c.x + 0.5f, c.y + 1.f);
    bolt.lineTo(c.x - 1.5f, c.y + 6.f);
    bolt.lineTo(c.x + 3.5f, c.y - 1.5f);
    bolt.lineTo(c.x, c.y - 1.5f);
    bolt.closeSubPath();
    g.fillPath(bolt);
}

void grille(Graphics& g, Point<float> c)
{
    g.setColour(kGold);
    g.fillEllipse(Rectangle<float>(28.f, 28.f).withCentre(c));
    g.setColour(Colour(0xff111111));
    for(int i = -2; i <= 2; i++)
    {
        const float h = i == 0 ? 16.f : (std::abs(i) == 1 ? 14.f : 9.f);
        g.fillRoundedRectangle(Rectangle<float>(2.2f, h).withCentre(c.translated(float(i) * 4.4f, 0)), 1.1f);
    }
}

/** The little headphone-wearing character next to the volume knob. */
void character(Graphics& g, Point<float> c)
{
    g.setColour(kSilk);
    const auto head = Rectangle<float>(34.f, 30.f).withCentre(c);
    g.drawRoundedRectangle(head, 9.f, 1.8f);

    Path band;
    band.addCentredArc(c.x, c.y + 2.f, 22.f, 22.f, 0.f, -1.25f, 1.25f, true);
    g.strokePath(band, PathStrokeType(1.8f));
    g.fillRoundedRectangle(Rectangle<float>(6.f, 12.f).withCentre(c.translated(-20.f, 3.f)), 2.f);
    g.fillRoundedRectangle(Rectangle<float>(6.f, 12.f).withCentre(c.translated(20.f, 3.f)), 2.f);

    for(float dx : {-7.f, 7.f})
    {
        const auto eye = Rectangle<float>(11.f, 11.f).withCentre(c.translated(dx, -2.f));
        g.setColour(kSilk);
        g.fillEllipse(eye);
        g.setColour(Colour(0xff111111));
        g.fillEllipse(eye.reduced(3.2f).translated(0.8f, 0.5f));
    }
    g.setColour(kSilk);
    g.drawLine(c.x - 3.f, c.y + 9.f, c.x + 3.f, c.y + 9.f, 1.6f);
}

void chevrons(Graphics& g, Point<float> c)
{
    g.setColour(kSilk);
    auto chevron = [&g](float x, float y, float dir) {
        g.drawLine(x, y, x - dir * 3.5f, y - 3.5f, 1.4f);
        g.drawLine(x, y, x - dir * 3.5f, y + 3.5f, 1.4f);
    };
    chevron(c.x - 9.f, c.y, -1.f);
    chevron(c.x - 5.f, c.y, -1.f);
    g.drawLine(c.x, c.y - 5.f, c.x, c.y + 5.f, 1.6f);
    chevron(c.x + 5.f, c.y, 1.f);
    chevron(c.x + 9.f, c.y, 1.f);
}

void tapeWave(Graphics& g, Point<float> c)
{
    Path p;
    for(int i = 0; i <= 30; i++)
    {
        const float t = float(i) / 30.f;
        const float x = c.x - 15.f + 30.f * t;
        const float y = c.y + std::sin(t * MathConstants<float>::twoPi * 2.f) * 4.f * std::sin(t * MathConstants<float>::pi);
        if(i == 0)
            p.startNewSubPath(x, y);
        else
            p.lineTo(x, y);
    }
    g.setColour(kSilk);
    g.strokePath(p, PathStrokeType(1.4f));
}

ColourGradient panelFill()
{
    const auto p = Layout::kPanel;
    return ColourGradient(Colour(0xff181818), 0, p.getY(), Colour(0xff0b0b0b), 0, p.getBottom(), false);
}

Rectangle<float> functionKeyBounds(Point<float> centre)
{
    return Rectangle<float>(Layout::kFunctionKey, Layout::kFunctionKey).withCentre(centre);
}

/** Thick gold frame hugging a group of keys. Repaints its inside with the
 *  panel black, hiding anything drawn there before. Returns the outer bounds. */
Rectangle<float> keyFrame(Graphics& g, Rectangle<float> keys)
{
    constexpr float gap = 6.f, width = 8.f, corner = 12.f;
    const auto      r = keys.expanded(gap + width * 0.5f);
    g.setGradientFill(panelFill());
    g.fillRoundedRectangle(r, corner);
    g.setColour(kGold);
    g.drawRoundedRectangle(r, corner, width);
    return r.expanded(width * 0.5f);
}
} // namespace

// ============================================================================
// Input

void Input::set(int button, Source source, bool held)
{
    auto&         bits = sources_[size_t(button)];
    const uint8_t was  = bits;
    bits               = held ? uint8_t(bits | source) : uint8_t(bits & ~source);

    if((was != 0) != (bits != 0))
        processor_.post({emu::PanelEvent::Type::Button, int16_t(button), int16_t(bits != 0)});
    if(was != bits && onButtonChanged)
        onButtonChanged(button);
}

void Input::releaseAll()
{
    for(int b = 0; b < int(sources_.size()); b++)
    {
        set(b, Mouse, false);
        set(b, Latch, false);
        set(b, Shift, false);
    }
}

void Input::turn(int encoder, int detents)
{
    if(detents != 0)
        processor_.post({emu::PanelEvent::Type::Turn, int16_t(encoder), int16_t(detents)});
}

void Input::setTogglePlay(bool play)
{
    processor_.setTogglePlay(play);
}

bool Input::togglePlay() const
{
    return processor_.getTogglePlay();
}

// ============================================================================
// Layout

Point<float> Layout::blackKey(int i)
{
    const float x = (whiteKey(emu::panel::kBlackKeyAfterWhite[i]).x + whiteKey(emu::panel::kBlackKeyAfterWhite[i] + 1).x) * 0.5f;
    return {x, 312.f};
}

Point<float> Layout::pthLed(int i)
{
    static const Point<float> leds[emu::panel::kNumPthLeds] = {
        {158.f, 152.f}, // CHOMPI
        {247.f, 152.f}, // speed
        {359.f, 152.f}, // start
        {471.f, 152.f}, // end
        {583.f, 152.f}, // magic
        {672.f, 144.f}, // looper reverse
        {742.f, 144.f}, // looper forward
        {808.f, 144.f}, // play reel
        {875.f, 144.f}, // loop reel
        {1006.f, 152.f}, // volume
    };
    return leds[i];
}

Point<float> Layout::encoder(int hwIndex)
{
    static const Point<float> knobs[emu::panel::kNumEncoders] = {
        {359.f, 220.f},  // start
        {471.f, 220.f},  // end
        {583.f, 220.f},  // magic
        {247.f, 220.f},  // speed
        {707.f, 222.f},  // looper (purple): x keeps its ring and the deck frame out of each other's black
        {1006.f, 220.f}, // volume: under its LED, clear of the bolt glyph
    };
    return knobs[hwIndex];
}

// ============================================================================
// Background

Background::Background()
{
    setInterceptsMouseClicks(false, false);
    setBufferedToImage(true);
}

void Background::paint(Graphics& g)
{
    // Backdrop
    g.setGradientFill(ColourGradient(Colour(0xffdedcdb), 0, 0, Colour(0xffcac8c7), 0, float(kHeight), false));
    g.fillAll();

    // Carry handle
    {
        Path handle;
        handle.addRoundedRectangle(325.f, 2.f, 450.f, 130.f, 44.f);
        Path hole;
        hole.addRoundedRectangle(390.f, 29.f, 320.f, 53.f, 22.f);
        handle.setUsingNonZeroWinding(false);
        handle.addPath(hole);

        DropShadow(Colours::black.withAlpha(0.3f), 18, {6, 10}).drawForPath(g, handle);
        g.setGradientFill(ColourGradient(Colour(0xff2b2b2b), 0, 2, Colour(0xff111111), 0, 112, false));
        g.fillPath(handle);
        g.setColour(Colours::white.withAlpha(0.08f));
        g.strokePath(handle, PathStrokeType(1.2f));
    }

    // Panel
    const auto panel = Layout::kPanel;
    {
        Path body;
        body.addRoundedRectangle(panel, 7.f);
        DropShadow(Colours::black.withAlpha(0.45f), 26, {9, 16}).drawForPath(g, body);
        g.setGradientFill(panelFill());
        g.fillPath(body);
        g.setColour(Colours::white.withAlpha(0.07f));
        g.strokePath(body, PathStrokeType(1.f));
    }

    for(auto p : {Point<float>{48, 135}, {1052, 135}, {48, 428}, {517, 428}, {1052, 428}, {255, 312}, {517, 312}, {712, 312}})
        screw(g, p);

    // ---- CHOMPI key and mode switch ----
    const auto chompiFrame = keyFrame(g, functionKeyBounds(Layout::kChompiKey));

    {
        const float y = Layout::kToggle.y; // the switch's wiring is symmetric about it
        iconBox(g, Rectangle<float>(47.f, 19.f).withCentre({87.5f, y - 35.f}), false);
        plusCircle(g, {75.f, y - 35.f}, 5.f);
        moons(g, {96.f, y - 35.f}, 4.5f);
        iconBox(g, Rectangle<float>(47.f, 19.f).withCentre({87.5f, y + 35.f}), false);
        moons(g, {80.f, y + 35.f}, 4.5f);
        starGlyph(g, {100.f, y + 35.f}, 5.f);

        g.setColour(kSilk);
        Path wire;
        wire.startNewSubPath(64.f, y - 35.f);
        wire.lineTo(56.f, y - 35.f);
        wire.quadraticTo(50.f, y - 35.f, 50.f, y - 28.5f);
        wire.lineTo(50.f, y + 28.5f);
        wire.quadraticTo(50.f, y + 35.f, 56.f, y + 35.f);
        wire.lineTo(64.f, y + 35.f);
        g.strokePath(wire, PathStrokeType(kSilkStroke));
        g.drawLine(50.f, y, 75.f, y, kSilkStroke);
        g.drawLine(99.f, y, chompiFrame.getX(), y, kSilkStroke);
    }

    // ---- speed ----
    arcArrow(g, Layout::encoder(emu::panel::kEncSpeed), 39.f, -105.f, 105.f);
    iconBox(g, Rectangle<float>(24.f, 17.f).withCentre({247.f, 264.f}), true);
    speedometer(g, {247.f, 264.f});

    // ---- start / end window ----
    {
        const auto start = Layout::encoder(emu::panel::kEncStart);
        const auto end   = Layout::encoder(emu::panel::kEncEnd);
        arcArrow(g, start, 39.f, -105.f, 62.f);
        arcArrow(g, end, 39.f, 105.f, -62.f);

        g.setColour(kSilk);
        Path frame;
        frame.startNewSubPath(start.x - 37.7f, 230.f);
        frame.lineTo(start.x - 37.7f, 268.f);
        frame.quadraticTo(start.x - 37.7f, 282.f, start.x - 23.f, 282.f);
        frame.lineTo(end.x + 23.f, 282.f);
        frame.quadraticTo(end.x + 37.7f, 282.f, end.x + 37.7f, 268.f);
        frame.lineTo(end.x + 37.7f, 230.f);
        g.strokePath(frame, PathStrokeType(kSilkStroke));
        waveformStrip(g, start.x - 14.f, end.x + 14.f, 268.f);
    }

    // ---- magic ----
    arcArrow(g, Layout::encoder(emu::panel::kEncMagic), 39.f, -105.f, 105.f);
    iconBox(g, Rectangle<float>(24.f, 17.f).withCentre({583.f, 264.f}), true);
    wand(g, {583.f, 264.f});

    // ---- looper: purple knob ring, tape deck ----
    {
        const auto looper = Layout::encoder(emu::panel::kEncLooper);
        arcArrow(g, looper, 73.f, -140.f, -40.f);
        chevrons(g, {looper.x, 144.f});

        g.setColour(Colour(0xff0d0d0d));
        g.fillEllipse(Rectangle<float>(124.f, 124.f).withCentre(looper));
        g.setColour(kGold);
        g.drawEllipse(Rectangle<float>(118.f, 118.f).withCentre(looper), 7.f);

        // After the ring: the ring overlaps this frame, and must merge into its
        // gold rather than show inside it.
        const auto deckFrame = keyFrame(g, functionKeyBounds(Layout::kPlayKey).getUnion(functionKeyBounds(Layout::kLoopKey)));

        g.setColour(kSilk);
        g.drawRoundedRectangle(Rectangle<float>(786.f, 127.f, 111.f, 34.f), 16.f, kSilkStroke);
        g.drawLine(800.f, 161.f, 800.f, deckFrame.getY(), kSilkStroke);
        g.drawLine(883.f, 161.f, 883.f, deckFrame.getY(), kSilkStroke);
        tapeWave(g, {841.5f, 144.f});
    }

    // ---- volume, mic, character ----
    grille(g, {949.f, 152.f});
    {
        g.setColour(kSilk);
        Path wire;
        wire.startNewSubPath(949.f, 166.f);
        wire.lineTo(949.f, 200.f);
        wire.quadraticTo(949.f, 207.f, 955.f, 210.f);
        g.strokePath(wire, PathStrokeType(kSilkStroke));
        micGlyph(g, {961.f, 220.f});
        boltGlyph(g, {1057.f, 220.f});

        Path wire2;
        wire2.startNewSubPath(1057.f, 229.f);
        wire2.lineTo(1057.f, 268.f);
        wire2.lineTo(1029.f, 268.f);
        g.strokePath(wire2, PathStrokeType(kSilkStroke));
    }
    character(g, {1004.f, 292.f});
    g.setColour(kGold);
    g.setFont(FontOptions(11.5f, Font::bold));
    g.drawText("CHIMPO", Rectangle<float>(80.f, 14.f).withCentre({1004.f, 320.f}), Justification::centred);

    // ---- key numbers ----
    g.setColour(kSilk);
    g.setFont(FontOptions(13.f, Font::bold));
    for(int i = 0; i < 14; i++)
        g.drawText(String(i + 1), Rectangle<float>(30.f, 16.f).withCentre({Layout::whiteKey(i).x, 428.f}), Justification::centred);
    moons(g, {Layout::whiteKey(14).x - 4.f, 428.f}, 4.5f);
    plusCircle(g, {Layout::whiteKey(14).x + 9.f, 428.f}, 4.5f);
}

// ============================================================================
// Led

namespace
{
float bezelRadius(Led::Style s)
{
    switch(s)
    {
        case Led::Style::Large: return 13.f;
        case Led::Style::Small: return 9.f;
        case Led::Style::Reel: return 14.f;
    }
    return 13.f;
}
} // namespace

Led::Led(int index, Style style) : index_(index), style_(style)
{
    setInterceptsMouseClicks(false, false);
    const int size = int(std::ceil(bezelRadius(style) * 2.f * kGlowScale));
    setSize(size, size);
}

void Led::setColour(Colour c)
{
    if(c == colour_)
        return;
    colour_ = c;
    repaint();
}

void Led::paint(Graphics& g)
{
    const auto  c     = getLocalBounds().toFloat().getCentre();
    const float bezel = bezelRadius(style_);
    const float dome  = style_ == Style::Large ? 10.f : 7.f;

    switch(style_)
    {
        case Style::Large:
            g.setColour(kGold);
            g.fillEllipse(Rectangle<float>(bezel * 2, bezel * 2).withCentre(c));
            break;
        case Style::Small:
            g.setColour(Colour(0xff4a4a48));
            g.fillEllipse(Rectangle<float>(bezel * 2, bezel * 2).withCentre(c));
            break;
        case Style::Reel:
        {
            g.setColour(kGold);
            g.fillEllipse(Rectangle<float>(bezel * 2, bezel * 2).withCentre(c));
            g.setColour(Colour(0xff0d0d0d));
            for(int i = 0; i < 6; i++)
            {
                const float a = MathConstants<float>::twoPi * float(i) / 6.f;
                g.fillEllipse(Rectangle<float>(3.4f, 3.4f).withCentre(c + Point<float>(std::sin(a), std::cos(a)) * 11.f));
            }
            break;
        }
    }

    const Colour lit  = displayColour(colour_);
    const float  b    = brightness(lit);
    const auto   ring = Rectangle<float>(dome * 2, dome * 2).withCentre(c);

    if(b > 0.f)
    {
        glow(g, c, bezel * kGlowScale * 0.5f, lit, 0.55f * b);
        ColourGradient domeFill(lit.interpolatedWith(Colours::white, 0.55f), c.translated(-dome * 0.25f, -dome * 0.3f), lit, c.translated(dome, dome), true);
        g.setGradientFill(domeFill);
        g.fillEllipse(ring);
    }
    else
    {
        g.setGradientFill(ColourGradient(Colour(0xff5a5a58), c.translated(-dome * 0.4f, -dome * 0.4f), Colour(0xff1c1c1c), c.translated(dome, dome), true));
        g.fillEllipse(ring);
    }

    g.setColour(Colours::white.withAlpha(0.35f));
    g.fillEllipse(Rectangle<float>(dome * 0.7f, dome * 0.45f).withCentre(c.translated(-dome * 0.3f, -dome * 0.45f)));
}

// ============================================================================
// Key

Key::Key(Input& input, int button, Style style, int smtLed) : input_(input), button_(button), style_(style), smtLed_(smtLed)
{
    const float size = style == Style::White ? Layout::kWhiteKeySize
                       : style == Style::Black ? Layout::kBlackKeySize
                                               : Layout::kFunctionKey;
    setSize(int(size) + 24, int(size) + 24); // room for the backlight halo
    setMouseCursor(MouseCursor::PointingHandCursor);
}

void Key::setBacklight(Colour c)
{
    if(c == backlight_)
        return;
    backlight_ = c;
    repaint();
}

void Key::setDropTarget(bool target)
{
    if(target == dropTarget_)
        return;
    dropTarget_ = target;
    repaint();
}

Rectangle<float> Key::capBounds() const
{
    const float size = style_ == Style::White ? Layout::kWhiteKeySize
                       : style_ == Style::Black ? Layout::kBlackKeySize
                                                : Layout::kFunctionKey;
    return Rectangle<float>(size, size).withCentre(getLocalBounds().toFloat().getCentre());
}

bool Key::hitTest(int x, int y)
{
    return capBounds().contains(float(x), float(y));
}

void Key::paint(Graphics& g)
{
    const bool down = input_.isDown(button_);
    auto       cap  = capBounds();

    Colour top, bottom, edge, face;
    switch(style_)
    {
        case Style::White: top = Colour(0xfffbfbf9); bottom = Colour(0xffdcdcd7); edge = Colour(0xffa9a9a3); break;
        case Style::Black: top = Colour(0xff2e2e2e); bottom = Colour(0xff1a1a1a); edge = Colour(0xff050505); break;
        case Style::Chompi: top = kRedKey.brighter(0.15f); bottom = kRedKey.darker(0.25f); edge = kRedKey.darker(0.6f); break;
        case Style::Play: top = kTealKey.brighter(0.15f); bottom = kTealKey.darker(0.25f); edge = kTealKey.darker(0.6f); break;
        case Style::Loop: top = kYellowKey.brighter(0.1f); bottom = kYellowKey.darker(0.2f); edge = kYellowKey.darker(0.6f); break;
    }
    face = top;

    // Drop shadow / skirt, then the cap, pushed down a little while held.
    g.setColour(Colours::black.withAlpha(0.55f));
    g.fillRoundedRectangle(cap.translated(0, 3.f), 8.f);
    if(down)
        cap = cap.translated(0, 2.f);

    g.setGradientFill(ColourGradient(top, 0, cap.getY(), bottom, 0, cap.getBottom(), false));
    g.fillRoundedRectangle(cap, 8.f);
    g.setColour(edge);
    g.drawRoundedRectangle(cap, 8.f, 1.f);

    const auto inner = cap.reduced(4.f).withTrimmedBottom(3.f);
    g.setGradientFill(ColourGradient(face.brighter(0.04f), 0, inner.getY(), face.darker(0.04f), 0, inner.getBottom(), false));
    g.fillRoundedRectangle(inner, 6.f);
    if(down)
    {
        g.setColour(Colours::black.withAlpha(0.12f));
        g.fillRoundedRectangle(cap, 8.f);
    }

    const Point<float> c = cap.getCentre();
    switch(style_)
    {
        case Style::White:
        case Style::Black:
        {
            const auto slot = Rectangle<float>(16.f, 7.f).withCentre({c.x, cap.getY() + 14.f});
            // Firmware lights a held key white, invisible on a white cap: show neutral
            // backlights there in the CHOMPI key's coral, at the same level.
            const auto& bl    = backlight_;
            const bool  coral = style_ == Style::White && bl.getRed() == bl.getGreen() && bl.getGreen() == bl.getBlue();
            const float level = bl.getFloatRed();
            const Colour lit  = displayColour(coral ? Colour::fromFloatRGBA(kRedKey.getFloatRed() * level,
                                                                            kRedKey.getFloatGreen() * level,
                                                                            kRedKey.getFloatBlue() * level, 1.f)
                                                    : bl);
            const float b = brightness(lit);
            if(b > 0.f)
            {
                g.setColour(lit.withAlpha(0.13f * b));
                g.fillRoundedRectangle(cap, 8.f);
                glow(g, slot.getCentre(), 22.f, lit, 0.6f * b);
                g.setColour(coral ? lit : lit.interpolatedWith(Colours::white, 0.35f));
            }
            else
                g.setColour(style_ == Style::White ? Colour(0xff4b4b4a) : Colour(0xff4d4d4d));
            g.fillRoundedRectangle(slot, 3.f);
            break;
        }
        case Style::Chompi:
        {
            // Face: two big eyes, small mouth, cheeks.
            for(float dx : {-11.f, 11.f})
            {
                const auto eye = Rectangle<float>(15.f, 15.f).withCentre(c.translated(dx, -5.f));
                g.setColour(Colour(0xfffff3ef));
                g.fillEllipse(eye);
                g.setColour(Colour(0xff2a1210));
                g.drawEllipse(eye, 1.6f);
                g.fillEllipse(eye.reduced(4.6f).translated(1.f, 0.8f));
            }
            g.setColour(Colour(0xff2a1210));
            Path mouth;
            mouth.addCentredArc(c.x - 2.5f, c.y + 8.f, 2.5f, 2.5f, 0.f, MathConstants<float>::halfPi, MathConstants<float>::halfPi * 3.f, true);
            mouth.addCentredArc(c.x + 2.5f, c.y + 8.f, 2.5f, 2.5f, 0.f, MathConstants<float>::halfPi, MathConstants<float>::halfPi * 3.f, true);
            g.strokePath(mouth, PathStrokeType(1.5f));
            g.setColour(Colour(0xffb3302a));
            for(float dx : {-17.f, 17.f})
                for(float k : {-2.5f, 0.f, 2.5f})
                    g.drawLine(c.x + dx + k - 1.2f, c.y + 9.f, c.x + dx + k + 1.2f, c.y + 6.f, 1.2f);
            break;
        }
        case Style::Play:
        {
            g.setColour(Colour(0xff0e1414));
            Path tri;
            tri.addTriangle(c.x - 13.f, c.y - 10.f, c.x - 13.f, c.y + 10.f, c.x + 3.f, c.y);
            g.fillPath(tri);
            g.fillRoundedRectangle(c.x + 5.f, c.y - 10.f, 4.5f, 20.f, 1.f);
            g.fillRoundedRectangle(c.x + 11.5f, c.y - 10.f, 4.5f, 20.f, 1.f);
            break;
        }
        case Style::Loop:
        {
            // Repeat symbol: two arrows chasing round a rounded rectangle.
            g.setColour(Colour(0xff15120a));
            const float w = 13.f, h = 8.f, r = 6.f;
            Path loop;
            loop.startNewSubPath(c.x - w, c.y + 2.f);
            loop.lineTo(c.x - w, c.y - h + r);
            loop.quadraticTo(c.x - w, c.y - h, c.x - w + r, c.y - h);
            loop.lineTo(c.x + w - 7.f, c.y - h);
            loop.startNewSubPath(c.x + w, c.y - 2.f);
            loop.lineTo(c.x + w, c.y + h - r);
            loop.quadraticTo(c.x + w, c.y + h, c.x + w - r, c.y + h);
            loop.lineTo(c.x - w + 7.f, c.y + h);
            g.strokePath(loop, PathStrokeType(3.2f, PathStrokeType::curved, PathStrokeType::rounded));
            Path heads;
            heads.addTriangle(c.x + w + 1.f, c.y - h, c.x + w - 8.f, c.y - h - 5.5f, c.x + w - 8.f, c.y - h + 5.5f);
            heads.addTriangle(c.x - w - 1.f, c.y + h, c.x - w + 8.f, c.y + h - 5.5f, c.x - w + 8.f, c.y + h + 5.5f);
            g.fillPath(heads);
            break;
        }
    }

    if(input_.isLatched(button_))
    {
        g.setColour(kGold);
        g.drawRoundedRectangle(cap.expanded(3.f), 10.f, 2.f);
    }
    if(dropTarget_)
    {
        g.setColour(Colours::white.withAlpha(0.9f));
        g.drawRoundedRectangle(cap.expanded(6.f), 12.f, 2.5f);
    }
}

void Key::mouseDown(const MouseEvent& e)
{
    // Option/Alt-click (or right-click) latches the key down, for combinations
    // a single mouse can't hold (CHOMPI + key, play + loop).
    if(e.mods.isAltDown() || e.mods.isPopupMenu())
    {
        input_.set(button_, Input::Latch, !input_.isLatched(button_));
        return;
    }
    input_.set(button_, Input::Mouse, true);
}

void Key::mouseUp(const MouseEvent&)
{
    input_.set(button_, Input::Mouse, false);
}

// ============================================================================
// Knob

Knob::Knob(Input& input, int encoder, Style style, float pixelsPerDetent)
: input_(input), encoder_(encoder), style_(style), pixelsPerDetent_(pixelsPerDetent)
{
    const float r = style == Style::Purple ? Layout::kPurpleCap : Layout::kKnobSkirt;
    setSize(int(r * 2.f) + 6, int(r * 2.f) + 6);
    setMouseCursor(MouseCursor::UpDownResizeCursor);
}

static void drawPointer(Graphics& g, Point<float> c, float angle, float from, float to, float width)
{
    const auto dir = Point<float>(std::sin(angle), -std::cos(angle));
    Path       p;
    p.startNewSubPath(c + dir * from);
    p.lineTo(c + dir * to);
    g.setColour(Colours::white.withAlpha(0.85f));
    g.strokePath(p, PathStrokeType(width, PathStrokeType::curved, PathStrokeType::rounded));
}

void Knob::paint(Graphics& g)
{
    const auto c = getLocalBounds().toFloat().getCentre();

    if(style_ == Style::Small)
    {
        const float skirt = Layout::kKnobSkirt;
        g.setColour(Colours::black.withAlpha(0.6f));
        g.fillEllipse(Rectangle<float>(skirt * 2, skirt * 2).withCentre(c.translated(0, 2.f)));
        g.setGradientFill(ColourGradient(Colour(0xff353535), c.translated(-skirt, -skirt), Colour(0xff0c0c0c), c.translated(skirt, skirt), false));
        g.fillEllipse(Rectangle<float>(skirt * 2, skirt * 2).withCentre(c));

        // Knurling turns with the knob.
        g.setColour(Colours::white.withAlpha(0.07f));
        for(int i = 0; i < 36; i++)
        {
            const float a  = angle_ + MathConstants<float>::twoPi * float(i) / 36.f;
            const auto  dir = Point<float>(std::sin(a), -std::cos(a));
            g.drawLine(Line<float>(c + dir * (skirt - 4.f), c + dir * (skirt - 0.5f)), 1.f);
        }

        // The knurling alone is too faint to read rotation.
        const float cap = 19.f;
        drawPointer(g, c, angle_, cap + 2.5f, skirt - 3.f, 2.5f);

        g.setColour(Colours::black.withAlpha(0.5f));
        g.fillEllipse(Rectangle<float>(cap * 2, cap * 2).withCentre(c.translated(0, 1.5f)));
        g.setGradientFill(ColourGradient(Colours::white, c.translated(-cap * 0.5f, -cap * 0.6f), Colour(0xffc4c4c2), c.translated(cap, cap), true));
        g.fillEllipse(Rectangle<float>(cap * 2, cap * 2).withCentre(c));
        g.setColour(Colour(0xffb8b8b6));
        g.drawEllipse(Rectangle<float>(cap * 2, cap * 2).withCentre(c), 0.8f);
        return;
    }

    const float r = Layout::kPurpleCap;
    g.setColour(Colours::black.withAlpha(0.55f));
    g.fillEllipse(Rectangle<float>(r * 2, r * 2).withCentre(c.translated(0, 2.5f)));
    ColourGradient body(kPurple.brighter(0.55f), c.translated(-r * 0.35f, -r * 0.45f), kPurple.darker(0.55f), c.translated(r * 0.8f, r * 0.9f), true);
    body.addColour(0.45, kPurple);
    g.setGradientFill(body);
    g.fillEllipse(Rectangle<float>(r * 2, r * 2).withCentre(c));
    g.setColour(kPurple.darker(0.8f));
    g.drawEllipse(Rectangle<float>(r * 2, r * 2).withCentre(c).reduced(0.5f), 1.f);

    // Dimple on the cap shows rotation.
    const auto dir    = Point<float>(std::sin(angle_), -std::cos(angle_));
    const auto dimple = c + dir * (r * 0.6f);
    g.setGradientFill(ColourGradient(kPurple.darker(0.6f), dimple.translated(-3, -3), kPurple.brighter(0.3f), dimple.translated(4, 4), true));
    g.fillEllipse(Rectangle<float>(13.f, 13.f).withCentre(dimple));

    g.setColour(Colours::white.withAlpha(0.28f));
    g.fillEllipse(Rectangle<float>(r * 0.7f, r * 0.45f).withCentre(c.translated(-r * 0.32f, -r * 0.48f)));

    // The dimple alone is too low-contrast to read rotation.
    drawPointer(g, c, angle_, r * 0.76f, r * 0.92f, 3.f);
}

void Knob::emit(float detents)
{
    pending_ += detents;
    const int whole = int(pending_);
    if(whole == 0)
        return;
    pending_ -= float(whole);
    input_.turn(encoder_, whole);

    const float degreesPerDetent = style_ == Style::Purple ? 4.f : 10.f;
    angle_ += degreesToRadians(degreesPerDetent) * float(whole) / (pixelsPerDetent_ < 2.f ? 3.f : 1.f);
    repaint();
}

void Knob::mouseDown(const MouseEvent& e)
{
    dragged_  = false;
    lastDrag_ = e.getPosition();
    pending_  = 0;
}

void Knob::mouseDrag(const MouseEvent& e)
{
    if(e.getDistanceFromDragStart() > 3)
        dragged_ = true;
    if(!dragged_)
        return;

    const auto  pos   = e.getPosition();
    const float delta = float((pos.x - lastDrag_.x) - (pos.y - lastDrag_.y));
    lastDrag_         = pos;
    emit(delta / pixelsPerDetent_);
}

void Knob::mouseUp(const MouseEvent&)
{
    if(dragged_)
        return;
    // A click is a push of the encoder.
    const int sw = emu::panel::kEncoderSwitch[encoder_];
    input_.set(sw, Input::Mouse, true);
    input_.set(sw, Input::Mouse, false);
}

void Knob::mouseWheelMove(const MouseEvent&, const MouseWheelDetails& wheel)
{
    const float dir = wheel.isReversed ? -1.f : 1.f;
    emit(dir * wheel.deltaY * 120.f / pixelsPerDetent_);
}

// ============================================================================
// Toggle

Toggle::Toggle(Input& input) : input_(input)
{
    setSize(30, 46);
    setMouseCursor(MouseCursor::PointingHandCursor);
}

void Toggle::paint(Graphics& g)
{
    const auto c     = getLocalBounds().toFloat().getCentre();
    const auto plate = Rectangle<float>(24.f, 40.f).withCentre(c);
    g.setGradientFill(ColourGradient(Colour(0xffdcb862), plate.getX(), plate.getY(), Colour(0xff94722b), plate.getRight(), plate.getBottom(), false));
    g.fillRoundedRectangle(plate, 3.f);
    g.setColour(Colour(0xff15120b));
    g.fillRoundedRectangle(Rectangle<float>(10.f, 30.f).withCentre(c), 2.f);

    const bool play  = input_.togglePlay();
    const auto lever = Rectangle<float>(14.f, 16.f).withCentre(c.translated(0, play ? -7.f : 7.f));
    g.setGradientFill(ColourGradient(Colour(0xff3d3d3d), lever.getX(), lever.getY(), Colour(0xff0b0b0b), lever.getX(), lever.getBottom(), false));
    g.fillRoundedRectangle(lever, 2.5f);
    g.setColour(Colours::white.withAlpha(0.18f));
    g.drawLine(lever.getX() + 2.f, lever.getY() + 2.f, lever.getRight() - 2.f, lever.getY() + 2.f, 1.f);
}

void Toggle::mouseDown(const MouseEvent&)
{
    input_.setTogglePlay(!input_.togglePlay());
    repaint();
}

} // namespace panel_ui
