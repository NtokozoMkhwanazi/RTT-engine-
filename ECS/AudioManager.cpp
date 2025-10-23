#include "AudioManager.h"
#include <iostream>
#include <fstream>
#include <vector>

ALCdevice* AudioManager::device = nullptr;
ALCcontext* AudioManager::context = nullptr;
std::map<std::string, ALuint> AudioManager::buffers;
std::map<std::string, ALuint> AudioManager::sources;

bool AudioManager::Init() {
    device = alcOpenDevice(nullptr);
    if (!device) {
        std::cerr << "Failed to open OpenAL device\n";
        return false;
    }

    context = alcCreateContext(device, nullptr);
    if (!context) {
        std::cerr << "Failed to create OpenAL context\n";
        return false;
    }

    alcMakeContextCurrent(context);
    return true;
}

void AudioManager::Shutdown() {
    for (auto& src : sources) alDeleteSources(1, &src.second);
    for (auto& buf : buffers) alDeleteBuffers(1, &buf.second);
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(context);
    alcCloseDevice(device);
}

bool AudioManager::LoadWav(const std::string& name, const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open WAV file: " << filePath << "\n";
        return false;
    }

    char riff[4];
    file.read(riff, 4);
    file.seekg(22);

    short channels;
    file.read(reinterpret_cast<char*>(&channels), 2);

    int sampleRate;
    file.read(reinterpret_cast<char*>(&sampleRate), 4);

    file.seekg(34);
    short bitsPerSample;
    file.read(reinterpret_cast<char*>(&bitsPerSample), 2);

    file.seekg(40);
    int dataSize;
    file.read(reinterpret_cast<char*>(&dataSize), 4);

    std::vector<char> data(dataSize);
    file.read(data.data(), dataSize);

    ALenum format = 0;
    if (channels == 1 && bitsPerSample == 8) format = AL_FORMAT_MONO8;
    else if (channels == 1 && bitsPerSample == 16) format = AL_FORMAT_MONO16;
    else if (channels == 2 && bitsPerSample == 8) format = AL_FORMAT_STEREO8;
    else if (channels == 2 && bitsPerSample == 16) format = AL_FORMAT_STEREO16;
    else {
        std::cerr << "Unsupported WAV format\n";
        return false;
    }

    ALuint buffer;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, format, data.data(), dataSize, sampleRate);

    ALuint source;
    alGenSources(1, &source);
    alSourcei(source, AL_BUFFER, buffer);

    buffers[name] = buffer;
    sources[name] = source;

    return true;
}

void AudioManager::Play(const std::string& name) {
    auto it = sources.find(name);
    if (it != sources.end()) {
        alSourcePlay(it->second);
    } else {
        std::cerr << "Sound not found: " << name << "\n";
    }
}
