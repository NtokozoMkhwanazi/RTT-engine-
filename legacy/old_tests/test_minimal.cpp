/**
 * Integration Test Application
 * 
 * Unified test that ties all systems together for end-to-end testing.
 * Use this after individual system tests pass to verify everything works together.
 * 
 * This is a MINIMAL version - focused on integration, not feature testing.
 * For feature-specific testing, use the dedicated test applications:
 *   - physics_test: Physics and collision
 *   - animation_test: Animation and motion matching
 *   - world_test: Terrain and world generation
 * 
 * Build: make
 * Run:   ./bin/run
 */

#include "shaderSystem/Shader.h"
#include "modelSystem/Model.h"
#include "cameraSystem/flyCamera.h"
#include "animationSystem/Animator.h"
#include "animationSystem/AnimationStateMachine.h"
#include "animationSystem/HybridMMFSM.h"
#include "physicsSystem/RigidBody.h"
#include "physicsSystem/Floor.h"
#include "world/Terrain.h"
#include "world/VegetationSystem.h"
#include "shaderSystem/stb_image.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>
#include <memory>

// ================= GLOBAL SYSTEMS =================
flyCamera camera(glm::vec3(0.0f, 5.0f, 15.0f), glm::vec3(0, 2, 0), -90.0f, 0.0f, 10.0f);
ModelManager modelManager;
Animator* animator = nullptr;
HybridMMFSM* hybrid = nullptr;
Terrain* terrain = nullptr;
VegetationSystem* vegetation = nullptr;

// ================= CONFIGURATION =================
struct IntegrationConfig {
    bool enablePhysics = true;
    bool enableAnimation = true;
    bool enableTerrain = true;
    bool enableVegetation = true;
    float dt = 0.016f;
} config;

// ================= CALLBACKS =================
void framebuffer_size_callback(GLFWwindow *, int w, int h) { 
    glViewport(0, 0, w, h); 
}

void mouse_callback(GLFWwindow *window, double xpos, double ypos) {
    static double lastX = xpos, lastY = ypos;
    static bool firstMouse = true;
    if (firstMouse) { lastX = xpos; lastY = ypos; firstMouse = false; }
    float dx = (float)(xpos - lastX);
    float dy = (float)(lastY - ypos);
    lastX = xpos;
    lastY = ypos;
    camera.Yaw += dx * 0.1f;
    camera.Pitch = glm::clamp(camera.Pitch + dy * 0.1f, -80.0f, 80.0f);
}

void scroll_callback(GLFWwindow *, double, double yoffset) {
    camera.DistanceToTarget -= (float)yoffset * 0.5f;
    camera.DistanceToTarget = glm::clamp(camera.DistanceToTarget, 5.0f, 50.0f);
}

// ================= INITIALIZATION =================
bool initializeSystems() {
    std::cout << "\n=== Initializing Integration Test ===\n\n";
    
    // Load character model
    std::cout << "Loading character model...\n";
    modelManager.load("Bot", "assets/bot.fbx");
    Model* character = modelManager.get("Bot");
    if (!character) {
        std::cerr << "ERROR: Failed to load character!\n";
        return false;
    }
    std::cout << "  Character loaded: " << character->GetMeshCount() << " meshes\n";
    
    // Setup animation
    if (config.enableAnimation) {
        std::cout << "\nInitializing animation system...\n";
        const Skeleton& skeleton = character->GetSkeleton();
        animator = new Animator();
        animator->Initialize(&skeleton);
        
        hybrid = new HybridMMFSM();
        hybrid->Initialize(&skeleton, animator);
        std::cout << "  Animation system ready\n";
    }
    
    // Setup terrain
    if (config.enableTerrain) {
        std::cout << "\nInitializing terrain...\n";
        TerrainConfig tConfig;
        tConfig.chunkSize = 100.0f;
        tConfig.viewDistance = 8;
        tConfig.lodDistance = 50.0f;
        tConfig.heightScale = 80.0f;
        terrain = new Terrain(tConfig);
        std::cout << "  Terrain ready\n";
    }
    
    // Setup vegetation
    if (config.enableVegetation && terrain) {
        std::cout << "\nInitializing vegetation...\n";
        VegetationConfig vConfig;
        vConfig.treeDensity = 0.02f;
        vConfig.maxTreesPerChunk = 80;
        vegetation = new VegetationSystem(vConfig);
        std::cout << "  Vegetation ready\n";
    }
    
    std::cout << "\n=== Integration Test Ready ===\n\n";
    return true;
}

