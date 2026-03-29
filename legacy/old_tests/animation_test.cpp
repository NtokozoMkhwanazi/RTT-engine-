/**
 * Animation System Test Application
 * 
 * Focused real-time testing for animation, skeletal animation, and motion matching.
 * Use this for debugging animation issues and memory leaks in isolation.
 * 
 * Build: make animation_test
 * Run:   ./bin/animation_test
 */

#include "shaderSystem/Shader.h"
#include "modelSystem/Model.h"
#include "cameraSystem/flyCamera.h"
#include "animationSystem/Animator.h"
#include "animationSystem/AssimpAnimationLoader.h"
#include "animationSystem/AnimationStateMachine.h"
#include "animationSystem/HybridMMFSM.h"
#include "motionMatching/MotionMatcher.h"
#include "boneSystem/Skeleton.h"
#include "boneSystem/BoneDebug.h"
#include "utils/FBXLoader.h"
#include "shaderSystem/stb_image.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>
#include <memory>
#include <vector>

// ================= CAMERA =================
flyCamera camera(
    glm::vec3(0.0f, 2.0f, 8.0f),
    glm::vec3(0, 1.5f, 0),
    -90.0f,
    0.0f,
    8.0f
);

// Global window pointer for input checks
GLFWwindow* g_window = nullptr;

// ================= ANIMATION SYSTEM =================
ModelManager ModelManager;
Animator* animator = nullptr;
AnimationStateMachine* fsm = nullptr;
HybridMMFSM* hybrid = nullptr;

// Loaded animations
std::shared_ptr<Animation> idleAnim = nullptr;
std::shared_ptr<Animation> walkAnim = nullptr;
std::shared_ptr<Animation> runAnim = nullptr;
std::shared_ptr<Animation> jumpAnim = nullptr;

// ================= TEST CONFIGURATION =================
struct AnimationTestConfig {
    bool showSkeleton = false;
    bool showBoneNames = false;
    bool useHybrid = true;
    bool enableRootMotion = true;
    float animationSpeed = 1.0f;
} animConfig;

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
    camera.DistanceToTarget -= (float)yoffset * 0.3f;
    camera.DistanceToTarget = glm::clamp(camera.DistanceToTarget, 3.0f, 20.0f);
}

// ================= ANIMATION HELPERS =================
void printAnimationInfo(const std::shared_ptr<Animation>& anim, const std::string& name) {
    if (!anim) {
        std::cout << "  " << name << ": NOT LOADED\n";
        return;
    }
    
    std::cout << "  " << name << ":\n";
    std::cout << "    Duration: " << anim->duration << "s\n";
    std::cout << "    Bones: " << anim->boneAnimations.size() << "\n";
    std::cout << "    Ticks/sec: " << anim->ticksPerSecond << "\n";
    std::cout << "    Frames: " << (int)(anim->duration * 30.0f) << " @30fps\n";
}

void setupAnimationSystem(Model* character) {
    if (!character) return;

    const Skeleton& skeleton = character->GetSkeleton();

    // Create animator
    animator = new Animator(&skeleton);

    // Create FSM
    fsm = new AnimationStateMachine(animator);
    fsm->initialize();

    // Create Hybrid MM+FSM
    hybrid = new HybridMMFSM();
    hybrid->Initialize(&skeleton, animator);
    
    std::cout << "\nAnimation system initialized:\n";
    std::cout << "  Animator: OK\n";
    std::cout << "  FSM: OK\n";
    std::cout << "  Hybrid MM+FSM: OK\n";
    std::cout << "  Skeleton bones: " << skeleton.bones.size() << "\n";
}

void loadAnimations(const std::string& modelPath) {
    Assimp::Importer importer;
    
    auto loadAnim = [&](const std::string& animName, const std::string& animFile)
        -> std::shared_ptr<Animation> {

        const aiScene* scene = importer.ReadFile(
            animFile,
            aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace
        );

        if (!scene || !scene->HasAnimations()) {
            std::cerr << "Failed to load animation: " << animFile << "\n";
            return nullptr;
        }

        Animation anim = AssimpAnimationLoader::LoadAnimation(
            scene,
            scene->mAnimations[0]
        );
        
        return std::make_shared<Animation>(anim);
    };
    
    std::cout << "\nLoading animations from: " << modelPath << "\n";
    
    idleAnim = loadAnim("Idle", modelPath);
    walkAnim = loadAnim("Walk", modelPath);
    runAnim = loadAnim("Run", modelPath);
    jumpAnim = loadAnim("Jump", modelPath);
    
    // Print info
    std::cout << "\nLoaded animations:\n";
    printAnimationInfo(idleAnim, "Idle");
    printAnimationInfo(walkAnim, "Walk");
    printAnimationInfo(runAnim, "Run");
    printAnimationInfo(jumpAnim, "Jump");
    
    // Fix durations for mixamo animations
    auto fixDuration = [](std::shared_ptr<Animation>& anim, float maxDuration) {
        if (anim && anim->duration > maxDuration) {
            anim->duration = maxDuration;
            std::cout << "  Fixed " << anim->name << " duration to " << maxDuration << "s\n";
        }
    };
    
    fixDuration(idleAnim, 3.0f);
    fixDuration(walkAnim, 2.0f);
    fixDuration(runAnim, 1.5f);
    fixDuration(jumpAnim, 2.0f);
}

