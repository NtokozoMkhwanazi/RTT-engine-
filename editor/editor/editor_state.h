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
#include "viewport_framebuffer.h"
#include "imgui_context.h"
#include <memory>
#include <string>

// Forward declaration — AnimatedCharacter is defined in the engine binary
// (test.cpp / PlayModeController). The Editor holds a weak pointer for
// gizmo manipulation; null when no character is loaded.
class AnimatedCharacter;

// ============================================================================
// Editor Class (RAII wrapper around all editor global state)
// ============================================================================
namespace Editor {

class Editor {
public:
    Editor();
    ~Editor();

    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    bool initialize();
    void shutdown();
    bool isInitialized() const noexcept { return initialized_; }

    // ECS / rendering accessors
    ecs::World& world() noexcept { return world_; }
    const ecs::World& world() const noexcept { return world_; }
    Renderer& renderer() noexcept { return renderer_; }
    const Renderer& renderer() const noexcept { return renderer_; }
    flyCamera* camera() noexcept { return camera_.get(); }
    const flyCamera* camera() const noexcept { return camera_.get(); }

    ViewportFramebuffer& viewportFramebuffer() noexcept { return viewportFB_; }
    const ViewportFramebuffer& viewportFramebuffer() const noexcept { return viewportFB_; }

    EntityCache& entityCache() noexcept { return entityCache_; }
    const EntityCache& entityCache() const noexcept { return entityCache_; }

    // Subsystem accessors (kept public for minimal call-site churn)
    ecs::RenderSystem& renderSystem() noexcept { return renderSystem_; }
    const ecs::RenderSystem& renderSystem() const noexcept { return renderSystem_; }
    ecs::ModelRenderSystem& modelRenderSystem() noexcept { return modelRenderSystem_; }
    const ecs::ModelRenderSystem& modelRenderSystem() const noexcept { return modelRenderSystem_; }
    ecs::LightSystem& lightSystem() noexcept { return lightSystem_; }
    const ecs::LightSystem& lightSystem() const noexcept { return lightSystem_; }
    ecs::GeospatialSystem& geospatialSystem() noexcept { return geospatialSystem_; }
    const ecs::GeospatialSystem& geospatialSystem() const noexcept { return geospatialSystem_; }
    ecs::GeoTerrainSystem& geoTerrainSystem() noexcept { return geoTerrainSystem_; }
    const ecs::GeoTerrainSystem& geoTerrainSystem() const noexcept { return geoTerrainSystem_; }
    GeoTerrainRenderer& geoTerrainRenderer() noexcept { return geoTerrainRenderer_; }
    const GeoTerrainRenderer& geoTerrainRenderer() const noexcept { return geoTerrainRenderer_; }

    // Selection
    ecs::EntityID selectedEntity() const noexcept { return selectedEntity_; }
    void setSelectedEntity(ecs::EntityID id) noexcept;

    // Gizmo / viewport state
    GizmoRenderer::GizmoType gizmoType() const noexcept { return gizmoType_; }
    void setGizmoType(GizmoRenderer::GizmoType t) noexcept { gizmoType_ = t; }

    GizmoRenderer::SpaceType spaceType() const noexcept { return spaceType_; }
    void setSpaceType(GizmoRenderer::SpaceType t) noexcept { spaceType_ = t; }

    // Animated character (bot) — for gizmo manipulation when no ECS entity
    // is selected. Set by the engine binary when a character is loaded.
    AnimatedCharacter* character() const noexcept { return character_; }
    void setCharacter(AnimatedCharacter* c) noexcept { character_ = c; }

    GizmoRenderer::GizmoAxis hoveredAxis() const noexcept { return hoveredAxis_; }
    void setHoveredAxis(GizmoRenderer::GizmoAxis a) noexcept { hoveredAxis_ = a; }

    bool showGrid() const noexcept { return showGrid_; }
    void setShowGrid(bool v) noexcept { showGrid_ = v; }

    bool showGizmo() const noexcept { return showGizmo_; }
    void setShowGizmo(bool v) noexcept { showGizmo_ = v; }

    int renderMode() const noexcept { return renderMode_; }
    void setRenderMode(int m) noexcept {
        renderMode_ = m;
        showWireframe_ = (m == 1) ? 1 : 0;  // Wire mode drives the legacy toggle
    }

    int showWireframe() const noexcept { return showWireframe_; }
    void setShowWireframe(int v) noexcept {
        showWireframe_ = v;
        if (v) {
            renderMode_ = 1;            // legacy toggle ON -> Wire
        } else if (renderMode_ == 1) {  // OFF from Wire -> back to Lit
            renderMode_ = 0;
        }
    }

    bool isViewing() const noexcept { return isViewing_; }
    void setIsViewing(bool v) noexcept { isViewing_ = v; }

    glm::vec2& lastMousePos() noexcept { return lastMousePos_; }
    const glm::vec2& lastMousePos() const noexcept { return lastMousePos_; }

    // Game state
    bool isPlaying() const noexcept { return isPlaying_; }
    void setPlaying(bool v) noexcept { isPlaying_ = v; }

    bool wasPlaying() const noexcept { return wasPlaying_; }
    void setWasPlaying(bool v) noexcept { wasPlaying_ = v; }

    float gameSpeed() const noexcept { return gameSpeed_; }
    void setGameSpeed(float s) noexcept { gameSpeed_ = s; }

    float fps() const noexcept { return fps_; }
    void setFps(float f) noexcept { fps_ = f; }

