/**
 * ============================================================================
 * 3D GAME ENGINE - MAIN APPLICATION
 * ============================================================================
 * 
 * Features:
 * - Character with Hybrid MM+FSM animation system
 * - Open world terrain with LOD streaming
 * - Vegetation and world object placement
 * - Physics-based foot IK
 * - AAA-quality third-person camera
 * 
 * Controls:
 * - WASD: Move character
 * - Space: Jump
 * - Shift: Sprint
 * - Ctrl: Crouch
 * - Mouse: Look (orbit camera)
 * - Q/E: Camera pivot up/down
 * - R: Reset camera
 * - C: Toggle camera mode (orbit/fixed)
 * - F: Toggle wireframe
 * - B: Bone debug visualization
 * - G: Foot IK status
 * - H: Hybrid MM+FSM debug
 * - M: Cycle view models
 * - F6: Demo recording/playback
 * - ESC: Exit
 * ============================================================================
 */

#define GLM_ENABLE_EXPERIMENTAL

// ==================== ENGINE HEADERS ====================
#include "shaderSystem/Shader.h"
#include "shaderSystem/Skybox.h"
#include "modelSystem/Model.h"
#include "utils/FBXLoader.h"
#include "cameraSystem/flyCamera.h"
#include "animationSystem/Animator.h"
#include "animationSystem/AssimpAnimationLoader.h"
#include "animationSystem/AnimationStateMachine.h"
#include "animationSystem/AnimationLayerSystem.h"
#include "motionMatching/MotionMatcher.h"
#include "animationSystem/HybridMMFSM.h"
#include "shaderSystem/stb_image.h"
#include "physicsSystem/RigidBody.h"
#include "physicsSystem/Floor.h"
#include "physicsSystem/Physics.h"
#include "physicsSystem/AdvancedConstraints.h"
#include "world/Terrain.h"
#include "world/VegetationSystem.h"
#include "world/WorldObjectManager.h"
#include "demo/DemoRecorder.h"
#include "utils/ModelPlacer.h"
#include "renderer/DefaultTexture.h"

// ==================== SYSTEM HEADERS ====================
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <iostream>
#include <functional>
#include <vector>
#include <cmath>
#include <memory>
#include <random>
#include <algorithm>

// ============================================================================
// ENGINE STATE - Encapsulated global state
// ============================================================================
struct EngineState {
    // Core systems
    flyCamera* camera = nullptr;
    ModelManager* modelManager = nullptr;
    DemoRecorder* demoRecorder = nullptr;
    
    // Character systems
    Model* character = nullptr;
    Animator* animator = nullptr;
    HybridMMFSM* hybrid = nullptr;
    AnimationStateMachine* fsm = nullptr;
    
    // Physics systems
    PhysicsWorld* physics = nullptr;
    
    // World systems
    Terrain* terrain = nullptr;
    VegetationSystem* vegetation = nullptr;
    WorldObjectManager* worldObjects = nullptr;
    
    // Character state
    glm::vec3 characterPos{0.0f, 0.0f, 0.0f};
    glm::vec3 characterVelocity{0.0f};
    glm::vec3 cameraPivot{0.0f, 2.0f, 0.0f};
    float rotationAngle = 0.0f;
    bool isGrounded = false;
    
    // Camera settings
    float cameraDistance = 21.0f;
    float cameraHeight = 7.0f;
    float cameraZoomMin = 10.0f;
    float cameraZoomMax = 50.0f;
    bool cameraFollowEnabled = true;
    bool cameraFixedMode = false;
    
    // Debug state
    int debugMode = 0;  // 0=normal, 1=bone IDs, 2=weights
    bool wireframe = false;
    bool pausePhysics = false;
    
    // Water
    float waterLevel = 5.0f;
    
    // Model viewing
    int currentModelIndex = 0;
    std::vector<std::string> modelNames;
    
    // Timing
    float lastTime = 0.0f;
    int frameCount = 0;
    float fpsTimer = 0.0f;
};

// Global engine state (single instance)
static EngineState g_engine;

// ============================================================================
// CAMERA SYSTEM
// ============================================================================
namespace CameraSystem {

void Initialize(glm::vec3 startPos) {
    g_engine.camera = new flyCamera(
        startPos,
        glm::vec3(0, 2, 0),
        -90.0f,
        0.0f,
        10.0f
    );
}

void SetupCallbacks(GLFWwindow* window) {
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow*, int w, int h) {
        glViewport(0, 0, w, h);
    });
    
    glfwSetCursorPosCallback(window, [](GLFWwindow* window, double xpos, double ypos) {
        if (g_engine.cameraFixedMode) return;
        
        static double lastX = xpos, lastY = ypos;
        static bool firstMouse = true;
        
        if (firstMouse) {
            lastX = xpos;
            lastY = ypos;
            firstMouse = false;
        }
        
        float dx = static_cast<float>(xpos - lastX);
        float dy = static_cast<float>(lastY - ypos);
        lastX = xpos;
        lastY = ypos;
        
        g_engine.camera->Yaw += dx * 0.1f;
        g_engine.camera->Pitch = glm::clamp(
            g_engine.camera->Pitch + dy * 0.1f, 
            -80.0f, 80.0f
        );
    });
    
    glfwSetScrollCallback(window, [](GLFWwindow*, double, double yoffset) {
        g_engine.cameraDistance -= static_cast<float>(yoffset) * 2.5f * 0.1f;
        g_engine.cameraDistance = glm::clamp(
            g_engine.cameraDistance, 
            g_engine.cameraZoomMin, 
            g_engine.cameraZoomMax
        );
    });
}

