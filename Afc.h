#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

//==============================================================================
/*
    Afc.h — Acoustic Feedback Cancellation engine (DeHowl v3.0)

    THE IDEA ("prediction, not reaction"):
    The notch bank reacts to feedback after it is already audible. AFC instead
    MODELS the acoustic path loudspeaker -> room -> microphone with an adaptive
    filter (NLMS, 2048 taps = ~43 ms of room response at 48 kHz). Every sample,
    it predicts how much of what the speakers just played is arriving back at
    the microphone, and subtracts that prediction BEFORE it can loop again.
    When the model converges, the system behaves as if the speakers were
    (partially) disconnected from the mic — typically 6..15 dB extra
    gain-before-feedback, with NO tonal holes.

    THE CLOSED-LOOP TRAP (why naive AEC fails here):
    In a conference call, the reference (far-end voice) is independent of the
    local talker. Here the reference is our OWN output — a processed copy of
    the mic signal. Reference and desired signal are correlated, so plain NLMS
    would start "cancelling" the preacher's voice itself. Three defences:

      1. DECORRELATION: a small single-sideband frequency shift (default 5 Hz)
         in the forward path. The loop signal is never perfectly correlated
         with itself any more — the classic hearing-aid trick. (Bonus: the
         shift alone already breaks the phase condition feedback needs.)
      2. CAUTIOUS ADAPTATION: small normalised step size, adaptation gated on
         reference energy, update error clamped, slow tap leakage so a stale
         room model fades away.
      3. DIVERGENCE GUARD: if the "cancelled" signal becomes louder than the
         raw mic signal for ~50 ms, the model is hurting — its taps are halved
         (graceful retreat, no audible reset click).

    Everything here is real-time safe: no allocation, no locks, no system
    calls after prepare(). Pure C++, no JUCE dependency (unit-testable).
*/

//==============================================================================
// Single-sideband frequency shifter — zero added latency.
//
// An IIR Hilbert transformer (two parallel 4-section allpass chains with a
// ~90 degree phase difference across the audio band — Olli Niemitalo's
// classic coefficients) produces an analytic signal (I + jQ); multiplying by
// e^(j*2*pi*shift*t) and taking the real part shifts EVERY frequency up by
// 'shift' Hz. Unlike pitch shifting, this is inharmonic — which is exactly
// what continuously breaks the feedback loop's phase condition.
class FreqShifter
{
public:
    void prepare (double sampleRate)
    {
        fs = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        for (int i = 0; i < 4; ++i)
        {
            cA[i] = kA[i] * kA[i];
            cB[i] = kB[i] * kB[i];
        }
        setShiftHz (shiftHz);
        reset();
    }

    void setShiftHz (double hz) noexcept
    {
        shiftHz = hz;
        inc = kTwoPi * hz / fs;
    }

    void reset() noexcept
    {
        for (auto& s : apA) s = {};
        for (auto& s : apB) s = {};
        bDelay = 0.0;
        phase  = 0.0;
    }

    float process (float x) noexcept
    {
        // Path A -> in-phase component
        double i = (double) x;
        for (int k = 0; k < 4; ++k)
            i = apA[k].process (i, cA[k]);

        // Path B (one extra sample of delay) -> quadrature component
        double q = bDelay;
        bDelay = (double) x;
        for (int k = 0; k < 4; ++k)
            q = apB[k].process (q, cB[k]);

        // SSB upshift: Re{ (I + jQ) * e^(j*phase) }
        const double y = i * std::cos (phase) - q * std::sin (phase);

        phase += inc;
        if (phase >= kTwoPi)
            phase -= kTwoPi;

        return (float) y;
    }

private:
    // 2nd-order allpass section in z^-2:  H(z) = (c + z^-2) / (1 + c*z^-2)
    struct AP2
    {
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

        double process (double x, double c) noexcept
        {
            const double y = c * (x - y2) + x2;
            x2 = x1;  x1 = x;
            y2 = y1;  y1 = y;
            return y;
        }
    };

    static constexpr double kTwoPi = 6.283185307179586476925286766559;

    // Niemitalo's Hilbert-pair allpass coefficients (squared in prepare())
    static constexpr double kA[4] = { 0.6923877778065, 0.9360654322959,
                                      0.9882295226860, 0.9987488452737 };
    static constexpr double kB[4] = { 0.4021921162426, 0.8561710882420,
                                      0.9722909545651, 0.9952884791278 };

    AP2    apA[4], apB[4];
    double cA[4] {}, cB[4] {};
    double bDelay = 0.0, phase = 0.0, inc = 0.0;
    double fs = 48000.0, shiftHz = 5.0;
};

//==============================================================================
// One channel of NLMS feedback cancellation.
//
//   mic in ──(−)──────────────► clean ("error") signal e  ──► rest of DeHowl
//             ▲
//          ŷ (prediction)
//             │
//        FIR  w[2048]  ◄── adapts on e
//             │
//        reference ring  ◄── pushReference(): the FINAL output samples
//                            (exactly what the loudspeakers are playing)
//
// The reference ring is stored DOUBLE-WRITTEN (each sample at pos and
// pos + kTaps) so the most recent kTaps samples are always one contiguous
// run — the predict and update loops become straight-line dot products the
// compiler can vectorise.
class AfcChannel
{
public:
    static constexpr int kTaps = 2048;            // ~43 ms of room response @48k

