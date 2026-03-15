#pragma once

// ============================================================================
// DEMO CONFIGURATION
// ============================================================================
// Settings optimized for recording a tech demo video
// ============================================================================

struct DemoConfig {
    // =========================================================================
    // VIDEO SETTINGS
    // =========================================================================
    
    // Resolution (1920x1080 for YouTube/Twitter)
    static constexpr int windowWidth = 1920;
    static constexpr int windowHeight = 1080;
    
    // Target FPS (60 for smooth playback)
    static constexpr float targetFPS = 60.0f;
    static constexpr float frameTime = 1.0f / targetFPS;
    
    // =========================================================================
    // DEMO SEQUENCE
    // =========================================================================
    
    // Total demo duration in seconds
    static constexpr float demoDuration = 150.0f;  // 2.5 minutes
    
    // Camera positions for each segment (time, position, target)
    struct CameraKeyframe {
        float time;           // When this keyframe starts (seconds)
        float position[3];    // Camera position
        float target[3];      // Look at target
        float fov;            // Field of view
    };
    
    // =========================================================================
    // FEATURE TOGGLES
    // =========================================================================
    
    static constexpr bool showDebugUI = false;       // Hide FPS counter, debug text
    static constexpr bool showWireframe = false;     // Solid rendering
    static constexpr bool enableVSync = false;       // Disable for consistent recording
    static constexpr bool hideConsole = true;        // Hide console output
    
    // =========================================================================
    // SCENE SETTINGS
    // =========================================================================
    
    // Time of day (affects lighting)
    static constexpr float timeOfDay = 14.0f;  // 2 PM - good lighting
    
    // Weather
    static constexpr bool enableFog = true;
    static constexpr float fogDensity = 0.003f;
    
    // Character
    static constexpr float characterX = 0.0f;
    static constexpr float characterY = 10.0f;  // Start on terrain
    static constexpr float characterZ = 0.0f;
    
    // Camera
    static constexpr float cameraDistance = 20.0f;  // Pulled back for wide shots
    static constexpr float cameraHeight = 8.0f;
    static constexpr float cameraSmooth = 5.0f;
    
    // =========================================================================
    // DEMO SEGMENTS
    // =========================================================================
    /*
    Segment 1 (0:00-0:20):  Wide terrain pan
    Segment 2 (0:20-0:40):  Character walk → run → crouch
    Segment 3 (0:40-1:00):  Foot IK close-up
    Segment 4 (1:00-1:20):  Water entry
    Segment 5 (1:20-1:40):  Forest walk-through
    Segment 6 (1:40-2:00):  Camera orbit showcase
    Segment 7 (2:00-2:20):  Performance stats (F1)
    Segment 8 (2:20-2:30):  End card
    */
};
