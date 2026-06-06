#include "PluginProcessor.h"
#include "PluginEditor.h"

extern "C"
{
 #include <rnnoise.h>
}

//==============================================================================
DeHowlProcessor::~DeHowlProcessor()
{
    destroyRnStates();
}

void DeHowlProcessor::createRnStates()
{
    destroyRnStates();
    for (auto& c : rn)
    {
        c.state = rnnoise_create (nullptr);   // nullptr = built-in trained model
        c.pending.fill (0.0f);
        c.done.fill (0.0f);
        c.pos = 0;
    }
}

void DeHowlProcessor::destroyRnStates()
{
    for (auto& c : rn)
    {
        if (c.state != nullptr)
        {
            rnnoise_destroy (c.state);
            c.state = nullptr;
        }
    }
}

//==============================================================================
DeHowlProcessor::DeHowlProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    pSensitivity = apvts.getRawParameterValue ("sensitivity");
    pMaxNotches  = apvts.getRawParameterValue ("maxNotches");
    pDepth       = apvts.getRawParameterValue ("depth");
    pQ           = apvts.getRawParameterValue ("q");
    pMode        = apvts.getRawParameterValue ("mode");
    pOutput      = apvts.getRawParameterValue ("output");
    pBypass      = apvts.getRawParameterValue ("bypass");
    pAiClear     = apvts.getRawParameterValue ("aiClear");
    pRoomLearn   = apvts.getRawParameterValue ("roomLearn");
    pLowCut      = apvts.getRawParameterValue ("lowCut");
    pHighCut     = apvts.getRawParameterValue ("highCut");
    pLowCutOn    = apvts.getRawParameterValue ("lowCutOn");
    pHighCutOn   = apvts.getRawParameterValue ("highCutOn");

    magDb.resize ((size_t) fftSize / 2 + 1, -120.0f);
    prefixSum.resize ((size_t) fftSize / 2 + 2, 0.0f);
    roomAvgDb.resize ((size_t) fftSize / 2 + 1, -120.0f);
    roomPrefix.resize ((size_t) fftSize / 2 + 2, 0.0f);
    candidates.reserve (64);
}

juce::AudioProcessorValueTreeState::ParameterLayout DeHowlProcessor::createLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;

    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "sensitivity", 1 }, "Sensitivity",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 60.0f));

    p.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "maxNotches", 1 }, "Max Notches", 1, kMaxNotches, 8));

    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "depth", 1 }, "Max Depth (dB)",
        juce::NormalisableRange<float> (6.0f, 30.0f, 0.5f), 18.0f));

    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "q", 1 }, "Notch Width (Q)",
        juce::NormalisableRange<float> (10.0f, 200.0f, 1.0f), 30.0f));

    p.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "mode", 1 }, "Mode",
        juce::StringArray { "Latch", "Auto Release" }, 0));

    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "output", 1 }, "Output (dB)",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f));

    p.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    p.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "aiClear", 1 }, "AI Clear Voice", false));

    p.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "roomLearn", 1 }, "Room Learning EQ", true));

    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "lowCut", 1 }, "Low Cut (Hz)",
        juce::NormalisableRange<float> (20.0f, 400.0f, 1.0f, 0.5f), 120.0f));

    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "highCut", 1 }, "High Cut (Hz)",
        juce::NormalisableRange<float> (2000.0f, 20000.0f, 10.0f, 0.5f), 9000.0f));

    p.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "lowCutOn", 1 }, "Low Cut On", false));

    p.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "highCutOn", 1 }, "High Cut On", false));

    return { p.begin(), p.end() };
}

//==============================================================================
void DeHowlProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    holdFrames = (int) std::ceil (8.0 * sr / (double) hopSize);   // ~8 seconds

    ring.fill (0.0f);
    ringPos = 0;
    hopCount = 0;
    candidates.clear();
    clearAllNotches();

    for (int ch = 0; ch < 2; ++ch) { hpFilt[ch].reset(); lpFilt[ch].reset(); }
    curHpFreq = curLpFreq = -1.0f;   // force coefficient recompute

    // AI denoise: the RNNoise network is trained for 48 kHz audio
    rnSampleRateOk = std::abs (sr - 48000.0) < 1.0;
    createRnStates();

    resetLearning();
    publishDisplay();
}

