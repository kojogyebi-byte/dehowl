#include "PluginEditor.h"

#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace
{
    // Returns the standalone app's device manager, or nullptr when running
    // as a plug-in inside a DAW (where the host owns the audio devices).
    juce::AudioDeviceManager* getStandaloneDeviceManager()
    {
       #if JucePlugin_Build_Standalone
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
            return &holder->deviceManager;
       #endif
        return nullptr;
    }
}

namespace
{
    const juce::Colour kBg      (0xff14181f);
    const juce::Colour kPanel   (0xff1c222c);
    const juce::Colour kInset   (0xff262d3a);
    const juce::Colour kText    (0xffd7dde8);
    const juce::Colour kTextDim (0xff8b94a7);
    const juce::Colour kAccent  (0xffe5484d);
    const juce::Colour kGood    (0xff3dd6a3);
}

//==============================================================================
DeHowlLookAndFeel::DeHowlLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId,      kText);
    setColour (juce::Slider::textBoxOutlineColourId,   juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId,              kTextDim);
    setColour (juce::ComboBox::backgroundColourId,     kPanel);
    setColour (juce::ComboBox::outlineColourId,        kInset);
    setColour (juce::ComboBox::textColourId,           kText);
    setColour (juce::ComboBox::arrowColourId,          kAccent);
    setColour (juce::TextButton::buttonColourId,       kPanel);
    setColour (juce::TextButton::textColourOffId,      kText);
    setColour (juce::PopupMenu::backgroundColourId,    kPanel);
    setColour (juce::PopupMenu::textColourId,          kText);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, kAccent);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
}

void DeHowlLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                          float pos, float startAngle, float endAngle, juce::Slider&)
{
    auto bounds = juce::Rectangle<float> ((float) x, (float) y,
                                          (float) width, (float) height).reduced (6.0f);
    const float size   = juce::jmin (bounds.getWidth(), bounds.getHeight());
    auto square        = bounds.withSizeKeepingCentre (size, size);
    const auto centre  = square.getCentre();
    const float radius = size * 0.5f - 2.0f;
    const float angle  = startAngle + pos * (endAngle - startAngle);
    const float lineW  = juce::jmax (3.0f, radius * 0.18f);
    const float arcR   = radius - lineW * 0.5f;

    // knob face
    g.setColour (kPanel);
    const float faceR = arcR - lineW * 0.9f;
    g.fillEllipse (juce::Rectangle<float> (faceR * 2.0f, faceR * 2.0f).withCentre (centre));

    // background track
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
    g.setColour (kInset);
    g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // value arc
    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, angle, true);
    g.setColour (kAccent);
    g.strokePath (value, juce::PathStrokeType (lineW, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // pointer dot
    const juce::Point<float> tip (centre.x + arcR * std::sin (angle),
                                  centre.y - arcR * std::cos (angle));
    const float dotR = lineW * 0.62f;
    g.setColour (juce::Colours::white);
    g.fillEllipse (juce::Rectangle<float> (dotR * 2.0f, dotR * 2.0f).withCentre (tip));
}

//==============================================================================
void LevelMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (kInset);
    g.fillRoundedRectangle (r, 4.0f);

    const float db   = juce::Decibels::gainToDecibels (level, -60.0f);
    const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);

    auto bar = r.reduced (2.0f);
    bar = bar.removeFromLeft (juce::jmax (0.0f, bar.getWidth() * frac));
    g.setColour (db > -6.0f ? kAccent : kGood);
    g.fillRoundedRectangle (bar, 3.0f);
}

//==============================================================================
void NotchPanel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    g.setColour (kPanel);
    g.fillRoundedRectangle (bounds.toFloat(), 8.0f);

    g.setColour (kTextDim);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText ("ACTIVE NOTCHES", bounds.removeFromTop (22), juce::Justification::centred);

    const int n = DeHowlProcessor::kMaxNotches;
    auto area = getLocalBounds().reduced (10).withTrimmedTop (22);
    const float cw = (float) area.getWidth() / (float) n;

    int shown = 0;
    for (int i = 0; i < n; ++i)
    {
        const float f = proc.displayFreq [(size_t) i].load();
        const float d = proc.displayDepth[(size_t) i].load();

        juce::Rectangle<float> col ((float) area.getX() + (float) i * cw,
                                    (float) area.getY(),
                                    cw - 6.0f,
                                    (float) area.getHeight());

        auto barArea = col.withTrimmedBottom (18.0f);
        g.setColour (kInset);
        g.fillRoundedRectangle (barArea, 4.0f);

        if (f > 0.0f)
        {
            ++shown;
            const float h = juce::jlimit (0.05f, 1.0f, d / 30.0f) * barArea.getHeight();
            auto bar      = barArea.removeFromBottom (h);
            g.setColour (kAccent);
            g.fillRoundedRectangle (bar, 4.0f);

            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            const auto txt = f < 1000.0f ? juce::String ((int) f) + "Hz"
                                         : juce::String (f / 1000.0f, 1) + "k";
            g.drawText (txt, col.removeFromBottom (16.0f), juce::Justification::centred);
        }
    }

    if (shown == 0)
    {
        g.setColour (juce::Colours::darkgrey);
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText ("listening...", area, juce::Justification::centred);
    }
}

