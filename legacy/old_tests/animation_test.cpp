/**
 * ============================================================================
 * ANIMATION TEST - Full ECS Animation System
 * ============================================================================
 *
 * This test demonstrates the fully integrated ECS animation system:
 * - Loads bot character from assets/bot.fbx
 * - Loads animations (Idle, Walk, Run, Jump, etc.)
 * - Creates animated entity with AnimatorComponent + SkeletonComponent
 * - Plays animations with blending
 * - Shows real-time animation state
 *
 * Controls:
 * - 1: Play Idle animation
 * - 2: Play Walk animation
 * - 3: Play Run animation
 * - 4: Play Jump animation
 * - 5: Blend Walk -> Run
 * - Space: Pause/Resume
 * - R: Reset to Idle
 * - ESC: Exit
 * ============================================================================
 */

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "ecs/systems/Systems.h"

#include "animationSystem/Animation.h"
#include "animationSystem/Animator.h"
#include "animationSystem/AssimpAnimationLoader.h"
#include "boneSystem/Skeleton.h"
#include "boneSystem/BoneName.h"
#include "modelSystem/Model.h"
#include "cameraSystem/flyCamera.h"
#include "utils/FBXLoader.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <iostream>
#include <map>
#include <string>

// ============================================================================
// Global State
// ============================================================================
static ecs::World g_world;
static flyCamera* g_camera = nullptr;

// Animation entities
static ecs::Entity g_botEntity;
static std::map<std::string, Animation*> g_animations;
static int g_currentAnimIndex = 0;
static bool g_isPaused = false;

// Engine animator (for loading animations)
static Animator* g_engineAnimator = nullptr;
static Skeleton* g_engineSkeleton = nullptr;

// ============================================================================
// Animation Loading Helper
// ============================================================================
Animation* LoadAnimationFromFile(const std::string& path) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate | aiProcess_FlipUVs);

    if (!scene || !scene->HasAnimations()) {
        return nullptr;
    }

    // Load animation with pose correction
    Animation animTemp = AssimpAnimationLoader::LoadAnimationWithPoseCorrection(
        scene, scene->mAnimations[0]);

    // Copy to heap for storage
    return new Animation(animTemp);
}

// ============================================================================
// Animation Loading
// ============================================================================
bool loadCharacterAndAnimations() {
    std::cout << "\n=== Loading Character and Animations ===" << std::endl;

    // Load bot model
    std::cout << "Loading bot.fbx..." << std::endl;
    ModelLoadResult botResult = LoadFBXModel("assets/bot.fbx", "Bot");
    Model* botModel = botResult.model;
    
    if (!botModel) {
        std::cerr << "Failed to load bot.fbx" << std::endl;
        return false;
    }

    // Get skeleton from model (const cast for our purposes)
    const Skeleton* constSkel = &botModel->GetSkeleton();
    g_engineSkeleton = const_cast<Skeleton*>(constSkel);
    
    if (!g_engineSkeleton || g_engineSkeleton->bones.empty()) {
        std::cerr << "No skeleton found in bot.fbx" << std::endl;
        return false;
    }
    std::cout << "  Skeleton loaded: " << g_engineSkeleton->bones.size() << " bones" << std::endl;

    // Create engine animator for loading animations
    g_engineAnimator = new Animator(g_engineSkeleton);

    // Load animations
    std::vector<std::pair<std::string, std::string>> animFiles = {
        {"Idle", "assets/Idle.fbx"},
        {"Walk", "assets/Walking.fbx"},
        {"Run", "assets/Run.fbx"},
        {"Jump", "assets/Jump.fbx"},
        {"Fall", "assets/Fall.fbx"},
        {"Crouch", "assets/Crouching.fbx"},
    };

    for (const auto& [name, file] : animFiles) {
        std::cout << "Loading " << name << " from " << file << "..." << std::endl;
        
        Animation* anim = LoadAnimationFromFile(file);
        if (anim) {
            g_animations[name] = anim;
            std::cout << "  Loaded: " << anim->GetDuration() << "s duration" << std::endl;
        } else {
            std::cout << "  Failed to load" << std::endl;
        }
    }

    std::cout << "\nLoaded " << g_animations.size() << " animations" << std::endl;
    return !g_animations.empty();
}