void setupHybridAnimations() {
    if (!hybrid || !idleAnim || !walkAnim || !runAnim) return;
    
    // Load locomotion animations for motion matching
    hybrid->LoadLocomotionAnimation("Idle", idleAnim);
    hybrid->LoadLocomotionAnimation("Walk", walkAnim);
    hybrid->LoadLocomotionAnimation("Run", runAnim);
    
    // Load jump for FSM
    if (jumpAnim) {
        hybrid->LoadStateAnimation(HybridState::JUMP, "Jump", jumpAnim);
    }
    
    // Build databases
    hybrid->BuildDatabases();
    
    std::cout << "\nHybrid system configured:\n";
    std::cout << "  Locomotion: Idle, Walk, Run\n";
    std::cout << "  Jump: " << (jumpAnim ? "Loaded" : "Not loaded") << "\n";
}

// ================= RENDERING =================
Shader* modelShader = nullptr;
Shader* skeletonShader = nullptr;

void setupShaders() {
    modelShader = new Shader("shaderSystem/VS.glsl", "shaderSystem/FS.glsl");
    
    // Use simple colored shader for skeleton debug
    skeletonShader = new Shader("shaderSystem/skyboxVS.glsl", "shaderSystem/skyboxFS.glsl");
}

void renderModel(Model* model, const glm::mat4& modelMatrix) {
    if (!model || !modelShader || !animator) return;
    
    modelShader->use();
    modelShader->setMat4("model", modelMatrix);
    modelShader->setMat4("view", camera.GetViewMatrix());
    modelShader->setMat4("projection", camera.GetProjectionMatrix(1280.0f / 720.0f));
    modelShader->setVec3("lightPos", glm::vec3(5.0f, 10.0f, 5.0f));
    modelShader->setVec3("viewPos", camera.Position);
    
    model->Draw(*modelShader, *animator);
}

void renderSkeleton(const Skeleton& skeleton, Animator* anim, const glm::mat4& modelMatrix) {
    if (!animConfig.showSkeleton || !skeletonShader || !anim) return;
    
    // Get bone transforms
    const std::vector<glm::mat4>& boneMatrices = anim->GetFinalBoneMatrices();
    
    // Draw bones as points with labels (simplified)
    skeletonShader->use();
    
    glm::mat4 mvp = camera.GetProjectionMatrix(1280.0f / 720.0f) * 
                    camera.GetViewMatrix() * 
                    modelMatrix;
    
    skeletonShader->setMat4("MVP", mvp);
    
    // Just draw first few bones for debugging
    for (size_t i = 0; i < std::min(size_t(10), skeleton.bones.size()); i++) {
        glm::vec3 pos = glm::vec3(boneMatrices[i][3]);
        
        float vertices[] = {
            pos.x, pos.y, pos.z, 1.0f, 0.0f, 0.0f,
            pos.x, pos.y + 0.5f, pos.z, 1.0f, 0.0f, 0.0f
        };
        
        GLuint VAO, VBO;
        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
        
        glBindVertexArray(VAO);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        
        glDrawArrays(GL_LINES, 0, 2);
        
        glDeleteVertexArrays(1, &VAO);
        glDeleteBuffers(1, &VBO);
    }
}

