#if PLATFORM_WINDOWS

#include "Audio/Audio.h"
#include "Audio/AudioConstants.h"
#include "System/System.h"

#include "Assets/SoundWave.h"
#include "Log.h"

#include <xaudio2.h>

#include <algorithm>
#include <mutex>
#include <vector>

static IXAudio2* sXAudio2 = nullptr;
static IXAudio2MasteringVoice* sMasterVoice = nullptr;
static IXAudio2SourceVoice* sSourceVoices[AUDIO_MAX_VOICES] = { };
static XAUDIO2_BUFFER sSourceBuffers[AUDIO_MAX_VOICES] = { };
static uint8_t* sStereoConvertedBuffers[AUDIO_MAX_VOICES] = { };

// A looping voice with a scheduled stop (AUD_Play's maxFrames): its passes of the whole sound still to submit, in buffers
// that each play it up to 255 times (LoopCount 254), then the last pass's frames (its tail, a buffer of its own that
// ends the stream). Submitted a few buffers at a time (SubmitLimitedPasses, again from AUD_Update): XAudio2 queues 64
// at most, and a short sound looped a long time needs more.
struct LimitedLoop
{
    uint32_t fullPasses = 0;
    uint32_t tailFrames = 0;
    bool     pending = false;
};
static LimitedLoop sLimitedLoops[AUDIO_MAX_VOICES] = { };
static const uint32_t kLimitedQueue = 16;

static void SubmitLimitedPasses(uint32_t voiceIndex)
{
    LimitedLoop& l = sLimitedLoops[voiceIndex];
    IXAudio2SourceVoice* voice = sSourceVoices[voiceIndex];
    if (!l.pending || voice == nullptr)
        return;

    XAUDIO2_VOICE_STATE state;
    voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    uint32_t queued = state.BuffersQueued;
    while (l.pending && queued < kLimitedQueue)
    {
        XAUDIO2_BUFFER b = sSourceBuffers[voiceIndex];   // (the sound's data, as the first buffer has it)
        b.PlayBegin = 0;
        b.LoopBegin = 0;
        b.LoopLength = 0;
        if (l.fullPasses > 0)
        {
            const uint32_t plays = glm::min<uint32_t>(l.fullPasses, XAUDIO2_MAX_LOOP_COUNT + 1);
            b.PlayLength = 0;                            // (the whole sound)
            b.LoopCount = plays - 1;
            l.fullPasses -= plays;
            b.Flags = (l.fullPasses == 0 && l.tailFrames == 0) ? XAUDIO2_END_OF_STREAM : 0;
        }
        else
        {
            b.PlayLength = l.tailFrames;                 // (the last pass: ends on its frame)
            b.LoopCount = 0;
            b.Flags = XAUDIO2_END_OF_STREAM;
            l.tailFrames = 0;
        }
        l.pending = (l.fullPasses > 0 || l.tailFrames > 0);
        if (voice->SubmitSourceBuffer(&b) < 0)
        {
            LogError("XAudio2: a scheduled stop's buffer was refused; the sound ends early");
            l = LimitedLoop();
            break;
        }
        ++queued;
    }
}

// Push-PCM streams (AUD_OpenStream). Each submitted chunk is a heap copy that
// XAudio2 references until it has played; OnBufferEnd (on the XAudio2 thread)
// hands it back so the main thread can free it.
#define AUD_MAX_PCM_STREAMS 4

class PcmStreamCallback : public IXAudio2VoiceCallback
{
public:
    std::mutex mMutex;
    std::vector<BYTE*> mFinished;

    void STDMETHODCALLTYPE OnBufferEnd(void* context) override
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mFinished.push_back((BYTE*)context);
    }

    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 bytesRequired) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void* context) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void* context) override {}
    void STDMETHODCALLTYPE OnVoiceError(void* context, HRESULT error) override {}
};

struct PcmStream
{
    IXAudio2SourceVoice* mVoice = nullptr;
    PcmStreamCallback* mCallback = nullptr;
    std::vector<BYTE*> mLiveBuffers;
    uint64_t mPlayedBase = 0;
    bool mPaused = true;
};

static PcmStream sPcmStreams[AUD_MAX_PCM_STREAMS];

static PcmStream* GetPcmStream(uint32_t streamId)
{
    if (streamId == 0 || streamId > AUD_MAX_PCM_STREAMS)
        return nullptr;

    PcmStream* stream = &sPcmStreams[streamId - 1];
    return (stream->mVoice != nullptr) ? stream : nullptr;
}

