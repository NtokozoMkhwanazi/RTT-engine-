/**
 * GPU Profiler & UI Demo - FIXED
 * 
 * Demonstrates the integrated GPU profiler and Dear ImGui UI system.
 * 
 * Controls:
 * - Mouse: Look around
 * - WASD: Move camera
 * - 1-5: Toggle UI windows
 * - Escape: Exit
 */

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "ecs/systems/Systems.h"

#include "renderer/GPUProfilerAdvanced.h"
#include "renderer/EngineUI.h"
#include "renderer/DebugRenderer.h"
#include "renderer/Renderer.h"
#include "cameraSystem/flyCamera.h"
#include "shaderSystem/Shader.h"
#include "modelSystem/Model.h"

#include <iostream>
#include <random>

// ============================================================================
// Global State
// ============================================================================
static ecs::World g_world;
static flyCamera* g_camera = nullptr;
static std::vector<ecs::Entity> g_boxEntities;

// Renderer and shaders
static Renderer* g_renderer = nullptr;
static Shader* g_shader = nullptr;
static Model* g_model = nullptr;

// ============================================================================
// Create a simple cube model programmatically
// ============================================================================
Model* createCubeModel() {
    // Create cube mesh data with bone data (default: bone 0, weight 1.0)
    float vertices[] = {
        // positions          // normals              // texcoords          // bone IDs          // weights
        -0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  0.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  1.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  0.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  0.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  0.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  0.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  0.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,  0.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f,  0.5f, -0.5f, -1.0f,  0.0f,  0.0f,  1.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,  1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,  1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f, -0.5f,  0.5f, -1.0f,  0.0f,  0.0f,  0.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,  0.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f,  0.5f,  0.5f,  1.0f,  0.0f,  0.0f,  0.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f,  0.5f, -0.5f,  1.0f,  0.0f,  0.0f,  1.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f, -0.5f, -0.5f,  1.0f,  0.0f,  0.0f,  1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f, -0.5f, -0.5f,  1.0f,  0.0f,  0.0f,  1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f, -0.5f,  0.5f,  1.0f,  0.0f,  0.0f,  0.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f,  0.5f,  0.5f,  1.0f,  0.0f,  0.0f,  0.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,  0.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,  1.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,  1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,  1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,  0.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,  0.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,  0.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,  1.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,  1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
         0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,  1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,  0.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,  0.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f
    };

    unsigned int indices[] = {
        0, 1, 2, 2, 3, 4, 5, 6, 7, 7, 8, 9, 10, 11, 12, 12, 13, 14,
        15, 16, 17, 17, 18, 19, 20, 21, 22, 22, 23, 24, 25, 26, 27, 27, 28, 29,
        30, 31, 32, 32, 33, 34, 35, 36, 37, 37, 38, 39, 40, 41, 42, 42, 43, 44,
        45, 46, 47, 47, 48, 49, 50, 51, 52, 52, 53, 54, 55, 56, 57, 57, 58, 59
    };

    GLuint VAO, VBO, EBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);

    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 14 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 14 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 14 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(5, 4, GL_FLOAT, 14 * sizeof(float), (void*)(8 * sizeof(float)));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, 14 * sizeof(float), (void*)(10 * sizeof(float)));
    glEnableVertexAttribArray(6);

    glBindVertexArray(0);
    return Model::CreateFromVAO(VAO, 36);
}

// ============================================================================
// Initialize ECS World
// ============================================================================
void initializeECS() {
    std::cout << "=== Initializing ECS World ===\n";

    g_world.init();

    // Create renderer and shader
    g_renderer = new Renderer();
    g_renderer->Initialize();
    g_shader = new Shader("shaderSystem/VS.glsl", "shaderSystem/FS.glsl");
    g_model = createCubeModel();

    // Add systems with renderer setup
    auto& renderSystem = g_world.addSystem<ecs::RenderSystem>();
    renderSystem.setRenderer(g_renderer);
    renderSystem.setModel(g_model);
    renderSystem.setDefaultShaderProgram(g_shader->ID);

    auto& cameraSystem = g_world.addSystem<ecs::CameraSystem>();
    cameraSystem.setPriority(-100);

    auto& physicsSystem = g_world.addSystem<ecs::PhysicsSystem>();
    physicsSystem.setGravity(glm::vec3(0.0f, -9.81f, 0.0f));

    g_world.addSystem<ecs::LightSystem>();

    std::cout << "  Added " << g_world.getSystemCount() << " systems\n";
    std::cout << "  Renderer and shader initialized\n";
}