// ============================================================================
// Create Animated Entity
// ============================================================================
ecs::Entity createAnimatedCharacter(const glm::vec3& position) {
    std::cout << "\n=== Creating Animated Character ===" << std::endl;

    auto entity = g_world.createEntity();

    // Add transform
    auto& transform = g_world.addComponent<ecs::TransformComponent>(entity);
    transform.position = position;
    transform.scale = glm::vec3(1.0f);

    // Add skeleton component - use engine skeleton directly via AnimatorComponent
    // The AnimationSystem will sample animations and update bone matrices
    auto& skeleton = g_world.addComponent<ecs::SkeletonComponent>(entity);
    
    // For now, create a simple bone hierarchy (root + hips + legs)
    // In production, this would be populated from the model's skeleton
    if (g_engineSkeleton && !g_engineSkeleton->bones.empty()) {
        // Copy bone count and create basic structure
        size_t boneCount = g_engineSkeleton->bones.size();
        skeleton.bones.resize(boneCount);
        skeleton.rootBoneIndex = g_engineSkeleton->rootBoneIndex;
        
        // Initialize bones with identity transforms
        // The AnimationSystem will populate these from animation data
        for (size_t i = 0; i < boneCount; i++) {
            skeleton.bones[i].name = "bone_" + std::to_string(i);
            skeleton.bones[i].parentIndex = -1;  // Would need parent info from engine
            skeleton.bones[i].inverseBindMatrix = glm::mat4(1.0f);
            skeleton.bones[i].localTransform = glm::mat4(1.0f);
            skeleton.bones[i].worldTransform = glm::mat4(1.0f);
        }
        
        std::cout << "  Skeleton: " << skeleton.bones.size() << " bones" << std::endl;
    }

    // Add animator component
    auto& animator = g_world.addComponent<ecs::AnimatorComponent>(entity);
    
    // Add animations to animator
    for (const auto& [name, anim] : g_animations) {
        animator.addAnimation(anim);
        std::cout << "  Added animation: " << name << std::endl;
    }

    // Add name
    g_world.addComponent<ecs::NameComponent>(entity, "Bot");

    return entity;
}

// ============================================================================
// Initialize ECS World
// ============================================================================
void initializeECS() {
    std::cout << "\n=== Initializing ECS World ===" << std::endl;

    // Initialize the world
    g_world.init();

    // Add integrated ECS systems
    g_world.addSystem<ecs::RenderSystem>();

    auto& cameraSystem = g_world.addSystem<ecs::CameraSystem>();
    cameraSystem.setPriority(-100);

    auto& animationSystem = g_world.addSystem<ecs::AnimationSystem>();
    animationSystem.setPriority(-50);

    g_world.addSystem<ecs::LightSystem>();

    std::cout << "  Added " << g_world.getSystemCount() << " integrated ECS systems" << std::endl;
}

// ============================================================================
// Create Camera Entity
// ============================================================================
ecs::Entity createCamera() {
    auto entity = g_world.createEntity();

    auto& transform = g_world.addComponent<ecs::TransformComponent>(entity);
    transform.position = glm::vec3(0.0f, 2.0f, 5.0f);

    auto& camera = g_world.addComponent<ecs::CameraComponent>(entity);
    camera.fov = 45.0f;
    camera.nearPlane = 0.1f;
    camera.farPlane = 1000.0f;
    camera.isActive = true;

    g_world.addComponent<ecs::NameComponent>(entity, "MainCamera");

    return entity;
}

