#include "Assets/SoundWave.h"

#include "Log.h"
#include "Stream.h"
#include "Property.h"
#include "AudioManager.h"

#include "Audio/Audio.h"
#include "Audio/DspAdpcm.h"
#include "System/System.h"
#include "Engine.h"

#include <cmath>
#include <vector>

FORCE_LINK_DEF(SoundWave);
DEFINE_ASSET(SoundWave);

static const char* sGcCodecStrings[] =
{
    "Project Default",
    "PCM",
    "DSP-ADPCM",
};
static_assert(int32_t(GcSoundCodec::Count) == 3, "Need to update string conversion table");

// This is causing very loud sound corruption on 3DS, need to
// debug this if we are going to turn it back on.
#define LQ_CONSOLE_AUDIO 0

bool SoundWave::HandlePropChange(Datum* datum, uint32_t index, const void* newValue)
{
    Property* prop = static_cast<Property*>(datum);
    SoundWave* soundWave = (SoundWave*)prop->mOwner;
    bool handled = false;

    if (prop->mName == "Play")
    {
        AudioManager::PlaySound2D(soundWave);
        handled = true;
    }
    else if (prop->mName == "Stop")
    {
        AudioManager::StopSounds(soundWave);
        handled = true;
    }
    else
    {
        HandleAssetPropChange(datum, index, newValue);
    }

    return handled;
}

SoundWave::SoundWave()
{
    mType = SoundWave::GetStaticType();
}

SoundWave::~SoundWave()
{

}

int32_t SoundWave::GetFileReadLimit(const char* path)
{
#if PLATFORM_DOLPHIN && !EDITOR
    // A Stream sound leaves its audio ON THE DISC and reads it as it plays (see Audio_Dolphin):
    // its Vorbis, or -- cooked uncompressed -- its PCM. So only the front of the file is loaded:
    // everything up to the audio. Peek at it to find out whether this is one, and where its audio
    // starts.
    mDiscSize = 0;
    mDiscPcm = false;
    mDiscAdpcm = false;

    Stream head;
    if (!head.ReadFile(path, true, 1024))
    {
        return 0;
    }

    AssetHeader header = ReadHeader(head);
    head.SetAssetVersion(header.mVersion);

    std::string name;
    head.ReadString(name);

    // volume, pitch, class, compress, compress internal, stream, (max instances,) (GameCube codec,) six format words,
    // payload (compressed or not, before ASSET_VERSION_SOUND_GC_CODEC), size
    const bool maxInstances = (header.mVersion >= ASSET_VERSION_SOUND_MAX_INSTANCES);
    const bool gcCodec = (header.mVersion >= ASSET_VERSION_SOUND_GC_CODEC);
    const uint32_t need = 4 + 4 + 1 + 3 + (maxInstances ? 1 : 0) + (gcCodec ? 1 : 0) + 6 * 4 + 1 + 4;
    if (header.mVersion < ASSET_VERSION_SOUND_STREAM || head.GetPos() + need > head.GetSize())
    {
        return 0;
    }

    head.ReadFloat();
    head.ReadFloat();
    head.ReadInt8();
    head.ReadBool();
    head.ReadBool();
    bool streamed = head.ReadBool();
    if (maxInstances)
    {
        head.ReadUint8();
    }
    if (gcCodec)
    {
        head.ReadUint8();
    }
    const uint32_t channels = head.ReadUint32();
    for (uint32_t i = 1; i < 6; ++i)
    {
        head.ReadUint32();
    }
    const uint8_t payload = gcCodec ? head.ReadUint8() : uint8_t(head.ReadBool() ? SOUND_PAYLOAD_VORBIS : SOUND_PAYLOAD_PCM);
    bool compressed = (payload == SOUND_PAYLOAD_VORBIS);

#if PLATFORM_GAMECUBE
    if (payload == SOUND_PAYLOAD_DSP_ADPCM)
    {
        // DSP-ADPCM (GameCube Codec): a Stream sound's nibble data is left on the disc, decoded as it plays or as it's
        // copied to ARAM; the headers before it load (LoadDspAdpcm). Its blocks are as the cook lays out a Stream
        // sound's (SOUND_ADPCM_INTERLEAVE): anything else is loaded whole and decoded into ARAM, as a sound effect is.
        if (!streamed || head.GetPos() + 4 + 16 > head.GetSize())
        {
            return 0;
        }
        head.ReadUint32();                                  // (the payload's size)
        const uint32_t blockAt = head.GetPos();
        const uint32_t adpcmChannels = head.ReadUint8();
        const uint32_t layout = head.ReadUint8();
        head.ReadUint16();
        const uint32_t interleave = head.ReadUint32();
        const uint32_t channelBytes = head.ReadUint32();
        if (adpcmChannels < 1 || adpcmChannels > 2 || adpcmChannels != channels || layout != 1 ||
            interleave != SOUND_ADPCM_INTERLEAVE || channelBytes == 0 || (channelBytes & 31) != 0)
        {
            return 0;
        }
        mDiscSize = adpcmChannels * channelBytes;
        mDiscOffset = blockAt + ((16 + DspAdpcm::kHeaderSize * adpcmChannels + 31) & ~31u);
        mDiscPath = path;
        mDiscAdpcm = true;
        return int32_t(mDiscOffset);
    }
#endif
    (void)channels;
    if (payload > SOUND_PAYLOAD_VORBIS)
    {
        return 0;   // (loaded whole: LoadStream says what it can't play)
    }

    // (PCM left on the disc only with AudioDecodeThread=1, as the rest of the ARAM tracks: without it, an uncompressed
    //  Stream sound is loaded whole, as ever)
    if (!streamed || (!compressed && !GetEngineConfig()->mAudioDecodeThread))
    {
        return 0;
    }

    mDiscSize = head.ReadUint32();      // (the Vorbis's size, or the PCM's: the same word either way)
    mDiscOffset = head.GetPos();
    mDiscPath = path;
    mDiscPcm = !compressed;
    return int32_t(mDiscOffset);
#else
    (void)path;
    return 0;
#endif
}