// ============================================================================
// Create Scene
// ============================================================================
void createScene() {
    std::cout << "\n=== Creating Scene ===\n";

    // Create camera - positioned to see the scene
    auto cameraEntity = g_world.createEntity();
    auto& transform = g_world.addComponent<ecs::TransformComponent>(cameraEntity);
    transform.position = glm::vec3(0.0f, 8.0f, 15.0f);  // Higher and further back
    transform.setEulerAngles(glm::vec3(glm::radians(-20.0f), 0.0f, 0.0f));  // Look down slightly
    
    auto& camera = g_world.addComponent<ecs::CameraComponent>(cameraEntity);
    camera.fov = 60.0f;  // Wider FOV to see more
    camera.nearPlane = 0.1f;
    camera.farPlane = 1000.0f;
    camera.isActive = true;
    
    g_world.addComponent<ecs::NameComponent>(cameraEntity, "MainCamera");
    std::cout << "  Camera at (0, 8, 15) looking down\n";

    // Create sun
    auto lightEntity = g_world.createEntity();
    auto& lightTransform = g_world.addComponent<ecs::TransformComponent>(lightEntity);
    lightTransform.setEulerAngles(glm::vec3(glm::radians(45.0f), glm::radians(45.0f), 0.0f));
    
    auto& light = g_world.addComponent<ecs::LightComponent>(lightEntity);
    light.setDirectional(glm::vec3(1.0f), 1.0f);
    light.castShadows = false;
    
    g_world.addComponent<ecs::NameComponent>(lightEntity, "Sun");
    std::cout << "  Directional light created\n";

    // Create floor (static box)
    auto floorEntity = g_world.createEntity();
    auto& floorTransform = g_world.addComponent<ecs::TransformComponent>(floorEntity);
    floorTransform.position = glm::vec3(0.0f, -0.5f, 0.0f);  // Just below origin
    floorTransform.scale = glm::vec3(50.0f, 1.0f, 50.0f);  // Large flat box

    auto& floorMesh = g_world.addComponent<ecs::MeshComponent>(floorEntity);
    floorMesh.meshID = 0;
    floorMesh.visible = true;

    auto& floorRigidbody = g_world.addComponent<ecs::RigidBodyComponent>(floorEntity);
    floorRigidbody.bodyType = ecs::RigidBodyType::STATIC;  // Static - doesn't move
    floorRigidbody.colliderType = ecs::ColliderType::BOX;
    floorRigidbody.boxSize = floorTransform.scale;
    floorRigidbody.initialize();

    g_world.addComponent<ecs::NameComponent>(floorEntity, "Floor");
    std::cout << "  Floor created (50x50)\n";

    // Create some physics boxes ON THE FLOOR
    std::cout << "  Creating physics boxes...\n";
    for (int i = 0; i < 5; ++i) {
        auto entity = g_world.createEntity();

        auto& transform = g_world.addComponent<ecs::TransformComponent>(entity);
        // Spawn boxes in a row on the floor
        transform.position = glm::vec3(
            -4.0f + i * 2.0f,  // Spread along X axis
            2.0f,               // Start above floor (will fall)
            0.0f                // Center on Z
        );
        transform.scale = glm::vec3(0.8f);  // Cube

        auto& mesh = g_world.addComponent<ecs::MeshComponent>(entity);
        mesh.meshID = 0;
        mesh.visible = true;

        auto& rigidbody = g_world.addComponent<ecs::RigidBodyComponent>(entity);
        rigidbody.bodyType = ecs::RigidBodyType::DYNAMIC;
        rigidbody.colliderType = ecs::ColliderType::BOX;
        rigidbody.boxSize = transform.scale;
        rigidbody.mass = 1.0f;
        rigidbody.restitution = 0.5f;  // Bouncy
        rigidbody.initialize();

        g_world.addComponent<ecs::NameComponent>(entity, "Box_" + std::to_string(i));
        g_boxEntities.push_back(entity);

        std::cout << "    Box " << i << " at (" << transform.position.x
                  << ", " << transform.position.y << ", " << transform.position.z << ")\n";
    }

    std::cout << "  Created " << g_boxEntities.size() << " boxes\n";
}