// ============================================================================
// Create Directional Light
// ============================================================================
ecs::Entity createDirectionalLight() {
    auto entity = g_world.createEntity();

    auto& transform = g_world.addComponent<ecs::TransformComponent>(entity);
    transform.setEulerAngles(glm::vec3(glm::radians(45.0f), glm::radians(45.0f), 0.0f));

    auto& light = g_world.addComponent<ecs::LightComponent>(entity);
    light.setDirectional(glm::vec3(1.0f), 1.0f);
    light.castShadows = false;

    g_world.addComponent<ecs::NameComponent>(entity, "Sun");

    return entity;
}

// ============================================================================
// Get animation index by name
// ============================================================================
int getAnimationIndexByName(ecs::AnimatorComponent* animator, const std::string& name) {
    if (!animator) return -1;
    
    for (size_t i = 0; i < animator->animations.size(); i++) {
        // Compare pointers since we stored them in g_animations
        auto it = g_animations.find(name);
        if (it != g_animations.end() && animator->animations[i] == it->second) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ============================================================================
// Process Input
// ============================================================================
void processInput(GLFWwindow* window, float deltaTime) {
    // Animation selection
    static bool lastKeyState[10] = {false};
    
    const int keyMap[] = {GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3, GLFW_KEY_4, GLFW_KEY_5};
    const std::string animNames[] = {"Idle", "Walk", "Run", "Jump", "Crouch"};

    for (int i = 0; i < 5; i++) {
        bool keyState = glfwGetKey(window, keyMap[i]) == GLFW_PRESS;
        if (keyState && !lastKeyState[i]) {
            std::cout << "\nPlaying: " << animNames[i] << std::endl;
            
            auto* animator = g_world.getComponent<ecs::AnimatorComponent>(g_botEntity);
            if (animator) {
                int targetIndex = getAnimationIndexByName(animator, animNames[i]);
                if (targetIndex >= 0) {
                    animator->play(targetIndex, true);
                    g_currentAnimIndex = targetIndex;
                }
            }
        }
        lastKeyState[i] = keyState;
    }

    // Pause/Resume
    static bool lastSpaceState = false;
    bool spaceState = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
    if (spaceState && !lastSpaceState) {
        g_isPaused = !g_isPaused;
        auto* animator = g_world.getComponent<ecs::AnimatorComponent>(g_botEntity);
        if (animator) {
            if (g_isPaused) {
                animator->pause();
                std::cout << "\nAnimation Paused" << std::endl;
            } else {
                animator->resume();
                std::cout << "\nAnimation Resumed" << std::endl;
            }
        }
    }
    lastSpaceState = spaceState;

    // Blend Walk -> Run
    static bool lastBState = false;
    bool bState = glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS;
    if (bState && !lastBState) {
        std::cout << "\nBlending Walk -> Run" << std::endl;
        
        auto* animator = g_world.getComponent<ecs::AnimatorComponent>(g_botEntity);
        if (animator) {
            int walkIndex = getAnimationIndexByName(animator, "Walk");
            int runIndex = getAnimationIndexByName(animator, "Run");
            
            if (walkIndex >= 0 && runIndex >= 0) {
                animator->previousAnimation = animator->currentAnimation;
                animator->currentAnimation = runIndex;
                animator->isBlending = true;
                animator->blendTime = 0.0f;
                animator->blendDuration = 0.3f;
            }
        }
    }
    lastBState = bState;

    // Reset to Idle
    static bool lastRState = false;
    bool rState = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
    if (rState && !lastRState) {
        std::cout << "\nReset to Idle" << std::endl;
        
        auto* animator = g_world.getComponent<ecs::AnimatorComponent>(g_botEntity);
        if (animator) {
            int idleIndex = getAnimationIndexByName(animator, "Idle");
            if (idleIndex >= 0) {
                animator->play(idleIndex, true);
            }
        }
    }
    lastRState = rState;

    // Exit
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, true);
    }
}

