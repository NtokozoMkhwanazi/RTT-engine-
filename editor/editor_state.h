#ifndef EDITOR_STATE_H
#define EDITOR_STATE_H

#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "ecs/systems/Systems.h"
#include "ecs/systems/ModelRenderSystem.h"
#include "cameraSystem/flyCamera.h"
#include "renderer/Renderer.h"
#include "geospatial/GPSTracker.h"
#include "gizmo_renderer.h"
#include "scene_panel_config.h"
#include "ecs/systems/GeoTerrainSystem.h"
#include "renderer/GeoTerrainRenderer.h"
#include "geo_config_panel.h"
#include "ui_cache.h"
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
    ecs::ModelRenderSystem modelRenderSystem;
    ecs::GeospatialSystem geospatialSystem;
    ecs::GeoTerrainSystem geoTerrainSystem;
    GeoTerrainRenderer geoTerrainRenderer;

    // Scene panel configuration (flexible, not hardcoded)
    EditorUI::ScenePanelConfig scenePanelConfig;
    int scenePanelActiveTab = 0;  // Legacy support

    // Geo panel state
    UI::GeoPanelState geoPanelState;

    // Entity cache (avoids per-frame ECS iteration)
    EntityCache entityCache;

    // Selection
    ecs::EntityID selectedEntity = ecs::INVALID_ENTITY_ID;
    
    // Gizmo state
    GizmoRenderer::GizmoType gizmoType = GizmoRenderer::GizmoType::Translate;
    GizmoRenderer::SpaceType spaceType = GizmoRenderer::SpaceType::World;
    GizmoRenderer::GizmoAxis hoveredAxis = GizmoRenderer::GizmoAxis::None;
    bool showGrid = true;
    bool showGizmo = true;
    int showWireframe = 0;  // 0=Fill, 1=Line, 2=Point
    
    // Viewport camera
    bool isViewing = false;
    glm::vec2 lastMousePos;
    
    // UI state
    struct {
        char searchBuffer[128] = "";
        char pathBuffer[256] = "/Game/Assets";
        int leftPanelTab = 0;
        int bottomPanelTab = 0;

        // Flexible panel dimensions (as percentage of window, or fixed minimum)
        float leftPanelWidth = 280.0f;
        float rightPanelWidth = 300.0f;
        float bottomPanelHeight = 180.0f;
        float toolbarHeight = 38.0f;
        float statusBarHeight = 24.0f;
        float minPanelWidth = 200.0f;
        float maxPanelWidth = 600.0f;
        float minBottomHeight = 100.0f;
        float maxBottomHeight = 400.0f;

        // Panel visibility
        bool showOutliner = true;
        bool showDetails = true;
        bool showToolbox = true;
        bool showViewport = true;
        bool showConsole = false;
        bool showContentBrowser = false;
        bool showGameMode = false;
        bool showProfiler = false;
        bool showPreferences = false;
        bool showModelLoader = false;
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