//==============================================================================
DeHowlEditor::DeHowlEditor (DeHowlProcessor& p)
    : AudioProcessorEditor (p), proc (p), panel (p), meter (p)
{
    setLookAndFeel (&lnf);

    setupRotary (sens,  lSens,  "Sensitivity");
    setupRotary (maxN,  lMaxN,  "Max Notches");
    setupRotary (depth, lDepth, "Max Depth");
    setupRotary (q,     lQ,     "Width (Q)");
    setupRotary (out,   lOut,   "Output");

    mode.addItemList (juce::StringArray { "Latch", "Auto Release" }, 1);
    addAndMakeVisible (mode);

    clearBtn.onClick = [this] { proc.requestClearNotches(); };
    addAndMakeVisible (clearBtn);

    // Device selection: only meaningful in the standalone app.
    // Every Core Audio interface (Focusrite, SSL, PreSonus, UA, Behringer,
    // MOTU, RME, built-in...) appears here automatically.
    devicesBtn.onClick = [this] { toggleDevicePanel(); };
    addAndMakeVisible (devicesBtn);
    devicesBtn.setVisible (getStandaloneDeviceManager() != nullptr);

    addAndMakeVisible (panel);
    addAndMakeVisible (meter);

    aSens  = std::make_unique<SliderAttachment>   (proc.apvts, "sensitivity", sens);
    aMaxN  = std::make_unique<SliderAttachment>   (proc.apvts, "maxNotches",  maxN);
    aDepth = std::make_unique<SliderAttachment>   (proc.apvts, "depth",       depth);
    aQ     = std::make_unique<SliderAttachment>   (proc.apvts, "q",           q);
    aOut   = std::make_unique<SliderAttachment>   (proc.apvts, "output",      out);
    aMode  = std::make_unique<ComboBoxAttachment> (proc.apvts, "mode",        mode);

    setSize (660, 540);
}

void DeHowlEditor::toggleDevicePanel()
{
    if (deviceSelector == nullptr)
    {
        auto* dm = getStandaloneDeviceManager();
        if (dm == nullptr)
            return;

        auto sel = std::make_unique<juce::AudioDeviceSelectorComponent> (
                       *dm,
                       0, 2,      // input channels  (min, max)
                       0, 2,      // output channels (min, max)
                       false,     // MIDI inputs
                       false,     // MIDI output
                       true,      // stereo pairs
                       false);    // show advanced options (sample rate, buffer size)
        sel->setItemHeight (22);
        deviceSelector = std::move (sel);
        addChildComponent (*deviceSelector);
    }

    showingDevices = ! showingDevices;
    deviceSelector->setVisible (showingDevices);
    panel.setVisible (! showingDevices);
    devicesBtn.setButtonText (showingDevices ? "Back" : "Audio Devices");
    resized();
}

DeHowlEditor::~DeHowlEditor()
{
    setLookAndFeel (nullptr);
}

void DeHowlEditor::setupRotary (juce::Slider& s, juce::Label& l, const juce::String& name)
{
    s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 18);
    addAndMakeVisible (s);

    l.setText (name, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setFont (juce::Font (juce::FontOptions (13.0f)));
    addAndMakeVisible (l);
}

void DeHowlEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);

    // small badge: the notch-curve logo
    juce::Rectangle<float> badge (16.0f, 12.0f, 34.0f, 34.0f);
    g.setColour (kAccent);
    g.fillRoundedRectangle (badge, 9.0f);

    juce::Path curve;
    const float yMid = badge.getCentreY() - 3.0f;
    curve.startNewSubPath (badge.getX() + 6.0f, yMid);
    curve.lineTo (badge.getCentreX() - 6.0f, yMid);
    curve.lineTo (badge.getCentreX(),        badge.getBottom() - 8.0f);
    curve.lineTo (badge.getCentreX() + 6.0f, yMid);
    curve.lineTo (badge.getRight() - 6.0f,   yMid);
    g.setColour (juce::Colours::white);
    g.strokePath (curve, juce::PathStrokeType (2.6f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
    g.drawText ("DeHowl", 60, 10, 300, 24, juce::Justification::left);

    g.setColour (kTextDim);
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.drawText ("Live Feedback Suppressor  |  zero latency", 60, 34, 320, 16,
                juce::Justification::left);

    g.setColour (kTextDim);
    g.setFont (juce::Font (juce::FontOptions (10.5f)));
    g.drawText ("OUT", getWidth() - 232, 18, 28, 12, juce::Justification::right);

    // footer credit
    g.setColour (kTextDim.withAlpha (0.8f));
    g.setFont (juce::Font (juce::FontOptions (11.5f)));
    g.drawText (juce::String::fromUTF8 ("DeHowl v1.2  \xc2\xb7  created by Kwadwo Gyebi  \xc2\xb7  Shamaapps"),
                0, getHeight() - 22, getWidth(), 16, juce::Justification::centred);
}

void DeHowlEditor::resized()
{
    meter.setBounds (getWidth() - 196, 18, 180, 12);

    auto r = getLocalBounds().reduced (16);
    r.removeFromTop (48);   // header (painted directly)

    auto knobRow = r.removeFromTop (132);
    const int w = knobRow.getWidth() / 5;

    auto place = [&] (juce::Slider& s, juce::Label& l)
    {
        auto a = knobRow.removeFromLeft (w).reduced (4);
        l.setBounds (a.removeFromTop (18));
        s.setBounds (a);
    };
    place (sens,  lSens);
    place (maxN,  lMaxN);
    place (depth, lDepth);
    place (q,     lQ);
    place (out,   lOut);

    r.removeFromTop (8);
    auto ctrlRow = r.removeFromTop (34);
    mode.setBounds     (ctrlRow.removeFromLeft (180));
    ctrlRow.removeFromLeft (12);
    clearBtn.setBounds (ctrlRow.removeFromLeft (140));
    ctrlRow.removeFromLeft (12);
    devicesBtn.setBounds (ctrlRow.removeFromLeft (140));

    r.removeFromTop (12);
    r.removeFromBottom (18);   // footer credit (painted directly)
    panel.setBounds (r);
    if (deviceSelector != nullptr)
        deviceSelector->setBounds (r);
}
