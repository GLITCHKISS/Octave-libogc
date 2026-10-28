#pragma once

#include "Asset.h"

// The GameCube Codec property: how the GameCube cook writes a sound. Project Default follows Config.ini's
// GameCubeSoundCodec (0 PCM, the default; 1 DSP-ADPCM), for sounds that aren't Stream: a Stream sound is DSP-ADPCM only
// marked so itself. Every other platform, the editor and the source asset keep the sound's PCM (or Vorbis) whatever it says.
enum class GcSoundCodec : uint8_t
{
    ProjectDefault = 0,
    Pcm = 1,
    DspAdpcm = 2,

    Count
};

// What a cooked SoundWave's audio is (the byte after its format words; a bool, compressed or not, before
// ASSET_VERSION_SOUND_GC_CODEC -- the same 0 or 1).
enum SoundPayload : uint8_t
{
    SOUND_PAYLOAD_PCM = 0,
    SOUND_PAYLOAD_VORBIS = 1,
    SOUND_PAYLOAD_DSP_ADPCM = 2,     // the GameCube cook only (SoundWave::SaveStream)
};

// A Stream sound's DSP-ADPCM is laid out in blocks of this many bytes a channel, the channels' blocks in turn (so the
// console reads it from the disc front to back); a sound effect's channels one after the other.
#define SOUND_ADPCM_INTERLEAVE 2048

#if PLATFORM_GAMECUBE
// A DSP-ADPCM sound as the GameCube loaded it (SoundWave::LoadStream): what decoding it takes.
struct SoundAdpcmInfo
{
    uint32_t channels = 0;          // 1 or 2 (0: not DSP-ADPCM)
    uint32_t layout = 0;            // 0: the channels one after the other; 1: blocks of `interleave` bytes in turn
    uint32_t interleave = 0;        // (layout 1)
    uint32_t channelBytes = 0;      // a channel's data: whole 8-byte frames, padded to 32 bytes
    uint32_t samples = 0;           // a channel's samples
    int16_t  coefs[2][16] = {};     // each channel's 8 predictor pairs
    int16_t  yn1[2] = {};           // each channel's history at its start (0)
    int16_t  yn2[2] = {};
};
#endif

class SoundWave : public Asset
{
public:

    DECLARE_ASSET(SoundWave, Asset);

    SoundWave();
    ~SoundWave();

    virtual void LoadStream(Stream& stream, Platform platform) override;
    virtual int32_t GetFileReadLimit(const char* path) override;
    virtual void SaveStream(Stream& stream, Platform platform) override;
    virtual void Create() override;
    virtual void Destroy() override;
    virtual bool Import(const std::string& path, ImportOptions* options) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;
    virtual glm::vec4 GetTypeColor() override;
    virtual const char* GetTypeName() override;
    virtual const char* GetTypeImportExt() override;

    void SetPcmData(uint8_t* data, uint32_t size, uint32_t numSamples, uint32_t bitsPerSample, uint32_t numChannels, uint32_t sampleRate);

    void SetVolumeMultiplier(float volume);
    float GetVolumeMultiplier() const;

    void SetPitchMultiplier(float pitch);
    float GetPitchMultiplier() const;

    void SetAudioClass(int8_t audioClass);
    int8_t GetAudioClass() const;

    // How many copies of this sound may play at once; 0 (the default) is any number. Playing one
    // more stops the oldest (AudioManager), so 1 makes a sound cut itself off.
    void SetMaxInstances(uint8_t maxInstances) { mMaxInstances = maxInstances; }
    uint8_t GetMaxInstances() const { return mMaxInstances; }

    // The GameCube cook's codec for it (GcSoundCodec); only the GameCube cook acts on it.
    void SetGameCubeCodec(GcSoundCodec codec) { mGcCodec = int32_t(codec); }
    GcSoundCodec GetGameCubeCodec() const { return GcSoundCodec(mGcCodec); }
#if EDITOR
    // Whether the GameCube cook writes it as DSP-ADPCM: marked so, or Project Default with Config.ini's
    // GameCubeSoundCodec=1 and not a Stream sound.
    bool CooksAsDspAdpcm() const;
#endif