    void prepare()
    {
        w.assign     ((size_t) kTaps,     0.0f);
        dline.assign ((size_t) kTaps * 2, 0.0f);
        pos = 0;
        refPow = 0.0;
        micPow = errPow = 1.0e-12;
        erleSmoothed = 0.0f;
        erleCounter  = 0;
        leakCounter  = 0;
        badCount     = 0;
    }

    // Editor button — safe to call from processBlock after an atomic request
    void resetModel() noexcept
    {
        std::fill (w.begin(), w.end(), 0.0f);
        micPow = errPow = 1.0e-12;
        erleSmoothed = 0.0f;
        badCount = 0;
    }

    // Push one sample of the FINAL output (post output-gain) — the speaker feed
    void pushReference (float x) noexcept
    {
        pos = (pos + 1) & (kTaps - 1);
        dline[(size_t) pos]         = x;
        dline[(size_t) pos + kTaps] = x;

        // smoothed per-sample reference power (time constant ~ filter length)
        refPow = kPowRef * refPow + (1.0 - kPowRef) * (double) x * (double) x;
    }

    // Subtract the predicted feedback from one mic sample, adapt, return clean
    float cancel (float mic) noexcept
    {
        // window of the kTaps most recent reference samples, oldest first —
        // contiguous thanks to the double-written ring
        const float* x = dline.data() + pos + 1;

        // ---- predict: y = w . x  (4 independent partial sums vectorise) ----
        double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
        for (int j = 0; j < kTaps; j += 4)
        {
            s0 += (double) w[(size_t) j    ] * (double) x[j    ];
            s1 += (double) w[(size_t) j + 1] * (double) x[j + 1];
            s2 += (double) w[(size_t) j + 2] * (double) x[j + 2];
            s3 += (double) w[(size_t) j + 3] * (double) x[j + 3];
        }
        double yHat = s0 + s1 + s2 + s3;

        // sanity clamp — a broken model must never be louder than the show
        yHat = std::clamp (yHat, -2.0, 2.0);

        const float e = mic - (float) yHat;

        // ---- power tracking (~100 ms) for ERLE + divergence guard ----
        micPow = kPowSig * micPow + (1.0 - kPowSig) * (double) mic * (double) mic;
        errPow = kPowSig * errPow + (1.0 - kPowSig) * (double) e   * (double) e;

        // ---- NLMS update (gated, clamped, leaky) ----
        const double winEnergy = (double) kTaps * refPow;
        if (winEnergy > 1.0e-7)                       // only learn when output is audible
        {
            const float eAd = std::clamp (e, -1.0f, 1.0f);
            const float g   = (float) (kMu * (double) eAd / (winEnergy + 1.0e-10));
            for (int j = 0; j < kTaps; ++j)
                w[(size_t) j] += g * x[j];
        }

        // tap leakage: stale rooms fade out over ~1 minute
        if (++leakCounter >= 480)
        {
            leakCounter = 0;
            for (auto& t : w)
                t *= 0.99995f;
        }

        // divergence guard: "cleaned" louder than raw mic for ~50 ms -> retreat
        if (errPow > 2.0 * micPow + 1.0e-10)
        {
            if (++badCount >= 2400)
            {
                badCount = 0;
                for (auto& t : w)
                    t *= 0.5f;
            }
        }
        else if (badCount > 0)
        {
            --badCount;
        }

        // ---- ERLE: how many dB of feedback the model is removing ----
        if (++erleCounter >= 256)
        {
            erleCounter = 0;
            const float erle = 10.0f * std::log10 (
                (float) ((micPow + 1.0e-12) / (errPow + 1.0e-12)));
            const float clamped = std::clamp (erle, -10.0f, 40.0f);
            erleSmoothed += 0.2f * (clamped - erleSmoothed);
        }

        return e;
    }

    float getErleDb() const noexcept { return erleSmoothed; }

private:
    static constexpr double kMu     = 0.02;     // normalised NLMS step size —
                                                // deliberately slow: protects the
                                                // voice from closed-loop bias
                                                // (verified in simulation: 0.15
                                                // cancels ~50% of the voice,
                                                // 0.02 keeps ~85-90%)
    static constexpr double kPowRef = 0.9995;   // ref power smoothing (~kTaps)
    static constexpr double kPowSig = 0.9998;   // mic/err power smoothing (~100 ms)

    std::vector<float> w;       // adaptive taps (oldest lag first)
    std::vector<float> dline;   // double-written reference ring (2 * kTaps)
    int    pos = 0;
    double refPow = 0.0, micPow = 1.0e-12, errPow = 1.0e-12;
    float  erleSmoothed = 0.0f;
    int    erleCounter = 0, leakCounter = 0, badCount = 0;
};
