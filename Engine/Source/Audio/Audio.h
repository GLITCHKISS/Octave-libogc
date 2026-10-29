#pragma once

#include "EngineTypes.h"

class Stream;
class SoundWave;
class Audio3D;

struct PcmFormat
{
    uint32_t mNumChannels = 2;
    uint32_t mSampleRate = 44100;
    uint32_t mBytesPerSample = 2;
};

void AUD_Initialize();
void AUD_Shutdown();
void AUD_Update();

// maxFrames: a scheduled stop (0: none). The voice ends by itself once it has played that many of the sound's sample
// frames from where it started (looping, its passes counted together), on that very sample: not when game code stops
// it, which only lands on the platform's next mix. AUD_IsPlaying is false from then on.
void AUD_Play(
    uint32_t voiceIndex,
    SoundWave* soundWave,
    float volume,
    float pitch,
    bool loop,
    float startTime,
    bool spatial,
    uint32_t maxFrames = 0);

void AUD_Stop(uint32_t voiceIndex);
bool AUD_IsPlaying(uint32_t voiceIndex);
void AUD_SetVolume(uint32_t voiceIndex, float leftVolume, float rightVolume);
void AUD_SetPitch(uint32_t voiceIndex, float pitch);

uint8_t* AUD_AllocWaveBuffer(uint32_t size);
void AUD_FreeWaveBuffer(void* buffer);
void AUD_ProcessWaveBuffer(SoundWave* soundWave);

#if PLATFORM_GAMECUBE
// Sound effects live in ARAM (see Audio_Dolphin.cpp). Frees a SoundWave's ARAM block.
void AUD_FreeAram(uint32_t aramAddress);
// ARAM: total and free bytes, and the bytes of sound held there.
void AUD_GetAramStats(uint32_t& total, uint32_t& free, uint32_t& sounds);
// A disc-streamed sound (loaded) copied into ARAM and played from there once whole: no decoding while it plays, and
// several at once cost the CPU nothing. Only the sounds asked for: the rest stream. About its PCM's size in ARAM
// (1 MB of ARAM always kept free: a track that doesn't fit streams). False: not a disc stream, or no ARAM.
bool AUD_CopyTrackToAram(SoundWave* soundWave);
// The tracks asked for so far copied now (a loading screen's wait), not in the background in play: returns once
// they're all made or skipped, or after timeoutMs.
void AUD_WaitForTracks(uint32_t timeoutMs);
// A disc-streamed sound's copy in ARAM freed as the sound goes, any voice playing from it stopped. True if one could
// have been.
bool AUD_ReleaseTrack(SoundWave* soundWave);
// A DSP-ADPCM sound (a GameCube cook) loaded into memory: decoded on the CPU into ARAM as the 16-bit PCM it plays as, a
// piece at a time (its PCM is never whole in main memory), then played as any ARAM sound. No ARAM, or no room: decoded
// into main memory instead. `data` is its channels' nibble data (SoundWave::GetAdpcmInfo's layout). False: no memory.
bool AUD_StoreAdpcm(SoundWave* soundWave, const uint8_t* data, uint32_t size);
#endif

// Streaming PCM voices: push audio that is produced at runtime (e.g. a video's
// soundtrack). Data is 16-bit signed little-endian interleaved PCM. Streams start
// paused. AUD_OpenStream() returns 0 if no stream could be opened.
uint32_t AUD_OpenStream(uint32_t sampleRate, uint32_t numChannels);
void AUD_CloseStream(uint32_t streamId);
void AUD_QueueStreamData(uint32_t streamId, const uint8_t* data, uint32_t size);
uint64_t AUD_GetStreamPlayedFrames(uint32_t streamId);   // since open / last flush
void AUD_SetStreamPaused(uint32_t streamId, bool paused);
void AUD_SetStreamVolume(uint32_t streamId, float volume);
void AUD_FlushStream(uint32_t streamId);

// Platform Independent
void AUD_EncodeVorbis(Stream& inStream, Stream& outStream, PcmFormat format);
void AUD_DecodeVorbis(Stream& inStream, Stream& outStream, PcmFormat format);