// ============================================================================
// Process Input
// ============================================================================
void processInput(GLFWwindow* window, float deltaTime) {
    // UI window toggles
    static bool lastKeyState[10] = {false};
    const int keyMap[] = {GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3, GLFW_KEY_4, GLFW_KEY_5};
    
    for (int i = 0; i < 5; i++) {
        bool keyState = glfwGetKey(window, keyMap[i]) == GLFW_PRESS;
        if (keyState && !lastKeyState[i]) {
            switch (i) {
                case 0: 
                    EngineUI::getInstance().setShowGPUProfiler(
                        !EngineUI::getInstance().getConfig().showGPUProfiler);
                    UI_LOG_INFO("GPU Profiler toggled", "UI");
                    break;
                case 1: 
                    EngineUI::getInstance().setShowHierarchy(
                        !EngineUI::getInstance().getConfig().showHierarchy);
                    UI_LOG_INFO("Hierarchy toggled", "UI");
                    break;
                case 2: 
                    EngineUI::getInstance().setShowInspector(
                        !EngineUI::getInstance().getConfig().showInspector);
                    UI_LOG_INFO("Inspector toggled", "UI");
                    break;
                case 3: 
                    EngineUI::getInstance().setShowConsole(
                        !EngineUI::getInstance().getConfig().showConsole);
                    UI_LOG_INFO("Console toggled", "UI");
                    break;
                case 4:
                    // Reset camera
                    g_camera->Position = glm::vec3(0.0f, 8.0f, 15.0f);
                    g_camera->Target = glm::vec3(0.0f, 0.0f, 0.0f);
                    UI_LOG_INFO("Camera reset", "Input");
                    break;
            }
        }
        lastKeyState[i] = keyState;
    }
    
    // Exit
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, true);
    }
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  GPU Profiler & UI Demo (FIXED)" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "GPU Profiler & UI Demo", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    // Initialize GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        return -1;
    }

    // Enable depth testing and vsync
    glEnable(GL_DEPTH_TEST);
    glfwSwapInterval(1);

    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;
    std::cout << "GLSL Version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << std::endl;

    // Initialize camera
    g_camera = new flyCamera(
        glm::vec3(0.0f, 8.0f, 15.0f),  // Start position
        glm::vec3(0, 0, 0),             // Look at origin
        -90.0f,
        0.0f,
        10.0f
    );

    // Initialize Debug Renderer
    std::cout << "\nInitializing Debug Renderer..." << std::endl;
    if (!g_debugRenderer.initialize()) {
        std::cerr << "Failed to initialize Debug Renderer" << std::endl;
        return -1;
    }

    // Initialize GPU Profiler
    std::cout << "Initializing GPU Profiler..." << std::endl;
    if (!AdvancedGPUProfiler::getInstance().initialize()) {
        std::cerr << "Failed to initialize GPU Profiler" << std::endl;
        return -1;
    }

    // Initialize UI
    std::cout << "Initializing UI System..." << std::endl;
    EngineUI::Config config;
    config.showGPUProfiler = true;
    config.showHierarchy = true;
    config.showInspector = true;
    config.showConsole = true;
    
    if (!EngineUI::getInstance().initialize(window, config)) {
        std::cerr << "Failed to initialize UI" << std::endl;
        return -1;
    }

    // Initialize ECS
    initializeECS();
    createScene();

    // Log startup
    UI_LOG_INFO("Demo started successfully", "Main");
    UI_LOG_INFO("Press 1-4 to toggle UI windows", "UI");
    UI_LOG_INFO("Press 5 to reset camera", "Input");
    UI_LOG_INFO("Press ESC to exit", "Input");

    std::cout << "\n========================================" << std::endl;
    std::cout << "  Controls:" << std::endl;
    std::cout << "  Mouse: Look around" << std::endl;
    std::cout << "  WASD: Move camera" << std::endl;
    std::cout << "  1-4: Toggle UI windows" << std::endl;
    std::cout << "  5: Reset camera" << std::endl;
    std::cout << "  ESC: Exit" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Main loop
    float lastTime = 0.0f;
    int frameCount = 0;
    float fpsTimer = 0.0f;

    while (!glfwWindowShouldClose(window)) {
        // Calculate delta time
        float currentTime = static_cast<float>(glfwGetTime());
        float deltaTime = currentTime - lastTime;
        lastTime = currentTime;

        // FPS counter
        frameCount++;
        fpsTimer += deltaTime;
        if (fpsTimer >= 1.0f) {
            std::cout << "FPS: " << frameCount << " | Entities: " 
                      << g_world.getEntityCount() << std::endl;
            frameCount = 0;
            fpsTimer = 0.0f;
        }

        // Process input
        processInput(window, deltaTime);

        // Mouse look
        static double lastX = 0, lastY = 0;
        double xpos, ypos;
        glfwGetCursorPos(window, &xpos, &ypos);

        float xoffset = static_cast<float>(xpos - lastX);
        float yoffset = static_cast<float>(lastY - ypos);
        lastX = xpos;
        lastY = ypos;

        g_camera->ProcessMouseMovement(xoffset, yoffset);

        // Clear screen
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Begin GPU profiling
        BEGIN_GPU_FRAME();

        // Update ECS world (profiled)
        {
            PROFILE_GPU_SCOPE("ECS Update");
            {
                PROFILE_GPU_SCOPE("Physics");
                g_world.update(deltaTime);
            }
        }

        // Update camera system
        auto* cameraSystem = g_world.getSystem<ecs::CameraSystem>();
        glm::mat4 view, projection;
        
        if (cameraSystem) {
            PROFILE_GPU_SCOPE("Camera Update");
            view = cameraSystem->getViewMatrix();
            projection = cameraSystem->getProjectionMatrix();

            auto* renderSystem = g_world.getSystem<ecs::RenderSystem>();
            if (renderSystem) {
                renderSystem->setViewProjection(view, projection);
            }
        } else {
            // Fallback camera matrices
            view = glm::lookAt(
                g_camera->Position,
                g_camera->Target,
                glm::vec3(0.0f, 1.0f, 0.0f)
            );
            projection = glm::perspective(glm::radians(60.0f), 1280.0f/720.0f, 0.1f, 1000.0f);
        }

        // Render debug visualization BEFORE ImGui
        {
            PROFILE_GPU_SCOPE("Debug Render");
            g_debugRenderer.beginFrame(view, projection);
            
            // Draw grid floor
            DEBUG_DRAW_GRID(glm::vec3(0.0f, 0.0f, 0.0f), 50.0f, 50, glm::vec3(0.3f, 0.3f, 0.3f));
            
            // Draw axes at origin
            DEBUG_DRAW_AXES(2.0f);
            
            // Draw boxes for each entity
            for (const auto& entity : g_boxEntities) {
                auto* transform = g_world.getComponent<ecs::TransformComponent>(entity);
                if (transform) {
                    DEBUG_DRAW_BOX(transform->position, transform->scale, glm::vec3(1.0f, 0.5f, 0.2f));
                }
            }
            
            g_debugRenderer.endFrame();
        }

        // Render ECS world (profiled)
        {
            PROFILE_GPU_SCOPE("ECS Render");
            g_world.render();
        }

        // Render UI (profiled) - AFTER scene rendering
        {
            PROFILE_GPU_SCOPE("UI Render");
            EngineUI::getInstance().render();
        }

        // End GPU profiling
        END_GPU_FRAME();

        // Swap buffers and poll events
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    std::cout << "\n=== Cleaning up ===" << std::endl;

    // Print final GPU stats
    AdvancedGPUProfiler::getInstance().printResults();

    g_world.shutdown();
    EngineUI::getInstance().shutdown();
    AdvancedGPUProfiler::getInstance().shutdown();
    g_debugRenderer.shutdown();

    // Cleanup renderer resources
    delete g_shader;
    delete g_model;
    delete g_renderer;
    delete g_camera;

    glfwDestroyWindow(window);
    glfwTerminate();

    std::cout << "Cleanup complete" << std::endl;
    return 0;
}