void SoundWave::LoadStream(Stream& stream, Platform platform)
{
    Asset::LoadStream(stream, platform);

    // Properties
    mVolumeMultiplier = stream.ReadFloat();
    mPitchMultiplier = stream.ReadFloat();
    mAudioClass = stream.ReadInt8();
    mCompress = stream.ReadBool();
    mCompressInternal = stream.ReadBool();
    if (stream.GetAssetVersion() >= ASSET_VERSION_SOUND_STREAM)
    {
        mStream = stream.ReadBool();
    }
    if (stream.GetAssetVersion() >= ASSET_VERSION_SOUND_MAX_INSTANCES)
    {
        mMaxInstances = stream.ReadUint8();
    }
    if (stream.GetAssetVersion() >= ASSET_VERSION_SOUND_GC_CODEC)
    {
        mGcCodec = stream.ReadUint8();
        if (mGcCodec >= int32_t(GcSoundCodec::Count))
        {
            mGcCodec = int32_t(GcSoundCodec::ProjectDefault);
        }
    }

    // Waveform Format
    mNumChannels = stream.ReadUint32();
    mBitsPerSample = stream.ReadUint32();
    mSampleRate = stream.ReadUint32();
    mNumSamples = stream.ReadUint32();
    mBlockAlign = stream.ReadUint32();
    mByteRate = stream.ReadUint32();

    const uint8_t payload = (stream.GetAssetVersion() >= ASSET_VERSION_SOUND_GC_CODEC) ?
        stream.ReadUint8() : uint8_t(stream.ReadBool() ? SOUND_PAYLOAD_VORBIS : SOUND_PAYLOAD_PCM);
    if (payload == SOUND_PAYLOAD_DSP_ADPCM)
    {
        LoadDspAdpcm(stream);
        return;
    }
    if (payload > SOUND_PAYLOAD_DSP_ADPCM)
    {
        LogError("SoundWave '%s': unknown audio (%u); no sound.", GetName().c_str(), uint32_t(payload));
        return;
    }
    bool compressed = (payload == SOUND_PAYLOAD_VORBIS);

    if (compressed)
    {
        uint32_t compressedSize = stream.ReadUint32();
        OCT_UNUSED(compressedSize); // Unused in non-editor

#if EDITOR
        // In Editor, we want to keep the compressed data around so in case we save the file again,
        // we won't be recompressing the sound a second time (adding more artifacts / distortion).
        mCompressedData = new uint8_t[compressedSize];
        mCompressedSize = compressedSize;
        memcpy(mCompressedData, stream.GetData() + stream.GetPos(), compressedSize);
#elif PLATFORM_DOLPHIN
        if (mStream && mDiscSize > 0)
        {
            // Streamed from the disc: GetFileReadLimit() stopped the load short of the audio,
            // so there is nothing more here to read, and nothing is kept.
            AUD_ProcessWaveBuffer(this);
            return;
        }

        if (mStream)
        {
            // No file to go back to (an embedded build): the old way. Keep the compressed
            // Vorbis in RAM and decode it on
            // the fly at playback (see Audio_Dolphin). Skips the full PCM decode,
            // so a long track costs its compressed size (~KB) instead of MBs of
            // decoded PCM.
            mCompressedData = new uint8_t[compressedSize];
            mCompressedSize = compressedSize;
            memcpy(mCompressedData, stream.GetData() + stream.GetPos(), compressedSize);
            AUD_ProcessWaveBuffer(this);
            return;
        }
#endif

        Stream outStream;
        PcmFormat format;
        format.mBytesPerSample = (mBitsPerSample / 8);
        format.mNumChannels = mNumChannels;
        format.mSampleRate = mSampleRate;
        AUD_DecodeVorbis(stream, outStream, format);

        mWaveDataSize = outStream.GetSize();
        mWaveData = AUD_AllocWaveBuffer(mWaveDataSize);
        if (mWaveData == nullptr)
        {
            // Too large to fit in RAM. Fail gracefully instead of writing to null.
            LogError("SoundWave '%s': could not allocate %u bytes of decoded audio "
                     "(too large for RAM). Enable Stream on this sound.",
                     GetName().c_str(), mWaveDataSize);
            mWaveDataSize = 0;
            return;
        }
        memcpy(mWaveData, outStream.GetData(), mWaveDataSize);
    }
    else
    {
#if PLATFORM_DOLPHIN && !EDITOR
        if (mStream && mDiscSize > 0)
        {
            // Streamed from the disc as PCM (cooked uncompressed): GetFileReadLimit() stopped the
            // load short of it, so nothing more is read here. Played from ARAM once copied there,
            // streamed till then (Audio_Dolphin).
            AUD_ProcessWaveBuffer(this);
            return;
        }
#endif
        // Waveform
        mWaveDataSize = stream.ReadUint32();
        mWaveData = AUD_AllocWaveBuffer(mWaveDataSize);
        if (mWaveData == nullptr)
        {
            LogError("SoundWave '%s': could not allocate %u bytes of audio.",
                     GetName().c_str(), mWaveDataSize);
            mWaveDataSize = 0;
            return;
        }
        for (uint32_t i = 0; i < mWaveDataSize; ++i)
        {
            mWaveData[i] = stream.ReadUint8();
        }
    }

    AUD_ProcessWaveBuffer(this);
}

