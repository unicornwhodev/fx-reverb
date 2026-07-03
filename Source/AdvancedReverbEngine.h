#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>

class AdvancedReverbEngine
{
public:
    enum Algorithm
    {
        room = 0,
        plate,
        hall,
        chamber,
        space
    };

    enum Quality
    {
        eco = 0,
        studio,
        high
    };

    struct Parameters
    {
        int algorithm = hall;
        int quality = studio;
        float size = 0.62f;
        float decay = 0.52f;
        float predelayMs = 18.0f;
        float damping = 0.42f;
        float width = 1.0f;
        float earlyLevelDb = -8.0f;
        float tailLevelDb = 0.0f;
        float diffusion = 0.72f;
        float lowCutHz = 120.0f;
        float highCutHz = 12000.0f;
        float modDepth = 0.28f;
        float modRate = 0.22f;
        bool freeze = false;
    };

    void prepare(double newSampleRate, int maxBlockSize)
    {
        sampleRate = juce::jmax(1000.0, newSampleRate);
        juce::ignoreUnused(maxBlockSize);

        const int maxPredelaySamples = (int) std::ceil(sampleRate * 0.45);
        for (auto& channel : predelay)
            channel.assign((size_t) maxPredelaySamples, 0.0f);
        predelayWrite = 0;

        const int maxTankSamples = (int) std::ceil(sampleRate * 2.5);
        for (auto& line : lines)
            line.prepare(maxTankSamples);

        const std::array<float, 4> diffusionMs { 4.7f, 8.3f, 12.9f, 17.1f };
        for (int ch = 0; ch < 2; ++ch)
            for (size_t i = 0; i < diffusers[(size_t) ch].size(); ++i)
                diffusers[(size_t) ch][i].prepare((int) std::round(diffusionMs[i] * 0.001f * (float) sampleRate));

        reset();
    }

    void reset()
    {
        for (auto& channel : predelay)
            std::fill(channel.begin(), channel.end(), 0.0f);
        predelayWrite = 0;

        for (auto& line : lines)
            line.reset();

        for (auto& channel : diffusers)
            for (auto& diffuser : channel)
                diffuser.reset();

        lfoPhase = 0.0f;
    }