// ================= UPDATE =================
void updateAnimation(float dt, Model* character) {
    if (!animator || !character) return;
    
    CharacterInput input;
    
    // Get input
    if (glfwGetKey(g_window, GLFW_KEY_W) == GLFW_PRESS) {
        input.moveDirection = glm::vec2(0.0f, 1.0f);
        input.moveMagnitude = glfwGetKey(g_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 1.0f : 0.5f;
    } else if (glfwGetKey(g_window, GLFW_KEY_S) == GLFW_PRESS) {
        input.moveDirection = glm::vec2(0.0f, -1.0f);
        input.moveMagnitude = 0.5f;
    } else if (glfwGetKey(g_window, GLFW_KEY_A) == GLFW_PRESS) {
        input.moveDirection = glm::vec2(-1.0f, 0.0f);
        input.moveMagnitude = 0.5f;
    } else if (glfwGetKey(g_window, GLFW_KEY_D) == GLFW_PRESS) {
        input.moveDirection = glm::vec2(1.0f, 0.0f);
        input.moveMagnitude = 0.5f;
    }
    
    input.jump = glfwGetKey(g_window, GLFW_KEY_SPACE) == GLFW_PRESS;
    input.crouch = glfwGetKey(g_window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
    input.sprint = glfwGetKey(g_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
    input.grounded = true;
    
    if (animConfig.useHybrid && hybrid) {
        // Update hybrid system
        HybridMMFSMState hybridState;
        hybridState.velocity = glm::vec3(0.0f, 0.0f, input.moveMagnitude * 3.0f);
        hybridState.grounded = input.grounded;
        hybridState.moveDirection = input.moveDirection;
        hybridState.moveMagnitude = input.moveMagnitude;
        
        hybrid->Update(dt, hybridState);
    } else if (fsm) {
        // Update FSM
        fsm->update(dt, input);
    }
    
    // Update animator
    animator->Update(dt);
}

// ================= MAIN =================
int main() {
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  ANIMATION SYSTEM TEST\n";
    std::cout << "========================================\n";
    std::cout << "\nControls:\n";
    std::cout << "  Mouse - Orbit camera\n";
    std::cout << "  Scroll - Zoom in/out\n";
    std::cout << "  W/S - Walk forward/backward\n";
    std::cout << "  A/D - Strafe left/right\n";
    std::cout << "  SPACE - Jump\n";
    std::cout << "  LEFT SHIFT - Sprint\n";
    std::cout << "  LEFT CTRL - Crouch\n";
    std::cout << "  B - Toggle skeleton visualization\n";
    std::cout << "  H - Toggle hybrid MM+FSM\n";
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

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Animation Test", nullptr, nullptr);
    g_window = window;  // Set global pointer
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

    // Load model
    std::cout << "Loading model...\n";
    ModelManager.load("Bot", "assets/bot.fbx");
    Model* character = ModelManager.get("Bot");
    
    if (!character) {
        std::cerr << "Failed to load character model!\n";
        glfwTerminate();
        return -1;
    }
    
    std::cout << "Model loaded: " << character->GetMeshCount() << " meshes\n";
    std::cout << "Size: " << character->GetSize().x << " x " 
              << character->GetSize().y << " x " 
              << character->GetSize().z << "\n";

    // Setup animation system
    setupAnimationSystem(character);
    setupShaders();
    loadAnimations("assets/bot.fbx");
    setupHybridAnimations();

    std::cout << "\nAnimation test ready!\n\n";

    // Main loop
    float dt = 0.016f;
    while (!glfwWindowShouldClose(window)) {
        // Input
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);
        
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) {
            camera.Position = glm::vec3(0.0f, 2.0f, 8.0f);
            camera.Yaw = -90.0f;
            camera.Pitch = 0.0f;
        }
        
        static bool lastB = false;
        if (glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS && !lastB) {
            animConfig.showSkeleton = !animConfig.showSkeleton;
            std::cout << "Skeleton debug: " << (animConfig.showSkeleton ? "ON" : "OFF") << "\n";
        }
        lastB = glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS;
        
        static bool lastH = false;
        if (glfwGetKey(window, GLFW_KEY_H) == GLFW_PRESS && !lastH) {
            animConfig.useHybrid = !animConfig.useHybrid;
            std::cout << "Hybrid MM+FSM: " << (animConfig.useHybrid ? "ON" : "OFF") << "\n";
            std::cout << "Current state: " << HybridStateToString(hybrid->GetCurrentState()) << "\n";
        }
        lastH = glfwGetKey(window, GLFW_KEY_H) == GLFW_PRESS;

        // Update
        updateAnimation(dt, character);

        // Render
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::mat4 modelMatrix = glm::mat4(1.0f);
        renderModel(character, modelMatrix);
        renderSkeleton(character->GetSkeleton(), animator, modelMatrix);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    delete animator;
    delete fsm;
    delete hybrid;
    delete modelShader;
    delete skeletonShader;
    
    glfwDestroyWindow(window);
    glfwTerminate();
    
    std::cout << "\nAnimation test exited cleanly.\n";
    return 0;
}