void DeHowlProcessor::resetLearning()
{
    std::fill (roomAvgDb.begin(), roomAvgDb.end(), -120.0f);
    roomFrames = 0;
    roomAssignCounter = 0;
    for (auto& t : tones)
    {
        t.active = false;
        t.freqHz = 0.0f;
        t.targetDepth = t.currentDepth = 0.0f;
        t.filt[0].reset();
        t.filt[1].reset();
    }
    roomCuts.store (0);
}

bool DeHowlProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

//==============================================================================
void DeHowlProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    if (clearRequest.exchange (false))
    {
        clearAllNotches();
        publishDisplay();
    }
    if (resetLearnRequest.exchange (false))
        resetLearning();

    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, numSamples);

    // ---- input meter ----
    float inMag = buffer.getMagnitude (0, 0, numSamples);
    if (numIn > 1)
        inMag = juce::jmax (inMag, buffer.getMagnitude (1, 0, numSamples));
    if (inMag > inPeak.load())
        inPeak.store (inMag);

    // ---- bypass: pass audio through untouched ----
    if (pBypass->load() > 0.5f)
    {
        if (inMag > outPeak.load())
            outPeak.store (inMag);
        return;
    }

    // ---- vocal band: low cut / high cut (runs BEFORE the detector, so
    //      rumble and hiss are removed from both the sound and the analysis) ----
    {
        const float hpF = pLowCut->load();
        const float lpF = pHighCut->load();

        if (std::abs (hpF - curHpFreq) > 0.5f)
        {
            curHpFreq = hpF;
            hpFilt[0].setHighPass (sr, (double) hpF);
            hpFilt[1].setHighPass (sr, (double) hpF);
        }
        if (std::abs (lpF - curLpFreq) > 0.5f)
        {
            curLpFreq = lpF;
            lpFilt[0].setLowPass (sr, (double) lpF);
            lpFilt[1].setLowPass (sr, (double) lpF);
        }

        const bool useHp = pLowCutOn->load()  > 0.5f;   // toggled by clicking the knob name
        const bool useLp = pHighCutOn->load() > 0.5f;
        const int  vbChannels = juce::jmin (2, numIn);

        if (useHp)
            for (int ch = 0; ch < vbChannels; ++ch)
            {
                float* d = buffer.getWritePointer (ch);
                auto&  f = hpFilt[ch];
                for (int s = 0; s < numSamples; ++s)
                    d[s] = f.processSample (d[s]);
            }
        if (useLp)
            for (int ch = 0; ch < vbChannels; ++ch)
            {
                float* d = buffer.getWritePointer (ch);
                auto&  f = lpFilt[ch];
                for (int s = 0; s < numSamples; ++s)
                    d[s] = f.processSample (d[s]);
            }
    }

    // ---- feed the analyser with a mono mix ----
    // (channel pointers hoisted out of the sample loop — no per-sample calls)
    const float* in0 = buffer.getReadPointer (0);
    const float* in1 = numIn > 1 ? buffer.getReadPointer (1) : nullptr;
    const float invCh = in1 != nullptr ? 0.5f : 1.0f;

    for (int s = 0; s < numSamples; ++s)
    {
        float m = in0[s];
        if (in1 != nullptr)
            m += in1[s];
        m *= invCh;

        hopPeak = juce::jmax (hopPeak, std::abs (m));
        ring[(size_t) ringPos] = m;
        ringPos = (ringPos + 1) & (fftSize - 1);

        if (++hopCount >= hopSize)
        {
            hopCount = 0;
            analyse();
        }
    }

    // ---- run the notch bank (zero latency audio path) ----
    const int filtChannels = juce::jmin (2, numIn);
    for (auto& n : notches)
    {
        if (! n.active || n.currentDepth < 0.3f)
            continue;

        for (int ch = 0; ch < filtChannels; ++ch)
        {
            float* d = buffer.getWritePointer (ch);
            auto&  f = n.filt[ch];
            for (int s = 0; s < numSamples; ++s)
                d[s] = f.processSample (d[s]);
        }
    }

    // ---- learned room-EQ cuts (gentle, wide; fixes hollow/boxy resonances) ----
    if (pRoomLearn->load() > 0.5f)
    {
        for (auto& t : tones)
        {
            if (! t.active || t.currentDepth < 0.2f)
                continue;

            for (int ch = 0; ch < filtChannels; ++ch)
            {
                float* d = buffer.getWritePointer (ch);
                auto&  f = t.filt[ch];
                for (int s = 0; s < numSamples; ++s)
                    d[s] = f.processSample (d[s]);
            }
        }
    }

    // ---- AI Clear Voice: RNNoise neural network (offline, embedded model) ----
    if (pAiClear->load() > 0.5f)
    {
        if (rnSampleRateOk && rn[0].state != nullptr)
        {
            aiStatus.store (1);
            for (int ch = 0; ch < filtChannels; ++ch)
            {
                auto&  c = rn[ch];
                float* d = buffer.getWritePointer (ch);

                for (int s = 0; s < numSamples; ++s)
                {
                    const float wet = c.done[(size_t) c.pos];
                    c.pending[(size_t) c.pos] = d[s];
                    d[s] = wet;

                    if (++c.pos >= kRnFrame)
                    {
                        c.pos = 0;
                        float fin [kRnFrame], fout [kRnFrame];
                        for (int i = 0; i < kRnFrame; ++i)
                            fin[i] = c.pending[(size_t) i] * 32768.0f;
                        rnnoise_process_frame (c.state, fout, fin);
                        for (int i = 0; i < kRnFrame; ++i)
                            c.done[(size_t) i] = fout[i] * (1.0f / 32768.0f);
                    }
                }
            }
        }
        else
        {
            aiStatus.store (2);   // needs 48000 Hz — passing through unprocessed
        }
    }
    else
    {
        aiStatus.store (0);
    }

    buffer.applyGain (juce::Decibels::decibelsToGain (pOutput->load()));

    const float mag = buffer.getMagnitude (0, numSamples);
    if (mag > outPeak.load())
        outPeak.store (mag);
}