    void process(const juce::AudioBuffer<float>& input,
                 juce::AudioBuffer<float>& wet,
                 const Parameters& params)
    {
        const int numSamples = input.getNumSamples();
        jassert(wet.getNumChannels() >= 2);
        jassert(wet.getNumSamples() >= numSamples);
        if (wet.getNumChannels() < 2 || wet.getNumSamples() < numSamples)
            return;

        wet.clear(0, 0, numSamples);
        wet.clear(1, 0, numSamples);

        if (numSamples <= 0 || predelay[0].empty())
            return;

        const auto spec = getAlgorithmSpec(params.algorithm);
        const int activeLines = params.quality == eco ? 4 : 8;
        const float rt60 = getRt60Seconds(params.decay, spec.decayScale);
        const float sizeScale = spec.sizeScale * juce::jmap(params.size, 0.0f, 1.0f, 0.58f, 2.35f);
        const float feedbackDrive = params.freeze ? 0.9985f : 1.0f;
        const float inputGain = params.freeze ? 0.0f : spec.inputGain;
        const float diffusionFeedback = juce::jlimit(0.08f, 0.78f, params.diffusion * spec.diffusionScale);
        const float earlyGain = juce::Decibels::decibelsToGain(params.earlyLevelDb);
        const float tailGain = juce::Decibels::decibelsToGain(params.tailLevelDb) * (params.quality == high ? 1.03f : 1.0f);
        const float predelaySamples = juce::jlimit(0.0f, (float) predelay[0].size() - 2.0f,
                                                   params.predelayMs * 0.001f * (float) sampleRate);

        const float hpCoeff = makeHighpassCoeff(params.lowCutHz);
        const float lpCoeff = makeLowpassCoeff(params.highCutHz * juce::jmap(params.damping, 0.0f, 1.0f, 1.0f, 0.38f));
        const float modRate = juce::jlimit(0.01f, 2.5f, params.modRate * spec.modRateScale);
        const float modDepthSamples = juce::jlimit(0.0f, 42.0f, params.modDepth * spec.modDepthScale * (params.quality == high ? 1.35f : 1.0f));
        const float lfoInc = juce::MathConstants<float>::twoPi * modRate / (float) sampleRate;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float inL = input.getSample(0, sample);
            const float inR = input.getNumChannels() > 1 ? input.getSample(1, sample) : inL;

            writePredelay(inL, inR);
            const float preL = readPredelay(0, predelaySamples);
            const float preR = readPredelay(1, predelaySamples);

            float earlyL = 0.0f;
            float earlyR = 0.0f;
            renderEarlyReflections(params, spec, predelaySamples, earlyL, earlyR);

            float tankInL = diffusers[0][0].process(preL, diffusionFeedback);
            float tankInR = diffusers[1][0].process(preR, diffusionFeedback);
            const int diffuserCount = params.quality == high ? 4 : (params.quality == studio ? 3 : 2);
            for (int i = 1; i < diffuserCount; ++i)
            {
                tankInL = diffusers[0][(size_t) i].process(tankInL, diffusionFeedback);
                tankInR = diffusers[1][(size_t) i].process(tankInR, diffusionFeedback);
            }

            std::array<float, 8> taps {};
            for (int i = 0; i < activeLines; ++i)
            {
                const float baseDelay = baseDelaySeconds[(size_t) i] * (float) sampleRate * sizeScale;
                const float phase = lfoPhase * lfoRates[(size_t) i] + lfoOffsets[(size_t) i];
                const float mod = std::sin(phase) * modDepthSamples;
                taps[(size_t) i] = lines[(size_t) i].read(baseDelay + mod);
            }

            float tailL = 0.0f;
            float tailR = 0.0f;
            for (int i = 0; i < activeLines; ++i)
            {
                const float pan = linePan[(size_t) i];
                tailL += taps[(size_t) i] * (1.0f - pan);
                tailR += taps[(size_t) i] * pan;
            }
            const float tailNorm = activeLines > 4 ? 0.185f : 0.265f;
            tailL *= tailNorm;
            tailR *= tailNorm;

            for (int i = 0; i < activeLines; ++i)
            {
                float mixed = 0.0f;
                for (int j = 0; j < activeLines; ++j)
                    mixed += hadamardSign(i, j) * taps[(size_t) j];
                mixed /= std::sqrt((float) activeLines);

                const float delaySeconds = baseDelaySeconds[(size_t) i] * sizeScale;
                const float feedback = juce::jlimit(0.05f, 0.9985f,
                    std::pow(0.001f, delaySeconds / juce::jmax(0.08f, rt60)) * feedbackDrive);

                const float injected = ((i & 1) == 0 ? tankInL : tankInR) * inputGain * lineInputGains[(size_t) i];
                const float filtered = lines[(size_t) i].filterFeedback(mixed, hpCoeff, lpCoeff);
                lines[(size_t) i].write(juce::jlimit(-1.25f, 1.25f, injected + filtered * feedback));
            }

            float outL = earlyL * earlyGain + tailL * tailGain;
            float outR = earlyR * earlyGain + tailR * tailGain;
            applyWidth(outL, outR, params.width, spec.crossfeed);

            wet.setSample(0, sample, std::isfinite(outL) ? juce::jlimit(-2.0f, 2.0f, outL) : 0.0f);
            wet.setSample(1, sample, std::isfinite(outR) ? juce::jlimit(-2.0f, 2.0f, outR) : 0.0f);

            predelayWrite = (predelayWrite + 1) % (int) predelay[0].size();
            lfoPhase += lfoInc;
            if (lfoPhase > juce::MathConstants<float>::twoPi)
                lfoPhase -= juce::MathConstants<float>::twoPi;
        }
    }

