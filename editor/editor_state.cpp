#include "editor_state.h"
#include "AnimatedCharacter.h"  // A: complete type for NPCSpawnAsyncState::Capsule::~Capsule (unique_ptr<AnimatedCharacter> member)
#include "mesh_builder.h"
#include "shader_manager.h"
#include "grid_renderer.h"
#include "renderer/MeshRegistry.h"
#include "renderer/GPUProfilerAdvanced.h"
#include <iostream>

// Legacy global instance preserved for backward compatibility.
Editor::Editor g_editor;

namespace Editor {

Editor::Editor() = default;

// NPCSpawnAsyncState::Capsule holds std::unique_ptr<AnimatedCharacter>
// (forward-declared in editor_state.h); define its destructor here where
// AnimatedCharacter is complete so the staged character is destroyed on the
// main thread when an async NPC spawn is committed or abandoned.
Editor::UIState::NPCSpawnAsyncState::Capsule::~Capsule() = default;


Editor::~Editor() {
    // RAII: ensure cleanup runs even if shutdown() was not called.
    shutdown();
}

void Editor::setSelectedEntity(ecs::EntityID id) noexcept {
    // Trigger the viewport selection pulse only on a *real* change of selection
    // to a valid entity (clearing or re-selecting the same ID is silent).
    if (id != selectedEntity_ && id != ecs::INVALID_ENTITY_ID) {
        triggerSelectionPulse(kPulseDuration);
    }
    selectedEntity_ = id;
    // Symmetric with setSelectedPropInstance: picking an ECS entity releases
    // any prop gizmo so the two selection modes never overlap.
    clearSelectedPropInstance();
}

bool Editor::initialize() {
    if (initialized_) {
        return true;
    }

    std::cout << "=== RTT Engine Editor ===\n";

    // Initialize resources
    ShaderManager::InitShaders();
    MeshBuilder::InitAll();
    GridRenderer::Init(100, 1.0f);  // was: Init(20, 1.0f) — 20-unit grid was a
    viewportFB_.initialize(1280, 720);

    // Register meshes
    auto& meshReg = MeshRegistry::getInstance();
    meshReg.registerMesh(ecs::MeshType::Cube, MeshBuilder::GetCube().vao, MeshBuilder::GetCube().vbo,
                         MeshBuilder::GetCube().ebo, MeshBuilder::GetCube().indexCount);
    meshReg.registerMesh(ecs::MeshType::Sphere, MeshBuilder::GetSphere().vao, MeshBuilder::GetSphere().vbo,
                         MeshBuilder::GetSphere().ebo, MeshBuilder::GetSphere().indexCount);
    meshReg.registerMesh(ecs::MeshType::Plane, MeshBuilder::GetPlane().vao, MeshBuilder::GetPlane().vbo,
                         MeshBuilder::GetPlane().ebo, MeshBuilder::GetPlane().indexCount);
    meshReg.registerMesh(ecs::MeshType::Cylinder, MeshBuilder::GetCylinder().vao, MeshBuilder::GetCylinder().vbo,
                         MeshBuilder::GetCylinder().ebo, MeshBuilder::GetCylinder().indexCount);
    meshReg.registerMesh(ecs::MeshType::Cone, MeshBuilder::GetCone().vao, MeshBuilder::GetCone().vbo,
                         MeshBuilder::GetCone().ebo, MeshBuilder::GetCone().indexCount);
    meshReg.registerMesh(ecs::MeshType::Torus, MeshBuilder::GetTorus().vao, MeshBuilder::GetTorus().vbo,
                         MeshBuilder::GetTorus().ebo, MeshBuilder::GetTorus().indexCount);

    std::cout << "[MeshRegistry] Registered 6 mesh types\n";

    // Set texture for renderer
    renderer_.SetDefaultTexture(MeshBuilder::GetProceduralTexture());

    // Initialize camera and ECS
    // Camera at (0,5,10) looking at origin with -20 degree pitch (looking down at objects)
    camera_ = std::make_unique<flyCamera>(glm::vec3(0, 5, 10), glm::vec3(0, 0, 0), -90, -20, 10);
    world_.init();
    world_.addSystem<ecs::PhysicsSystem>().setGravity(glm::vec3(0, -9.81f, 0));

    // Initialize Renderer and Render System
    renderer_.Initialize();
    renderSystem_.setRenderer(&renderer_);
    renderSystem_.setWorld(&world_);
    renderSystem_.setDefaultShaderProgram(ShaderManager::GetMainShaderProgram());
    world_.addStaticSystem(&renderSystem_);

    // Initialize Model Render System
    modelRenderSystem_.setRenderer(&renderer_);
    modelRenderSystem_.setWorld(&world_);
    modelRenderSystem_.setDefaultShaderProgram(ShaderManager::GetMainShaderProgram());
    world_.addStaticSystem(&modelRenderSystem_);

    // Light System - collects editor light entities (Add menu -> Point Light)
    // so the frame loop can drive the renderer's single shader light from
    // them. Uses world forEach (archetype storage) to see CreateLight()
    // entities.
    lightSystem_.setWorld(&world_);
    world_.addStaticSystem(&lightSystem_);

    // Initialize Geospatial System
#ifndef DISABLE_GEOSPATIAL
    geospatialSystem_.initialize(-33.8568, 151.2153, 50.0);
    geospatialSystem_.setGPSMode(GPSTracker::Mode::SIMULATED_WALK);

    // Initialize GeoTerrain System (map/terrain integration)
    ecs::GeoTerrainConfig terrainConfig;
    terrainConfig.terrainSize = 2000.0f;
    terrainConfig.heightScale = 100.0f;
    terrainConfig.gridResolution = 256;
    geoTerrainSystem_.initialize(-33.8568, 151.2153, terrainConfig);
    geoTerrainSystem_.setGeospatialSystem(&geospatialSystem_);
    geoTerrainSystem_.generateTerrain();

    world_.addStaticSystem(&geospatialSystem_);
    world_.addStaticSystem(&geoTerrainSystem_);
#endif

    initialized_ = true;
    std::cout << "Editor initialized\n";
    return true;
}

void Editor::shutdown() {
    if (!initialized_) {
        return;
    }

    std::cout << "Shutting down editor...\n";

    initialized_ = false;

    AdvancedGPUProfiler::getInstance().shutdown();

    world_.shutdown();

    renderer_.Shutdown();
#ifndef DISABLE_GEOSPATIAL
    geospatialSystem_.shutdown();
#endif

    GridRenderer::Cleanup();
    MeshBuilder::CleanupAll();
    ShaderManager::CleanupShaders();
    viewportFB_.cleanup();

    camera_.reset();

    std::cout << "Editor shutdown complete\n";
}

} // namespace Editor

// Legacy free functions delegate to the global Editor instance.
void InitEditor() {
    g_editor.initialize();
}

void CleanupEditor() {
    g_editor.shutdown();
}