void SoundWave::SaveStream(Stream& stream, Platform platform)
{
    Asset::SaveStream(stream, platform);

    // Properties
    stream.WriteFloat(mVolumeMultiplier);
    stream.WriteFloat(mPitchMultiplier);
    stream.WriteInt8(mAudioClass);
    stream.WriteBool(mCompress);
    stream.WriteBool(mCompressInternal);
    stream.WriteBool(mStream);
    stream.WriteUint8(mMaxInstances);
    stream.WriteUint8(uint8_t(mGcCodec));

#if EDITOR
    // GameCube Codec: DSP-ADPCM is the GameCube cook's alone. Every other platform, and the source asset (Platform::Count),
    // as ever below -- as is a sound it can't be made of (logged).
    if (platform == Platform::GameCube && CooksAsDspAdpcm() && SaveDspAdpcm(stream))
    {
        return;
    }
#endif

    uint32_t numChannels = mNumChannels;
    uint32_t bitsPerSample = mBitsPerSample;
    uint32_t sampleRate = mSampleRate;
    uint32_t numSamples = mNumSamples;
    uint32_t blockAlign = mBlockAlign;
    uint32_t byteRate = mByteRate;

    uint8_t* waveData = mWaveData;
    uint32_t waveDataSize = mWaveDataSize;

    uint8_t* lqWaveData = nullptr;
    bool lqConvert = false;

#if LQ_CONSOLE_AUDIO
    if (platform == Platform::GameCube ||
        platform == Platform::Wii ||
        platform == Platform::N3DS)
    {
        // Maybe only temporary, these platforms will use low-quality audio.
        // In the future allow compressing to OGG
        lqConvert = true;

        lqWaveData = new uint8_t[mNumSamples];
        memset(lqWaveData, 0, mNumSamples);

        const uint8_t* srcData = mWaveData;
        uint8_t* dstData = lqWaveData;

        uint32_t numSamples = mNumSamples;
        uint32_t lqWaveDataSize = mWaveDataSize;

        // First convert all samples to 8 bit
        for (uint32_t i = 0; i < numSamples; ++i)
        {
            if (mBitsPerSample == 8)
            {
                *dstData = *srcData;

                dstData++;
                srcData++;
            }
            else
            {
                int16_t sample16;
                memcpy(&sample16, srcData, sizeof(int16_t));
                uint8_t sample8 = (uint8_t)((sample16 + 32768) >> 8);
                *dstData = sample8;

                dstData++;
                srcData += 2;
            }
        }

        if (mBitsPerSample == 16)
        {
            lqWaveDataSize /= 2;
        }

        // Then convert to mono by averaging left + right channels
        if (mNumChannels == 2)
        {
            for (uint32_t i = 0; i < numSamples / 2; ++i)
            {
                uint8_t sample1 = lqWaveData[i * 2 + 0];
                uint8_t sample2 = lqWaveData[i * 2 + 1];

                float sampleAvg = (float(sample1) + float(sample2)) / 2.0f;
                lqWaveData[i] = uint8_t(sampleAvg + 0.5);
            }

            numSamples /= 2;
            lqWaveDataSize /= 2;
        }

        // Lastly merge samples to make 22050 Hz (if original is 44100 Hz).
        uint32_t lowSampleRate = mSampleRate;
        if (mSampleRate == 44100)
        {
            lowSampleRate = 22050;

            for (uint32_t i = 0; i < numSamples / 2; ++i)
            {
                uint8_t sample1 = lqWaveData[i * 2 + 0];
                uint8_t sample2 = lqWaveData[i * 2 + 1];

                float sampleAvg = (float(sample1) + float(sample2)) / 2.0f;
                lqWaveData[i] = uint8_t(sampleAvg + 0.5);
            }

            numSamples /= 2;
            lqWaveDataSize /= 2;
        }

        // Then write the data to stream (using mono, 8-bit, 22050 Hz settings)
        numChannels = 1;
        bitsPerSample = 8;
        sampleRate = lowSampleRate;
        numSamples = numSamples;
        blockAlign = 1;
        byteRate = lowSampleRate;

        waveData = lqWaveData;
        waveDataSize = lqWaveDataSize;
    }
#endif

    // Waveform Format
    stream.WriteUint32(numChannels);
    stream.WriteUint32(bitsPerSample);
    stream.WriteUint32(sampleRate);
    stream.WriteUint32(numSamples);
    stream.WriteUint32(blockAlign);
    stream.WriteUint32(byteRate);

    bool compress = mCompress;
    if (platform == Platform::Count)
    {
        // In editor, we only compress the data (destructive) if the compress internal flag is set.
        // This feature is to reduce file sizes for big sounds like music. But once you compress internally,
        // storing the data uncompressed later will not result in any better audio. Use only when needed.
        compress = (mCompress && mCompressInternal);
    }

    stream.WriteUint8(compress ? SOUND_PAYLOAD_VORBIS : SOUND_PAYLOAD_PCM);   // (the bool it was: the same byte)

    if (compress)
    {
        if (mCompressedData != nullptr && !lqConvert)
        {
            // Writeout the already-computed compressed OGG data
            stream.WriteUint32(mCompressedSize);
            stream.WriteBytes(mCompressedData, mCompressedSize);
        }
        else
        {
            // Output to a separate stream so we can determine the total size of the compressed data.
            Stream outStream;
            Stream inStream((char*)waveData, waveDataSize);

            PcmFormat format;
            format.mBytesPerSample = (bitsPerSample / 8);
            format.mNumChannels = numChannels;
            format.mSampleRate = sampleRate;

            AUD_EncodeVorbis(inStream, outStream, format);

            uint32_t compressedSize = outStream.GetSize();
            stream.WriteUint32(compressedSize);
            stream.WriteBytes((uint8_t*)outStream.GetData(), outStream.GetSize());
        }
    }
    else
    {
        // Waveform
        stream.WriteUint32(waveDataSize);
        for (uint32_t i = 0; i < waveDataSize; ++i)
        {
            stream.WriteUint8(waveData[i]);
        }
    }

    if (lqWaveData != nullptr)
    {
        delete lqWaveData;
        lqWaveData = nullptr;
    }
}