private:
    struct AlgorithmSpec
    {
        float sizeScale;
        float decayScale;
        float diffusionScale;
        float inputGain;
        float modDepthScale;
        float modRateScale;
        float crossfeed;
        std::array<float, 6> earlyMs;
        std::array<float, 6> earlyGain;
    };

    class Allpass
    {
    public:
        void prepare(int delaySamples)
        {
            buffer.assign((size_t) juce::jmax(1, delaySamples), 0.0f);
            writePos = 0;
        }

        void reset()
        {
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            writePos = 0;
        }

        float process(float input, float feedback)
        {
            if (buffer.empty())
                return input;

            const float delayed = buffer[(size_t) writePos];
            const float output = delayed - input;
            buffer[(size_t) writePos] = input + delayed * feedback;
            writePos = (writePos + 1) % (int) buffer.size();
            return output;
        }

    private:
        std::vector<float> buffer;
        int writePos = 0;
    };

    class ModDelayLine
    {
    public:
        void prepare(int maxSamples)
        {
            buffer.assign((size_t) juce::jmax(8, maxSamples), 0.0f);
            reset();
        }

        void reset()
        {
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            writePos = 0;
            hpX = hpY = lpY = 0.0f;
        }

        float read(float delaySamples) const
        {
            if (buffer.empty())
                return 0.0f;

            const float maxDelay = (float) buffer.size() - 2.0f;
            const float delay = juce::jlimit(1.0f, maxDelay, delaySamples);
            float readPos = (float) writePos - delay;
            while (readPos < 0.0f)
                readPos += (float) buffer.size();

            const int a = ((int) std::floor(readPos)) % (int) buffer.size();
            const int b = (a + 1) % (int) buffer.size();
            const float frac = readPos - (float) a;
            return buffer[(size_t) a] + (buffer[(size_t) b] - buffer[(size_t) a]) * frac;
        }

        void write(float value)
        {
            if (buffer.empty())
                return;

            buffer[(size_t) writePos] = value;
            writePos = (writePos + 1) % (int) buffer.size();
        }

        float filterFeedback(float input, float hpCoeff, float lpCoeff)
        {
            const float hp = hpCoeff * (hpY + input - hpX);
            hpX = input;
            hpY = hp;
            lpY += lpCoeff * (hp - lpY);
            return lpY;
        }

    private:
        std::vector<float> buffer;
        int writePos = 0;
        float hpX = 0.0f;
        float hpY = 0.0f;
        float lpY = 0.0f;
    };

    static AlgorithmSpec getAlgorithmSpec(int algorithm)
    {
        switch (algorithm)
        {
            case room:    return { 0.72f, 0.55f, 0.78f, 0.62f, 0.45f, 1.20f, 0.08f, { 5.2f, 8.7f, 13.4f, 21.0f, 29.0f, 37.0f }, { 0.78f, -0.58f, 0.42f, -0.30f, 0.22f, -0.16f } };
            case plate:   return { 0.92f, 0.82f, 1.18f, 0.54f, 0.95f, 0.85f, 0.04f, { 7.5f, 11.1f, 16.3f, 23.6f, 31.2f, 44.0f }, { 0.46f, 0.40f, -0.34f, 0.28f, -0.18f, 0.12f } };
            case chamber: return { 0.82f, 0.70f, 0.95f, 0.58f, 0.55f, 1.05f, 0.10f, { 6.0f, 12.4f, 19.8f, 28.0f, 38.0f, 51.0f }, { 0.66f, -0.52f, 0.34f, 0.24f, -0.18f, 0.12f } };
            case space:   return { 1.42f, 1.85f, 1.08f, 0.42f, 1.40f, 0.62f, 0.16f, { 14.0f, 24.0f, 37.0f, 55.0f, 76.0f, 109.0f }, { 0.34f, 0.28f, -0.24f, 0.18f, -0.13f, 0.10f } };
            case hall:
            default:      return { 1.12f, 1.15f, 1.00f, 0.50f, 0.75f, 0.92f, 0.12f, { 9.0f, 15.2f, 24.5f, 36.0f, 52.0f, 73.0f }, { 0.52f, -0.44f, 0.32f, 0.24f, -0.17f, 0.13f } };
        }
    }

    static float getRt60Seconds(float decay, float scale)
    {
        const float curved = decay * decay;
        return juce::jmap(curved, 0.0f, 1.0f, 0.22f, 18.0f) * scale;
    }

    static int hadamardSign(int row, int col) noexcept
    {
        unsigned int v = (unsigned int) (row & col);
        v ^= v >> 4;
        v ^= v >> 2;
        v ^= v >> 1;
        return (v & 1u) == 0u ? 1 : -1;
    }

    float makeLowpassCoeff(float cutoffHz) const noexcept
    {
        const float cutoff = juce::jlimit(800.0f, 20000.0f, cutoffHz);
        return 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * cutoff / (float) sampleRate);
    }

    float makeHighpassCoeff(float cutoffHz) const noexcept
    {
        const float cutoff = juce::jlimit(20.0f, 1200.0f, cutoffHz);
        const float rc = 1.0f / (2.0f * juce::MathConstants<float>::pi * cutoff);
        const float dt = 1.0f / (float) sampleRate;
        return rc / (rc + dt);
    }

    void writePredelay(float left, float right)
    {
        predelay[0][(size_t) predelayWrite] = left;
        predelay[1][(size_t) predelayWrite] = right;
    }

    float readPredelay(int channel, float delaySamples) const
    {
        const auto& buffer = predelay[(size_t) channel];
        if (buffer.empty())
            return 0.0f;

        float readPos = (float) predelayWrite - juce::jlimit(0.0f, (float) buffer.size() - 2.0f, delaySamples);
        while (readPos < 0.0f)
            readPos += (float) buffer.size();

        const int a = ((int) std::floor(readPos)) % (int) buffer.size();
        const int b = (a + 1) % (int) buffer.size();
        const float frac = readPos - (float) a;
        return buffer[(size_t) a] + (buffer[(size_t) b] - buffer[(size_t) a]) * frac;
    }

    void renderEarlyReflections(const Parameters& params,
                                const AlgorithmSpec& spec,
                                float predelaySamples,
                                float& left,
                                float& right) const
    {
        const float sizeShift = juce::jmap(params.size, 0.0f, 1.0f, 0.55f, 1.8f);
        for (size_t i = 0; i < spec.earlyMs.size(); ++i)
        {
            const float delay = predelaySamples + spec.earlyMs[i] * 0.001f * (float) sampleRate * sizeShift;
            const float tapL = readPredelay(0, delay);
            const float tapR = readPredelay(1, delay * (1.0f + 0.018f * (float) i));
            const float gain = spec.earlyGain[i] * (1.0f - params.diffusion * 0.22f);
            if ((i & 1u) == 0u)
            {
                left += tapL * gain;
                right += tapR * gain * 0.86f;
            }
            else
            {
                left += tapR * gain * 0.82f;
                right += tapL * gain;
            }
        }
    }

    static void applyWidth(float& left, float& right, float width, float crossfeed)
    {
        const float mid = 0.5f * (left + right);
        const float side = 0.5f * (left - right) * juce::jlimit(0.0f, 1.35f, width * 1.18f);
        left = mid + side;
        right = mid - side;

        const float cf = crossfeed * (1.0f - juce::jlimit(0.0f, 1.0f, width) * 0.45f);
        const float l = left;
        const float r = right;
        left = l * (1.0f - cf) + r * cf;
        right = r * (1.0f - cf) + l * cf;
    }

    double sampleRate = 44100.0;
    std::array<std::vector<float>, 2> predelay;
    int predelayWrite = 0;
    std::array<ModDelayLine, 8> lines;
    std::array<std::array<Allpass, 4>, 2> diffusers;
    float lfoPhase = 0.0f;

    static constexpr std::array<float, 8> baseDelaySeconds { 0.0297f, 0.0371f, 0.0411f, 0.0537f, 0.0613f, 0.0719f, 0.0831f, 0.0973f };
    static constexpr std::array<float, 8> lfoRates { 0.73f, 0.61f, 0.89f, 0.51f, 1.07f, 0.43f, 0.79f, 0.67f };
    static constexpr std::array<float, 8> lfoOffsets { 0.0f, 1.4f, 2.7f, 4.2f, 5.1f, 0.8f, 3.6f, 5.8f };
    static constexpr std::array<float, 8> linePan { 0.12f, 0.82f, 0.28f, 0.68f, 0.42f, 0.58f, 0.04f, 0.94f };
    static constexpr std::array<float, 8> lineInputGains { 0.82f, 0.78f, 0.74f, 0.70f, 0.68f, 0.65f, 0.62f, 0.60f };
};
