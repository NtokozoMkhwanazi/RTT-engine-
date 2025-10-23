#pragma once
#include <AL/al.h>
#include <AL/alc.h>
#include <string>
#include <map>

class AudioManager {
public:
    static bool Init();
    static void Shutdown();
    static bool LoadWav(const std::string& name, const std::string& filePath);
    static void Play(const std::string& name);
private:
    static ALCdevice* device;
    static ALCcontext* context;
    static std::map<std::string, ALuint> buffers;
    static std::map<std::string, ALuint> sources;
};