// ============================================================================
// Print Animation State
// ============================================================================
void printAnimationState() {
    static float lastPrintTime = 0.0f;
    float currentTime = glfwGetTime();
    
    if (currentTime - lastPrintTime >= 0.5f) {
        auto* animator = g_world.getComponent<ecs::AnimatorComponent>(g_botEntity);
        if (animator) {
            std::cout << "\r" << std::flush;
            std::cout << "Time: " << animator->currentTime << "s  |  ";
            std::cout << "Playing: " << (animator->isPlaying ? "Yes" : "No") << "  |  ";
            std::cout << "Blending: " << (animator->isBlending ? "Yes" : "No") << "  |  ";
            std::cout << "Frame: " << animator->getNormalizedTime() * 100 << "%" << std::flush;
        }
        lastPrintTime = currentTime;
    }
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  ECS Animation Test" << std::endl;
    std::cout << "========================================" << std::endl;

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "ECS Animation Test", nullptr, nullptr);
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

    // Initialize camera
    g_camera = new flyCamera(
        glm::vec3(0.0f, 2.0f, 5.0f),
        glm::vec3(0, 1.5f, 0),
        -90.0f,
        0.0f,
        10.0f
    );

    // Initialize ECS
    initializeECS();

    // Load character and animations
    if (!loadCharacterAndAnimations()) {
        std::cerr << "Failed to load character and animations" << std::endl;
        glfwTerminate();
        return -1;
    }

    // Create scene
    std::cout << "\n=== Creating Scene ===" << std::endl;
    createCamera();
    createDirectionalLight();
    g_botEntity = createAnimatedCharacter(glm::vec3(0.0f, 0.0f, 0.0f));

    // Play first animation (Idle)
    auto* initialAnimator = g_world.getComponent<ecs::AnimatorComponent>(g_botEntity);
    if (initialAnimator) {
        int idleIndex = getAnimationIndexByName(initialAnimator, "Idle");
        if (idleIndex >= 0) {
            initialAnimator->play(idleIndex, true);
        }
    }
    std::cout << "\nPlaying: Idle" << std::endl;

    std::cout << "\n========================================" << std::endl;
    std::cout << "  Controls:" << std::endl;
    std::cout << "  1-5: Play animations" << std::endl;
    std::cout << "  B:   Blend Walk -> Run" << std::endl;
    std::cout << "  Space: Pause/Resume" << std::endl;
    std::cout << "  R:   Reset to Idle" << std::endl;
    std::cout << "  Mouse: Look around" << std::endl;
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
            std::cout << "\nFPS: " << frameCount << std::endl;
            frameCount = 0;
            fpsTimer = 0.0f;
        }

        // Process input
        processInput(window, deltaTime);

        // Mouse look
        static double lastX = 0, lastY = 0;
        double xpos, ypos;
        glfwGetCursorPos(window, &xpos, &ypos);

        if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS ||
            glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
            float xoffset = static_cast<float>(xpos - lastX);
            float yoffset = static_cast<float>(lastY - ypos);
            lastX = xpos;
            lastY = ypos;

            g_camera->ProcessMouseMovement(xoffset, yoffset);
        } else {
            lastX = xpos;
            lastY = ypos;
        }

        // Clear screen
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Update ECS world (updates animation system)
        if (!g_isPaused) {
            g_world.update(deltaTime);
        }

        // Update camera system and get view/projection
        auto* cameraSystem = g_world.getSystem<ecs::CameraSystem>();
        if (cameraSystem) {
            glm::mat4 view = cameraSystem->getViewMatrix();
            glm::mat4 projection = cameraSystem->getProjectionMatrix();

            auto* renderSystem = g_world.getSystem<ecs::RenderSystem>();
            if (renderSystem) {
                renderSystem->setViewProjection(view, projection);
            }
        }

        // Render ECS world
        g_world.render();

        // Print animation state
        printAnimationState();

        // Swap buffers and poll events
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    std::cout << "\n\n=== Cleaning up ===" << std::endl;

    g_world.shutdown();
    
    // Cleanup animations
    for (auto& [name, anim] : g_animations) {
        delete anim;
    }
    g_animations.clear();
    
    delete g_engineAnimator;
    delete g_camera;

    glfwDestroyWindow(window);
    glfwTerminate();

    std::cout << "Cleanup complete" << std::endl;
    return 0;
}