void Update(float dt, float characterRotation) {
    if (!g_engine.cameraFollowEnabled) return;
    
    const float CAMERA_SMOOTH = 60.0f;
    const float PIVOT_SMOOTH = 40.0f;
    
    // Update pivot
    glm::vec3 targetPivot = g_engine.characterPos + glm::vec3(0, 2.0f, 0);
    if (std::isfinite(targetPivot.x) && std::isfinite(targetPivot.y) && std::isfinite(targetPivot.z)) {
        g_engine.cameraPivot = glm::mix(
            g_engine.cameraPivot, 
            targetPivot, 
            glm::clamp(PIVOT_SMOOTH * dt, 0.0f, 1.0f)
        );
    }
    
    if (g_engine.cameraFixedMode) {
        // Fixed behind character
        glm::vec3 forward;
        forward.x = sin(glm::radians(characterRotation));
        forward.z = cos(glm::radians(characterRotation));
        forward.y = 0.0f;
        
        if (glm::length(forward) > 0.0001f) {
            forward = glm::normalize(forward);
        } else {
            forward = glm::vec3(0.0f, 0.0f, 1.0f);
        }
        
        glm::vec3 targetCamPos = g_engine.characterPos - forward * g_engine.cameraDistance 
                                + glm::vec3(0, g_engine.cameraHeight, 0);
        
        if (std::isfinite(targetCamPos.x) && std::isfinite(targetCamPos.y) && std::isfinite(targetCamPos.z)) {
            g_engine.camera->Position = glm::mix(
                g_engine.camera->Position, 
                targetCamPos, 
                glm::clamp(CAMERA_SMOOTH * dt, 0.0f, 1.0f)
            );
            g_engine.camera->Target = g_engine.cameraPivot;
        }
    } else {
        // Orbit mode
        float yawRad = glm::radians(g_engine.camera->Yaw);
        float pitchRad = glm::radians(g_engine.camera->Pitch);
        
        glm::vec3 camOffset;
        camOffset.x = cos(pitchRad) * sin(yawRad) * g_engine.cameraDistance;
        camOffset.y = sin(pitchRad) * g_engine.cameraDistance + g_engine.cameraHeight;
        camOffset.z = cos(pitchRad) * cos(yawRad) * g_engine.cameraDistance;
        
        glm::vec3 targetCamPos = g_engine.cameraPivot + camOffset;
        
        if (std::isfinite(targetCamPos.x) && std::isfinite(targetCamPos.y) && std::isfinite(targetCamPos.z)) {
            g_engine.camera->Position = glm::mix(
                g_engine.camera->Position, 
                targetCamPos, 
                glm::clamp(CAMERA_SMOOTH * dt, 0.0f, 1.0f)
            );
            g_engine.camera->Target = g_engine.cameraPivot;
        }
    }
}

void Cleanup() {
    delete g_engine.camera;
    g_engine.camera = nullptr;
}

} // namespace CameraSystem

