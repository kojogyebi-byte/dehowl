#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>
#include <cmath>

//==============================================================================
// A simple, real-time-safe biquad (RBJ peaking EQ used as a variable-depth notch)
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() noexcept { z1 = z2 = 0.0f; }

    // Negative gainDb produces a narrow cut at 'freq' — exactly what we need
    void setPeak (double fs, double freq, double Q, double gainDb) noexcept
    {
        const double A  = std::pow (10.0, gainDb / 40.0);
        const double w0 = juce::MathConstants<double>::twoPi * freq / fs;
        const double cw = std::cos (w0);
        const double al = std::sin (w0) / (2.0 * Q);
        const double a0 = 1.0 + al / A;

        b0 = (float) ((1.0 + al * A) / a0);
        b1 = (float) ((-2.0 * cw)    / a0);
        b2 = (float) ((1.0 - al * A) / a0);
        a1 = (float) ((-2.0 * cw)    / a0);
        a2 = (float) ((1.0 - al / A) / a0);
    }

    float processSample (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

//==============================================================================
/*
    DeHowl — adaptive feedback suppressor.

    Audio path:  input -> bank of up to 12 narrow notch filters -> output gain.
                 Pure IIR filtering = ZERO added latency.

    Detection:   a 4096-point FFT runs in parallel on a mono mix of the input.
                 A frequency is flagged as feedback when it is a strong, narrow
                 peak that stands well above the surrounding spectrum AND
                 persists across several analysis frames (real music rarely
                 holds a single pure tone that dominates this way; feedback does).
*/
class DeHowlProcessor : public juce::AudioProcessor
{
public:
    static constexpr int kMaxNotches = 12;

    DeHowlProcessor();
    ~DeHowlProcessor() override = default;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    using AudioProcessor::processBlock;   // keep the double-precision overload visible
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    //==============================================================================
    const juce::String getName() const override { return "DeHowl"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Called from the editor ("Clear Notches" button) — real-time safe
    void requestClearNotches() noexcept { clearRequest.store (true); }

    // Lock-free snapshot of the notch bank, read by the editor at ~10 Hz
    std::array<std::atomic<float>, kMaxNotches> displayFreq  {};
    std::array<std::atomic<float>, kMaxNotches> displayDepth {};

    // Output peak level for the GUI meter (max-accumulated, editor resets it)
    std::atomic<float> outPeak { 0.0f };

    juce::AudioProcessorValueTreeState apvts;

private:
    //==============================================================================
    // Analysis
    static constexpr int fftOrder = 12;
    static constexpr int fftSize  = 1 << fftOrder;   // 4096 samples
    static constexpr int hopSize  = fftSize / 2;     // analyse every 2048 samples

    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize,
                                                 juce::dsp::WindowingFunction<float>::hann };

    std::array<float, (size_t) fftSize>     ring {};
    int ringPos = 0, hopCount = 0;
    std::array<float, (size_t) fftSize * 2> fftData {};
    std::vector<float> magDb;
    std::vector<float> prefixSum;      // running sum of magDb for O(1) local averages
    float hopPeak = 0.0f;              // peak |sample| in the current hop (silence gate)

    struct Candidate
    {
        int   bin = 0;
        int   frames = 0;
        bool  seen = false;
        float firstDb = -120.0f;       // level when first detected (growth tracking)
        float lastDb  = -120.0f;
    };
    std::vector<Candidate> candidates;

    //==============================================================================
    // Notch bank
    struct Notch
    {
        float  freqHz = 0.0f;
        float  targetDepth = 0.0f, currentDepth = 0.0f;   // dB of cut (positive numbers)
        int    framesSinceTrigger = 0;
        bool   active = false;
        Biquad filt[2];                                   // L / R states (same coeffs)
    };
    std::array<Notch, kMaxNotches> notches;

    double sr = 48000.0;
    int    holdFrames = 200;          // ~8 s before auto-release starts
    std::atomic<bool> clearRequest { false };

    // Cached raw parameter pointers
    std::atomic<float>* pSensitivity = nullptr;
    std::atomic<float>* pMaxNotches  = nullptr;
    std::atomic<float>* pDepth       = nullptr;
    std::atomic<float>* pQ           = nullptr;
    std::atomic<float>* pMode        = nullptr;   // 0 = Latch, 1 = Auto Release
    std::atomic<float>* pOutput      = nullptr;

    //==============================================================================
    void analyse();
    void registerCandidate (int bin, float levelDb);
    void promoteCandidates();
    bool hasStrongHarmonics (int bin) const;
    void triggerNotch (float freqHz);
    void updateNotches();
    void clearAllNotches();
    void publishDisplay();

    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeHowlProcessor)
};