static void FreeFinishedPcmBuffers(PcmStream& stream)
{
    std::vector<BYTE*> finished;
    {
        std::lock_guard<std::mutex> lock(stream.mCallback->mMutex);
        finished.swap(stream.mCallback->mFinished);
    }

    for (BYTE* buffer : finished)
    {
        auto it = std::find(stream.mLiveBuffers.begin(), stream.mLiveBuffers.end(), buffer);
        if (it != stream.mLiveBuffers.end())
        {
            stream.mLiveBuffers.erase(it);
            delete[] buffer;
        }
    }
}

// An attempt to reuse source voices?
//struct WaveFormat
//{
//    uint32_t mSampleRate = 44100;
//    uint8_t mNumChannels = 2;
//    uint8_t mBitsPerSample = 16;
//};
//static WaveFormat sLastWaveFormats[AUDIO_MAX_VOICES] = {};

void AUD_Initialize()
{
    if (XAudio2Create(&sXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR) < 0)
    {
        LogError("Failed to create XAudio2 engine");
    }

    if (sXAudio2->CreateMasteringVoice(&sMasterVoice) < 0)
    {
        LogError("Failed to create mastering voice");
    }

    // TODO: Prime the XAudio2 internal memory pool for source voices by allocating many.
    // And then immediately destroying them I suppose?
}

void AUD_Shutdown()
{
    for (uint32_t i = 0; i < AUD_MAX_PCM_STREAMS; ++i)
    {
        AUD_CloseStream(i + 1);
    }

    for (uint32_t i = 0; i < AUDIO_MAX_VOICES; ++i)
    {
        if (sSourceVoices[i] != nullptr)
        {
            sSourceVoices[i]->DestroyVoice();
            // TODO: Do we need to call delete??
            sSourceVoices[i] = nullptr;
        }
    }
    sMasterVoice->DestroyVoice();
    sXAudio2->Release();
}

void AUD_Update()
{
    for (uint32_t v = 0; v < AUDIO_MAX_VOICES; ++v)
    {
        SubmitLimitedPasses(v);   // (a looping voice with a scheduled stop: its passes kept queued)
    }

    for (uint32_t i = 0; i < AUD_MAX_PCM_STREAMS; ++i)
    {
        if (sPcmStreams[i].mVoice != nullptr)
        {
            FreeFinishedPcmBuffers(sPcmStreams[i]);
        }
    }
}