// ================= UPDATE =================
void updateSystems() {
    float dt = config.dt;
    
    // Update animation
    if (config.enableAnimation && hybrid && animator) {
        CharacterInput input;
        if (glfwGetKey(nullptr, GLFW_KEY_W) == GLFW_PRESS) {
            input.moveDirection = glm::vec2(0.0f, 1.0f);
            input.moveMagnitude = 1.0f;
        } else if (glfwGetKey(nullptr, GLFW_KEY_S) == GLFW_PRESS) {
            input.moveDirection = glm::vec2(0.0f, -1.0f);
            input.moveMagnitude = 1.0f;
        } else if (glfwGetKey(nullptr, GLFW_KEY_A) == GLFW_PRESS) {
            input.moveDirection = glm::vec2(-1.0f, 0.0f);
            input.moveMagnitude = 1.0f;
        } else if (glfwGetKey(nullptr, GLFW_KEY_D) == GLFW_PRESS) {
            input.moveDirection = glm::vec2(1.0f, 0.0f);
            input.moveMagnitude = 1.0f;
        }
        input.grounded = true;
        
        HybridMMFSMState state;
        state.moveDirection = input.moveDirection;
        state.moveMagnitude = input.moveMagnitude;
        state.velocity = glm::vec3(0.0f, 0.0f, input.moveMagnitude * 3.0f);
        state.grounded = input.grounded;
        
        hybrid->Update(dt, state);
        animator->Update(dt);
    }
    
    // Update terrain
    if (config.enableTerrain && terrain) {
        terrain->Update(camera.Position);
    }
    
    // Update vegetation
    if (config.enableVegetation && vegetation) {
        vegetation->Update(camera.Position);
    }
}

// ================= RENDERING =================
Shader* modelShader = nullptr;

void renderSystems() {
    Model* character = modelManager.get("Bot");
    
    // Render character
    if (character && modelShader) {
        modelShader->use();
        modelShader->setMat4("model", glm::mat4(1.0f));
        modelShader->setMat4("view", camera.GetViewMatrix());
        modelShader->setMat4("projection", camera.GetProjectionMatrix(1280.0f / 720.0f));
        modelShader->setVec3("lightPos", glm::vec3(5.0f, 10.0f, 5.0f));
        modelShader->setVec3("viewPos", camera.Position);
        character->Draw(*modelShader);
    }
    
    // Render terrain
    if (config.enableTerrain && terrain) {
        // Terrain rendering would go here
        // Simplified for minimal integration test
    }
    
    // Render vegetation
    if (config.enableVegetation && vegetation) {
        vegetation->Render(camera.Position);
    }
}

// ================= SHUTDOWN =================
void shutdownSystems() {
    std::cout << "\nShutting down systems...\n";
    
    delete animator;
    delete hybrid;
    delete terrain;
    delete vegetation;
    delete modelShader;
    
    std::cout << "  All systems shut down cleanly\n";
}

// ================= MAIN =================
int main() {
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  INTEGRATION TEST\n";
    std::cout << "========================================\n";
    std::cout << "\nControls:\n";
    std::cout << "  Mouse - Orbit camera\n";
    std::cout << "  Scroll - Zoom\n";
    std::cout << "  W/S/A/D - Move character\n";
    std::cout << "  1 - Toggle animation\n";
    std::cout << "  2 - Toggle terrain\n";
    std::cout << "  3 - Toggle vegetation\n";
    std::cout << "  R - Reset camera\n";
    std::cout << "  ESC - Exit\n";
    std::cout << "========================================\n\n";

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "GLFW init failed\n";
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Integration Test", nullptr, nullptr);
    if (!window) {
        std::cerr << "GLFW window creation failed\n";
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "GLAD failed\n";
        glfwTerminate();
        return -1;
    }

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    // Initialize systems
    if (!initializeSystems()) {
        glfwTerminate();
        return -1;
    }

    // Setup shader
    modelShader = new Shader("shaderSystem/shaderVS.glsl", "shaderSystem/shaderFS.glsl");

    std::cout << "Integration test running...\n\n";

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        // Input
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);
        
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) {
            camera.Position = glm::vec3(0.0f, 5.0f, 15.0f);
            camera.Yaw = -90.0f;
            camera.Pitch = 0.0f;
        }
        
        // Toggle systems
        static bool last1 = false, last2 = false, last3 = false;
        
        if (glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS && !last1) {
            config.enableAnimation = !config.enableAnimation;
            std::cout << "Animation: " << (config.enableAnimation ? "ON" : "OFF") << "\n";
        }
        last1 = glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS;
        
        if (glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS && !last2) {
            config.enableTerrain = !config.enableTerrain;
            std::cout << "Terrain: " << (config.enableTerrain ? "ON" : "OFF") << "\n";
        }
        last2 = glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS;
        
        if (glfwGetKey(window, GLFW_KEY_3) == GLFW_PRESS && !last3) {
            config.enableVegetation = !config.enableVegetation;
            std::cout << "Vegetation: " << (config.enableVegetation ? "ON" : "OFF") << "\n";
        }
        last3 = glfwGetKey(window, GLFW_KEY_3) == GLFW_PRESS;

        // Update
        updateSystems();

        // Render
        glClearColor(0.7f, 0.75f, 0.8f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderSystems();

        // FPS counter
        static float timer = 0.0f;
        static int frameCount = 0;
        frameCount++;
        timer += config.dt;
        if (timer >= 1.0f) {
            std::cout << "FPS: " << frameCount << "\n";
            timer = 0.0f;
            frameCount = 0;
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    shutdownSystems();
    glfwDestroyWindow(window);
    glfwTerminate();
    
    std::cout << "\nIntegration test exited cleanly.\n";
    return 0;
}