// ============================================================================
// CHARACTER SYSTEM
// ============================================================================
namespace CharacterSystem {

struct AnimationSet {
    std::shared_ptr<Animation> idle;
    std::shared_ptr<Animation> walk;
    std::shared_ptr<Animation> run;
    std::shared_ptr<Animation> jump;
    std::shared_ptr<Animation> fall;
    std::shared_ptr<Animation> crouch;
    std::shared_ptr<Animation> crouchWalk;
};

static AnimationSet g_animations;

void LoadAnimations(const std::string& assetPath) {
    Assimp::Importer importer;
    
    auto loadAnim = [&](const std::string& path, const std::string& name) 
                      -> std::shared_ptr<Animation> {
        const aiScene* scene = importer.ReadFile(path, 
            aiProcess_Triangulate | aiProcess_FlipUVs);
        if (scene && scene->HasAnimations()) {
            auto anim = std::make_shared<Animation>(
                AssimpAnimationLoader::LoadAnimationWithPoseCorrection(
                    scene, scene->mAnimations[0]
                )
            );
            anim->name = name;
            std::cout << "  [LOADED] " << name << ": " 
                      << anim->duration << "s, " 
                      << anim->boneAnimations.size() << " bones\n";
            return anim;
        }
        std::cerr << "  [FAILED] " << name << ": " << path << "\n";
        return nullptr;
    };
    
    std::cout << "\n=== LOADING ANIMATIONS ===\n";
    g_animations.idle = loadAnim(assetPath + "/Idle.fbx", "Idle");
    g_animations.walk = loadAnim(assetPath + "/Walking.fbx", "Walk");
    g_animations.run = loadAnim(assetPath + "/Run.fbx", "Run");
    g_animations.jump = loadAnim(assetPath + "/Jump.fbx", "Jump");
    g_animations.fall = loadAnim(assetPath + "/Fall.fbx", "Fall");
    g_animations.crouch = loadAnim(assetPath + "/Crouching.fbx", "Crouch");
    g_animations.crouchWalk = loadAnim(assetPath + "/CrouchingWalk.fbx", "CrouchWalk");
    
    // Fix animation durations for gameplay
    auto fixDuration = [](std::shared_ptr<Animation> anim, float duration, const std::string& name) {
        if (anim && anim->duration > 5.0f) {
            anim->duration = duration;
            std::cout << "  [FIXED] " << name << " duration: " << duration << "s\n";
        }
    };
    
    fixDuration(g_animations.idle, 2.5f, "Idle");
    fixDuration(g_animations.walk, 2.4f, "Walk");
    fixDuration(g_animations.run, 1.6f, "Run");
    fixDuration(g_animations.jump, 1.1f, "Jump");
    fixDuration(g_animations.fall, 1.0f, "Fall");
    fixDuration(g_animations.crouch, 0.6f, "Crouch");
    fixDuration(g_animations.crouchWalk, 1.8f, "CrouchWalk");
}

void Initialize(const std::string& modelPath) {
    std::cout << "\n=== INITIALIZING CHARACTER ===\n";

    // Load character model
    g_engine.character = g_engine.modelManager->get("Bot");
    if (!g_engine.character) {
        std::cerr << "[ERROR] Failed to load Bot model!\n";
        return;
    }

    const Skeleton& skeleton = g_engine.character->GetSkeleton();
    std::cout << "  Bones: " << skeleton.bones.size() << "\n";
    std::cout << "  Meshes: " << g_engine.character->GetMeshCount() << "\n";

    // Calculate model scale
    glm::vec3 modelSize = g_engine.character->GetSize();
    float modelScale = 10.0f / glm::max(modelSize.y, 1.0f);
    std::cout << "  Model size: " << modelSize.x << "x" << modelSize.y << "x" << modelSize.z << "\n";
    std::cout << "  Model scale: " << modelScale << "\n";

    // Place character on ground (not floating!)
    g_engine.characterPos = glm::vec3(0.0f, 0.0f, 0.0f);
    if (g_engine.terrain) {
        float terrainHeight = g_engine.terrain->getHeightAt(0.0f, 0.0f);
        if (std::isfinite(terrainHeight)) {
            // Account for animation root offset - feet are ~8.75 units below root
            // Character Y should be terrain height + foot offset so feet touch ground
            g_engine.characterPos.y = terrainHeight;
            std::cout << "  Character placed on terrain at Y=" << terrainHeight << "\n";
            std::cout << "  Note: Animation root is at Y≈100, feet at Y≈8.75 relative\n";
            std::cout << "  Foot IK floor height will be set to match terrain\n";
        } else {
            g_engine.characterPos.y = g_engine.waterLevel;
            std::cout << "  Character placed at water level Y=" << g_engine.waterLevel << " (invalid terrain)\n";
        }
    } else {
        g_engine.characterPos.y = g_engine.waterLevel;
        std::cout << "  Character placed at water level Y=" << g_engine.waterLevel << "\n";
    }

    // Create animator
    g_engine.animator = new Animator(const_cast<Skeleton*>(&skeleton));
    g_engine.animator->SetFootIKEnabled(true);

    // Configure foot IK - floor height = character's Y position + scaled foot offset
    // The animation has feet at Y≈8.75 relative to root, but model is scaled down
    Animator::FootIKSettings ikSettings;
    ikSettings.enabled = true;
    // Floor height = character position Y + scaled foot offset (8.75 * 0.055 ≈ 0.48)
    float scaledFootOffset = 8.75f * modelScale;  // ~0.48 units
    ikSettings.floorHeight = g_engine.characterPos.y + scaledFootOffset;
    ikSettings.ikStrength = 1.0f;
    ikSettings.footLockBlend = 0.95f;
    ikSettings.footLockReleaseSpeed = 3.0f;
    ikSettings.maxIKDistance = 0.2f;

    int leftFoot = skeleton.GetBoneIndex("leftfoot");
    int rightFoot = skeleton.GetBoneIndex("rightfoot");
    if (leftFoot < 0) leftFoot = skeleton.GetBoneIndex("LeftFoot");
    if (rightFoot < 0) rightFoot = skeleton.GetBoneIndex("RightFoot");

    ikSettings.leftFootBone = leftFoot;
    ikSettings.rightFootBone = rightFoot;
    g_engine.animator->SetFootIKSettings(ikSettings);

    std::cout << "  Foot IK: L=" << leftFoot << " R=" << rightFoot 
              << " floorY=" << ikSettings.floorHeight 
              << " (charY=" << g_engine.characterPos.y 
              << " + scaledOffset=" << scaledFootOffset << ")\n";
    
    // Initialize Hybrid MM+FSM
    g_engine.hybrid = new HybridMMFSM();
    g_engine.hybrid->Initialize(&skeleton, g_engine.animator);
    
    // Load animations into hybrid system
    if (g_animations.idle) 
        g_engine.hybrid->LoadLocomotionAnimation("Idle", g_animations.idle);
    if (g_animations.walk) 
        g_engine.hybrid->LoadLocomotionAnimation("Walk", g_animations.walk);
    if (g_animations.run) 
        g_engine.hybrid->LoadLocomotionAnimation("Run", g_animations.run);
    if (g_animations.crouch) 
        g_engine.hybrid->LoadStateAnimation(HybridState::CROUCH, "Crouch", g_animations.crouch);
    if (g_animations.crouchWalk) 
        g_engine.hybrid->LoadStateAnimation(HybridState::CROUCH_WALK, "CrouchWalk", g_animations.crouchWalk);
    if (g_animations.jump) 
        g_engine.hybrid->LoadStateAnimation(HybridState::JUMP, "Jump", g_animations.jump);
    if (g_animations.fall) 
        g_engine.hybrid->LoadStateAnimation(HybridState::FALL, "Fall", g_animations.fall);
    
    g_engine.hybrid->BuildDatabases();
    
    // Add FSM transitions
    g_engine.hybrid->AddTransition(HybridState::LOCOMOTION, HybridState::JUMP, 0.1f,
        []() { return g_engine.hybrid->GetCharacterState().jump && 
                      g_engine.hybrid->GetCharacterState().grounded; }, true);
    
    g_engine.hybrid->AddTransition(HybridState::LOCOMOTION, HybridState::FALL, 0.15f,
        []() { return !g_engine.hybrid->GetCharacterState().grounded && 
                      g_engine.hybrid->GetCharacterState().velocity.y < -0.5f; }, true);
    
    g_engine.hybrid->AddTransition(HybridState::LOCOMOTION, HybridState::CROUCH, 0.2f,
        []() { return g_engine.hybrid->GetCharacterState().crouch && 
                      g_engine.hybrid->GetCharacterState().grounded; }, true);
    
    g_engine.hybrid->AddTransition(HybridState::JUMP, HybridState::LOCOMOTION, 0.1f,
        []() { return g_engine.hybrid->GetCharacterState().grounded; }, true);
    
    g_engine.hybrid->AddTransition(HybridState::FALL, HybridState::LOCOMOTION, 0.1f,
        []() { return g_engine.hybrid->GetCharacterState().grounded; }, true);
    
    g_engine.hybrid->AddTransition(HybridState::CROUCH, HybridState::LOCOMOTION, 0.2f,
        []() { return !g_engine.hybrid->GetCharacterState().crouch && 
                      g_engine.hybrid->GetCharacterState().grounded; }, true);
    
    std::cout << "  Hybrid MM+FSM: Initialized\n";

    // Play idle animation and initialize bone matrices
    if (g_animations.idle && g_engine.animator) {
        g_engine.animator->Play(g_animations.idle.get());
        g_engine.animator->SetCurrentTime(0.0f);
        g_engine.animator->Update(0.0f);  // Initial bone matrix calculation
        
        // Verify bone positions are valid (debug)
        const auto& boneMatrices = g_engine.animator->GetFinalBoneMatrices();
        if (!boneMatrices.empty()) {
            std::cout << "  Bone matrices: " << boneMatrices.size() << " bones\n";
            // Check foot bones for NaN
            if (leftFoot >= 0 && leftFoot < (int)boneMatrices.size()) {
                glm::vec3 footPos = glm::vec3(boneMatrices[leftFoot][3]);
                bool valid = std::isfinite(footPos.x) && std::isfinite(footPos.y) && std::isfinite(footPos.z);
                std::cout << "  Left foot bone " << leftFoot << ": " << (valid ? "VALID" : "NaN!") 
                          << " pos=(" << footPos.x << "," << footPos.y << "," << footPos.z << ")\n";
            }
        }
    }
}

struct CharacterInput {
    glm::vec2 moveDirection{0.0f};
    float moveMagnitude = 0.0f;
    bool jump = false;
    bool crouch = false;
    bool sprint = false;
    bool grounded = false;
    float verticalVelocity = 0.0f;
};

CharacterInput ProcessInput(GLFWwindow* window, float dt) {
    CharacterInput input;
    
    // WASD movement
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) input.moveDirection.y = 1.0f;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) input.moveDirection.y = -1.0f;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) input.moveDirection.x = 1.0f;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) input.moveDirection.x = -1.0f;
    
    // Normalize
    if (glm::length(input.moveDirection) > 1.0f) {
        input.moveDirection = glm::normalize(input.moveDirection);
    }
    input.moveMagnitude = glm::length(input.moveDirection);
    
    // Actions
    input.crouch = (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS);
    input.sprint = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS);
    
    // Jump (edge-triggered)
    static bool lastJump = false;
    bool jumpKey = (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS);
    input.jump = jumpKey && !lastJump;
    lastJump = jumpKey;
    
    // Grounded check
    if (g_engine.terrain) {
        float terrainHeight = g_engine.terrain->getHeightAt(
            g_engine.characterPos.x, 
            g_engine.characterPos.z
        );
        input.grounded = (g_engine.characterPos.y <= terrainHeight + 0.01f);
    } else {
        input.grounded = (g_engine.characterPos.y <= g_engine.waterLevel + 0.01f);
    }
    
    return input;
}

