#pragma once

#include "EngineTypes.h"

class SoundWave;
class Audio3D;

class AudioManager
{
public:

    static void Initialize();
    static void Shutdown();
    static void Update(float deltaTime);

    // stopAfter (seconds, 0: none): a scheduled stop. The sound ends by itself once it has played that much of its own
    // audio from where it started: stopAfter * its sample rate sample frames (the same frames at any pitch, so at another
    // pitch a different time). Looping, it loops till then. It ends on that very sample, which a stop from game code
    // can't do: that lands on the platform's next mix (GameCube: ~21 ms blocks). Then IsSoundPlaying is false.
    static void PlaySound2D(
        SoundWave* soundWave,
        float volumeMult = 1.0f,
        float pitchMult = 1.0f,
        float startTime = 0.0f,
        bool loop = false,
        int32_t priority = 0,
        double stopAfter = 0.0);

    static void PlaySound3D(
        SoundWave* soundWave,
        glm::vec3 worldPosition,
        float innerRadius,
        float outerRadius,
        AttenuationFunc attenFunc = AttenuationFunc::Linear,
        float volumeMult = 1.0f,
        float pitchMult = 1.0f,
        float startTime = 0.0f,
        bool loop = false,
        int32_t priority = 0,
        double stopAfter = 0.0);

    // A playing sound's volume and pitch; its priority too if one is given (>= 0), else it keeps
    // the one it was played with.
    static void UpdateSound(
        SoundWave* soundWave,
        float volume,
        float pitch,
        bool loop = false,
        int32_t priority = -1);

    static void StopComponent(Audio3D* comp);
    static void StopSounds(SoundWave* soundWave);
    static void StopSound(const std::string& name);
    static void StopAllSounds();

    static bool IsSoundPlaying(SoundWave* soundWave);

    static void SetAudioClassVolume(int8_t audioClass, float volume);
    static void SetAudioClassPitch(int8_t audioClass, float pitch);
    static float GetAudioClassVolume(int8_t audioClass);
    static float GetAudioClassPitch(int8_t audioClass);

    static void SetMasterVolume(float volume);
    static void SetMasterPitch(float pitch);
    static float GetMasterVolume();
    static float GetMasterPitch();
};
