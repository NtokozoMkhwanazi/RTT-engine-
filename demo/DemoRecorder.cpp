#include "DemoRecorder.h"
#include <iostream>
#include <cmath>
#include <algorithm>

// ============================================================================
// IMPLEMENTATION
// ============================================================================

DemoRecorder::DemoRecorder() {
    demoDuration = DemoConfig::demoDuration;
}

DemoRecorder::~DemoRecorder() {
    shutdown();
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void DemoRecorder::initialize() {
    setupDemoPath();
    std::cout << "[DemoRecorder] Initialized with " << keyframes.size() << " keyframes\n";
}

void DemoRecorder::shutdown() {
    stopRecording();
    stopPlayback();
}

// ============================================================================
// MODE TOGGLE
// ============================================================================

void DemoRecorder::toggleMode() {
    switch (currentMode) {
        case Mode::OFF:
            startRecording();
            std::cout << "[DemoRecorder] RECORDING mode - Press F6 to stop\n";
            break;
        case Mode::RECORDING:
            stopRecording();
            std::cout << "[DemoRecorder] Recording stopped. Press F6 to play back\n";
            break;
        case Mode::PLAYBACK:
            setMode(Mode::OFF);
            std::cout << "[DemoRecorder] Playback stopped\n";
            break;
    }
}

// ============================================================================
// RECORDING
// ============================================================================

void DemoRecorder::startRecording() {
    currentMode = Mode::RECORDING;
    currentTime = 0.0f;
    keyframes.clear();
    std::cout << "[DemoRecorder] Started recording\n";
}

void DemoRecorder::stopRecording() {
    currentMode = Mode::OFF;
    std::cout << "[DemoRecorder] Stopped recording - captured " 
              << keyframes.size() << " keyframes\n";
}

void DemoRecorder::recordKeyframe(const flyCamera& cam) {
    if (currentMode != Mode::RECORDING) return;
    
    Keyframe kf;
    kf.time = currentTime;
    kf.position = cam.Position;
    kf.target = cam.Target;  // Use Target directly
    kf.fov = cam.Zoom;
    
    keyframes.push_back(kf);
}

void DemoRecorder::saveRecording(const std::string& filename) {
    std::cout << "[DemoRecorder] Would save to: " << filename << "\n";
    // TODO: Implement file saving
}

// ============================================================================
// PLAYBACK
// ============================================================================

void DemoRecorder::startPlayback() {
    currentMode = Mode::PLAYBACK;
    currentTime = 0.0f;
    currentKeyframeIndex = 0;
    std::cout << "[DemoRecorder] Starting playback\n";
}

void DemoRecorder::stopPlayback() {
    currentMode = Mode::OFF;
}

void DemoRecorder::updateCamera(flyCamera& cam) {
    if (currentMode != Mode::PLAYBACK || keyframes.empty()) return;
    
    // Find current keyframes for interpolation
    if (currentKeyframeIndex >= (int)keyframes.size() - 1) {
        // End of demo
        stopPlayback();
        return;
    }
    
    const Keyframe& current = keyframes[currentKeyframeIndex];
    const Keyframe& next = keyframes[currentKeyframeIndex + 1];
    
    // Calculate interpolation factor
    float segmentStart = current.time;
    float segmentEnd = next.time;
    float segmentDuration = segmentEnd - segmentStart;
    
    if (segmentDuration <= 0.0f) {
        currentKeyframeIndex++;
        return;
    }
    
    float t = (currentTime - segmentStart) / segmentDuration;
    t = std::clamp(t, 0.0f, 1.0f);
    
    // Smooth interpolation (ease in/out)
    float smoothT = t * t * (3.0f - 2.0f * t);
    
    // Interpolate position
    glm::vec3 newPos = lerpPosition(current.position, next.position, smoothT);
    glm::vec3 newTarget = lerpPosition(current.target, next.target, smoothT);
    
    // Update camera - set Position and Target directly
    // The camera's vectors (Right, Up) will be recalculated next frame
    cam.Position = newPos;
    cam.Target = newTarget;
    
    // Manually recalculate Right and Up vectors
    glm::vec3 front = glm::normalize(newTarget - newPos);
    cam.Right = glm::normalize(glm::cross(front, cam.WorldUp));
    cam.Up = glm::normalize(glm::cross(cam.Right, front));
    
    // Move to next keyframe if needed
    if (t >= 1.0f) {
        currentKeyframeIndex++;
    }
}

// ============================================================================
// MAIN UPDATE
// ============================================================================

void DemoRecorder::update(float dt) {
    if (currentMode == Mode::OFF) return;
    
    currentTime += dt;
    
    if (currentTime >= demoDuration) {
        currentTime = 0.0f;
        currentKeyframeIndex = 0;
        std::cout << "[DemoRecorder] Demo loop complete, restarting\n";
    }
}

// ============================================================================
// DEMO PATH SETUP
// ============================================================================

void DemoRecorder::setupDemoPath() {
    // Pre-defined camera path for the demo
    // Each segment is ~20 seconds
    
    float time = 0.0f;
    const float segmentTime = 20.0f;
    
    // Segment 1: Wide terrain overview
    keyframes.push_back({time, {0.0f, 50.0f, 100.0f}, {0.0f, 0.0f, 0.0f}, 45.0f});
    time += segmentTime;
    
    // Segment 2: Character walk/run showcase
    keyframes.push_back({time, {-10.0f, 5.0f, 15.0f}, {0.0f, 2.0f, 0.0f}, 45.0f});
    time += segmentTime;
    
    // Segment 3: Foot IK close-up
    keyframes.push_back({time, {3.0f, 1.5f, 5.0f}, {0.0f, 0.5f, 0.0f}, 60.0f});
    time += segmentTime;
    
    // Segment 4: Water entry
    keyframes.push_back({time, {50.0f, 20.0f, 80.0f}, {50.0f, 5.0f, 70.0f}, 45.0f});
    time += segmentTime;
    
    // Segment 5: Forest walk-through
    keyframes.push_back({time, {-50.0f, 10.0f, -50.0f}, {-45.0f, 5.0f, -45.0f}, 50.0f});
    time += segmentTime;
    
    // Segment 6: Camera orbit
    keyframes.push_back({time, {0.0f, 10.0f, 30.0f}, {0.0f, 2.0f, 0.0f}, 45.0f});
    time += segmentTime;
    
    // Segment 7: Performance stats
    keyframes.push_back({time, {0.0f, 30.0f, 60.0f}, {0.0f, 5.0f, 0.0f}, 40.0f});
    time += segmentTime;
    
    // Segment 8: End position
    keyframes.push_back({time, {0.0f, 20.0f, 40.0f}, {0.0f, 2.0f, 0.0f}, 45.0f});
    time += segmentTime;
    
    std::cout << "[DemoRecorder] Demo path setup complete\n";
}

// ============================================================================
// HELPERS
// ============================================================================

glm::vec3 DemoRecorder::lerpPosition(const glm::vec3& a, const glm::vec3& b, float t) {
    return a + (b - a) * t;
}

float DemoRecorder::slerp(float a, float b, float t) {
    return a + (b - a) * t;
}

std::string DemoRecorder::getCurrentSegment() const {
    if (keyframes.empty()) return "Unknown";
    
    float time = currentTime;
    const float segmentTime = 20.0f;
    int segment = (int)(time / segmentTime);
    
    switch (segment) {
        case 0: return "1: Terrain Overview";
        case 1: return "2: Character Movement";
        case 2: return "3: Foot IK Close-up";
        case 3: return "4: Water System";
        case 4: return "5: Vegetation";
        case 5: return "6: Camera Orbit";
        case 6: return "7: Performance Stats";
        case 7: return "8: Ending";
        default: return "Complete";
    }
}
