#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>
#include <cmath>

struct DenoiseState;   // RNNoise neural network state (defined in rnnoise.h)

//==============================================================================
// A simple, real-time-safe biquad (RBJ peaking EQ used as a variable-depth notch).
// Coefficients AND states are double precision: essential for very high-Q
// (ultra-narrow) notches, where float32 poles sit too close to the unit circle.
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    void reset() noexcept { z1 = z2 = 0.0; }

    // Negative gainDb produces a narrow cut at 'freq' — exactly what we need
    void setPeak (double fs, double freq, double Q, double gainDb) noexcept
    {
        const double A  = std::pow (10.0, gainDb / 40.0);
        const double w0 = juce::MathConstants<double>::twoPi * freq / fs;
        const double cw = std::cos (w0);
        const double al = std::sin (w0) / (2.0 * Q);
        const double a0 = 1.0 + al / A;

        b0 = (1.0 + al * A) / a0;
        b1 = (-2.0 * cw)    / a0;
        b2 = (1.0 - al * A) / a0;
        a1 = (-2.0 * cw)    / a0;
        a2 = (1.0 - al / A) / a0;
    }

    float processSample (float x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float) y;
    }

    // 2nd-order Butterworth high-pass (low cut) — RBJ cookbook
    void setHighPass (double fs, double freq, double Q = 0.70710678) noexcept
    {
        const double w0 = juce::MathConstants<double>::twoPi * freq / fs;
        const double cw = std::cos (w0);
        const double al = std::sin (w0) / (2.0 * Q);
        const double a0 = 1.0 + al;
        b0 = ((1.0 + cw) * 0.5) / a0;
        b1 = (-(1.0 + cw))      / a0;
        b2 = ((1.0 + cw) * 0.5) / a0;
        a1 = (-2.0 * cw)        / a0;
        a2 = (1.0 - al)         / a0;
    }

    // 2nd-order Butterworth low-pass (high cut) — RBJ cookbook
    void setLowPass (double fs, double freq, double Q = 0.70710678) noexcept
    {
        const double w0 = juce::MathConstants<double>::twoPi * freq / fs;
        const double cw = std::cos (w0);
        const double al = std::sin (w0) / (2.0 * Q);
        const double a0 = 1.0 + al;
        b0 = ((1.0 - cw) * 0.5) / a0;
        b1 = (1.0 - cw)         / a0;
        b2 = ((1.0 - cw) * 0.5) / a0;
        a1 = (-2.0 * cw)        / a0;
        a2 = (1.0 - al)         / a0;
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
    ~DeHowlProcessor() override;

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
    void requestResetLearning() noexcept { resetLearnRequest.store (true); }

    // Status for the GUI: 0 = AI off, 1 = AI active, 2 = AI needs 48000 Hz
    std::atomic<int> aiStatus  { 0 };
    std::atomic<int> roomCuts  { 0 };   // how many learned room-EQ cuts are active

    // Lock-free snapshot of the notch bank, read by the editor at ~10 Hz
    std::array<std::atomic<float>, kMaxNotches> displayFreq  {};
    std::array<std::atomic<float>, kMaxNotches> displayDepth {};

    // Peak levels for the GUI meters (max-accumulated, editor resets them)
    std::atomic<float> inPeak  { 0.0f };
    std::atomic<float> outPeak { 0.0f };

    juce::AudioProcessorValueTreeState apvts;

private:
    //==============================================================================
    // Analysis: 8192-point FFT -> ~5.9 Hz per bin at 48 kHz, refined further by
    // parabolic interpolation (sub-Hz). 75% overlap keeps reaction time at ~43 ms.
    static constexpr int fftOrder = 13;
    static constexpr int fftSize  = 1 << fftOrder;   // 8192 samples
    static constexpr int hopSize  = 2048;            // analyse every 2048 samples

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
    std::atomic<bool> clearRequest      { false };
    std::atomic<bool> resetLearnRequest { false };

    //==============================================================================
    // Vocal band: low-cut (HPF) and high-cut (LPF) ahead of everything else
    Biquad hpFilt[2], lpFilt[2];
    float  curHpFreq = -1.0f, curLpFreq = -1.0f;

    //==============================================================================
    // AI noise/clutter reduction (RNNoise neural network, 48 kHz, 480-sample frames)
    static constexpr int kRnFrame = 480;
    struct RnChannel
    {
        DenoiseState* state = nullptr;
        std::array<float, (size_t) kRnFrame> pending {};   // dry samples being collected
        std::array<float, (size_t) kRnFrame> done {};      // last processed frame
        int pos = 0;
    };
    RnChannel rn[2];
    bool rnSampleRateOk = false;      // network is trained for 48 kHz only
    void createRnStates();
    void destroyRnStates();

    //==============================================================================
    // Room learning: long-term average spectrum -> gentle corrective EQ
    // (targets persistent resonances: "hollow"/boxy room modes & reflections)
    static constexpr int kMaxTones = 6;
    struct Tone
    {
        float  freqHz = 0.0f;
        float  targetDepth = 0.0f, currentDepth = 0.0f;   // dB of gentle cut
        bool   active = false;
        Biquad filt[2];
    };
    std::array<Tone, kMaxTones> tones;
    std::vector<float> roomAvgDb;     // EMA spectrum of the room
    std::vector<float> roomPrefix;    // prefix sums for envelope estimation
    int roomFrames = 0;
    int roomAssignCounter = 0;
    void updateRoomLearning();        // learn + (re)assign corrective cuts
    void updateTones();               // ramp depths, refresh coefficients
    void resetLearning();

    // Cached raw parameter pointers
    std::atomic<float>* pSensitivity = nullptr;
    std::atomic<float>* pMaxNotches  = nullptr;
    std::atomic<float>* pDepth       = nullptr;
    std::atomic<float>* pQ           = nullptr;
    std::atomic<float>* pMode        = nullptr;   // 0 = Latch, 1 = Auto Release
    std::atomic<float>* pOutput      = nullptr;
    std::atomic<float>* pBypass      = nullptr;
    std::atomic<float>* pAiClear     = nullptr;
    std::atomic<float>* pRoomLearn   = nullptr;
    std::atomic<float>* pLowCut      = nullptr;
    std::atomic<float>* pHighCut     = nullptr;

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
