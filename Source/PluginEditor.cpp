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


static const char* kLegalText =
"DeHowl  -  Live Feedback Suppressor\n"
"Copyright (c) 2026 Shamaapps. Created by Kwadwo Gyebi. All rights reserved.\n"
"\n"
"LICENSE: Personal, non-exclusive license to install and use this software on\n"
"devices you own or control. Redistribution or sale without written permission\n"
"from Shamaapps is not permitted. Full EULA: see LICENSE.md in the project.\n"
"\n"
"PRIVACY: All audio is processed locally on this device. This software does\n"
"not record audio, collect personal data, contain analytics or advertising,\n"
"or transmit anything over the internet.\n"
"\n"
"NO WARRANTY: This software is provided \"AS IS\", without warranty of any\n"
"kind, express or implied, including merchantability and fitness for a\n"
"particular purpose. To the maximum extent permitted by law, Shamaapps shall\n"
"not be liable for damages arising from use of this software, including\n"
"damage to equipment, hearing injury, or event disruption.\n"
"\n"
"SOUND SAFETY: This software assists with, but does not replace, competent\n"
"operation of a sound system. The operator remains responsible for safe\n"
"sound levels at all times.\n"
"\n"
"=====================================================================\n"
"THIRD-PARTY SOFTWARE NOTICES\n"
"=====================================================================\n"
"\n"
"RNNoise (https://github.com/xiph/rnnoise) - BSD 3-Clause License\n"
"\n"
"Copyright (c) 2017, Mozilla\n"
"Copyright (c) 2007-2017, Jean-Marc Valin\n"
"Copyright (c) 2005-2017, Xiph.Org Foundation\n"
"Copyright (c) 2003-2004, Mark Borgerding\n"
"\n"
"Redistribution and use in source and binary forms, with or without\n"
"modification, are permitted provided that the following conditions are met:\n"
"- Redistributions of source code must retain the above copyright notice,\n"
"  this list of conditions and the following disclaimer.\n"
"- Redistributions in binary form must reproduce the above copyright notice,\n"
"  this list of conditions and the following disclaimer in the documentation\n"
"  and/or other materials provided with the distribution.\n"
"- Neither the name of the Xiph.Org Foundation nor the names of its\n"
"  contributors may be used to endorse or promote products derived from this\n"
"  software without specific prior written permission.\n"
"\n"
"THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\"\n"
"AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE\n"
"IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE\n"
"ARE DISCLAIMED. IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS BE LIABLE\n"
"FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL\n"
"DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR\n"
"SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER\n"
"CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT\n"
"LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY\n"
"OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH\n"
"DAMAGE.\n"
"\n"
"---------------------------------------------------------------------\n"
"JUCE Framework (https://juce.com)\n"
"Used under the JUCE 8 End User License Agreement\n"
"(https://juce.com/legal/juce-8-licence/). The JUCE splash screen is\n"
"displayed in compliance with the free-tier license terms.\n";

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
    g.setColour (kPanel);
    g.fillRoundedRectangle (r, 5.0f);

    auto labelArea = r.removeFromLeft (46.0f);
    g.setColour (kTextDim);
    g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    g.drawText (label, labelArea, juce::Justification::centred);

    const float db = juce::Decibels::gainToDecibels (level, -60.0f);
    auto dbArea = r.removeFromRight (72.0f);

    auto barBg = r.reduced (2.0f);
    g.setColour (kInset);
    g.fillRoundedRectangle (barBg, 4.0f);

    const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    auto bar = barBg.removeFromLeft (juce::jmax (0.0f, barBg.getWidth() * frac));
    g.setColour (db > -6.0f  ? kAccent
               : db > -14.0f ? juce::Colour (0xffe7b549)
               :               kGood);
    g.fillRoundedRectangle (bar, 4.0f);

    g.setColour (kText);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText (db <= -59.5f ? juce::String ("quiet")
                             : juce::String (db, 1) + " dB",
                dbArea, juce::Justification::centred);
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
    : AudioProcessorEditor (p), proc (p), panel (p),
      inMeter (p.inPeak, "IN"), outMeter (p.outPeak, "OUT")
{
    setLookAndFeel (&lnf);

    setupRotary (sens,  lSens,  "Sensitivity");
    setupRotary (maxN,  lMaxN,  "Max Notches");
    setupRotary (depth, lDepth, "Max Depth");
    setupRotary (q,     lQ,     "Width (Q)");
    setupRotary (out,   lOut,   "Output");
    setupRotary (lowCut,  lLowCut,  "Low Cut");
    setupRotary (highCut, lHighCut, "High Cut");
    lowCut.setTextValueSuffix (" Hz");
    highCut.setTextValueSuffix (" Hz");

    mode.addItemList (juce::StringArray { "Latch", "Auto Release" }, 1);
    addAndMakeVisible (mode);

    bypassBtn.setClickingTogglesState (true);
    bypassBtn.setColour (juce::TextButton::buttonOnColourId, kAccent);
    bypassBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    addAndMakeVisible (bypassBtn);

    aiBtn.setClickingTogglesState (true);
    aiBtn.setColour (juce::TextButton::buttonOnColourId, kGood.withAlpha (0.85f));
    aiBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    addAndMakeVisible (aiBtn);

    roomBtn.setClickingTogglesState (true);
    roomBtn.setColour (juce::TextButton::buttonOnColourId, kGood.withAlpha (0.85f));
    roomBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    addAndMakeVisible (roomBtn);

    resetBtn.onClick = [this] { proc.requestResetLearning(); };
    addAndMakeVisible (resetBtn);

    legalBtn.onClick = [this] { toggleLegalPanel(); };
    addAndMakeVisible (legalBtn);

    legalBox.setMultiLine (true);
    legalBox.setReadOnly (true);
    legalBox.setScrollbarsShown (true);
    legalBox.setCaretVisible (false);
    legalBox.setFont (juce::Font (juce::FontOptions (12.0f)));
    legalBox.setColour (juce::TextEditor::backgroundColourId, kPanel);
    legalBox.setColour (juce::TextEditor::textColourId, kText);
    legalBox.setColour (juce::TextEditor::outlineColourId, kInset);
    legalBox.setText (juce::String::fromUTF8 (kLegalText), false);
    addChildComponent (legalBox);

    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    statusLabel.setColour (juce::Label::textColourId, kTextDim);
    addAndMakeVisible (statusLabel);
    startTimerHz (4);

   #if JucePlugin_Build_Standalone
    // CRITICAL: JUCE standalone apps mute the audio input by default
    // "to avoid feedback loops". For a feedback suppressor that protection
    // is self-defeating — the engine would only ever hear silence.
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        holder->getMuteInputValue().setValue (false);
   #endif

    clearBtn.onClick = [this] { proc.requestClearNotches(); };
    addAndMakeVisible (clearBtn);

    // Device selection: only meaningful in the standalone app.
    // Every Core Audio interface (Focusrite, SSL, PreSonus, UA, Behringer,
    // MOTU, RME, built-in...) appears here automatically.
    devicesBtn.onClick = [this] { toggleDevicePanel(); };
    addAndMakeVisible (devicesBtn);
    devicesBtn.setVisible (getStandaloneDeviceManager() != nullptr);

    addAndMakeVisible (panel);
    addAndMakeVisible (inMeter);
    addAndMakeVisible (outMeter);
    addChildComponent (deviceView);
    deviceView.setScrollBarsShown (true, false);

    aSens  = std::make_unique<SliderAttachment>   (proc.apvts, "sensitivity", sens);
    aMaxN  = std::make_unique<SliderAttachment>   (proc.apvts, "maxNotches",  maxN);
    aDepth = std::make_unique<SliderAttachment>   (proc.apvts, "depth",       depth);
    aQ     = std::make_unique<SliderAttachment>   (proc.apvts, "q",           q);
    aOut   = std::make_unique<SliderAttachment>   (proc.apvts, "output",      out);
    aLowCut  = std::make_unique<SliderAttachment>  (proc.apvts, "lowCut",      lowCut);
    aHighCut = std::make_unique<SliderAttachment>  (proc.apvts, "highCut",     highCut);
    aMode  = std::make_unique<ComboBoxAttachment> (proc.apvts, "mode",        mode);
    aBypass = std::make_unique<ButtonAttachment>  (proc.apvts, "bypass",      bypassBtn);
    aAi     = std::make_unique<ButtonAttachment>  (proc.apvts, "aiClear",     aiBtn);
    aRoom   = std::make_unique<ButtonAttachment>  (proc.apvts, "roomLearn",   roomBtn);

    setSize (800, 664);
}

