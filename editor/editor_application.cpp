#include <glad/glad.h>
#include "editor_application.h"
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

namespace Editor {

EditorApplication& EditorApplication::getInstance() {
    static EditorApplication instance;
    return instance;
}

EditorApplication::EditorApplication()
    : m_renderPipeline(Render::RenderPipeline::getInstance())
    , m_inputManager(Input::InputManager::getInstance())
    , m_worldManager(World::WorldManager::getInstance())
    , m_resourceManager(Resources::ResourceManager::getInstance()) {
}

EditorApplication::~EditorApplication() {
    shutdown();
}

bool EditorApplication::initialize(int windowWidth, int windowHeight) {
    m_windowWidth = windowWidth;
    m_windowHeight = windowHeight;
    m_state = AppState::Initializing;

    std::cout << "[EditorApplication] Initializing..." << std::endl;

    // Window creation should be done in main() for now as per test_refactored.cpp
    m_window = glfwGetCurrentContext();
    if (!m_window) {
        std::cerr << "[EditorApplication] Error: No GLFW context found!" << std::endl;
        return false;
    }

    if (!m_renderPipeline.initialize()) return false;
    
    m_inputManager.initialize(m_window);
    
    if (!m_worldManager.initialize(Config::getTerrainConfig(), Config::getVegetationConfig())) {
        std::cerr << "[EditorApplication] WorldManager initialization had issues" << std::endl;
    }

    m_state = AppState::Running;
    std::cout << "[EditorApplication] Initialized successfully" << std::endl;
    return true;
}

int EditorApplication::run() {
    m_lastTime = glfwGetTime();
    
    while (m_state != AppState::ShuttingDown && !glfwWindowShouldClose(m_window)) {
        float currentTime = glfwGetTime();
        float dt = currentTime - m_lastTime;
        m_lastTime = currentTime;

        processInput(dt);
        update(dt);
        render(dt);

        glfwPollEvents();
    }
    return 0;
}

void EditorApplication::shutdown() {
    if (m_state == AppState::ShuttingDown) return;
    
    m_state = AppState::ShuttingDown;
    m_worldManager.shutdown();
    m_inputManager.shutdown();
    m_renderPipeline.shutdown();
}

void EditorApplication::processInput(float dt) {
    m_inputManager.update();
}

void EditorApplication::update(float dt) {
    glm::vec3 cameraPos(0.0f); // Should be retrieved from camera system
    m_worldManager.update(cameraPos, dt);
}

void EditorApplication::render(float dt) {
    m_renderPipeline.beginFrame();
    
    // Default matrices for now
    glm::mat4 view(1.0f);
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), (float)m_windowWidth/m_windowHeight, 0.1f, 1000.0f);
    glm::vec3 cameraPos(0.0f);
    float fov = 45.0f;
    
    m_renderPipeline.renderScene(view, proj, cameraPos, fov);
    m_worldManager.render(view, proj, cameraPos);
    
    m_renderPipeline.renderUI();
    m_renderPipeline.endFrame();
    
    glfwSwapBuffers(m_window);
}

bool EditorApplication::loadScene(const std::string& scenePath) {
    std::cout << "[EditorApplication] Loading scene: " << scenePath << std::endl;
    return true;
}

bool EditorApplication::saveScene(const std::string& scenePath) {
    std::cout << "[EditorApplication] Saving scene: " << scenePath << std::endl;
    return true;
}

bool EditorApplication::newScene() {
    std::cout << "[EditorApplication] Creating new scene" << std::endl;
    return true;
}

void EditorApplication::enterPlayMode() {
    m_isPlaying = true;
    std::cout << "[EditorApplication] Entering Play Mode" << std::endl;
}

void EditorApplication::exitPlayMode() {
    m_isPlaying = false;
    std::cout << "[EditorApplication] Exiting Play Mode" << std::endl;
}

void EditorApplication::togglePlayMode() {
    if (m_isPlaying) exitPlayMode();
    else enterPlayMode();
}

void EditorApplication::onWindowResize(int width, int height) {
    m_windowWidth = width;
    m_windowHeight = height;
    m_renderPipeline.resizeViewport(width, height);
}

void EditorApplication::onKeyInput(int key, int action) {
    // Handle key input
}

void EditorApplication::createDefaultScene() {
    // Create default scene
}

} // namespace Editor
