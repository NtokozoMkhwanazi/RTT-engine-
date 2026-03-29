/**
 * World System Test Application
 * 
 * Focused real-time testing for terrain, chunks, vegetation, and world objects.
 * Use this for debugging world generation issues and memory leaks in isolation.
 * 
 * Build: make world_test
 * Run:   ./bin/world_test
 */

#include "shaderSystem/Shader.h"
#include "cameraSystem/flyCamera.h"
#include "world/Terrain.h"
#include "world/TerrainChunk.h"
#include "world/VegetationSystem.h"
#include "world/WorldObjectManager.h"
#include "world/WorldImporter.h"
#include "shaderSystem/stb_image.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/noise.hpp>

#include <iostream>
#include <vector>
#include <random>

// ================= CAMERA =================
flyCamera camera(
    glm::vec3(0.0f, 50.0f, 100.0f),
    glm::vec3(0, 0, 0),
    -90.0f,
    -20.0f,
    100.0f
);

// ================= WORLD SYSTEMS =================
Terrain* terrain = nullptr;
VegetationSystem* vegetation = nullptr;
WorldObjectManager* worldObjects = nullptr;

// ================= TEST CONFIGURATION =================
struct WorldTestConfig {
    int viewDistance = 8;
    int lodDistance = 50;
    float chunkSize = 100.0f;
    bool showWireframe = false;
    bool showVegetation = true;
    bool showWorldObjects = true;
    bool generateNew = false;
} worldConfig;

// ================= CALLBACKS =================
void framebuffer_size_callback(GLFWwindow *, int w, int h) { 
    glViewport(0, 0, w, h); 
}

void mouse_callback(GLFWwindow *window, double xpos, double ypos) {
    static double lastX = xpos, lastY = ypos;
    static bool firstMouse = true;

    if (firstMouse) {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
    }

    float dx = (float)(xpos - lastX);
    float dy = (float)(lastY - ypos);
    lastX = xpos;
    lastY = ypos;

    camera.Yaw += dx * 0.1f;
    camera.Pitch = glm::clamp(camera.Pitch + dy * 0.1f, -80.0f, 80.0f);
}

void scroll_callback(GLFWwindow *window, double xoffset, double yoffset) {
    camera.DistanceToTarget -= (float)yoffset * 2.0f;
    camera.DistanceToTarget = glm::clamp(camera.DistanceToTarget, 20.0f, 500.0f);
}

// ================= WORLD HELPERS =================
void setupTerrain() {
    TerrainConfig config;
    config.chunkSize = worldConfig.chunkSize;
    config.viewDistance = worldConfig.viewDistance;
    config.lodDistance = (float)worldConfig.lodDistance;
    config.heightScale = 80.0f;
    config.waterLevel = 5.0f;
    
    terrain = new Terrain(config);
    
    std::cout << "Terrain initialized:\n";
    std::cout << "  Chunk size: " << config.chunkSize << "m\n";
    std::cout << "  View distance: " << config.viewDistance << " chunks\n";
    std::cout << "  LOD distance: " << config.lodDistance << "m\n";
    std::cout << "  Height scale: " << config.heightScale << "m\n";
}

void setupVegetation() {
    if (!terrain) return;
    
    VegetationConfig config;
    config.treeDensity = 0.02f;
    config.maxTreesPerChunk = 80;
    config.grassDensity = 0.1f;
    config.showVegetation = worldConfig.showVegetation;
    
    vegetation = new VegetationSystem(config);
    
    std::cout << "\nVegetation initialized:\n";
    std::cout << "  Tree density: " << config.treeDensity << "\n";
    std::cout << "  Max trees/chunk: " << config.maxTreesPerChunk << "\n";
}

void setupWorldObjects() {
    worldObjects = new WorldObjectManager();
    
    // Load world object models
    std::cout << "\nLoading world objects...\n";
    
    // Try to load rock models
    if (worldObjects->loadModel("Rock1", "assets/World_objects/Rock1.fbx")) {
        std::cout << "  Loaded: Rock1\n";
    }
    if (worldObjects->loadModel("Rock2", "assets/World_objects/Rock2.fbx")) {
        std::cout << "  Loaded: Rock2\n";
    }
    if (worldObjects->loadModel("Stone", "assets/World_objects/stone.fbx")) {
        std::cout << "  Loaded: Stone\n";
    }
    
    std::cout << "World objects ready.\n";
}

void regenerateWorld() {
    std::cout << "\nRegenerating world...\n";
    
    delete terrain;
    delete vegetation;
    
    setupTerrain();
    setupVegetation();
    
    std::cout << "World regenerated!\n";
}

// ================= RENDERING =================
Shader* terrainShader = nullptr;
Shader* vegetationShader = nullptr;