void Update(float dt, const CharacterInput& input, glm::vec3& outRootMotion) {
    if (!g_engine.animator) return;

    // Model scale (same as render)
    const float modelScale = 10.0f / 180.0f;  // ~0.055

    // Validate character position (prevent NaN propagation)
    bool posValid = std::isfinite(g_engine.characterPos.x) && 
                    std::isfinite(g_engine.characterPos.y) && 
                    std::isfinite(g_engine.characterPos.z);
    bool rotValid = std::isfinite(g_engine.rotationAngle);
    
    if (!posValid) {
        static int warnCount = 0;
        if (++warnCount < 5) {
            std::cout << "[Character Update] ERROR: characterPos is NaN! Resetting to (0,40,0)\n";
        }
        g_engine.characterPos = glm::vec3(0.0f, 40.0f, 0.0f);
    }
    if (!rotValid) {
        static int warnCount = 0;
        if (++warnCount < 5) {
            std::cout << "[Character Update] ERROR: rotationAngle is NaN! Resetting to 0\n";
        }
        g_engine.rotationAngle = 0.0f;
    }

    // Update hybrid system
    if (g_engine.hybrid) {
        HybridMMFSMState hybridState;
        hybridState.moveDirection = input.moveDirection;
        hybridState.moveMagnitude = input.moveMagnitude;
        hybridState.jump = input.jump;
        hybridState.crouch = input.crouch;
        hybridState.sprint = input.sprint;
        hybridState.position = g_engine.characterPos;
        hybridState.grounded = input.grounded;
        
        // Calculate target velocity for motion matching
        float targetSpeed = input.moveMagnitude * (input.sprint ? 10.0f : 5.0f);
        if (targetSpeed > 0.001f && input.moveMagnitude > 0.1f) {
            // Convert move direction to world space (relative to camera)
            glm::vec3 camForward = glm::normalize(
                g_engine.camera->Target - g_engine.camera->Position
            );
            camForward.y = 0.0f;
            camForward = glm::normalize(camForward);
            glm::vec3 camRight = glm::normalize(
                glm::cross(camForward, glm::vec3(0.0f, 1.0f, 0.0f))
            );
            glm::vec3 moveDir = glm::normalize(
                camForward * input.moveDirection.y + 
                camRight * input.moveDirection.x
            );
            hybridState.velocity = moveDir * targetSpeed;
        }
        
        g_engine.hybrid->Update(dt, hybridState);
        
        // Extract root motion
        outRootMotion = g_engine.animator->ConsumeRootMotion();
    }
    
    // Update foot IK floor height based on terrain (with scale)
    float scaledFootOffset = 8.75f * modelScale;  // ~0.48 units
    
    if (g_engine.terrain) {
        float terrainHeight = g_engine.terrain->getHeightAt(
            g_engine.characterPos.x,
            g_engine.characterPos.z
        );
        if (std::isfinite(terrainHeight)) {
            Animator::FootIKSettings settings = g_engine.animator->footIKSettings;
            // Floor height = terrain + scaled foot offset
            settings.floorHeight = terrainHeight + scaledFootOffset;
            settings.enabled = true;
            g_engine.animator->SetFootIKSettings(settings);
        }
    }
    
    bool isMoving = (input.moveMagnitude > 0.1f);

    // Build model matrix with validation (include scale for proper IK)
    glm::mat4 modelMat;
    if (posValid && rotValid) {
        modelMat = glm::translate(glm::mat4(1.0f), g_engine.characterPos);
        modelMat *= glm::rotate(glm::mat4(1.0f),
            glm::radians(g_engine.rotationAngle),
            glm::vec3(0.0f, 1.0f, 0.0f)
        );
        modelMat *= glm::scale(glm::mat4(1.0f), glm::vec3(modelScale));
    } else {
        // Fallback to identity matrix
        modelMat = glm::mat4(1.0f);
        static int warnCount = 0;
        if (++warnCount < 3) {
            std::cout << "[FootIK] Using identity model matrix (invalid charPos/rot)\n";
        }
    }

    g_engine.animator->UpdateFootIK(dt, modelMat, isMoving);
    
    // Update animator
    g_engine.animator->Update(dt);
}

