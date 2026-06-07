#pragma once

#include "PluginProcessor.h"
#include "SystemCpu.h"

//==============================================================================
// DeHowl visual style: dark panel, red accent, clean arc knobs
class DeHowlLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DeHowlLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;
};

//==============================================================================
// Big labeled level meter (horizontal) with dB read-out
class LevelMeter : public juce::Component,
                   private juce::Timer
{
public:
    LevelMeter (std::atomic<float>& source, const juce::String& labelText)
        : src (source), label (labelText) { startTimerHz (24); }

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override
    {
        if (! isShowing())                 // window hidden/minimised: do nothing
            return;

        const float peak = src.exchange (0.0f);
        const float next = juce::jmax (peak, level * 0.86f);

        if (std::abs (next - level) > 1.0e-4f)   // repaint only on visible change
        {
            level = next;
            repaint();
        }
        else
        {
            level = next;
        }
    }

    std::atomic<float>& src;
    juce::String label;
    float level = 0.0f;
};

//==============================================================================
// Live read-out of which notches are currently engaged
class NotchPanel : public juce::Component,
                   private juce::Timer
{
public:
    explicit NotchPanel (DeHowlProcessor& p) : proc (p) { startTimerHz (10); }
    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override
    {
        if (! isShowing())
            return;

        // Repaint only if a notch actually changed since last frame
        bool changed = false;
        for (int i = 0; i < DeHowlProcessor::kMaxNotches; ++i)
        {
            const float f = proc.displayFreq [(size_t) i].load();
            const float d = proc.displayDepth[(size_t) i].load();
            if (std::abs (f - lastFreq[(size_t) i]) > 0.01f
                 || std::abs (d - lastDepth[(size_t) i]) > 0.05f)
            {
                lastFreq [(size_t) i] = f;
                lastDepth[(size_t) i] = d;
                changed = true;
            }
        }
        if (changed)
            repaint();
    }

    DeHowlProcessor& proc;
    std::array<float, DeHowlProcessor::kMaxNotches> lastFreq  {};
    std::array<float, DeHowlProcessor::kMaxNotches> lastDepth {};
};

//==============================================================================
class DeHowlEditor : public juce::AudioProcessorEditor,
                     private juce::Timer
{
public:
    explicit DeHowlEditor (DeHowlProcessor&);
    ~DeHowlEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    DeHowlProcessor& proc;
    DeHowlLookAndFeel lnf;

    juce::Slider sens, maxN, depth, q, out, lowCut, highCut;
    juce::Label  lSens, lMaxN, lDepth, lQ, lOut;
    juce::TextButton lowCutTgl { "Low Cut" }, highCutTgl { "High Cut" };
    juce::ComboBox  mode;
    juce::TextButton bypassBtn  { "Bypass" };
    juce::TextButton clearBtn   { "Clear Notches" };
    juce::TextButton devicesBtn { "Audio Devices" };
    juce::TextButton aiBtn      { "AI Clear Voice" };
    juce::TextButton roomBtn    { "Room Learn EQ" };
    juce::TextButton resetBtn   { "Reset Learning" };
    juce::TextButton afcBtn      { "AFC Predict" };
    juce::TextButton afcResetBtn { "Reset Model" };
    juce::Slider     afcShift;
    juce::Label      afcStatusLabel;
    juce::Label statusLabel;
    SystemCpu sysCpu;
    juce::TextEditor legalBox;
    bool showingLegal = false;
    void toggleLegalPanel();
    void refreshPanels();
    NotchPanel panel;
    LevelMeter inMeter, outMeter;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;  // standalone only
    juce::Viewport deviceView;                 // scrollable, so every option is reachable
    bool showingDevices = false;

    void toggleDevicePanel();

    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SliderAttachment>   aSens, aMaxN, aDepth, aQ, aOut, aLowCut, aHighCut, aAfcShift;
    std::unique_ptr<ComboBoxAttachment> aMode;
    std::unique_ptr<ButtonAttachment>   aBypass, aAi, aRoom, aLowCutOn, aHighCutOn, aAfc;

    void setupRotary (juce::Slider&, juce::Label&, const juce::String& name);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeHowlEditor)
};
