#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "editor_state.h"
#include "mesh_builder.h"
#include "shader_manager.h"
#include "grid_renderer.h"
#include "renderer/MeshRegistry.h"
#include "renderer/GPUProfilerAdvanced.h"
#include <iostream>

EditorState g_editor;

void EditorState::Framebuffer::init(int w, int h) {
    cleanup();
    width = w; height = h;

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "ERROR: Framebuffer incomplete!\n";
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void EditorState::Framebuffer::resize(int w, int h) {
    if (w <= 0 || h <= 0 || (w == width && h == height)) return;
    init(w, h);
}

void EditorState::Framebuffer::cleanup() {
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (colorTex) glDeleteTextures(1, &colorTex);
    if (rbo) glDeleteRenderbuffers(1, &rbo);
    fbo = 0; colorTex = 0; rbo = 0;
}

void EditorState::Framebuffer::bind() {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
}

void EditorState::Framebuffer::unbind() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void InitEditor() {
    std::cout << "=== RTT Engine Editor ===\n";
    
    // Initialize resources
    ShaderManager::InitShaders();
    MeshBuilder::InitAll();
    GridRenderer::Init(20, 1.0f);
    g_editor.viewportFB.init(1280, 720);
    
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
    g_editor.renderer.SetDefaultTexture(MeshBuilder::GetProceduralTexture());
    
    // Initialize camera and ECS
    // Camera at (0,5,10) looking at origin with -20 degree pitch (looking down at objects)
    g_editor.camera = new flyCamera(glm::vec3(0, 5, 10), glm::vec3(0, 0, 0), -90, -20, 10);
    g_editor.world.init();
    g_editor.world.addSystem<ecs::PhysicsSystem>().setGravity(glm::vec3(0, -9.81f, 0));
    
    // Initialize Renderer and Render System
    g_editor.renderer.Initialize();
    g_editor.renderSystem.setRenderer(&g_editor.renderer);
    g_editor.renderSystem.setWorld(&g_editor.world);
    g_editor.renderSystem.setDefaultShaderProgram(ShaderManager::GetMainShaderProgram());
    g_editor.world.addSystem(&g_editor.renderSystem);

    // Initialize Model Render System
    g_editor.modelRenderSystem.setRenderer(&g_editor.renderer);
    g_editor.modelRenderSystem.setWorld(&g_editor.world);
    g_editor.modelRenderSystem.setDefaultShaderProgram(ShaderManager::GetMainShaderProgram());
    g_editor.world.addSystem(&g_editor.modelRenderSystem);
    
    // Initialize Geospatial System
    g_editor.geospatialSystem.initialize(-33.8568, 151.2153, 50.0);
    g_editor.geospatialSystem.setGPSMode(GPSTracker::Mode::SIMULATED_WALK);

    // Initialize GeoTerrain System (map/terrain integration)
    ecs::GeoTerrainConfig terrainConfig;
    terrainConfig.terrainSize = 2000.0f;
    terrainConfig.heightScale = 100.0f;
    terrainConfig.gridResolution = 256;
    g_editor.geoTerrainSystem.initialize(-33.8568, 151.2153, terrainConfig);
    g_editor.geoTerrainSystem.setGeospatialSystem(&g_editor.geospatialSystem);
    g_editor.geoTerrainSystem.generateTerrain();

    // Initialize GeoTerrain Renderer
    g_editor.geoTerrainRenderer.initialize();

    g_editor.world.addSystem(&g_editor.geospatialSystem);
    g_editor.world.addSystem(&g_editor.geoTerrainSystem);
    
    std::cout << "Editor initialized\n";
}

void CleanupEditor() {
    std::cout << "Shutting down editor...\n";
    
    AdvancedGPUProfiler::getInstance().shutdown();
    
    g_editor.world.shutdown();
    
    GridRenderer::Cleanup();
    MeshBuilder::CleanupAll();
    ShaderManager::CleanupShaders();
    g_editor.viewportFB.cleanup();
    
    if (g_editor.camera) {
        delete g_editor.camera;
        g_editor.camera = nullptr;
    }
    
    std::cout << "Editor shutdown complete\n";
}
