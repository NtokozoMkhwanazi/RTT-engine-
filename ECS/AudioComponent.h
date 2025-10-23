#pragma once
#include "Component.h"
#include "AudioManager.h"
#include <string>

class AudioComponent : public Component {
public:
    void playSound(const std::string& name) {
        AudioManager::Play(name);
    }
};