// A GameCube cook's DSP-ADPCM (SaveDspAdpcm), after its payload byte: the block's headers read, then its data decoded
// into ARAM (AUD_StoreAdpcm) -- or, a Stream sound's left on the disc by GetFileReadLimit, left there: decoded as it
// plays, or as it's copied to ARAM (Audio_Dolphin). Only the GameCube reads one: no other build loads a GameCube cook.
void SoundWave::LoadDspAdpcm(Stream& stream)
{
    const uint32_t payloadSize = stream.ReadUint32();
#if PLATFORM_GAMECUBE && !EDITOR
    const uint32_t blockAt = stream.GetPos();
    SoundAdpcmInfo info;
    const char* why = nullptr;
    uint32_t headers = 0;
    if (blockAt + 16 > stream.GetSize())
    {
        why = "cut short";
    }
    else
    {
        info.channels = stream.ReadUint8();
        info.layout = stream.ReadUint8();
        stream.ReadUint16();
        info.interleave = stream.ReadUint32();
        info.channelBytes = stream.ReadUint32();
        stream.ReadUint32();                                // (the loop's start: 0, the whole sound loops)
        headers = (16 + DspAdpcm::kHeaderSize * info.channels + 31) & ~31u;
        if (info.channels < 1 || info.channels > 2 || info.channels != mNumChannels)
            why = "its channels";
        else if (info.layout > 1 || (info.layout == 1 && (info.interleave < 32 || info.interleave > SOUND_ADPCM_INTERLEAVE ||
                                                          (info.interleave & 31) != 0)))
            why = "its layout";
        else if ((info.channelBytes & 31) != 0 || payloadSize < headers + info.channels * info.channelBytes)
            why = "its size";
        else if (blockAt + headers > stream.GetSize())
            why = "cut short";
    }
    for (uint32_t c = 0; c < info.channels && why == nullptr; ++c)
    {
        uint8_t hdr[DspAdpcm::kHeaderSize];
        stream.ReadBytes(hdr, DspAdpcm::kHeaderSize);
        DspAdpcm::Channel ch;
        if (!DspAdpcm::ReadHeader(hdr, ch) || ch.numSamples == 0 || (c > 0 && ch.numSamples != info.samples) ||
            DspAdpcm::DataBytes(ch.numSamples) > info.channelBytes)
        {
            why = "its headers";
            break;
        }
        info.samples = ch.numSamples;
        memcpy(info.coefs[c], ch.coefs, sizeof(ch.coefs));
        info.yn1[c] = ch.yn1;
        info.yn2[c] = ch.yn2;
        info.ps[c] = ch.ps;
        info.loopPs[c] = ch.loopPs;
        info.loopYn1[c] = ch.loopYn1;
        info.loopYn2[c] = ch.loopYn2;
    }
    if (why != nullptr)
    {
        LogError("SoundWave '%s': DSP-ADPCM unreadable (%s); no sound.", GetName().c_str(), why);
        return;
    }
    mAdpcm = info;

    if (mStream && mDiscAdpcm && mDiscSize > 0)
    {
        // Left on the disc (GetFileReadLimit stopped the load at its data): nothing more here to read.
        AUD_ProcessWaveBuffer(this);
        return;
    }

    // In memory: a sound effect, or a Stream sound with no file to go back to (an embedded build). Its PCM (as
    // GetWaveDataSize gives it) goes to ARAM, decoded a piece at a time.
    const uint32_t dataAt = blockAt + headers;
    const uint32_t dataBytes = info.channels * info.channelBytes;
    if (dataAt + dataBytes > stream.GetSize())
    {
        LogError("SoundWave '%s': DSP-ADPCM cut short; no sound.", GetName().c_str());
        mAdpcm = SoundAdpcmInfo();
        return;
    }
    mWaveDataSize = info.samples * info.channels * 2;
    if (!AUD_StoreAdpcm(this, (const uint8_t*)stream.GetData() + dataAt, dataBytes))
    {
        mWaveDataSize = 0;
    }
    stream.SetPos(dataAt + dataBytes);
#else
    (void)payloadSize;
    LogError("SoundWave '%s': DSP-ADPCM is a GameCube cook's; it doesn't play here.", GetName().c_str());
#endif
}

