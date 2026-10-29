# Audio

System to play and manage sounds.

---
### PlaySound2D
Play a sound with no positional data.
TODO: Rename to PlaySound() if we add 2D support.

Sig: `Audio.PlaySound2D(sound, volume=1, pitch=1, startTime=0, loop=false, priority=0, stopAfter=0)`
 - Arg: `SoundWave sound` Sound wave to play
 - Arg: `number volume` Volume multiplier
 - Arg: `number pitch` Pitch multiplier
 - Arg: `number startTime` Start time offset
 - Arg: `boolean loop` Loop sound
 - Arg: `integer priority` Sound priority
 - Arg: `number stopAfter` A scheduled stop, in seconds of the sound's own audio (0 or nil: none). See below.

#### stopAfter: a scheduled stop
The sound ends by itself once it has played `stopAfter` seconds of its own audio from where it started:
`stopAfter * sample rate` sample frames, rounded to the nearest, counted at pitch 1.0 (at another pitch it's the same
frames, so a different time). A looping sound loops until then. The voice ends on that very sample, and then
`Audio.IsSoundPlaying` is false and its voice is free, as if it had played out.

Use it to end a sound at an exact point, music on a beat say. A stop from a script's timer
(`Audio.StopSounds`) only takes effect at the audio's next mix: libasnd mixes ~21 ms at a time on the GameCube, so a
timer stop on the end of a bar could let the start of the next beat through.

```lua
local BAR = 3.3568                                  -- seconds a bar of the track
Audio.PlaySound2D(music, 1.0, 1.0, 0.0, true, 0, BAR * 3)   -- loops, and ends on the last sample of its third bar
```

By platform: on Windows (XAudio2) and the GameCube (ARAM PCM voices, DSP-ADPCM voices with `DspAdpcm=1`, disc streams
and the `AUD_CopyTrackToAram` tracks) the voice ends on the sample. Wii (main-memory voices), Linux and Android do the
same in code, untested. On the 3DS only a stop within the sound's first pass is done; past it a looping sound loops on.
On the GameCube a loop shorter than about three of libasnd's mixes at its pitch (~64 ms at pitch 1) in a DSP-ADPCM or
main-memory voice can't queue its passes ahead: it's stopped at the edge of a mix instead (~21 ms), as a timer is.
At a pitch over 48 kHz (a 44.1 kHz sound past ~1.09) libasnd's seams already drop or hold a frame, so the end is on the
right sample of the last pass but its time can be a frame or two off.
---
### PlaySound3D
Play a sound at a position in 3D space.
TODO: Rename to PlaySoundAtPosition() to handle both 2D and 3D.

Sig: `Audio.PlaySound3D(sound, position, innerRadius, outerRadius, attenuationFunc=AttenuationFunc.Linear, volume=1, pitch=1, startTime=0, loop=false, priority=0, stopAfter=0)`
 - Arg: `SoundWave sound` Sound wave to play
 - Arg: `Vector position` Position of sound
 - Arg: `number innerRadius` Inner radius
 - Arg: `number outerRadius` Outer radius
 - Arg: `AttenuationFunc(integer) attenuationFunc` Attenuation function
 - Arg: `number volume` Volume multiplier
 - Arg: `number pitch` Pitch multiplier
 - Arg: `number startTime` Start time offset
 - Arg: `boolean loop` Loop sound
 - Arg: `integer priority` Sound priority
 - Arg: `number stopAfter` A scheduled stop, in seconds of the sound's own audio (0 or nil: none), as PlaySound2D's
---
### StopSounds
Stop sounds using a particular sound wave.

Alias: `StopSound`

Sig: `Audio.StopSounds(sound)`
 - Arg: `SoundWave sound` Sound wave to stop
---
### StopAllSounds
Stop all sounds.

Sig: `Audio.StopAllSounds()`

---
### UpdateSound
Updates volume, pitch, and priority for a currently playing sound wave. Note: This will update volume/pitch/priority for all currently playing sounds of the same sound wave asset. Without a priority the sound keeps the one it was played with.

Sig: `Audio.UpdateSound(sound, volume, pitch=1, priority)`
- Arg: `SoundWave sound` Sound wave to update
- Arg: `number volume` New volume
- Arg: `number pitch` New pitch
- Arg: `integer priority` New priority (optional: left as it is if not given)

---
### IsSoundPlaying
Check if a sound wave is playing.

Sig: `playing = Audio.IsSoundPlaying(sound)`
 - Arg: `SoundWave sound` Sound wave to check
 - Ret: `boolean playing` Is the sound playing
---
### SetAudioClassVolume
Set the volume of an audio class. Audio classes can be used to control the volume and pitch of multiple sounds.

Sig: `Audio.SetAudioClassVolume(volume)`
 - Arg: `number volume` Volume multiplier
---
### GetAudioClassVolume
Get the volume of an audio class. Audio classes can be used to control the volume and pitch of multiple sounds.

Sig: `volume = Audio.GetAudioClassVolume()`
 - Ret: `number volume` Volume multiplier
---
### SetAudioClassPitch
Set the pitch of an audio class. Audio classes can be used to control the volume and pitch of multiple sounds.

Sig: `Audio.SetAudioClassPitch(pitch)`
 - Arg: `number pitch` Pitch multiplier
---
### GetAudioClassPitch
Get the pitch of an audio class. Audio classes can be used to control the volume and pitch of multiple sounds.

Sig: `pitch = Audio.GetAudioClassPitch()`
 - Ret: `number pitch` Pitch multiplier
---
### SetMasterVolume
Set the master volume.

Sig: `Audio.SetMasterVolume(volume)`
 - Arg: `number volume` Master volume
---
### GetMasterVolume
Get the master volume.

Sig: `volume = Audio.GetMasterVolume()`
 - Ret: `number volume` Master volume
---
### SetMasterPitch
Set the master pitch.

Sig: `Audio.SetMasterPitch(pitch)`
 - Arg: `number pitch` Master pitch
---
### GetMasterPitch
Get the master pitch.

Sig: `pitch = Audio.GetMasterPitch()`
 - Ret: `number pitch` Master pitch
---
