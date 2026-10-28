// DspAdpcm.cpp -- see DspAdpcm.h for credits and licence notes.
//
// In Octave: the GameCube cook (SoundWave::SaveStream) encodes with it, in the editor only; the console reads the
// headers (ReadHeader). The encoder is left out of every build but the editor (DSPADPCM_ENCODER, DspAdpcm.h).
//
// MIT License
// Copyright (c) 2015 Jack Andersen (gc-dspadpcm-encode)
// Copyright (c) 2016 Alex Barney (VGAudio)
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software
// and associated documentation files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions: The above copyright notice
// and this permission notice shall be included in all copies or substantial portions of the
// Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "DspAdpcm.h"

#include <cmath>
#include <cfloat>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace DspAdpcm
{

// ============================================================================================
// Coefficient selection (Nintendo's algorithm: per-frame order-2 LPC records, averaged, then
// split 1 -> 2 -> 4 -> 8 with a k-means style refinement).
// ============================================================================================
namespace
{
#if DSPADPCM_ENCODER
typedef double tvec[3];

void InnerProductMerge(tvec vecOut, const int16_t* pcmBuf /* 28: [0..13] previous frame, [14..27] current */)
{
    for (int i = 0; i <= 2; i++)
    {
        vecOut[i] = 0.0;
        for (int x = 0; x < 14; x++)
            vecOut[i] -= pcmBuf[14 + x - i] * pcmBuf[14 + x];
    }
}

void OuterProductMerge(tvec mtxOut[3], const int16_t* pcmBuf)
{
    for (int x = 1; x <= 2; x++)
        for (int y = 1; y <= 2; y++)
        {
            mtxOut[x][y] = 0.0;
            for (int z = 0; z < 14; z++)
                mtxOut[x][y] += pcmBuf[14 + z - x] * pcmBuf[14 + z - y];
        }
}

bool AnalyzeRanges(tvec mtx[3], int* vecIdxsOut)
{
    double recips[3] = {};
    double val, tmp, mn, mx;

    for (int x = 1; x <= 2; x++)
    {
        val = std::max(std::fabs(mtx[x][1]), std::fabs(mtx[x][2]));
        if (val < DBL_EPSILON)
            return true;
        recips[x] = 1.0 / val;
    }

    int maxIndex = 0;
    for (int i = 1; i <= 2; i++)
    {
        for (int x = 1; x < i; x++)
        {
            tmp = mtx[x][i];
            for (int y = 1; y < x; y++)
                tmp -= mtx[x][y] * mtx[y][i];
            mtx[x][i] = tmp;
        }

        val = 0.0;
        for (int x = i; x <= 2; x++)
        {
            tmp = mtx[x][i];
            for (int y = 1; y < i; y++)
                tmp -= mtx[x][y] * mtx[y][i];
            mtx[x][i] = tmp;
            tmp = std::fabs(tmp) * recips[x];
            if (tmp >= val)
            {
                val = tmp;
                maxIndex = x;
            }
        }

        if (maxIndex != i)
        {
            for (int y = 1; y <= 2; y++)
            {
                tmp = mtx[maxIndex][y];
                mtx[maxIndex][y] = mtx[i][y];
                mtx[i][y] = tmp;
            }
            recips[maxIndex] = recips[i];
        }

        vecIdxsOut[i] = maxIndex;

        if (mtx[i][i] == 0.0)
            return true;

        if (i != 2)
        {
            tmp = 1.0 / mtx[i][i];
            for (int x = i + 1; x <= 2; x++)
                mtx[x][i] *= tmp;
        }
    }

    mn = 1.0e10;
    mx = 0.0;
    for (int i = 1; i <= 2; i++)
    {
        tmp = std::fabs(mtx[i][i]);
        if (tmp < mn) mn = tmp;
        if (tmp > mx) mx = tmp;
    }
    return mn / mx < 1.0e-10;
}

void BidirectionalFilter(tvec mtx[3], const int* vecIdxs, tvec vecOut)
{
    double tmp;
    for (int i = 1, x = 0; i <= 2; i++)
    {
        int index = vecIdxs[i];
        tmp = vecOut[index];
        vecOut[index] = vecOut[i];
        if (x != 0)
            for (int y = x; y <= i - 1; y++)
                tmp -= vecOut[y] * mtx[i][y];
        else if (tmp != 0.0)
            x = i;
        vecOut[i] = tmp;
    }

    for (int i = 2; i > 0; i--)
    {
        tmp = vecOut[i];
        for (int y = i + 1; y <= 2; y++)
            tmp -= vecOut[y] * mtx[i][y];
        vecOut[i] = tmp / mtx[i][i];
    }
    vecOut[0] = 1.0;
}

bool QuadraticMerge(tvec inOutVec)
{
    double v2 = inOutVec[2];
    double tmp = 1.0 - (v2 * v2);
    if (tmp == 0.0)
        return true;
    double v0 = (inOutVec[0] - (v2 * v2)) / tmp;
    double v1 = (inOutVec[1] - (inOutVec[1] * v2)) / tmp;
    inOutVec[0] = v0;
    inOutVec[1] = v1;
    return std::fabs(v1) > 1.0;
}

void FinishRecord(tvec in, tvec out)
{
    for (int z = 1; z <= 2; z++)
    {
        if (in[z] >= 1.0)
            in[z] = 0.9999999999;
        else if (in[z] <= -1.0)
            in[z] = -0.9999999999;
    }
    out[0] = 1.0;
    out[1] = (in[2] * in[1]) + in[1];
    out[2] = in[2];
}

void MatrixFilter(const tvec src, tvec dst)
{
    tvec mtx[3] = {};
    mtx[2][0] = 1.0;
    for (int i = 1; i <= 2; i++)
        mtx[2][i] = -src[i];

    for (int i = 2; i > 0; i--)
    {
        double val = 1.0 - (mtx[i][i] * mtx[i][i]);
        for (int y = 1; y <= i; y++)
            mtx[i - 1][y] = ((mtx[i][i] * mtx[i][y]) + mtx[i][y]) / val;
    }

    dst[0] = 1.0;
    for (int i = 1; i <= 2; i++)
    {
        dst[i] = 0.0;
        for (int y = 1; y <= i; y++)
            dst[i] += mtx[i][y] * dst[i - y];
    }
}

void MergeFinishRecord(const tvec src, tvec dst)
{
    tvec tmp = {};
    double val = src[0];

    dst[0] = 1.0;
    for (int i = 1; i <= 2; i++)
    {
        double v2 = 0.0;
        for (int y = 1; y < i; y++)
            v2 += dst[y] * src[i - y];

        if (val > 0.0)
            dst[i] = -(v2 + src[i]) / val;
        else
            dst[i] = 0.0;

        tmp[i] = dst[i];

        for (int y = 1; y < i; y++)
            dst[y] += dst[i] * dst[i - y];

        val *= 1.0 - (dst[i] * dst[i]);
    }
    FinishRecord(tmp, dst);
}

double ContrastVectors(const tvec source1, const tvec source2)
{
    double val = (source2[2] * source2[1] + -source2[1]) / (1.0 - source2[2] * source2[2]);
    double val1 = (source1[0] * source1[0]) + (source1[1] * source1[1]) + (source1[2] * source1[2]);
    double val2 = (source1[0] * source1[1]) + (source1[1] * source1[2]);
    double val3 = source1[0] * source1[2];
    return val1 + (2.0 * val * val2) + (2.0 * (-source2[1] * val + -source2[2]) * val3);
}

void FilterRecords(tvec vecBest[8], int exp, const tvec* records, int recordCount)
{
    tvec bufferList[8];
    int buffer1[8];
    tvec buffer2;

    for (int x = 0; x < 2; x++)
    {
        for (int y = 0; y < exp; y++)
        {
            buffer1[y] = 0;
            for (int i = 0; i <= 2; i++)
                bufferList[y][i] = 0.0;
        }
        for (int z = 0; z < recordCount; z++)
        {
            int index = 0;
            double value = 1.0e30;
            for (int i = 0; i < exp; i++)
            {
                double tempVal = ContrastVectors(vecBest[i], records[z]);
                if (tempVal < value)
                {
                    value = tempVal;
                    index = i;
                }
            }
            buffer1[index]++;
            MatrixFilter(records[z], buffer2);
            for (int i = 0; i <= 2; i++)
                bufferList[index][i] += buffer2[i];
        }

        for (int i = 0; i < exp; i++)
            if (buffer1[i] > 0)
                for (int y = 0; y <= 2; y++)
                    bufferList[i][y] /= buffer1[i];

        for (int i = 0; i < exp; i++)
            MergeFinishRecord(bufferList[i], vecBest[i]);
    }
}

inline int16_t Clamp16(int v) { return int16_t(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }
inline int Clamp4(int v) { return v > 7 ? 7 : (v < -8 ? -8 : v); }

// Encode one frame with one predictor pair; returns the scale exponent and squared error.
void EncodeCoef(const int16_t* pcmIn /* [0]=yn2 [1]=yn1 [2..15] samples */, int sampleCount,
                const int16_t coefs[2], int pcmOut[16], int adpcmOut[14], int& scalePower, double& totalDistance)
{
    int maxOverflow;
    int maxDistance = 0;

    pcmOut[0] = pcmIn[0];
    pcmOut[1] = pcmIn[1];

    // Prediction error against the source with a step of 1 -> the starting scale.
    for (int s = 0; s < sampleCount; s++)
    {
        int inputSample = pcmIn[s + 2];
        int predictedSample = (pcmIn[s] * coefs[1] + pcmIn[s + 1] * coefs[0]) / 2048;
        int distance = Clamp16(inputSample - predictedSample);
        if (std::abs(distance) > std::abs(maxDistance))
            maxDistance = distance;
    }

    scalePower = 0;
    while (scalePower <= 12 && (maxDistance > 7 || maxDistance < -8))
    {
        maxDistance /= 2;
        scalePower++;
    }
    scalePower = scalePower <= 1 ? -1 : scalePower - 2;

    // Raise the scale until the frame fits in 4 bits (closed loop: history is the decoded output).
    do
    {
        scalePower++;
        int scale = (1 << scalePower) * 2048;
        totalDistance = 0;
        maxOverflow = 0;

        for (int s = 0; s < sampleCount; s++)
        {
            int inputSample = pcmIn[s + 2] * 2048;
            int predictedSample = pcmOut[s] * coefs[1] + pcmOut[s + 1] * coefs[0];
            int distance = inputSample - predictedSample;
            // The official encoder rounds through a float divide; match it.
            int unclamped = (distance > 0)
                ? (int)((double)((float)distance / (float)scale) + (double)0.4999999f)
                : (int)((double)((float)distance / (float)scale) - (double)0.4999999f);

            int adpcmSample = Clamp4(unclamped);
            if (adpcmSample != unclamped)
            {
                int overflow = std::abs(unclamped - adpcmSample);
                if (overflow > maxOverflow) maxOverflow = overflow;
            }
            adpcmOut[s] = adpcmSample;

            int corrected = predictedSample + adpcmSample * scale;
            pcmOut[s + 2] = Clamp16((corrected + 1024) >> 11);
            double d = pcmIn[s + 2] - pcmOut[s + 2];
            totalDistance += d * d;
        }

        for (int x = maxOverflow + 8; x > 256; x >>= 1)
            if (++scalePower >= 12)
                scalePower = 11;

    } while (scalePower < 12 && maxOverflow > 1);
}

// pcmInOut[0]=yn2, [1]=yn1, [2..15] = frame samples. On return [2..15] hold the decoded output.
void EncodeFrame(int16_t pcmInOut[16], int sampleCount, uint8_t adpcmOut[8], const int16_t coefsIn[16])
{
    int pcmOut[8][16];
    int adpcmS[8][14];
    int scale[8];
    double dist[8];

    for (int i = 0; i < 8; i++)
        EncodeCoef(pcmInOut, sampleCount, &coefsIn[i * 2], pcmOut[i], adpcmS[i], scale[i], dist[i]);

    int best = 0;
    double mn = DBL_MAX;
    for (int i = 0; i < 8; i++)
        if (dist[i] < mn) { mn = dist[i]; best = i; }

    for (int s = 0; s < sampleCount; s++)
        pcmInOut[s + 2] = (int16_t)pcmOut[best][s + 2];

    adpcmOut[0] = uint8_t((best << 4) | (scale[best] & 0xF));
    for (int s = sampleCount; s < 14; s++)
        adpcmS[best][s] = 0;
    for (int i = 0; i < 7; i++)
        adpcmOut[i + 1] = uint8_t(((adpcmS[best][i * 2] & 0xF) << 4) | (adpcmS[best][i * 2 + 1] & 0xF));
}

#endif // DSPADPCM_ENCODER

inline void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
inline uint16_t Get16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t Get32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
} // namespace

#if DSPADPCM_ENCODER
void CalculateCoefficients(const int16_t* source, uint32_t numSamples, uint32_t stride, int16_t coefsOut[16])
{
    uint32_t frameCount = FrameCount(numSamples);
    int16_t pcmHist[28] = {};
    tvec vec1, vec2, mtx[3];
    int vecIdxs[3] = {};
    std::vector<double> recordStore(size_t(frameCount) * 2 * 3 + 3, 0.0);
    tvec* records = reinterpret_cast<tvec*>(recordStore.data());
    int recordCount = 0;
    tvec vecBest[8] = {};

    for (uint32_t sample = 0; sample < numSamples; sample += 14)
    {
        uint32_t n = std::min<uint32_t>(14, numSamples - sample);
        for (uint32_t z = 0; z < 14; z++)
            pcmHist[14 + z] = z < n ? source[size_t(sample + z) * stride] : 0;

        InnerProductMerge(vec1, pcmHist);
        if (std::fabs(vec1[0]) > 10.0)
        {
            OuterProductMerge(mtx, pcmHist);
            if (!AnalyzeRanges(mtx, vecIdxs))
            {
                BidirectionalFilter(mtx, vecIdxs, vec1);
                if (!QuadraticMerge(vec1))
                {
                    FinishRecord(vec1, records[recordCount]);
                    recordCount++;
                }
            }
        }
        memcpy(pcmHist, pcmHist + 14, 14 * sizeof(int16_t));
    }

    vec1[0] = 1.0;
    vec1[1] = 0.0;
    vec1[2] = 0.0;
    for (int z = 0; z < recordCount; z++)
    {
        MatrixFilter(records[z], vecBest[0]);
        for (int y = 1; y <= 2; y++)
            vec1[y] += vecBest[0][y];
    }
    if (recordCount > 0)   // silence: no records, all-zero coefficients come out below
        for (int y = 1; y <= 2; y++)
            vec1[y] /= recordCount;

    MergeFinishRecord(vec1, vecBest[0]);

    int exp = 1;
    for (int w = 0; w < 3;)
    {
        vec2[0] = 0.0;
        vec2[1] = -1.0;
        vec2[2] = 0.0;
        for (int i = 0; i < exp; i++)
            for (int y = 0; y <= 2; y++)
                vecBest[exp + i][y] = (0.01 * vec2[y]) + vecBest[i][y];
        ++w;
        exp = 1 << w;
        FilterRecords(vecBest, exp, records, recordCount);
    }

    for (int z = 0; z < 8; z++)
    {
        for (int k = 1; k <= 2; k++)
        {
            double d = -vecBest[z][k] * 2048.0;
            int16_t c;
            if (d > 0.0)
                c = (d > 32767.0) ? int16_t(32767) : int16_t(std::lround(d));
            else
                c = (d < -32768.0) ? int16_t(-32768) : int16_t(std::lround(d));
            coefsOut[z * 2 + (k - 1)] = c;
        }
    }
}

void EncodeWithCoefs(const int16_t* pcm, uint32_t numSamples, uint32_t stride, const int16_t coefs[16],
                     int16_t hist1, int16_t hist2, uint8_t* outData, int16_t* decodedOut)
{
    int16_t buf[16] = {};
    buf[0] = hist2;
    buf[1] = hist1;
    uint32_t frames = FrameCount(numSamples);
    for (uint32_t f = 0; f < frames; f++)
    {
        uint32_t base = f * kSamplesPerFrame;
        uint32_t n = std::min<uint32_t>(kSamplesPerFrame, numSamples - base);
        for (uint32_t s = 0; s < 14; s++)
            buf[2 + s] = s < n ? pcm[size_t(base + s) * stride] : 0;

        // Always encode all 14 slots (the padding is zeros), as the official tool does.
        EncodeFrame(buf, 14, outData + size_t(f) * kBytesPerFrame, coefs);

        if (decodedOut)
            for (uint32_t s = 0; s < n; s++)
                decodedOut[base + s] = buf[2 + s];
        buf[0] = buf[14];
        buf[1] = buf[15];
    }
}

void Encode(const int16_t* pcm, uint32_t numSamples, uint32_t stride, uint32_t sampleRate,
            bool loop, uint32_t loopStart, uint32_t loopEnd, Channel& out, std::vector<int16_t>* decodedOut)
{
    out = Channel();
    out.numSamples = numSamples;
    out.numNibbles = NibbleCount(numSamples);
    out.sampleRate = sampleRate;
    out.loop = loop;
    if (numSamples == 0)
        return;
    if (loopEnd == 0 || loopEnd >= numSamples) loopEnd = numSamples - 1;
    if (loopStart > loopEnd) loopStart = 0;
    out.loopStart = loop ? loopStart : 0;
    out.loopEnd = loop ? loopEnd : numSamples - 1;

    CalculateCoefficients(pcm, numSamples, stride, out.coefs);

    std::vector<int16_t> decoded(numSamples);
    out.data.assign(DataBytes(numSamples), 0);
    EncodeWithCoefs(pcm, numSamples, stride, out.coefs, 0, 0, out.data.data(), decoded.data());

    out.gain = 0;
    out.ps = out.data[0];
    out.yn1 = 0;
    out.yn2 = 0;
    if (loop)
    {
        // Loop context: what the decoder holds right before loopStart on the first pass, so the
        // second and later passes decode bit-identically to the first.
        out.loopPs = out.data[(loopStart / kSamplesPerFrame) * kBytesPerFrame];
        out.loopYn1 = loopStart >= 1 ? decoded[loopStart - 1] : 0;
        out.loopYn2 = loopStart >= 2 ? decoded[loopStart - 2] : 0;
    }
    if (decodedOut)
        decodedOut->swap(decoded);
}

#endif // DSPADPCM_ENCODER

void Decode(const Channel& ch, std::vector<int16_t>& out)
{
    out.assign(ch.numSamples, 0);
    int16_t yn1 = ch.yn1, yn2 = ch.yn2;
    for (uint32_t s = 0; s < ch.numSamples; s++)
    {
        uint32_t f = s / kSamplesPerFrame, i = s % kSamplesPerFrame;
        const uint8_t* fr = &ch.data[size_t(f) * kBytesPerFrame];
        uint8_t b = fr[1 + i / 2];
        int nib = (i & 1) ? (b & 0xF) : (b >> 4);
        out[s] = DecodeSample(nib, fr[0], ch.coefs, yn1, yn2);
    }
}

void DecodeAccelerator(const Channel& ch, uint32_t count, std::vector<int16_t>& out)
{
    out.assign(count, 0);
    if (ch.numSamples == 0) return;
    // Nibble addresses relative to the channel's data (the voice adds ARAM byte address * 2).
    uint32_t cur = SampleToNibble(0);                 // header "ca" = 2
    uint32_t start = SampleToNibble(ch.loopStart);    // voice loop address
    uint32_t end = SampleToNibble(ch.loopEnd);        // voice end address (inclusive)
    uint16_t ps = ch.ps & 0x7F;
    int16_t yn1 = ch.yn1, yn2 = ch.yn2;
    for (uint32_t o = 0; o < count; o++)
    {
        uint8_t b = ch.data[cur >> 1];
        int nib = (cur & 1) ? (b & 0xF) : (b >> 4);
        out[o] = DecodeSample(nib, ps, ch.coefs, yn1, yn2);
        uint32_t justRead = cur;
        cur += 1;
        if ((cur & 15) == 0)
        {
            ps = ch.data[cur >> 1] & 0x7F;   // new frame: latch its header byte, skip its 2 nibbles
            cur += 2;
        }
        if (justRead == end)
        {
            if (!ch.loop) return;            // one-shot: voice stops, rest stays zero
            cur = start;
            ps = ch.loopPs & 0x7F;
            yn1 = ch.loopYn1;
            yn2 = ch.loopYn2;
        }
    }
}

void WriteHeader(const Channel& ch, uint8_t o[kHeaderSize])
{
    memset(o, 0, kHeaderSize);
    Put32(o + 0x00, ch.numSamples);
    Put32(o + 0x04, ch.numNibbles);
    Put32(o + 0x08, ch.sampleRate);
    Put16(o + 0x0C, ch.loop ? 1 : 0);
    Put16(o + 0x0E, 0);                                // format 0 = ADPCM
    Put32(o + 0x10, SampleToNibble(ch.loop ? ch.loopStart : 0));
    Put32(o + 0x14, SampleToNibble(ch.loop ? ch.loopEnd : (ch.numSamples ? ch.numSamples - 1 : 0)));
    Put32(o + 0x18, SampleToNibble(0));                // initial current address
    for (int i = 0; i < 16; i++)
        Put16(o + 0x1C + i * 2, uint16_t(ch.coefs[i]));
    Put16(o + 0x3C, ch.gain);
    Put16(o + 0x3E, ch.ps);
    Put16(o + 0x40, uint16_t(ch.yn1));
    Put16(o + 0x42, uint16_t(ch.yn2));
    Put16(o + 0x44, ch.loopPs);
    Put16(o + 0x46, uint16_t(ch.loopYn1));
    Put16(o + 0x48, uint16_t(ch.loopYn2));
    // 0x4A..0x5F reserved (zero)
}

bool ReadHeader(const uint8_t in[kHeaderSize], Channel& ch)
{
    ch = Channel();
    ch.numSamples = Get32(in + 0x00);
    ch.numNibbles = Get32(in + 0x04);
    ch.sampleRate = Get32(in + 0x08);
    ch.loop = Get16(in + 0x0C) != 0;
    if (Get16(in + 0x0E) != 0) return false;
    uint32_t ls = Get32(in + 0x10), le = Get32(in + 0x14);
    auto nibToSample = [](uint32_t n) { return (n / 16) * 14 + (n % 16) - 2; };
    ch.loopStart = ch.loop ? nibToSample(ls) : 0;
    ch.loopEnd = nibToSample(le);
    for (int i = 0; i < 16; i++)
        ch.coefs[i] = int16_t(Get16(in + 0x1C + i * 2));
    ch.gain = Get16(in + 0x3C);
    ch.ps = Get16(in + 0x3E);
    ch.yn1 = int16_t(Get16(in + 0x40));
    ch.yn2 = int16_t(Get16(in + 0x42));
    ch.loopPs = Get16(in + 0x44);
    ch.loopYn1 = int16_t(Get16(in + 0x46));
    ch.loopYn2 = int16_t(Get16(in + 0x48));
    return true;
}

} // namespace DspAdpcm
