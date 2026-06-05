#include "PluginProcessor.h"
#include "PluginEditor.h"

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

    magDb.resize ((size_t) fftSize / 2 + 1, -120.0f);
    prefixSum.resize ((size_t) fftSize / 2 + 2, 0.0f);
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
        juce::NormalisableRange<float> (10.0f, 80.0f, 1.0f), 30.0f));

    p.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "mode", 1 }, "Mode",
        juce::StringArray { "Latch", "Auto Release" }, 0));

    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "output", 1 }, "Output (dB)",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f));

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
    publishDisplay();
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

        // Average of the surrounding spectrum (±48 bins, excluding ±3) via prefix sums
        const int lo  = juce::jmax (1, b - 48);
        const int hi  = juce::jmin (numBins - 1, b + 48);
        const int elo = juce::jmax (lo, b - 3);
        const int ehi = juce::jmin (hi, b + 3);

        const float sum = (prefixSum[(size_t) hi + 1] - prefixSum[(size_t) lo])
                        - (prefixSum[(size_t) ehi + 1] - prefixSum[(size_t) elo]);
        const int   cnt = (hi - lo + 1) - (ehi - elo + 1);
        const float localAvg = sum / (float) cnt;

        if (v - localAvg >= thresholdDb)
            registerCandidate (b, v);
    }

    promoteCandidates();
    updateNotches();
    publishDisplay();
}

void DeHowlProcessor::registerCandidate (int bin, float levelDb)
{
    for (auto& c : candidates)
    {
        if (std::abs (c.bin - bin) <= 3)
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
        if (hb >= numBins - 3)
            break;

        float best = -120.0f;
        for (int k = -3; k <= 3; ++k)
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

    // Already covering this frequency? (within 1/12 octave) -> bite deeper
    for (auto& n : notches)
    {
        if (n.active && std::abs (std::log2 (f / n.freqHz)) < (1.0f / 12.0f))
        {
            n.targetDepth        = juce::jmin (maxDepth, n.targetDepth + 4.0f);
            n.framesSinceTrigger = 0;
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