void Render(Shader& shader, const glm::mat4& projection, const glm::mat4& view) {
    if (!g_engine.character || !g_engine.animator) return;

    shader.use();
    shader.setMat4("projection", projection);
    shader.setMat4("view", view);

    // Build model matrix with proper scale
    // Model is ~180 units tall, scale down to ~10 units (human scale)
    float modelScale = 10.0f / 180.0f;  // ~0.055 scale factor
    glm::mat4 model = glm::translate(glm::mat4(1.0f), g_engine.characterPos);
    model *= glm::rotate(glm::mat4(1.0f),
        glm::radians(g_engine.rotationAngle),
        glm::vec3(0.0f, 1.0f, 0.0f)
    );
    model *= glm::scale(glm::mat4(1.0f), glm::vec3(modelScale));
    shader.setMat4("model", model);

    // CRITICAL: Enable skinning (default might be disabled)
    shader.setInt("uDisableSkinning", 0);

    // Upload bone matrices
    const auto& finalBones = g_engine.animator->GetFinalBoneMatrices();
    static GLuint boneTexID = 0;
    static int prevBoneCount = 0;
    int boneCount = static_cast<int>(finalBones.size());
    
    if (boneTexID == 0) {
        glGenTextures(1, &boneTexID);
    }
    
    int width = boneCount * 4;
    std::vector<glm::vec4> pixels(width);
    for (size_t i = 0; i < finalBones.size(); i++) {
        pixels[i*4+0] = finalBones[i][0];
        pixels[i*4+1] = finalBones[i][1];
        pixels[i*4+2] = finalBones[i][2];
        pixels[i*4+3] = finalBones[i][3];
    }
    
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, boneTexID);
    
    if (boneCount != prevBoneCount) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, 1, 0, 
                    GL_RGBA, GL_FLOAT, pixels.data());
        prevBoneCount = boneCount;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, 1, 
                       GL_RGBA, GL_FLOAT, pixels.data());
    }
    
    shader.setInt("boneTex", 10);
    shader.setInt("uPaletteSize", boneCount);
    shader.setVec3("lightPos", glm::vec3(10.0f, 10.0f, 10.0f));
    shader.setVec3("viewPos", g_engine.camera->Position);
    shader.setVec3("color", glm::vec3(0.8f, 0.6f, 0.4f));
    shader.setInt("uDebugMode", g_engine.debugMode);
    
    // Draw skinned meshes
    int meshesDrawn = 0;
    for (size_t i = 0; i < g_engine.character->GetMeshCount(); i++) {
        Mesh& mesh = g_engine.character->GetMesh(i);
        if (mesh.indices.empty()) {
            std::cerr << "[Character Render] Mesh " << i << " has NO indices!\n";
            continue;
        }
        if (mesh.VAO == 0) {
            std::cerr << "[Character Render] Mesh " << i << " has VAO=0!\n";
            continue;
        }
        mesh.Draw(shader);
        meshesDrawn++;
    }
    
    static int renderDebugFrame = 0;
    if (++renderDebugFrame % 60 == 0) {
        std::cout << "[Character Render] Drew " << meshesDrawn << "/" 
                  << g_engine.character->GetMeshCount() << " meshes"
                  << " | Bones: " << boneCount
                  << " | CharPos: (" << g_engine.characterPos.x << "," 
                  << g_engine.characterPos.y << "," << g_engine.characterPos.z << ")\n";
    }
}

void Cleanup() {
    delete g_engine.hybrid;
    delete g_engine.animator;
    g_engine.hybrid = nullptr;
    g_engine.animator = nullptr;
}

} // namespace CharacterSystem