    uint8_t* GetWaveData() const;
    uint32_t GetWaveDataSize() const;
    uint32_t GetNumChannels() const;
    uint32_t GetBitsPerSample() const;
    uint32_t GetSampleRate() const;
    uint32_t GetNumSamples() const;
    uint32_t GetBlockAlign() const;
    uint32_t GetByteRate() const;

    float GetDuration() const;

    bool GetStream() const;
    const uint8_t* GetCompressedData() const;
    uint32_t GetCompressedSize() const;

    // A Stream sound on console, loaded from a file: the compressed audio is NOT in memory.
    // It is this many bytes of the asset's own file, from this offset, read while it plays.
    const std::string& GetDiscPath() const { return mDiscPath; }
    uint32_t GetDiscOffset() const { return mDiscOffset; }
    uint32_t GetDiscSize() const { return mDiscSize; }
    // That range is PCM, not Vorbis: a Stream sound cooked uncompressed (Compress off), in the format it plays in --
    // as a sound effect's PCM (little-endian 16-bit or unsigned 8-bit, channels interleaved). Nothing to decode.
    bool GetDiscPcm() const { return mDiscPcm; }
#if PLATFORM_GAMECUBE
    // Cooked DSP-ADPCM (GameCube Codec): decoded on the CPU as it loads (a sound effect, into ARAM), or -- a Stream
    // sound left on the disc (GetDiscAdpcm) -- as it plays, or as it's copied to ARAM (AUD_CopyTrackToAram).
    bool IsAdpcm() const { return mAdpcm.channels != 0; }
    bool GetDiscAdpcm() const { return mDiscAdpcm; }
    const SoundAdpcmInfo& GetAdpcmInfo() const { return mAdpcm; }
#endif

    // GameCube: the PCM moved to ARAM (see Audio_Dolphin.cpp), at this ARAM address; 0 while it
    // is in main memory. GetWaveData() is null once it has moved; GetWaveDataSize() still holds.
    uint32_t GetAramAddress() const { return mAramAddress; }
    void MoveWaveDataToAram(uint32_t aramAddress);

protected:

    static bool HandlePropChange(Datum* datum, uint32_t index, const void* newValue);

    void LoadDspAdpcm(Stream& stream);
#if EDITOR
    bool SaveDspAdpcm(Stream& stream);
#endif

    uint8_t* mWaveData = nullptr;
    uint32_t mWaveDataSize = 0;

    uint8_t* mCompressedData = nullptr;
    uint32_t mCompressedSize = 0;

    std::string mDiscPath;
    uint32_t mDiscOffset = 0;
    uint32_t mDiscSize = 0;
    bool mDiscPcm = false;
    bool mDiscAdpcm = false;   // (GameCube: that range is DSP-ADPCM, in blocks: SoundAdpcmInfo)
#if PLATFORM_GAMECUBE
    SoundAdpcmInfo mAdpcm;
#endif

    uint32_t mAramAddress = 0;

    // Properties
    float mVolumeMultiplier = 1.0f;
    float mPitchMultiplier = 1.0f;
    int8_t mAudioClass = 0;
    bool mCompress = false;
    bool mCompressInternal = false;
    bool mStream = false;   // Stream-decode compressed audio on console (music)
    uint8_t mMaxInstances = 0;  // copies playing at once; 0 is any number
    int32_t mGcCodec = 0;       // GcSoundCodec: the GameCube cook's codec (Project Default)

    // Soundwave Format
    uint32_t mNumChannels = 1;
    uint32_t mBitsPerSample = 8;
    uint32_t mSampleRate = 22050;
    uint32_t mNumSamples = 0;
    uint32_t mBlockAlign = 0;
    uint32_t mByteRate = 0;
};