//==============================================================================
void DeHowlProcessor::analyse()
{
    // ---- silence gate: skip all FFT work when the input is essentially silent ----
    // (biggest CPU saving in practice — the analyser idles between songs/speech)
    const float gatePeak = hopPeak;
    hopPeak = 0.0f;

    if (gatePeak < 1.0e-4f)   // below ~-80 dBFS
    {
        for (auto it = candidates.begin(); it != candidates.end();)
        {
            it->seen = false;
            if (--it->frames <= 0) it = candidates.erase (it);
            else ++it;
        }
        updateNotches();      // auto-release keeps working during silence
        updateTones();
        publishDisplay();
        return;
    }

    // Copy the ring buffer in time order, window it, transform
    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = ring[(size_t) ((ringPos + i) & (fftSize - 1))];

    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
    window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    const int numBins = fftSize / 2;
    const float scale = 4.0f / (float) fftSize;   // ~dBFS for a windowed sine

    // dB conversion + prefix sum in one pass (prefix sum makes every
    // local-average lookup O(1) instead of a 96-bin loop per peak)
    prefixSum[0] = prefixSum[1] = 0.0f;
    magDb[0] = -120.0f;
    for (int b = 1; b < numBins; ++b)
    {
        const float v = juce::Decibels::gainToDecibels (fftData[(size_t) b] * scale, -120.0f);
        magDb[(size_t) b] = v;
        prefixSum[(size_t) b + 1] = prefixSum[(size_t) b] + v;
    }

    const int binLo = juce::jmax (2, (int) std::ceil  (80.0    * fftSize / sr));
    const int binHi = juce::jmin (numBins - 2,
                                  (int) std::floor (juce::jmin (12000.0, sr * 0.45) * fftSize / sr));

    // Sensitivity 0..100 % -> required peak-over-average: 30 dB (least) .. 12 dB (most sensitive)
    const float thresholdDb = 30.0f - 0.18f * pSensitivity->load();
    const float absFloorDb  = -60.0f;   // ignore anything this quiet

    for (int b = binLo; b <= binHi; ++b)
    {
        const float v = magDb[(size_t) b];
        if (v < absFloorDb)
            continue;
        if (! (v > magDb[(size_t) (b - 1)] && v >= magDb[(size_t) (b + 1)]))
            continue;   // must be a local maximum

        // Average of the surrounding spectrum (±64 bins, excluding ±4) via prefix sums
        const int lo  = juce::jmax (1, b - 64);
        const int hi  = juce::jmin (numBins - 1, b + 64);
        const int elo = juce::jmax (lo, b - 4);
        const int ehi = juce::jmin (hi, b + 4);

        const float sum = (prefixSum[(size_t) hi + 1] - prefixSum[(size_t) lo])
                        - (prefixSum[(size_t) ehi + 1] - prefixSum[(size_t) elo]);
        const int   cnt = (hi - lo + 1) - (ehi - elo + 1);
        const float localAvg = sum / (float) cnt;

        if (v - localAvg >= thresholdDb)
            registerCandidate (b, v);
    }

    promoteCandidates();
    updateNotches();
    if (pRoomLearn->load() > 0.5f)
        updateRoomLearning();
    updateTones();
    publishDisplay();
}

