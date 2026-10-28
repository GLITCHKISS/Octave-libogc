# SoundWave

An asset that represents a sound wave.

Inheritance:
* [Asset](Asset.md)

---
### GameCube Codec (editor property)
How the GameCube cook writes the sound. Other platforms, the editor's own playback and the source asset always keep
the sound's PCM (or Vorbis): the property does nothing there.

| Value | GameCube cook |
|---|---|
| Project Default (the default) | as Config.ini's `GameCubeSoundCodec` says, for sounds that aren't Stream; a Stream sound stays as it was |
| PCM | PCM (or Vorbis, with Compress), as before the property |
| DSP-ADPCM | DSP-ADPCM, Stream sounds too |

- DSP-ADPCM is 3.5x smaller than 16-bit PCM on the disc (1.75x than 8-bit) and lossy: about 40-45 dB SNR on most sounds,
  less on bright noisy ones (hiss, glitter, water). The cook log gives each sound's size and SNR:
  `AUDIO cook <name>: DSP-ADPCM, <ch> ch, <KB> (PCM <KB>), SNR <dB>`. Listen on the console before shipping.
- On the console it's decoded on the CPU into the 16-bit PCM it plays as: a sound effect into ARAM as it loads; a
  Stream sound as `AUD_CopyTrackToAram` copies it, or as it plays from the disc before that. It then plays (and loops)
  as any sound does. Compress is ignored for a DSP-ADPCM sound on the GameCube.
- Config.ini `GameCubeSoundCodec=0` (the default) or `1`, also in the editor's Optimization menu: the codec for the sounds
  left at Project Default.
- By script: `Tools/octkit.py`'s `set_sound_gc_codec(path, 'adpcm')` sets it in a source `.oct`: the byte after Max
  Instances, at 38 + the name's length (asset version 20; an older asset gains the byte).

---
### GetVolumeMultiplier
Get the volume multiplier.

Sig: `mult = SoundWave:GetVolumeMultiplier()`
 - Ret: `number mult` Volume multiplier
---
### SetVolumeMultiplier
Set the volume multiplier.

Sig: `SoundWave:SetVolumeMultiplier(mult)`
 - Arg: `number mult` Volume multiplier
---
### GetPitchMultiplier
Get the pitch multiplier.

Sig: `mult = SoundWave:GetPitchMultiplier()`
 - Ret: `number mult` Pitch multiplier
---
### SetPitchMultiplier
Set the pitch multiplier.

Sig: `SoundWave:SetPitchMultiplier(mult)`
 - Arg: `number mult` Pitch multiplier
---
### GetWaveDataSize
Get the wave data size in bytes.

Sig: `size = SoundWave:GetWaveDataSize()`
 - Ret: `integer size` Wave data size in bytes
---
### GetNumChannels
Get the number of sound channels. Either 1 (mono) or 2 (stereo).

Sig: `num = SoundWave:GetNumChannels()`
 - Ret: `integer num` Num channels
---
### GetBitsPerSample
Get the number of bits per sample. Either 8 or 16.

Sig: `bits = SoundWave:GetBitsPerSample()`
 - Ret: `integer bits` Bits per sample
---
### GetSampleRate
Get the sample rate. Either 22050 or 44100.

Sig: `rate = SoundWave:GetSampleRate()`
 - Ret: `integer rate` Sample rate
---
### GetNumSamples
Get the total number of samples.

Sig: `num = SoundWave:GetNumSamples()`
 - Ret: `integer num` Num samples
---
### GetByteRate
Get the byte rate.

Sig: `rate = SoundWave:GetByteRate()`
 - Ret: `integer rate` Byte rate
---
### GetDuration
Get the sound wave duration in seconds.

Sig: `duration = SoundWave:GetDuration()`
 - Ret: `number duration` Duration in seconds
---