void DeHowlEditor::timerCallback()
{
    if (! isShowing())
        return;

    const int ai   = proc.aiStatus.load();
    const int cuts = proc.roomCuts.load();

    juce::String s;
    s << "AI: " << (ai == 1 ? "active" : ai == 2 ? "needs 48000 Hz sample rate!" : "off");
    s << "    Room EQ: " << cuts << (cuts == 1 ? " cut" : " cuts");

    if (s != statusLabel.getText())
        statusLabel.setText (s, juce::dontSendNotification);
}

void DeHowlEditor::refreshPanels()
{
    panel.setVisible (! showingDevices && ! showingLegal);
    deviceView.setVisible (showingDevices);
    legalBox.setVisible (showingLegal);
    devicesBtn.setButtonText (showingDevices ? "Back" : "Audio Devices");
    legalBtn.setButtonText (showingLegal ? "Back" : "Legal");
    resized();
}

void DeHowlEditor::toggleLegalPanel()
{
    showingLegal = ! showingLegal;
    if (showingLegal)
        showingDevices = false;
    refreshPanels();
}

void DeHowlEditor::toggleDevicePanel()
{
    if (deviceSelector == nullptr)
    {
        auto* dm = getStandaloneDeviceManager();
        if (dm == nullptr)
            return;

        deviceSelector = std::make_unique<juce::AudioDeviceSelectorComponent> (
                             *dm,
                             0, 2,      // input channels  (min, max)
                             0, 2,      // output channels (min, max)
                             false,     // MIDI inputs
                             false,     // MIDI output
                             true,      // stereo pairs
                             false);    // false = sample rate & buffer size always visible
        deviceSelector->setItemHeight (24);
        deviceView.setViewedComponent (deviceSelector.get(), false);
    }

    showingDevices = ! showingDevices;
    if (showingDevices)
        showingLegal = false;
    refreshPanels();
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

    // footer credit
    g.setColour (kTextDim.withAlpha (0.8f));
    g.setFont (juce::Font (juce::FontOptions (11.5f)));
    g.drawText (juce::String::fromUTF8 ("DeHowl v2.3  \xc2\xb7  created by Kwadwo Gyebi  \xc2\xb7  Shamaapps"),
                0, getHeight() - 22, getWidth(), 16, juce::Justification::centred);
}