//==============================================================================
// Learn the room's long-term spectral fingerprint and place up to 6 gentle,
// wide cuts on persistent resonances (hollowness, boxiness, reflection build-up).
void DeHowlProcessor::updateRoomLearning()
{
    const int numBins = fftSize / 2;

    // slow exponential average: the "memory" of how this room sounds
    const float a = 0.01f;   // ~4-5 seconds of memory per frame at 43 ms hops
    for (int b = 1; b < numBins; ++b)
        roomAvgDb[(size_t) b] += a * (magDb[(size_t) b] - roomAvgDb[(size_t) b]);

    ++roomFrames;
    if (roomFrames < 120)        // ~5 s warm-up before drawing any conclusions
        return;
    if (++roomAssignCounter < 23)   // re-evaluate the cuts ~once per second
        return;
    roomAssignCounter = 0;

    // broad spectral envelope of the learned average (prefix sums again)
    roomPrefix[0] = roomPrefix[1] = 0.0f;
    for (int b = 1; b < numBins; ++b)
        roomPrefix[(size_t) b + 1] = roomPrefix[(size_t) b] + roomAvgDb[(size_t) b];

    const int binLo = juce::jmax (2, (int) std::ceil  (120.0   * fftSize / sr));
    const int binHi = juce::jmin (numBins - 2,
                                  (int) std::floor (juce::jmin (8000.0, sr * 0.4) * fftSize / sr));

    struct Ridge { float freq = 0, excess = 0; };
    Ridge picked[kMaxTones];
    int   numPicked = 0;

    for (int b = binLo; b <= binHi; ++b)
    {
        const float v = roomAvgDb[(size_t) b];
        if (v < -55.0f)
            continue;
        if (! (v > roomAvgDb[(size_t) (b - 1)] && v >= roomAvgDb[(size_t) (b + 1)]))
            continue;

        const int lo  = juce::jmax (1, b - 128);
        const int hi  = juce::jmin (numBins - 1, b + 128);
        const int elo = juce::jmax (lo, b - 16);
        const int ehi = juce::jmin (hi, b + 16);
        const float sum = (roomPrefix[(size_t) hi + 1] - roomPrefix[(size_t) lo])
                        - (roomPrefix[(size_t) ehi + 1] - roomPrefix[(size_t) elo]);
        const int   cnt = (hi - lo + 1) - (ehi - elo + 1);
        const float excess = v - sum / (float) cnt;

        if (excess < 4.0f)
            continue;

        const float freq = (float) b * (float) (sr / (double) fftSize);

        // keep the strongest ridges, at least 1/4 octave apart
        bool merged = false;
        for (int i = 0; i < numPicked; ++i)
        {
            if (std::abs (std::log2 (freq / picked[i].freq)) < 0.25f)
            {
                if (excess > picked[i].excess)
                    picked[i] = { freq, excess };
                merged = true;
                break;
            }
        }
        if (! merged)
        {
            if (numPicked < kMaxTones)
            {
                picked[numPicked++] = { freq, excess };
            }
            else
            {
                int weakest = 0;
                for (int i = 1; i < kMaxTones; ++i)
                    if (picked[i].excess < picked[weakest].excess)
                        weakest = i;
                if (excess > picked[weakest].excess)
                    picked[weakest] = { freq, excess };
            }
        }
    }

    // match ridges to existing tone slots; fade out anything no longer needed
    bool slotUsed[kMaxTones] = {};
    for (int i = 0; i < numPicked; ++i)
    {
        const float depth = juce::jlimit (0.0f, 6.0f, picked[i].excess - 3.0f);

        int found = -1;
        for (int t = 0; t < kMaxTones; ++t)
            if (tones[(size_t) t].active && ! slotUsed[t]
                 && std::abs (std::log2 (picked[i].freq / tones[(size_t) t].freqHz)) < 0.33f)
            { found = t; break; }

        if (found < 0)
            for (int t = 0; t < kMaxTones; ++t)
                if (! tones[(size_t) t].active && ! slotUsed[t])
                { found = t; break; }

        if (found < 0)
            continue;

        auto& t = tones[(size_t) found];
        slotUsed[found] = true;
        if (! t.active)
        {
            t.freqHz = picked[i].freq;
            t.currentDepth = 0.0f;
            t.filt[0].reset();
            t.filt[1].reset();
            t.active = true;
        }
        t.targetDepth = depth;
    }

    for (int t = 0; t < kMaxTones; ++t)
        if (tones[(size_t) t].active && ! slotUsed[t])
            tones[(size_t) t].targetDepth = 0.0f;   // fades away in updateTones()
}