#if EDITOR
bool SoundWave::CooksAsDspAdpcm() const
{
    switch (GcSoundCodec(mGcCodec))
    {
    case GcSoundCodec::DspAdpcm:
        return true;
    case GcSoundCodec::Pcm:
        return false;
    default:
        // Project Default: Config.ini's, for sounds that aren't Stream (a Stream sound's footprint and load path change
        // with it -- about 3x its Vorbis on the disc -- so it's marked one by one).
        return !mStream && GetEngineConfig()->mGameCubeSoundCodec == 1;
    }
}

// The GameCube cook's DSP-ADPCM (GameCube Codec). Each channel is encoded on its own (DspAdpcm: Nintendo's method),
// loop-ready from its first sample, so the same bytes play once or loop (a sound loops or not as it's played, not as
// it's cooked; a loop from sample 0 decodes the same every time round). Written after the properties, in place of the
// format words and the PCM:
//   the format words: channels, 16 (what it decodes to), rate, samples (all channels', as ever), 2 * channels, byte rate
//   u8 payload (SOUND_PAYLOAD_DSP_ADPCM), u32 the bytes after this word
//   u8 channels, u8 layout (0: the channels one after the other; 1, a Stream sound's: blocks of `interleave` bytes a
//   channel in turn), u16 0, u32 interleave (0 for layout 0), u32 a channel's data bytes (whole 8-byte frames, padded to
//   32), u32 the loop's start (0)
//   a channel's standard 0x60-byte DSP header each (big-endian: DspAdpcm::WriteHeader); zeros to 32 bytes from the u8
//   channels on
//   the data
// False: it can't be made of this sound -- logged, and nothing written: it's cooked as PCM.
bool SoundWave::SaveDspAdpcm(Stream& stream)
{
    const uint32_t channels = mNumChannels;
    const uint32_t samples = (channels == 1 || channels == 2) ? mNumSamples / channels : 0;   // a channel's
    const uint32_t bytesPerSample = mBitsPerSample / 8;
    const char* why = nullptr;
    if (channels != 1 && channels != 2)
        why = "not mono or stereo";
    else if (mBitsPerSample != 8 && mBitsPerSample != 16)
        why = "not 8 or 16-bit";
    else if (mWaveData == nullptr || samples == 0)
        why = "no audio";
    else if (mWaveDataSize < samples * channels * bytesPerSample)
        why = "short of its samples";
    if (why != nullptr)
    {
        LogWarning("AUDIO cook %s: not DSP-ADPCM (%s): PCM", GetName().c_str(), why);
        return false;
    }

    // 16-bit, the channels interleaved: as it is (little-endian), or from 8-bit unsigned.
    std::vector<int16_t> pcm(size_t(samples) * channels);
    for (size_t i = 0; i < pcm.size(); ++i)
    {
        if (bytesPerSample == 2)
            pcm[i] = int16_t(uint16_t(mWaveData[i * 2]) | (uint16_t(mWaveData[i * 2 + 1]) << 8));
        else
            pcm[i] = int16_t((int32_t(mWaveData[i]) - 128) * 256);
    }

    const uint32_t channelBytes = DspAdpcm::DataBytesAram(samples);
    const uint32_t layout = mStream ? 1 : 0;
    const uint32_t interleave = mStream ? SOUND_ADPCM_INTERLEAVE : 0;
    const uint32_t headers = (16 + DspAdpcm::kHeaderSize * channels + 31) & ~31u;
    const uint32_t dataBytes = channels * channelBytes;

    DspAdpcm::Channel enc[2];
    double signal = 0.0;
    double noise = 0.0;
    for (uint32_t c = 0; c < channels; ++c)
    {
        std::vector<int16_t> decoded;   // (what the console will play)
        DspAdpcm::Encode(pcm.data() + c, samples, channels, mSampleRate, true, 0, samples - 1, enc[c], &decoded);
        for (uint32_t s = 0; s < samples; ++s)
        {
            const double x = pcm[size_t(s) * channels + c];
            const double e = x - decoded[s];
            signal += x * x;
            noise += e * e;
        }
        enc[c].data.resize(channelBytes, 0);
    }

    stream.WriteUint32(channels);
    stream.WriteUint32(16);
    stream.WriteUint32(mSampleRate);
    stream.WriteUint32(mNumSamples);
    stream.WriteUint32(2 * channels);
    stream.WriteUint32(mSampleRate * 2 * channels);

    stream.WriteUint8(SOUND_PAYLOAD_DSP_ADPCM);
    stream.WriteUint32(headers + dataBytes);

    stream.WriteUint8(uint8_t(channels));
    stream.WriteUint8(uint8_t(layout));
    stream.WriteUint16(0);
    stream.WriteUint32(interleave);
    stream.WriteUint32(channelBytes);
    stream.WriteUint32(0);
    for (uint32_t c = 0; c < channels; ++c)
    {
        uint8_t hdr[DspAdpcm::kHeaderSize];
        DspAdpcm::WriteHeader(enc[c], hdr);
        stream.WriteBytes(hdr, DspAdpcm::kHeaderSize);
    }
    for (uint32_t i = 16 + DspAdpcm::kHeaderSize * channels; i < headers; ++i)
    {
        stream.WriteUint8(0);
    }
    if (layout == 0)
    {
        for (uint32_t c = 0; c < channels; ++c)
            stream.WriteBytes(enc[c].data.data(), channelBytes);
    }
    else
    {
        for (uint32_t at = 0; at < channelBytes; at += interleave)
        {
            const uint32_t len = glm::min<uint32_t>(interleave, channelBytes - at);
            for (uint32_t c = 0; c < channels; ++c)
                stream.WriteBytes(enc[c].data.data() + at, len);
        }
    }

    const double snr = (noise > 0.0 && signal > 0.0) ? 10.0 * log10(signal / noise) : 99.0;
    LogDebug("AUDIO cook %s: DSP-ADPCM, %u ch, %u KB (PCM %u KB), SNR %.1f dB%s%s%s", GetName().c_str(), channels,
             (headers + dataBytes + 1023) / 1024, (mWaveDataSize + 1023) / 1024, snr, mStream ? ", Stream (blocks)" : "",
             mCompress ? "; Compress ignored on GameCube" : "",
             (mCompress && mCompressInternal) ? "; from its Vorbis (Compress Internal): lossy twice" : "");
    return true;
}
#endif