void setupShaders() {
    // Terrain shader
    const char* terrainVS = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aTex;
out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoords;
out float Height;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
void main() {
    FragPos = vec3(model * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(model))) * aNormal;
    TexCoords = aTex;
    Height = aPos.y;
    gl_Position = projection * view * vec4(FragPos, 1.0);
}
)";
    
    const char* terrainFS = R"(
#version 330 core
in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoords;
in float Height;
out vec4 FragColor;
uniform vec3 viewPos;
uniform float WaterLevel;
void main() {
    // Height-based coloring
    vec3 color;
    if (Height < WaterLevel) {
        color = vec3(0.76, 0.7, 0.5);  // Sand
    } else if (Height < 30.0) {
        color = vec3(0.1, 0.5, 0.1);   // Grass
    } else if (Height < 60.0) {
        color = vec3(0.4, 0.35, 0.3);  // Rock
    } else {
        color = vec3(0.95, 0.95, 0.95); // Snow
    }
    
    // Simple lighting
    vec3 lightDir = normalize(vec3(50.0, 100.0, 50.0));
    float diff = max(dot(Normal, lightDir), 0.0);
    vec3 ambient = 0.3 * color;
    vec3 diffuse = diff * color;
    
    // Fog
    float dist = length(FragPos - viewPos);
    float fogFactor = exp(-0.005 * dist);
    fogFactor = clamp(fogFactor, 0.0, 1.0);
    
    vec3 fogColor = vec3(0.7, 0.75, 0.8);
    vec3 finalColor = mix(fogColor, ambient + diffuse, fogFactor);
    
    FragColor = vec4(finalColor, 1.0);
}
)";
    
    terrainShader = new Shader();
    terrainShader->CompileFromSource(terrainVS, terrainFS);
    
    // Vegetation shader (simple instanced)
    const char* vegVS = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aTex;
layout(location=3) in mat4 aInstanceMatrix;
out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoords;
uniform mat4 view;
uniform mat4 projection;
void main() {
    FragPos = vec3(aInstanceMatrix * vec4(aPos, 1.0));
    Normal = mat3(aInstanceMatrix) * aNormal;
    TexCoords = aTex;
    gl_Position = projection * view * vec4(FragPos, 1.0);
}
)";
    
    const char* vegFS = R"(
#version 330 core
in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoords;
out vec4 FragColor;
void main() {
    vec3 color = vec3(0.1, 0.4, 0.1);  // Green
    vec3 lightDir = normalize(vec3(50.0, 100.0, 50.0));
    float diff = max(dot(Normal, lightDir), 0.0);
    FragColor = vec4(0.3 * color + diff * color, 1.0);
}
)";
    
    vegetationShader = new Shader();
    vegetationShader->CompileFromSource(vegVS, vegFS);
}