// ============================================================================
// WORLD SYSTEM
// ============================================================================
namespace WorldSystem {

void Initialize() {
    std::cout << "\n=== INITIALIZING WORLD ===\n";
    
    // Terrain
    Terrain::TerrainConfig terrainConfig;
    terrainConfig.chunkSize = 100.0f;
    terrainConfig.chunkResolution = 64;
    terrainConfig.viewDistance = 8;
    terrainConfig.lodDistance = 50.0f;
    terrainConfig.heightmapSize = 1024;
    terrainConfig.heightScale = 80.0f;
    
    g_engine.terrain = new Terrain(terrainConfig);
    g_engine.terrain->initialize();
    
    // Vegetation
    VegetationSystem::VegetationConfig vegConfig;
    vegConfig.treeDensity = 0.03f;
    vegConfig.grassDensity = 0.8f;
    vegConfig.rockDensity = 0.02f;
    vegConfig.minTreeHeight = 4.0f;
    vegConfig.maxTreeHeight = 12.0f;
    vegConfig.maxTreesPerChunk = 80;
    vegConfig.maxRocksPerChunk = 50;
    
    g_engine.vegetation = new VegetationSystem(vegConfig);
    
    // World objects
    g_engine.worldObjects = new WorldObjectManager();
    g_engine.worldObjects->initialize("assets/World_objects/");
    
    std::cout << "  Terrain: " << terrainConfig.chunkSize 
              << "m chunks, LOD=" << terrainConfig.viewDistance << "\n";
    std::cout << "  Vegetation: density=" << vegConfig.treeDensity << "\n";
    std::cout << "  World objects: " << g_engine.worldObjects->getObjectCount() 
              << " types\n";
}

void Update(float dt) {
    if (!g_engine.terrain) return;
    
    // Stream terrain based on camera position
    g_engine.terrain->update(g_engine.camera->Position, dt);
    
    // Update world objects
    if (g_engine.worldObjects) {
        g_engine.worldObjects->update(g_engine.camera->Position, dt);
    }
}

void Render(const glm::mat4& projection, const glm::mat4& view) {
    if (!g_engine.terrain) return;
    
    // Terrain shader
    static Shader* terrainShader = nullptr;
    if (!terrainShader) {
        terrainShader = new Shader("world/terrainVS.glsl", "world/terrainFS.glsl");
    }
    
    terrainShader->use();
    terrainShader->setMat4("projection", projection);
    terrainShader->setMat4("view", view);
    terrainShader->setMat4("model", glm::mat4(1.0f));
    terrainShader->setVec3("terrainColor", glm::vec3(0.2f, 0.5f, 0.2f));
    terrainShader->setFloat("waterLevel", g_engine.waterLevel);
    terrainShader->setVec3("cameraPos", g_engine.camera->Position);
    
    // Render terrain with frustum culling
    g_engine.terrain->render(
        g_engine.camera->Position, 
        45.0f, 
        1280.0f / 720.0f, 
        0.1f, 
        1000.0f
    );
    
    // Render world objects
    if (g_engine.worldObjects) {
        g_engine.worldObjects->render(view, projection, g_engine.camera->Position);
    }
}

void Cleanup() {
    delete g_engine.worldObjects;
    delete g_engine.vegetation;
    delete g_engine.terrain;
    g_engine.worldObjects = nullptr;
    g_engine.vegetation = nullptr;
    g_engine.terrain = nullptr;
}

} // namespace WorldSystem

// ============================================================================
// WATER SYSTEM
// ============================================================================
namespace WaterSystem {

static GLuint waterVAO = 0;
static GLuint waterVBO = 0;
static GLuint waterEBO = 0;
static GLsizei waterIndexCount = 0;

void Initialize(float size, int subdivisions) {
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    
    float halfSize = size * 0.5f;
    float step = size / subdivisions;
    
    // Generate vertices
    for (int z = 0; z <= subdivisions; z++) {
        for (int x = 0; x <= subdivisions; x++) {
            Vertex vertex;
            vertex.Position = glm::vec3(
                -halfSize + x * step,
                g_engine.waterLevel,
                -halfSize + z * step
            );
            vertex.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
            vertex.TexCoords = glm::vec2(
                static_cast<float>(x) / subdivisions,
                static_cast<float>(z) / subdivisions
            );
            vertex.Tangent = glm::vec3(1.0f, 0.0f, 0.0f);
            vertex.Bitangent = glm::vec3(0.0f, 0.0f, 1.0f);
            vertex.BoneIDs = glm::ivec4(0);
            vertex.Weights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
            vertices.push_back(vertex);
        }
    }
    
    // Generate indices
    for (int z = 0; z < subdivisions; z++) {
        for (int x = 0; x < subdivisions; x++) {
            int topLeft = z * (subdivisions + 1) + x;
            int topRight = topLeft + 1;
            int bottomLeft = (z + 1) * (subdivisions + 1) + x;
            int bottomRight = bottomLeft + 1;
            
            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);
            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }
    
    // Create buffers
    glGenVertexArrays(1, &waterVAO);
    glGenBuffers(1, &waterVBO);
    glGenBuffers(1, &waterEBO);
    
    glBindVertexArray(waterVAO);
    
    glBindBuffer(GL_ARRAY_BUFFER, waterVBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), 
                vertices.data(), GL_STATIC_DRAW);
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, waterEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, 
                indices.size() * sizeof(unsigned int), 
                indices.data(), GL_STATIC_DRAW);
    
    // Vertex attributes
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), 
                         (void*)offsetof(Vertex, Normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), 
                         (void*)offsetof(Vertex, TexCoords));
    
    glBindVertexArray(0);

    waterIndexCount = static_cast<GLsizei>(indices.size());
    std::cout << "  Water: " << vertices.size() << " vertices, "
              << indices.size() / 3 << " triangles\n";
}