void DeHowlProcessor::updateTones()
{
    int activeCount = 0;
    const bool enabled = pRoomLearn->load() > 0.5f;

    for (auto& t : tones)
    {
        if (! t.active)
            continue;

        const float target = enabled ? t.targetDepth : 0.0f;
        const float diff   = target - t.currentDepth;
        const float step   = juce::jlimit (-0.25f, 0.25f, diff);   // very gentle moves

        if (std::abs (step) > 0.02f)
        {
            t.currentDepth += step;
            t.filt[0].setPeak (sr, (double) t.freqHz, 4.0, (double) -t.currentDepth);
            t.filt[1].setPeak (sr, (double) t.freqHz, 4.0, (double) -t.currentDepth);
        }

        if (target <= 0.01f && t.currentDepth < 0.2f)
        {
            t.active = false;
            t.currentDepth = t.targetDepth = 0.0f;
            t.filt[0].reset();
            t.filt[1].reset();
            continue;
        }
        ++activeCount;
    }
    roomCuts.store (activeCount);
}

void DeHowlProcessor::registerCandidate (int bin, float levelDb)
{
    for (auto& c : candidates)
    {
        if (std::abs (c.bin - bin) <= 4)
        {
            c.bin    = bin;
            c.lastDb = levelDb;
            c.frames++;
            c.seen = true;
            return;
        }
    }
    candidates.push_back ({ bin, 1, true, levelDb, levelDb });
}

// Pitched instruments/voices have strong harmonics at 2f and 3f.
// Acoustic feedback is (almost) a pure tone. Use this to avoid notching music.
bool DeHowlProcessor::hasStrongHarmonics (int bin) const
{
    const int numBins = fftSize / 2;
    const float fundDb = magDb[(size_t) bin];

    for (int h = 2; h <= 3; ++h)
    {
        const int hb = h * bin;
        if (hb >= numBins - 6)
            break;

        float best = -120.0f;
        for (int k = -6; k <= 6; ++k)
            best = juce::jmax (best, magDb[(size_t) (hb + k)]);

        if (best > fundDb - 18.0f)
            return true;
    }
    return false;
}

void DeHowlProcessor::promoteCandidates()
{
    for (auto it = candidates.begin(); it != candidates.end();)
    {
        if (! it->seen)
        {
            if (--it->frames <= 0) { it = candidates.erase (it); continue; }
            it->seen = false;
            ++it;
            continue;
        }

        // Feedback grows exponentially; music sustains or decays.
        // A peak that has gained 9+ dB since first sighting is almost
        // certainly a howl building up -> kill it after only 2 frames.
        const bool growingFast = (it->lastDb - it->firstDb) >= 9.0f;

        // A peak with strong harmonics is probably a held musical note ->
        // demand much longer persistence before notching it.
        const int persistNeeded = growingFast            ? 2
                                : hasStrongHarmonics (it->bin) ? 8
                                :                          3;

        if (it->frames >= persistNeeded)
        {
            // Parabolic interpolation for sub-bin frequency accuracy
            const int   b  = it->bin;
            const float ym = magDb[(size_t) (b - 1)];
            const float y0 = magDb[(size_t) b];
            const float yp = magDb[(size_t) (b + 1)];
            const float den = ym - 2.0f * y0 + yp;
            const float delta = (std::abs (den) > 1.0e-9f)
                                    ? juce::jlimit (-0.5f, 0.5f, 0.5f * (ym - yp) / den)
                                    : 0.0f;
            const float freq = ((float) b + delta) * (float) (sr / (double) fftSize);

            triggerNotch (freq);
            it = candidates.erase (it);
            continue;
        }
        it->seen = false;
        ++it;
    }
}