    int frameCount() const noexcept { return frameCount_; }
    void setFrameCount(int c) noexcept { frameCount_ = c; }

    float lastRenderTime() const noexcept { return lastRenderTime_; }
    void setLastRenderTime(float t) noexcept { lastRenderTime_ = t; }

    bool shouldClose() const noexcept { return shouldClose_; }
    void setShouldClose(bool v) noexcept { shouldClose_ = v; }

    bool showAbout() const noexcept { return showAbout_; }
    void setShowAbout(bool v) noexcept { showAbout_ = v; }
    bool& showAboutRef() noexcept { return showAbout_; }

    // Public sub-state structs (kept public to reduce churn; gradually migrate to private)
    struct UIState {
        char searchBuffer[128] = "";
        char pathBuffer[256] = "/Game/Assets";
        int leftPanelTab = 0;
        int bottomPanelTab = 0;
        float leftPanelWidth = 280.0f;
        float rightPanelWidth = 300.0f;
        float bottomPanelHeight = 180.0f;
        float toolbarHeight = 38.0f;
        float statusBarHeight = 24.0f;
        float minPanelWidth = 200.0f;
        float maxPanelWidth = 600.0f;
        float minBottomHeight = 100.0f;
        float maxBottomHeight = 400.0f;
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
        bool showSaveAs = false;
        bool showOpenScene = false;
        // Current scene file (relative to the project root).
        char sceneFile[128] = "scene.json";
        // Play-mode camera: 0 Follow, 1 Orbit, 2 Top-down, 3 First-person.
        int playCameraMode = 0;
        // Follow-camera tuning (World Settings panel sliders). Distance is
        // two-way synced with the live follow cam (scroll zoom updates it);
        // pitch is the Follow mode's resting look angle.
        float playCameraDistance = 4.0f;
        float playCameraPitch = 10.0f;
        // Gizmo snapping (viewport toolbar + World Settings panel). When
        // snapEnabled the transform gizmo drags snap to these increments:
        // translate in world units, rotate in degrees, scale as a factor.
        bool snapEnabled = false;
        float snapTranslate = 1.0f;
        float snapRotate = 15.0f;
        float snapScale = 0.5f;

        // ----- World / culling / LOD debug tuning (World Settings panel) -----
        struct WorldSettings {
            // Hard cap on the number of visible prop instances drawn per frame
            // (default 500). The culling pass never submits more than this many
            // instances across all prop meshes, keeping the viewport interactive
            // with huge procedural worlds. Exposed as a slider in World Settings.
            int maxVisibleInstances = 500;
            // LOD distance bands, expressed as a fraction of each prop mesh's cull
            // radius (radius is the per-mesh fade/drop distance). Surviving instances
            // are tinted by band when cullingDebug: (0, band1] green / LOD0, (band1,
            // band2] yellow / LOD1, (band2, band3] red / LOD2, past band3 dropped.
            float lodBand1 = 0.40f;
            float lodBand2 = 0.70f;
            float lodBand3 = 0.92f;
            // When true: tint each visible prop by its LOD band, draw a translucent
            // AABB bounds box around every visible prop, and print a visible-count
            // overlay so you can see exactly what the culler kept/dropped.
            bool cullingDebug = false;
        } worldSettings;
    } uiState;

    EditorUI::ScenePanelConfig scenePanelConfig;
    int scenePanelActiveTab = 0;
    UI::GeoPanelState geoPanelState;

    struct DebugConfig {
        bool verbose = true;
    } debugConfig;

private:
    bool initialized_ = false;

    ecs::World world_;
    Renderer renderer_;
    ecs::RenderSystem renderSystem_;
    ecs::ModelRenderSystem modelRenderSystem_;
    ecs::LightSystem lightSystem_;
    ecs::GeospatialSystem geospatialSystem_;
    ecs::GeoTerrainSystem geoTerrainSystem_;
    GeoTerrainRenderer geoTerrainRenderer_;

    std::unique_ptr<flyCamera> camera_;
    ViewportFramebuffer viewportFB_;
    EntityCache entityCache_;

    ecs::EntityID selectedEntity_ = ecs::INVALID_ENTITY_ID;
    GizmoRenderer::GizmoType gizmoType_ = GizmoRenderer::GizmoType::Translate;
    GizmoRenderer::SpaceType spaceType_ = GizmoRenderer::SpaceType::World;
    GizmoRenderer::GizmoAxis hoveredAxis_ = GizmoRenderer::GizmoAxis::None;

    AnimatedCharacter* character_ = nullptr;  // Set by engine binary when loaded

    bool showGrid_ = true;
    bool showGizmo_ = true;
    int renderMode_ = 0;        // 0=Lit, 1=Wire, 2=Normals, 3=Unlit
    int showWireframe_ = 0;
    bool isViewing_ = false;
    glm::vec2 lastMousePos_{0.0f, 0.0f};

    bool isPlaying_ = false;
    bool wasPlaying_ = false;
    float gameSpeed_ = 1.0f;

    float fps_ = 0.0f;
    int frameCount_ = 0;
    float lastRenderTime_ = 0.0f;

    bool shouldClose_ = false;
    bool showAbout_ = false;
};

} // namespace Editor

// Legacy global accessor. New code should prefer dependency injection.
extern Editor::Editor g_editor;

// Legacy free functions preserved for compatibility.
void InitEditor();
void CleanupEditor();

#endif // EDITOR_STATE_H