void AUD_Play(
    uint32_t voiceIndex,
    SoundWave* soundWave,
    float volume,
    float pitch,
    bool loop,
    float startTime,
    bool spatial,
    uint32_t maxFrames)
{
    OCT_ASSERT(sSourceVoices[voiceIndex] == nullptr);

    bool monoInput = (soundWave->GetNumChannels() == 1);
    if (monoInput)
    {
        OCT_ASSERT(sStereoConvertedBuffers[voiceIndex] == nullptr);
        sStereoConvertedBuffers[voiceIndex] = new uint8_t[soundWave->GetWaveDataSize() * 2];

        uint32_t numSamples = soundWave->GetNumSamples();
        uint32_t sampleSize = (soundWave->GetBitsPerSample() == 8) ? 1 : 2;

        uint8_t* srcData = soundWave->GetWaveData();
        uint8_t* dstData = sStereoConvertedBuffers[voiceIndex];

        for (uint32_t i = 0; i < numSamples; ++i)
        {
            memcpy(dstData, srcData, sampleSize);
            memcpy(dstData + sampleSize, srcData, sampleSize);

            srcData += sampleSize;
            dstData += (sampleSize * 2);
        }
    }

    float startPercent = startTime / soundWave->GetDuration();
    startPercent = glm::clamp(startPercent, 0.0f, 1.0f);
    uint32_t startingSample = uint32_t(startPercent * soundWave->GetNumSamples());

    sSourceBuffers[voiceIndex].Flags = XAUDIO2_END_OF_STREAM;
    sSourceBuffers[voiceIndex].AudioBytes = soundWave->GetWaveDataSize();
    sSourceBuffers[voiceIndex].pAudioData = soundWave->GetWaveData();
    sSourceBuffers[voiceIndex].PlayBegin = startingSample;
    sSourceBuffers[voiceIndex].PlayLength = 0;
    sSourceBuffers[voiceIndex].LoopBegin = 0;
    sSourceBuffers[voiceIndex].LoopLength = 0;
    sSourceBuffers[voiceIndex].LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;

    // A scheduled stop: the voice ends on that frame. Within the first pass (or not looping): this buffer's PlayLength.
    // Looping past it: this buffer plays the first pass only, and the passes after it follow in buffers of their own
    // (SubmitLimitedPasses), gapless, the last one cut at its frame.
    sLimitedLoops[voiceIndex] = LimitedLoop();
    // (In sample frames, as PlayBegin and PlayLength are: GetNumSamples counts each channel's samples.)
    const uint32_t numFrames = soundWave->GetWaveDataSize() / glm::max<uint32_t>(soundWave->GetBlockAlign(), 1u);
    if (maxFrames != 0 && numFrames > startingSample)
    {
        const uint32_t firstPass = numFrames - startingSample;
        sSourceBuffers[voiceIndex].LoopCount = 0;
        if (!loop || maxFrames <= firstPass)
        {
            sSourceBuffers[voiceIndex].PlayLength = glm::min(maxFrames, firstPass);
        }
        else
        {
            sSourceBuffers[voiceIndex].Flags = 0;   // (more follows)
            const uint32_t rest = maxFrames - firstPass;
            sLimitedLoops[voiceIndex].fullPasses = rest / numFrames;
            sLimitedLoops[voiceIndex].tailFrames = rest % numFrames;
            sLimitedLoops[voiceIndex].pending = true;
        }
    }

    WAVEFORMATEX waveFormat = {};
    waveFormat.wFormatTag = WAVE_FORMAT_PCM;
    waveFormat.nChannels = soundWave->GetNumChannels();
    waveFormat.nSamplesPerSec = soundWave->GetSampleRate();
    waveFormat.wBitsPerSample = (uint16_t)soundWave->GetBitsPerSample();
    waveFormat.nBlockAlign = soundWave->GetBlockAlign();
    waveFormat.nAvgBytesPerSec = soundWave->GetByteRate();
    waveFormat.cbSize = 0;

    if (monoInput)
    {
        // Use ephemeral stereo buffer
        sSourceBuffers[voiceIndex].AudioBytes *= 2;
        sSourceBuffers[voiceIndex].pAudioData = sStereoConvertedBuffers[voiceIndex];
        waveFormat.nAvgBytesPerSec *= 2;
        waveFormat.nBlockAlign *= 2;
        waveFormat.nChannels = 2;
    }

    if (sXAudio2->CreateSourceVoice(&sSourceVoices[voiceIndex], &waveFormat) >= 0)
    {
        sSourceVoices[voiceIndex]->SubmitSourceBuffer(&sSourceBuffers[voiceIndex]);
        SubmitLimitedPasses(voiceIndex);

        // Spatial sounds will update their volume every frame.
        sSourceVoices[voiceIndex]->SetVolume(spatial ? 0.0f : volume);

        sSourceVoices[voiceIndex]->SetFrequencyRatio(pitch);
        sSourceVoices[voiceIndex]->Start();
    }
    else
    {
        LogError("Error creating XAUDIO2 source voice");
        OCT_ASSERT(0);
    }
}

void AUD_Stop(uint32_t voiceIndex)
{
    OCT_ASSERT(sSourceVoices[voiceIndex] != nullptr);
    sSourceVoices[voiceIndex]->Stop();
    sSourceVoices[voiceIndex]->DestroyVoice();
    sSourceVoices[voiceIndex] = nullptr;
    sLimitedLoops[voiceIndex] = LimitedLoop();

    if (sStereoConvertedBuffers[voiceIndex] != nullptr)
    {
        delete sStereoConvertedBuffers[voiceIndex];
        sStereoConvertedBuffers[voiceIndex] = nullptr;
    }
}

bool AUD_IsPlaying(uint32_t voiceIndex)
{
    OCT_ASSERT(sSourceVoices[voiceIndex] != nullptr);
    XAUDIO2_VOICE_STATE state;
    sSourceVoices[voiceIndex]->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return (state.BuffersQueued != 0);
}

void AUD_SetVolume(uint32_t voiceIndex, float leftVolume, float rightVolume)
{
    OCT_ASSERT(sSourceVoices[voiceIndex] != nullptr);

    // Use this version to set volume of all channels
    // sSourceVoices[voiceIndex]->SetVolume((leftVolume + rightVolume) / 2.0f);

    // Use this version to set volume of left/right ear
    sSourceVoices[voiceIndex]->SetVolume(1.0f);
    float volumes[2] = { leftVolume, rightVolume };
    sSourceVoices[voiceIndex]->SetChannelVolumes(2, volumes);
}

void AUD_SetPitch(uint32_t voiceIndex, float pitch)
{
    OCT_ASSERT(sSourceVoices[voiceIndex] != nullptr);
    sSourceVoices[voiceIndex]->SetFrequencyRatio(pitch);
}

uint8_t* AUD_AllocWaveBuffer(uint32_t size)
{
    return (uint8_t*)SYS_AlignedMalloc(size, 32);
}