void SoundWave::Create()
{
    Asset::Create();
}

void SoundWave::Destroy()
{
    Asset::Destroy();

    if (mWaveData != nullptr)
    {
        AudioManager::StopSounds(this);
        AUD_FreeWaveBuffer(mWaveData);
        mWaveData = nullptr;
    }

#if PLATFORM_GAMECUBE
    if (mAramAddress != 0)
    {
        // Stopped first: a playing voice reads its next buffer from this ARAM.
        AudioManager::StopSounds(this);
        AUD_FreeAram(mAramAddress);
        mAramAddress = 0;
    }
    if (mStream && mDiscSize > 0 && AUD_ReleaseTrack(this))
    {
        AudioManager::StopSounds(this);   // (its voices from its ARAM copy already stopped there: the sources too)
    }
#endif

    if (mCompressedData != nullptr)
    {
#if !EDITOR
        // Compressed data is normally editor-only, but streamed sounds keep it on
        // console too (decoded on the fly at playback -- see Audio_Dolphin).
        OCT_ASSERT(mStream);
#endif
        delete[] mCompressedData;
    }
}


bool SoundWave::Import(const std::string& path, ImportOptions* options)
{
    bool success = Asset::Import(path, options);
    if (!success)
    {
        return false;
    }

#if EDITOR
    Stream wavStream;
    wavStream.ReadFile(path.c_str(), false);
    uint8_t* wavData = (uint8_t*) wavStream.GetData();

    char fileFormat[5] = {};
    memcpy(fileFormat, wavData + 8, 4);
    OCT_ASSERT(strncmp(fileFormat, "WAVE", 4) == 0);

    uint16_t audioFormat = *((uint16_t*)(wavData + 20));
    OCT_ASSERT(audioFormat == 1);

    uint16_t numChannels = *((uint16_t*)(wavData + 22));
    uint32_t sampleRate = *((uint32_t*)(wavData + 24));
    uint32_t byteRate = *((uint32_t*)(wavData + 28));
    uint16_t blockAlign = *((uint16_t*)(wavData + 32));
    uint16_t bitsPerSample = *((uint16_t*)(wavData + 34));

    uint32_t wavSampleDataSize = *((uint32_t*)(wavData + 40));
    uint8_t* wavSampleData = (wavData + 44);

    uint32_t bytesPerSample = bitsPerSample / 8;
    uint32_t numSamples = wavSampleDataSize / bytesPerSample;

    bool validBitDepth = (bitsPerSample == 8 || bitsPerSample == 16);
    bool validNumChannels = (numChannels == 1 || numChannels == 2);

    if (validBitDepth && validNumChannels)
    {
        mWaveDataSize = numSamples * bytesPerSample;
        mWaveData = (uint8_t*)SYS_AlignedMalloc(mWaveDataSize, 32);
        OCT_ASSERT(mWaveDataSize == wavSampleDataSize);
        memcpy(mWaveData, wavSampleData, wavSampleDataSize);
    }
    else
    {
        LogError("Unsupported WAV Format");
        LogError("BitDepth = %d (expected 8 or 16)", bitsPerSample);
        LogError("NumChannels = %d (expected 1 or 2)", numChannels);

        success = false;
    }

    mNumChannels = (uint32_t)numChannels;
    mBitsPerSample = (uint32_t)bitsPerSample;
    mSampleRate = sampleRate;
    mNumSamples = numSamples;
    mBlockAlign = blockAlign;
    mByteRate = byteRate;

    if (success)
    {
        AUD_ProcessWaveBuffer(this);
    }
#endif

    return success;
}