void DeHowlProcessor::triggerNotch (float f)
{
    const float maxDepth = pDepth->load();

    // Already covering this frequency? (within 1/12 octave) -> bite deeper,
    // and refine the notch centre onto the freshly measured frequency
    for (auto& n : notches)
    {
        if (n.active && std::abs (std::log2 (f / n.freqHz)) < (1.0f / 12.0f))
        {
            n.freqHz += 0.35f * (f - n.freqHz);      // glide onto the true centre
            n.targetDepth        = juce::jmin (maxDepth, n.targetDepth + 4.0f);
            n.framesSinceTrigger = 0;

            if (n.currentDepth > 0.3f)               // re-centre the live filters now
            {
                const double Q = (double) pQ->load();
                n.filt[0].setPeak (sr, (double) n.freqHz, Q, (double) -n.currentDepth);
                n.filt[1].setPeak (sr, (double) n.freqHz, Q, (double) -n.currentDepth);
            }
            return;
        }
    }

    const int allowed = (int) pMaxNotches->load();
    int activeCount = 0;
    for (auto& n : notches)
        if (n.active)
            ++activeCount;

    Notch* slot = nullptr;
    if (activeCount < allowed)
    {
        for (auto& n : notches)
            if (! n.active) { slot = &n; break; }
    }
    if (slot == nullptr)
    {
        // All slots used: recycle the one untouched the longest
        int bestAge = -1;
        for (auto& n : notches)
            if (n.active && n.framesSinceTrigger > bestAge)
            {
                bestAge = n.framesSinceTrigger;
                slot = &n;
            }
    }
    if (slot == nullptr)
        return;

    slot->freqHz             = f;
    slot->targetDepth        = juce::jmin (12.0f, maxDepth);
    slot->currentDepth       = 0.0f;
    slot->framesSinceTrigger = 0;
    slot->active             = true;
    slot->filt[0].reset();
    slot->filt[1].reset();
}

void DeHowlProcessor::updateNotches()
{
    const bool   autoMode = pMode->load() > 0.5f;
    const float  maxDepth = pDepth->load();
    const double Q        = (double) pQ->load();

    for (auto& n : notches)
    {
        if (! n.active)
            continue;

        n.framesSinceTrigger++;
        n.targetDepth = juce::jmin (n.targetDepth, maxDepth);

        if (autoMode && n.framesSinceTrigger > holdFrames)
        {
            n.targetDepth -= 0.6f;            // gentle fade-out once the room is stable
            if (n.targetDepth < 1.0f)
            {
                n.active = false;
                n.targetDepth = n.currentDepth = 0.0f;
                n.filt[0].reset();
                n.filt[1].reset();
                continue;
            }
        }

        const float diff = n.targetDepth - n.currentDepth;
        const float step = juce::jlimit (-0.8f, 6.0f, diff);   // fast attack, slow release
        if (std::abs (step) > 0.05f)
        {
            n.currentDepth += step;
            n.filt[0].setPeak (sr, (double) n.freqHz, Q, (double) -n.currentDepth);
            n.filt[1].setPeak (sr, (double) n.freqHz, Q, (double) -n.currentDepth);
        }
    }
}

void DeHowlProcessor::clearAllNotches()
{
    for (auto& n : notches)
    {
        n.active = false;
        n.freqHz = 0.0f;
        n.targetDepth = n.currentDepth = 0.0f;
        n.framesSinceTrigger = 0;
        n.filt[0].reset();
        n.filt[1].reset();
    }
    candidates.clear();
}

void DeHowlProcessor::publishDisplay()
{
    for (int i = 0; i < kMaxNotches; ++i)
    {
        displayFreq[(size_t) i].store (notches[(size_t) i].active ? notches[(size_t) i].freqHz : 0.0f);
        displayDepth[(size_t) i].store (notches[(size_t) i].currentDepth);
    }
}

//==============================================================================
void DeHowlProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void DeHowlProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* DeHowlProcessor::createEditor()
{
    return new DeHowlEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DeHowlProcessor();
}