void Render(Shader& shader, const glm::mat4& projection, const glm::mat4& view, float time) {
    if (waterVAO == 0) return;
    
    shader.use();
    shader.setMat4("projection", projection);
    shader.setMat4("view", view);
    shader.setMat4("model", glm::mat4(1.0f));
    shader.setFloat("time", time);
    shader.setFloat("waterLevel", g_engine.waterLevel);
    shader.setVec3("viewPos", g_engine.camera->Position);
    
    // Enable blending for transparent water
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    
    glBindVertexArray(waterVAO);
    glDrawElements(GL_TRIANGLES, waterIndexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void Cleanup() {
    if (waterVAO != 0) glDeleteVertexArrays(1, &waterVAO);
    if (waterVBO != 0) glDeleteBuffers(1, &waterVBO);
    if (waterEBO != 0) glDeleteBuffers(1, &waterEBO);
    waterVAO = 0;
    waterVBO = 0;
    waterEBO = 0;
}

} // namespace WaterSystem

// ============================================================================
// INPUT HANDLING
// ============================================================================
namespace Input {

void ProcessKeyboard(GLFWwindow* window, float dt) {
    // Exit
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, true);
    }
    
    // Camera pivot
    float moveSpeed = 10.0f * dt;
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
        g_engine.cameraPivot.y -= moveSpeed;
    }
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) {
        g_engine.cameraPivot.y += moveSpeed;
    }
    
    // Reset camera
    if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) {
        g_engine.cameraPivot = glm::vec3(0, 2, 0);
        g_engine.cameraDistance = 21.0f;
        g_engine.camera->Yaw = -90;
        g_engine.camera->Pitch = 0;
    }
    
    // Toggle camera mode
    static bool lastC = false;
    if (glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS && !lastC) {
        g_engine.cameraFixedMode = !g_engine.cameraFixedMode;
        std::cout << ">>> Camera: " 
                  << (g_engine.cameraFixedMode ? "FIXED" : "ORBIT") << "\n";
        if (g_engine.cameraFixedMode) {
            g_engine.camera->Yaw = -90;
            g_engine.camera->Pitch = 0;
        }
    }
    lastC = (glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS);
    
    // Toggle wireframe
    static bool lastF = false;
    if (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS && !lastF) {
        g_engine.wireframe = !g_engine.wireframe;
        glPolygonMode(GL_FRONT_AND_BACK, 
                     g_engine.wireframe ? GL_LINE : GL_FILL);
        std::cout << ">>> Wireframe: " 
                  << (g_engine.wireframe ? "ON" : "OFF") << "\n";
    }
    lastF = (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS);
    
    // Bone debug
    static bool lastB = false;
    if (glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS && !lastB) {
        g_engine.debugMode = (g_engine.debugMode + 1) % 3;
        const char* modes[] = {"OFF", "Bone IDs", "Weights"};
        std::cout << ">>> Bone debug: " << modes[g_engine.debugMode] << "\n";
    }
    lastB = (glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS);
    
    // Foot IK debug
    static bool lastG = false;
    if (glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS && !lastG && g_engine.animator) {
        std::cout << "\n=== FOOT IK STATUS ===\n";
        std::cout << "Enabled: " << (g_engine.animator->footIKSettings.enabled ? "YES" : "NO") << "\n";
        std::cout << "Floor height: " << g_engine.animator->footIKSettings.floorHeight << "\n";
        std::cout << "Character Y: " << g_engine.characterPos.y << "\n";
        g_engine.animator->DebugDrawFootIK();
    }
    lastG = (glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS);
    
    // Hybrid debug
    static bool lastH = false;
    if (glfwGetKey(window, GLFW_KEY_H) == GLFW_PRESS && !lastH && g_engine.hybrid) {
        std::cout << "\n=== HYBRID MM+FSM DEBUG ===\n";
        std::cout << g_engine.hybrid->GetDebugInfo();
        std::cout << "[CHARACTER] Pos=(" << g_engine.characterPos.x 
                  << ", " << g_engine.characterPos.y 
                  << ", " << g_engine.characterPos.z << ")\n";
    }
    lastH = (glfwGetKey(window, GLFW_KEY_H) == GLFW_PRESS);
    
    // Demo recording
    static bool lastF6 = false;
    if (glfwGetKey(window, GLFW_KEY_F6) == GLFW_PRESS && !lastF6 && g_engine.demoRecorder) {
        g_engine.demoRecorder->toggleMode();
    }
    lastF6 = (glfwGetKey(window, GLFW_KEY_F6) == GLFW_PRESS);
    
    // Model cycling
    static bool lastM = false;
    if (glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS && !lastM) {
        g_engine.modelNames = g_engine.modelManager->getModelNames();
        if (!g_engine.modelNames.empty()) {
            g_engine.currentModelIndex = (g_engine.currentModelIndex + 1) % 
                                        g_engine.modelNames.size();
            std::cout << ">>> View: " << g_engine.modelNames[g_engine.currentModelIndex] << "\n";
        }
    }
    lastM = (glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS);
}

} // namespace Input

// ============================================================================
// ENGINE CORE
// ============================================================================
namespace Engine {

bool Initialize() {
    std::cout << "\n========================================\n";
    std::cout << "  3D GAME ENGINE\n";
    std::cout << "========================================\n\n";
    
    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW\n";
        return false;
    }
    
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GL_TRUE);
    
    // Create window
    GLFWwindow* window = glfwCreateWindow(1280, 720, 
        "3D Engine - Hybrid MM+FSM", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return false;
    }
    
    glfwMakeContextCurrent(window);
    
    // Initialize GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD\n";
        return false;
    }
    
    // OpenGL state
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    
    std::cout << "OpenGL: " << glGetString(GL_VERSION) << "\n";
    std::cout << "GLSL: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << "\n\n";
    
    return true;
}

void LoadAssets() {
    std::cout << "=== LOADING ASSETS ===\n";
    
    // Model manager
    g_engine.modelManager = new ModelManager();
    g_engine.modelManager->load("Bot", "assets/bot.fbx");
    g_engine.modelManager->load("Bear", "assets/World_objects/Bear_DEMO.fbx");
    g_engine.modelManager->load("Datsun", "assets/datsun.fbx");
    g_engine.modelManager->load("Rock1", "assets/World_objects/Rock1.fbx");
    g_engine.modelManager->load("Rock2", "assets/World_objects/Rock2.fbx");
    g_engine.modelManager->load("Stone", "assets/World_objects/stone.fbx");
    
    g_engine.modelManager->printAll();
    
    // Load animations
    CharacterSystem::LoadAnimations("assets");
    
    // Demo recorder
    g_engine.demoRecorder = new DemoRecorder();
    g_engine.demoRecorder->initialize();
    
    std::cout << "\n";
}