void AUD_FreeWaveBuffer(void* buffer)
{
    SYS_AlignedFree(buffer);
}

void AUD_ProcessWaveBuffer(SoundWave* soundWave)
{

}

uint32_t AUD_OpenStream(uint32_t sampleRate, uint32_t numChannels)
{
    if (sXAudio2 == nullptr || sampleRate == 0 || numChannels < 1 || numChannels > 2)
    {
        return 0;
    }

    for (uint32_t i = 0; i < AUD_MAX_PCM_STREAMS; ++i)
    {
        PcmStream& stream = sPcmStreams[i];
        if (stream.mVoice != nullptr)
        {
            continue;
        }

        WAVEFORMATEX waveFormat = {};
        waveFormat.wFormatTag = WAVE_FORMAT_PCM;
        waveFormat.nChannels = (WORD)numChannels;
        waveFormat.nSamplesPerSec = sampleRate;
        waveFormat.wBitsPerSample = 16;
        waveFormat.nBlockAlign = (WORD)(numChannels * 2);
        waveFormat.nAvgBytesPerSec = sampleRate * waveFormat.nBlockAlign;
        waveFormat.cbSize = 0;

        stream.mCallback = new PcmStreamCallback();

        if (sXAudio2->CreateSourceVoice(&stream.mVoice, &waveFormat, 0, XAUDIO2_DEFAULT_FREQ_RATIO, stream.mCallback) < 0)
        {
            LogError("Error creating XAudio2 streaming voice");
            stream.mVoice = nullptr;
            delete stream.mCallback;
            stream.mCallback = nullptr;
            return 0;
        }

        stream.mPlayedBase = 0;
        stream.mPaused = true;
        return i + 1;
    }

    return 0;
}

void AUD_CloseStream(uint32_t streamId)
{
    PcmStream* stream = GetPcmStream(streamId);
    if (stream == nullptr)
    {
        return;
    }

    // DestroyVoice waits for any in-progress callbacks, so everything is safe to free after.
    stream->mVoice->Stop();
    stream->mVoice->DestroyVoice();
    stream->mVoice = nullptr;

    for (BYTE* buffer : stream->mLiveBuffers)
    {
        delete[] buffer;
    }
    stream->mLiveBuffers.clear();

    delete stream->mCallback;
    stream->mCallback = nullptr;
}

void AUD_QueueStreamData(uint32_t streamId, const uint8_t* data, uint32_t size)
{
    PcmStream* stream = GetPcmStream(streamId);
    if (stream == nullptr || data == nullptr || size == 0)
    {
        return;
    }

    FreeFinishedPcmBuffers(*stream);

    BYTE* buffer = new BYTE[size];
    memcpy(buffer, data, size);

    XAUDIO2_BUFFER xaBuffer = {};
    xaBuffer.AudioBytes = size;
    xaBuffer.pAudioData = buffer;
    xaBuffer.pContext = buffer;

    if (stream->mVoice->SubmitSourceBuffer(&xaBuffer) >= 0)
    {
        stream->mLiveBuffers.push_back(buffer);
    }
    else
    {
        delete[] buffer;
    }
}

uint64_t AUD_GetStreamPlayedFrames(uint32_t streamId)
{
    PcmStream* stream = GetPcmStream(streamId);
    if (stream == nullptr)
    {
        return 0;
    }

    XAUDIO2_VOICE_STATE state;
    stream->mVoice->GetState(&state);
    return state.SamplesPlayed - stream->mPlayedBase;
}

void AUD_SetStreamPaused(uint32_t streamId, bool paused)
{
    PcmStream* stream = GetPcmStream(streamId);
    if (stream == nullptr || stream->mPaused == paused)
    {
        return;
    }

    stream->mPaused = paused;

    if (paused)
    {
        stream->mVoice->Stop();
    }
    else
    {
        stream->mVoice->Start();
    }
}

void AUD_SetStreamVolume(uint32_t streamId, float volume)
{
    PcmStream* stream = GetPcmStream(streamId);
    if (stream != nullptr)
    {
        stream->mVoice->SetVolume(volume);
    }
}

void AUD_FlushStream(uint32_t streamId)
{
    PcmStream* stream = GetPcmStream(streamId);
    if (stream == nullptr)
    {
        return;
    }

    stream->mVoice->Stop();
    stream->mVoice->FlushSourceBuffers();

    XAUDIO2_VOICE_STATE state;
    stream->mVoice->GetState(&state);
    stream->mPlayedBase = state.SamplesPlayed;

    if (!stream->mPaused)
    {
        stream->mVoice->Start();
    }
}

#endif