void SoundWave::GatherProperties(std::vector<Property>& outProps)
{
    Asset::GatherProperties(outProps);

    static bool sFakePlay = false;
    outProps.push_back(Property(DatumType::Bool, "Play", this, &sFakePlay, 1, HandlePropChange));

    static bool sFakeStop = false;
    outProps.push_back(Property(DatumType::Bool, "Stop", this, &sFakeStop, 1, HandlePropChange));

    outProps.push_back(Property(DatumType::Float, "Volume Multiplier", this, &mVolumeMultiplier, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Float, "Pitch Multiplier", this, &mPitchMultiplier, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Byte, "Audio Class", this, &mAudioClass, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Bool, "Compress", this, &mCompress, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Bool, "Compress Internal", this, &mCompressInternal, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Bool, "Stream", this, &mStream, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Byte, "Max Instances", this, &mMaxInstances, 1, HandlePropChange));
    // GameCube builds only (the GameCube cook): DSP-ADPCM is 3.5x smaller, decoded on the console; lossy, so listen on
    // the console first. Project Default: Config.ini's GameCubeSoundCodec (for sounds that aren't Stream).
    outProps.push_back(Property(DatumType::Integer, "GameCube Codec", this, &mGcCodec, 1, HandlePropChange, NULL_DATUM,
                                int32_t(GcSoundCodec::Count), sGcCodecStrings));
}

