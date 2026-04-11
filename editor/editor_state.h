#ifndef EDITOR_STATE_H
#define EDITOR_STATE_H

#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "ecs/systems/Systems.h"
#include "cameraSystem/flyCamera.h"
#include "renderer/Renderer.h"
#include "geospatial/GPSTracker.h"
#include "gizmo_renderer.h"
#include <string>

// ============================================================================
// Editor Global State
// ============================================================================
struct EditorState {
    // ECS and rendering
    ecs::World world;
    flyCamera* camera = nullptr;
    Renderer renderer;
    ecs::RenderSystem renderSystem;
    ecs::GeospatialSystem geospatialSystem;
    
    // Selection
    ecs::EntityID selectedEntity = ecs::INVALID_ENTITY_ID;
    
    // Gizmo state
    GizmoRenderer::GizmoType gizmoType = GizmoRenderer::GizmoType::Translate;
    GizmoRenderer::SpaceType spaceType = GizmoRenderer::SpaceType::World;
    bool showGrid = true;
    bool showGizmo = true;
    bool showWireframe = false;
    
    // Viewport camera
    bool isViewing = false;
    glm::vec2 lastMousePos;
    
    // UI state
    struct {
        char searchBuffer[128] = "";
        char pathBuffer[256] = "/Game/Assets";
        int leftPanelTab = 0;
        int bottomPanelTab = 0;
    } uiState;
    
    // Game state
    bool isPlaying = false;
    bool wasPlaying = false;
    float gameSpeed = 1.0f;
    
    // Debug
    struct {
        bool verbose = true;
    } debugConfig;
    
    // Framebuffer
    struct Framebuffer {
        GLuint fbo = 0;
        GLuint colorTex = 0;
        GLuint rbo = 0;
        int width = 1280;
        int height = 720;
        
        void init(int w, int h);
        void resize(int w, int h);
        void cleanup();
        void bind();
        void unbind();
    } viewportFB;
    
    // FPS tracking
    float fps = 0.0f;
    int frameCount = 0;
    float lastRenderTime = 0;
    
    // About dialog
    bool showAbout = false;
    
    // Window close
    bool shouldClose = false;
};

// Global editor state
extern EditorState g_editor;

// Initialize/cleanup editor
void InitEditor();
void CleanupEditor();

#endif // EDITOR_STATE_H