void DeHowlEditor::resized()
{
    auto r = getLocalBounds().reduced (16);
    r.removeFromTop (48);   // header (painted directly)

    // big IN / OUT meters
    inMeter.setBounds  (r.removeFromTop (26));
    r.removeFromTop (8);
    outMeter.setBounds (r.removeFromTop (26));
    r.removeFromTop (12);

    auto knobRow = r.removeFromTop (132);
    const int w = knobRow.getWidth() / 7;

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
    place (lowCut,  lLowCut);
    place (highCut, lHighCut);

    r.removeFromTop (8);
    auto ctrlRow = r.removeFromTop (34);
    bypassBtn.setBounds (ctrlRow.removeFromLeft (100));
    ctrlRow.removeFromLeft (12);
    mode.setBounds     (ctrlRow.removeFromLeft (160));
    ctrlRow.removeFromLeft (12);
    clearBtn.setBounds (ctrlRow.removeFromLeft (130));
    ctrlRow.removeFromLeft (12);
    devicesBtn.setBounds (ctrlRow.removeFromLeft (130));
    ctrlRow.removeFromLeft (12);
    legalBtn.setBounds (ctrlRow.removeFromLeft (90));

    r.removeFromTop (8);
    auto aiRow = r.removeFromTop (34);
    aiBtn.setBounds    (aiRow.removeFromLeft (130));
    aiRow.removeFromLeft (12);
    roomBtn.setBounds  (aiRow.removeFromLeft (130));
    aiRow.removeFromLeft (12);
    resetBtn.setBounds (aiRow.removeFromLeft (130));
    aiRow.removeFromLeft (12);
    statusLabel.setBounds (aiRow);

    r.removeFromTop (12);
    r.removeFromBottom (18);   // footer credit (painted directly)
    panel.setBounds (r);
    deviceView.setBounds (r);
    legalBox.setBounds (r);
    if (deviceSelector != nullptr)
        deviceSelector->setSize (deviceView.getMaximumVisibleWidth(),
                                 juce::jmax (420, r.getHeight()));
}