glm::vec4 SoundWave::GetTypeColor()
{
    return glm::vec4(0.5f, 0.1f, 1.0f, 1.0f);
}

const char* SoundWave::GetTypeName()
{
    return "SoundWave";
}

const char* SoundWave::GetTypeImportExt()
{
    return ".wav";
}

void SoundWave::SetPitchMultiplier(float pitch)
{
    mPitchMultiplier = pitch;
}

float SoundWave::GetPitchMultiplier() const
{
    return mPitchMultiplier;
}

void SoundWave::SetAudioClass(int8_t audioClass)
{
    mAudioClass = audioClass;
}

int8_t SoundWave::GetAudioClass() const
{
    return mAudioClass;
}

void SoundWave::SetVolumeMultiplier(float volume)
{
    mVolumeMultiplier = volume;
}

void SoundWave::SetPcmData(uint8_t* data, uint32_t size, uint32_t numSamples, uint32_t bitsPerSample, uint32_t numChannels, uint32_t sampleRate)
{
    // Caller should make sure data is allocated with AUD_AllocWaveBuffer and freeing the previous buffer.
    mWaveData = data;
    mWaveDataSize = size;
    mNumSamples = numSamples;
    mBitsPerSample = bitsPerSample;
    mNumChannels = numChannels;
    mSampleRate = sampleRate;
    mBlockAlign = (numChannels * bitsPerSample) / 8;
    mByteRate = (sampleRate * numChannels * bitsPerSample) / 8;
}

float SoundWave::GetVolumeMultiplier() const
{
    return mVolumeMultiplier;
}

void SoundWave::MoveWaveDataToAram(uint32_t aramAddress)
{
    if (mWaveData != nullptr)
    {
        AUD_FreeWaveBuffer(mWaveData);
        mWaveData = nullptr;
    }
    mAramAddress = aramAddress;
}

uint8_t* SoundWave::GetWaveData() const
{
    return mWaveData;
}

uint32_t SoundWave::GetWaveDataSize() const
{
    return mWaveDataSize;
}

uint32_t SoundWave::GetNumChannels() const
{
    return mNumChannels;
}

uint32_t SoundWave::GetBitsPerSample() const
{
    return mBitsPerSample;
}

uint32_t SoundWave::GetSampleRate() const
{
    return mSampleRate;
}

uint32_t SoundWave::GetNumSamples() const
{
    return mNumSamples;
}

float SoundWave::GetDuration() const
{
    return float(mNumSamples) / mSampleRate;
}

bool SoundWave::GetStream() const
{
    return mStream;
}

const uint8_t* SoundWave::GetCompressedData() const
{
    return mCompressedData;
}

uint32_t SoundWave::GetCompressedSize() const
{
    return mCompressedSize;
}

uint32_t SoundWave::GetBlockAlign() const
{
    return mBlockAlign;
}

uint32_t SoundWave::GetByteRate() const
{
    return mByteRate;
}
