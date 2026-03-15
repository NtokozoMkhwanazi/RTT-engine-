#pragma once

// ============================================================================
// DEMO RECORDER - Automated Camera Path for Recording
// ============================================================================
// Records and plays back camera movements for smooth demo videos
// ============================================================================

#include "DemoConfig.h"
#include "../cameraSystem/flyCamera.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>

class DemoRecorder {
public:
    DemoRecorder();
    ~DemoRecorder();
    
    // =========================================================================
    // MODES
    // =========================================================================
    
    enum class Mode {
        OFF,              // Normal operation
        RECORDING,        // Recording camera keyframes
        PLAYBACK          // Playing back for demo
    };
    
    // =========================================================================
    // INITIALIZATION
    // =========================================================================
    
    void initialize();
    void shutdown();
    
    // =========================================================================
    // MAIN API
    // =========================================================================
    
    void update(float dt);
    
    // Set mode
    void setMode(Mode mode) { currentMode = mode; }
    Mode getMode() const { return currentMode; }
    
    // Toggle mode with hotkey (F6)
    void toggleMode();
    
    // =========================================================================
    // RECORDING
    // =========================================================================
    
    void startRecording();
    void stopRecording();
    void recordKeyframe(const flyCamera& cam);
    void saveRecording(const std::string& filename);
    
    // =========================================================================
    // PLAYBACK
    // =========================================================================
    
    void startPlayback();
    void stopPlayback();
    void updateCamera(flyCamera& cam);
    
    // =========================================================================
    // STATUS
    // =========================================================================
    
    bool isRecording() const { return currentMode == Mode::RECORDING; }
    bool isPlaying() const { return currentMode == Mode::PLAYBACK; }
    bool isActive() const { return currentMode != Mode::OFF; }
    
    float getCurrentTime() const { return currentTime; }
    float getDuration() const { return demoDuration; }
    int getKeyframeCount() const { return (int)keyframes.size(); }
    
    // Get current segment name
    std::string getCurrentSegment() const;
    
private:
    struct Keyframe {
        float time;
        glm::vec3 position;
        glm::vec3 target;
        float fov;
    };
    
    Mode currentMode{Mode::OFF};
    float currentTime{0.0f};
    float demoDuration{150.0f};  // 2.5 minutes
    
    std::vector<Keyframe> keyframes;
    int currentKeyframeIndex{0};
    
    // Interpolation
    glm::vec3 lerpPosition(const glm::vec3& a, const glm::vec3& b, float t);
    float slerp(float a, float b, float t);
    
    // Demo path setup
    void setupDemoPath();
};
