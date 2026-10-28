// DspAdpcm.h -- GameCube/Wii DSP-ADPCM encoder and decoder (standalone, no engine dependencies).
//
// The encoder is a C++ port of the open reimplementations of Nintendo's DSPADPCM tool:
//   - VGAudio, GcAdpcm codec            (C) 2016 Alex Barney,  MIT licence
//   - gc-dspadpcm-encode (grok.c)       (C) 2015 Jack Andersen, MIT licence
// The arithmetic (float rounding of the 4-bit step, the scale search, the 8-predictor
// correlation) follows VGAudio, which matches the official tool's output.
// The decoder follows the DSP accelerator's ADPCM path (as Dolphin's DSPAccelerator.cpp models it).
//
// Format recap: one stream per channel. A frame is 8 bytes = 1 header byte (predictor index in
// bits 6..4, scale exponent in bits 3..0) + 14 4-bit two's-complement samples, high nibble first.
// Addresses the DSP uses are NIBBLE addresses: frame f's header is nibbles 16f, 16f+1 and its
// samples are 16f+2 .. 16f+15.
#pragma once

// In Octave the encoder is the editor's alone (the GameCube cook, SoundWave::SaveStream): every other build has the
// decoders and the header reader only. Built on its own, outside the engine (a test tool), it has everything.
#ifndef DSPADPCM_ENCODER
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_DOLPHIN) || defined(PLATFORM_3DS)
#define DSPADPCM_ENCODER (EDITOR)
#else
#define DSPADPCM_ENCODER 1
#endif
#endif

#include <cstdint>
#include <cstddef>
#include <vector>

namespace DspAdpcm
{
    constexpr uint32_t kSamplesPerFrame = 14;
    constexpr uint32_t kNibblesPerFrame = 16;
    constexpr uint32_t kBytesPerFrame = 8;
    constexpr uint32_t kHeaderSize = 0x60;   // the standard DSPADPCMHEADER

    // ---- sample <-> nibble address helpers (relative to the start of the channel's data) ----
    inline uint32_t FrameCount(uint32_t samples) { return (samples + kSamplesPerFrame - 1) / kSamplesPerFrame; }
    // Nibble address of sample s (what goes in loop_start / loop_end / the voice's current address).
    inline uint32_t SampleToNibble(uint32_t s) { return (s / kSamplesPerFrame) * kNibblesPerFrame + (s % kSamplesPerFrame) + 2; }
    // Nibble count for n samples (header field num_nibbles): last sample's address + 1.
    inline uint32_t NibbleCount(uint32_t n)
    {
        uint32_t extra = n % kSamplesPerFrame;
        return (n / kSamplesPerFrame) * kNibblesPerFrame + (extra ? extra + 2 : 0);
    }
    // Bytes of ADPCM data written by Encode(): whole frames (the last frame is zero padded).
    inline uint32_t DataBytes(uint32_t n) { return FrameCount(n) * kBytesPerFrame; }
    // Same, padded to the 32-byte unit ARAM DMA wants.
    inline uint32_t DataBytesAram(uint32_t n) { return (DataBytes(n) + 31u) & ~31u; }

    // One encoded channel. Everything the DSP voice needs, plus the data.
    struct Channel
    {
        uint32_t numSamples = 0;
        uint32_t numNibbles = 0;
        uint32_t sampleRate = 0;
        bool     loop = false;
        uint32_t loopStart = 0;     // samples (inclusive)
        uint32_t loopEnd = 0;       // samples (inclusive, last sample played before wrapping)
        int16_t  coefs[16] = {};    // 8 pairs: coefs[2i] = c1 (applied to yn1), coefs[2i+1] = c2 (yn2), 1.11 fixed point
        uint16_t gain = 0;          // always 0 for ADPCM
        uint16_t ps = 0;            // initial predictor/scale (= header byte of frame 0)
        int16_t  yn1 = 0, yn2 = 0;  // initial history (0: the stream starts from silence)
        uint16_t loopPs = 0;        // header byte of the frame holding loopStart
        int16_t  loopYn1 = 0;       // decoded sample loopStart-1 (0 if loopStart == 0)
        int16_t  loopYn2 = 0;       // decoded sample loopStart-2 (0 if loopStart < 2)
        std::vector<uint8_t> data;  // DataBytes(numSamples) bytes of frames
    };

#if DSPADPCM_ENCODER
    // Pick the 8 predictor pairs for a sound (Nintendo's correlation + 3 rounds of splitting).
    // pcm is mono; stride lets it read one channel of interleaved audio (stride 2 = stereo).
    void CalculateCoefficients(const int16_t* pcm, uint32_t numSamples, uint32_t stride, int16_t outCoefs[16]);

    // Encode one channel. loopStart/loopEnd are in samples; pass loop=false for a one-shot.
    // loopEnd defaults to the last sample. If decodedOut is given, it receives the encoder's
    // reconstruction (identical to what the DSP will play).
    void Encode(const int16_t* pcm, uint32_t numSamples, uint32_t stride, uint32_t sampleRate,
                bool loop, uint32_t loopStart, uint32_t loopEnd, Channel& out,
                std::vector<int16_t>* decodedOut = nullptr);

    // Encode with caller-supplied coefficients (e.g. one coef set shared by a stream's blocks).
    void EncodeWithCoefs(const int16_t* pcm, uint32_t numSamples, uint32_t stride, const int16_t coefs[16],
                         int16_t hist1, int16_t hist2, uint8_t* outData, int16_t* decodedOut);

#endif // DSPADPCM_ENCODER

    // Straight decode of numSamples from the start (header ps per frame, history yn1/yn2).
    void Decode(const Channel& ch, std::vector<int16_t>& out);

    // Decode the way the DSP's accelerator does: nibble addressing, the predictor/scale latched
    // only when the address crosses into a new frame, and on reaching loop_end the address set
    // back to loop_start with loop_ps / loop_yn1 / loop_yn2 loaded. Produces count samples
    // (plays through the loop as many times as needed; a one-shot pads with zeros).
    void DecodeAccelerator(const Channel& ch, uint32_t count, std::vector<int16_t>& out);

    // The DSP's per-sample ADPCM step. nibble is the raw 4-bit value (0..15).
    inline int16_t DecodeSample(int nibble, uint16_t ps, const int16_t coefs[16], int16_t& yn1, int16_t& yn2)
    {
        int idx = (ps >> 4) & 7;
        int64_t c1 = coefs[idx * 2 + 0];
        int64_t c2 = coefs[idx * 2 + 1];
        int n = nibble & 0xF;
        if (n >= 8) n -= 16;
        int64_t scale = int64_t(1) << (ps & 0xF);
        // Dolphin: val = scale*nib + ((0x400 + c1*yn1 + c2*yn2) >> 11), saturated to s16.
        // (== (c1*yn1 + c2*yn2 + ((nib << shift) << 11) + 1024) >> 11.) The DSP accumulates in
        // 40 bits, so 64-bit here; it only differs from a 32-bit sum on inputs that overflow s32.
        int64_t v = scale * n + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        yn2 = yn1;
        yn1 = int16_t(v);
        return int16_t(v);
    }

    // Write / read the standard 0x60-byte big-endian DSPADPCMHEADER.
    void WriteHeader(const Channel& ch, uint8_t out[kHeaderSize]);
    bool ReadHeader(const uint8_t in[kHeaderSize], Channel& ch);   // fills everything but data
}