void Run(GLFWwindow* window) {
    // Initialize systems (order matters!)
    WorldSystem::Initialize();  // MUST be before CharacterSystem (for terrain height)
    CharacterSystem::Initialize("assets/bot.fbx");
    
    // Camera MUST initialize after character to position correctly
    glm::vec3 cameraStartPos = g_engine.characterPos + glm::vec3(0.0f, 10.0f, 30.0f);
    CameraSystem::Initialize(cameraStartPos);
    CameraSystem::SetupCallbacks(window);
    
    WaterSystem::Initialize(2000.0f, 50);
    
    // Shaders
    Shader characterShader("shaderSystem/VS.glsl", "shaderSystem/FS.glsl");
    Shader waterShader("world/waterVS.glsl", "world/waterFS.glsl");
    
    std::cout << "\n=== RUNNING ===\n\n";
    
    g_engine.lastTime = static_cast<float>(glfwGetTime());
    
    // Main loop
    while (!glfwWindowShouldClose(window)) {
        // Delta time
        float currentTime = static_cast<float>(glfwGetTime());
        float dt = std::min(currentTime - g_engine.lastTime, 0.1f);
        g_engine.lastTime = currentTime;
        
        // FPS counter
        g_engine.frameCount++;
        g_engine.fpsTimer += dt;
        if (g_engine.fpsTimer >= 5.0f) {
            std::cout << "[FPS] " << g_engine.frameCount 
                      << " (" << (1000.0f * g_engine.fpsTimer / g_engine.frameCount) 
                      << "ms/frame)\n";
            g_engine.frameCount = 0;
            g_engine.fpsTimer = 0.0f;
        }
        
        // Demo recorder
        if (g_engine.demoRecorder) {
            g_engine.demoRecorder->update(dt);
            if (g_engine.demoRecorder->isPlaying()) {
                g_engine.demoRecorder->updateCamera(*g_engine.camera);
            }
        }
        
        // Input
        Input::ProcessKeyboard(window, dt);
        
        // Character input
        CharacterSystem::CharacterInput charInput = 
            CharacterSystem::ProcessInput(window, dt);
        
        // Update character
        glm::vec3 rootMotion;
        if (!g_engine.pausePhysics) {
            CharacterSystem::Update(dt, charInput, rootMotion);
            
            // Apply root motion
            if (glm::length(rootMotion) > 0.0001f) {
                // Convert to world space (with NaN protection)
                glm::vec3 camForward = g_engine.camera->Target - g_engine.camera->Position;
                camForward.y = 0.0f;
                float camFwdLen = glm::length(camForward);
                if (camFwdLen > 0.001f) {
                    camForward = glm::normalize(camForward);
                } else {
                    camForward = glm::vec3(0.0f, 0.0f, 1.0f);  // Default forward
                }
                
                glm::vec3 camRight = glm::normalize(
                    glm::cross(camForward, glm::vec3(0.0f, 1.0f, 0.0f))
                );
                
                glm::vec3 moveDir = glm::normalize(
                    camForward * charInput.moveDirection.y +
                    camRight * charInput.moveDirection.x
                );
                
                // Validate moveDir
                if (!std::isfinite(moveDir.x) || !std::isfinite(moveDir.y) || !std::isfinite(moveDir.z)) {
                    moveDir = glm::vec3(0.0f, 0.0f, 1.0f);  // Default
                }

                // Calculate rotation
                if (charInput.moveMagnitude > 0.1f) {
                    float targetAngle = atan2(moveDir.x, moveDir.z) * 180.0f / 3.14159f;
                    float rotationSpeed = 180.0f * dt;
                    float angleDiff = targetAngle - g_engine.rotationAngle;
                    while (angleDiff > 180.0f) angleDiff -= 360.0f;
                    while (angleDiff < -180.0f) angleDiff += 360.0f;

                    if (std::abs(angleDiff) > 1.0f) {
                        if (std::abs(angleDiff) > rotationSpeed) {
                            g_engine.rotationAngle += std::copysign(rotationSpeed, angleDiff);
                        } else {
                            g_engine.rotationAngle = targetAngle;
                        }
                    }
                }

                g_engine.characterPos += moveDir * glm::length(rootMotion);
                
                // Validate character position after update
                if (!std::isfinite(g_engine.characterPos.x) || 
                    !std::isfinite(g_engine.characterPos.y) || 
                    !std::isfinite(g_engine.characterPos.z)) {
                    static int warnCount = 0;
                    if (++warnCount < 3) {
                        std::cout << "[RootMotion] characterPos became NaN! Resetting\n";
                    }
                    g_engine.characterPos = glm::vec3(0.0f, 40.0f, 0.0f);
                }
            }
            
            // Update world
            WorldSystem::Update(dt);
        }
        
        // Update camera
        CameraSystem::Update(dt, g_engine.rotationAngle);
        
        // Render
        glClearColor(0.5f, 0.7f, 0.9f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        
        glm::mat4 projection = glm::perspective(
            glm::radians(45.0f), 
            1280.0f / 720.0f, 
            0.1f, 
            1000.0f
        );
        glm::mat4 view = g_engine.camera->GetViewMatrix();
        
        // Render world
        WorldSystem::Render(projection, view);
        
        // Render water
        waterShader.use();
        waterShader.setMat4("projection", projection);
        waterShader.setMat4("view", view);
        WaterSystem::Render(waterShader, projection, view, currentTime);
        
        // Render character
        CharacterSystem::Render(characterShader, projection, view);
        
        glfwSwapBuffers(window);
        glfwPollEvents();
    }
}

void Cleanup() {
    std::cout << "\n=== CLEANUP ===\n";
    
    WaterSystem::Cleanup();
    WorldSystem::Cleanup();
    CharacterSystem::Cleanup();
    CameraSystem::Cleanup();
    
    delete g_engine.demoRecorder;
    delete g_engine.modelManager;
    
    g_engine.demoRecorder = nullptr;
    g_engine.modelManager = nullptr;
    
    glfwTerminate();
}

} // namespace Engine

// ============================================================================
// MAIN
// ============================================================================
int main() {
    if (!Engine::Initialize()) {
        return -1;
    }
    
    GLFWwindow* window = glfwGetCurrentContext();
    
    Engine::LoadAssets();
    Engine::Run(window);
    Engine::Cleanup();
    
    std::cout << "\nClosed.\n";
    return 0;
}