void renderTerrain() {
    if (!terrain || !terrainShader) return;
    
    terrainShader->use();
    terrainShader->setMat4("view", camera.GetViewMatrix());
    terrainShader->setMat4("projection", camera.GetProjectionMatrix(1280.0f / 720.0f));
    terrainShader->setVec3("viewPos", camera.Position);
    terrainShader->setFloat("WaterLevel", 5.0f);
    
    if (worldConfig.showWireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    } else {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    
    terrain->Render(camera.Position);
    
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

void renderVegetation() {
    if (!vegetation || !vegetationShader || !worldConfig.showVegetation) return;
    
    vegetationShader->use();
    vegetationShader->setMat4("view", camera.GetViewMatrix());
    vegetationShader->setMat4("projection", camera.GetProjectionMatrix(1280.0f / 720.0f));
    
    vegetation->Render(camera.Position);
}

void renderWorldObjects() {
    if (!worldObjects || !worldConfig.showWorldObjects) return;
    
    worldObjects->Render(camera.GetViewMatrix(), camera.GetProjectionMatrix(1280.0f / 720.0f));
}

void renderDebug() {
    if (!terrain) return;
    
    // Render chunk boundaries
    glDisable(GL_DEPTH_TEST);
    glBegin(GL_LINES);
    glColor3f(1.0f, 1.0f, 0.0f);
    
    auto chunks = terrain->GetActiveChunks();
    for (const auto& chunk : chunks) {
        float x = chunk.first.x * worldConfig.chunkSize;
        float z = chunk.first.y * worldConfig.chunkSize;
        float size = worldConfig.chunkSize;
        
        // Simple box outline at chunk position
        glVertex3f(x, 0, z);
        glVertex3f(x + size, 0, z);
        glVertex3f(x + size, 0, z);
        glVertex3f(x + size, 0, z + size);
        glVertex3f(x + size, 0, z + size);
        glVertex3f(x, 0, z + size);
        glVertex3f(x, 0, z + size);
        glVertex3f(x, 0, z);
    }
    
    glEnd();
    glEnable(GL_DEPTH_TEST);
}

// ================= UPDATE =================
void updateWorld(float dt) {
    if (!terrain) return;
    
    // Update active chunks based on camera position
    terrain->Update(camera.Position);
    
    // Update vegetation
    if (vegetation) {
        vegetation->Update(camera.Position);
    }
    
    // Update world objects
    if (worldObjects) {
        worldObjects->Update(camera.Position);
    }
}

// ================= MAIN =================
int main() {
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  WORLD SYSTEM TEST\n";
    std::cout << "========================================\n";
    std::cout << "\nControls:\n";
    std::cout << "  Mouse - Orbit camera\n";
    std::cout << "  Scroll - Zoom in/out\n";
    std::cout << "  W/S - Move forward/backward\n";
    std::cout << "  A/D - Strafe left/right\n";
    std::cout << "  Q/E - Move up/down\n";
    std::cout << "  F - Toggle wireframe\n";
    std::cout << "  V - Toggle vegetation\n";
    std::cout << "  O - Toggle world objects\n";
    std::cout << "  G - Regenerate world\n";
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

    GLFWwindow* window = glfwCreateWindow(1280, 720, "World Test", nullptr, nullptr);
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

    // Setup world systems
    setupShaders();
    setupTerrain();
    setupVegetation();
    setupWorldObjects();

    std::cout << "\nWorld test ready!\n\n";

    // Main loop
    float dt = 0.016f;
    float moveSpeed = 50.0f;
    
    while (!glfwWindowShouldClose(window)) {
        // Input
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);
        
        // Camera movement
        glm::vec3 forward = glm::normalize(glm::vec3(
            sin(glm::radians(camera.Yaw)),
            0,
            cos(glm::radians(camera.Yaw))
        ));
        glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
        
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
            camera.Position += forward * moveSpeed * dt;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
            camera.Position -= forward * moveSpeed * dt;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
            camera.Position -= right * moveSpeed * dt;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
            camera.Position += right * moveSpeed * dt;
        if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
            camera.Position.y += moveSpeed * dt;
        if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
            camera.Position.y -= moveSpeed * dt;
        
        // Reset camera
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) {
            camera.Position = glm::vec3(0.0f, 50.0f, 100.0f);
            camera.Yaw = -90.0f;
            camera.Pitch = -20.0f;
        }
        
        // Toggle wireframe
        static bool lastF = false;
        if (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS && !lastF) {
            worldConfig.showWireframe = !worldConfig.showWireframe;
            std::cout << "Wireframe: " << (worldConfig.showWireframe ? "ON" : "OFF") << "\n";
        }
        lastF = glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS;
        
        // Toggle vegetation
        static bool lastV = false;
        if (glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS && !lastV) {
            worldConfig.showVegetation = !worldConfig.showVegetation;
            std::cout << "Vegetation: " << (worldConfig.showVegetation ? "ON" : "OFF") << "\n";
        }
        lastV = glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS;
        
        // Toggle world objects
        static bool lastO = false;
        if (glfwGetKey(window, GLFW_KEY_O) == GLFW_PRESS && !lastO) {
            worldConfig.showWorldObjects = !worldConfig.showWorldObjects;
            std::cout << "World objects: " << (worldConfig.showWorldObjects ? "ON" : "OFF") << "\n";
        }
        lastO = glfwGetKey(window, GLFW_KEY_O) == GLFW_PRESS;
        
        // Regenerate world
        static bool lastG = false;
        if (glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS && !lastG) {
            regenerateWorld();
        }
        lastG = glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS;

        // Update
        updateWorld(dt);

        // Render
        glClearColor(0.7f, 0.75f, 0.8f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        renderTerrain();
        renderVegetation();
        renderWorldObjects();
        renderDebug();

        // Print stats
        static float timer = 0.0f;
        static int frameCount = 0;
        frameCount++;
        timer += dt;
        if (timer >= 1.0f) {
            auto chunks = terrain->GetActiveChunks();
            std::cout << "FPS: " << frameCount 
                      << ", Chunks: " << chunks.size()
                      << ", Camera: (" << camera.Position.x << ", " 
                      << camera.Position.y << ", " << camera.Position.z << ")\n";
            timer = 0.0f;
            frameCount = 0;
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    delete terrain;
    delete vegetation;
    delete worldObjects;
    delete terrainShader;
    delete vegetationShader;
    
    glfwDestroyWindow(window);
    glfwTerminate();
    
    std::cout << "\nWorld test exited cleanly.\n";
    return 0;
}
