#define GLM_ENABLE_EXPERIMENTAL

#include "shaderSystem/Shader.h"
#include "modelSystem/Model.h"
#include "cameraSystem/flyCamera.h"
#include "animationSystem/Animation.h"
#include "animationSystem/Animator.h"
#include "animationSystem/AnimationStateMachine.h"
#include "animationSystem/AssimpAnimationLoader.h"
#include "animationSystem/AnimationRetargeting.h"
#include "playerSystem/CharacterController.h"
#include "physicsSystem/RigidBody.h"
#include "physicsSystem/Physics.h"
#include "physicsSystem/Constraint.h"
#include "boneSystem/BoneName.h"
#include "boneSystem/DebugSkeleton.h"
#include "boneSystem/DebugDraw.h"
#include "renderer/Renderer.h"
#include "lighting/LightingSystem.h"
#include "memory/MemoryManager.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/string_cast.hpp>

#include <iostream>
#include <memory>
#include <vector>
#include <algorithm>
#include <map>

// ================= CONSTANTS =================
constexpr int MAX_INSTANCES = 1;

// ================= CAMERA =================
flyCamera camera(
    glm::vec3(0.0f, 3.0f, 8.0f),
    glm::vec3(0, 1, 0),
    -90.0f,
    15.0f,
    8.0f);

// ================= CALLBACKS =================
void framebuffer_size_callback(GLFWwindow *, int w, int h) { glViewport(0, 0, w, h); }
void mouse_callback(GLFWwindow *, double x, double y) { camera.ProcessMouseMovement((float)x, (float)y); }
void scroll_callback(GLFWwindow *, double, double y) { camera.ProcessMouseScroll((float)y); }

// =============================================================
// SMART BONE LOOKUP
// =============================================================
int GetBoneIndexSmart(const Skeleton &skel, const std::string &rawName)
{
    std::string key = NormalizeBoneName(rawName);
    auto it = skel.boneMapping.find(key);
    if (it != skel.boneMapping.end())
        return it->second;

    static const std::map<std::string, std::vector<std::string>> aliases = {
        {"lefttoe", {"lefttoebase", "lefttoe_end"}},
        {"righttoe", {"righttoebase", "righttoe_end"}},
        {"leftfoot", {"leftfoot"}},
        {"rightfoot", {"rightfoot"}}};

    auto a = aliases.find(key);
    if (a != aliases.end())
    {
        for (const auto &alt : a->second)
        {
            auto it2 = skel.boneMapping.find(alt);
            if (it2 != skel.boneMapping.end())
                return it2->second;
        }
    }
    return -1;
}

// ================= SIMPLE DEBUG MESH =================
struct SimpleMesh
{
    std::vector<glm::vec3> vertices;
    std::vector<unsigned int> indices;
    GLuint VAO, VBO, EBO;

    void createCube(float sx, float sy, float sz)
    {
        float hx = sx * 0.5f, hy = sy * 0.5f, hz = sz * 0.5f;
        vertices = {
            {-hx, -hy, -hz}, {hx, -hy, -hz}, {hx, hy, -hz}, {-hx, hy, -hz}, {-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz}};
        indices = {
            0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4,
            4, 5, 1, 1, 0, 4, 7, 6, 2, 2, 3, 7,
            4, 0, 3, 3, 7, 4, 5, 1, 2, 2, 6, 5};
        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
        glGenBuffers(1, &EBO);

        glBindVertexArray(VAO);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(glm::vec3), vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void *)0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
    }

    void Draw()
    {
        glBindVertexArray(VAO);
        glDrawElements(GL_TRIANGLES, (GLsizei)indices.size(), GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }
};

// =============================================================
// MAIN
// =============================================================
int main()
{
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window = glfwCreateWindow(1280, 720, "Animation Stable", nullptr, nullptr);
    glfwMakeContextCurrent(window);

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cerr << "Failed to initialize GLAD\n";
        return -1;
    }

    glEnable(GL_DEPTH_TEST);

    // ================= SHADERS =================
    Shader skinnedShader("shaderSystem/VS.glsl", "shaderSystem/FS.glsl");
    Shader flatShader("shaderSystem/flatVS.glsl", "shaderSystem/flatFS.glsl");

    // ================= MODEL =================
    Model character("assets/xboit.fbx");
    const Skeleton &skeleton = character.GetSkeleton();
    
    // Detect and validate the skeleton structure
    AnimationRetargeting::RigType detectedRigType = AnimationRetargeting::DetectRigType(skeleton);
    std::cout << "[MAIN] Detected rig type: ";
    switch(detectedRigType) {
        case AnimationRetargeting::RigType::MIXAMO_RIG:
            std::cout << "Mixamo Rig\n";
            break;
        case AnimationRetargeting::RigType::EPIC_MANNEQUIN:
            std::cout << "Epic Mannequin\n";
            break;
        case AnimationRetargeting::RigType::GENERIC_HUMAN:
            std::cout << "Generic Human\n";
            break;
        default:
            std::cout << "Unknown\n";
            break;
    }
    
    // Validate skeleton structure
    if (!AnimationRetargeting::ValidateSkeletonStructure(skeleton, detectedRigType)) {
        std::cout << "[MAIN] Warning: Skeleton structure validation failed for detected rig type\n";
    }

    // ================= BONE LOOKUP =================
    int leftFootBone = GetBoneIndexSmart(skeleton, "LeftFoot");
    int rightFootBone = GetBoneIndexSmart(skeleton, "RightFoot");
    int leftToeBone = GetBoneIndexSmart(skeleton, "LeftToe");
    int rightToeBone = GetBoneIndexSmart(skeleton, "RightToe");

    // ================= ANIMATIONS =================
    Assimp::Importer importer;
    auto LoadAnim = [&](const std::string &path) -> Animation
    {
        const aiScene *scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs);
        if (!scene || !scene->HasAnimations())
        {
            std::cerr << "Warning: animation not found: " << path << "\n";
            return Animation("Empty", 0.0f, 25.0f);
        }
        // Use the new pose correction loader for better Mixamo compatibility
        return AssimpAnimationLoader::LoadAnimationWithPoseCorrection(scene, scene->mAnimations[0]);
    };

    Animation idleAnim = LoadAnim("assets/Idle.fbx");
    Animation walkAnim = LoadAnim("assets/walk.fbx");
    Animation runAnim = LoadAnim("assets/run.fbx");

    // Check if animations need to be retargeted to the skeleton (for Mixamo compatibility)
    AnimationRetargeting::RigType idleAnimRigType = AnimationRetargeting::DetectRigTypeFromAnimation(idleAnim);
    AnimationRetargeting::RigType walkAnimRigType = AnimationRetargeting::DetectRigTypeFromAnimation(walkAnim);
    AnimationRetargeting::RigType runAnimRigType = AnimationRetargeting::DetectRigTypeFromAnimation(runAnim);
    
    std::cout << "[MAIN] Idle animation rig type: " << (idleAnimRigType == AnimationRetargeting::RigType::MIXAMO_RIG ? "Mixamo" : "Other") << "\n";
    std::cout << "[MAIN] Walk animation rig type: " << (walkAnimRigType == AnimationRetargeting::RigType::MIXAMO_RIG ? "Mixamo" : "Other") << "\n";
    std::cout << "[MAIN] Run animation rig type: " << (runAnimRigType == AnimationRetargeting::RigType::MIXAMO_RIG ? "Mixamo" : "Other") << "\n";
    
    // If animations are Mixamo but skeleton is different, consider retargeting
    if (idleAnimRigType == AnimationRetargeting::RigType::MIXAMO_RIG || 
        walkAnimRigType == AnimationRetargeting::RigType::MIXAMO_RIG || 
        runAnimRigType == AnimationRetargeting::RigType::MIXAMO_RIG) {
        std::cout << "[MAIN] Mixamo animations detected, applying pose corrections...\n";
        
        // Apply normalization to fix common Mixamo pose issues
        idleAnim = AnimationRetargeting::NormalizeToStandardPose(idleAnim, skeleton);
        walkAnim = AnimationRetargeting::NormalizeToStandardPose(walkAnim, skeleton);
        runAnim = AnimationRetargeting::NormalizeToStandardPose(runAnim, skeleton);
    }

    Animator animator(&skeleton);
    animator.Play(&idleAnim);

    AnimationStateMachine fsm(&animator);
    fsm.SetAnimations(&idleAnim, &walkAnim, &runAnim);

    // ================= PHYSICS =================
    PhysicsWorld physicsWorld;

    auto floorBody = std::make_shared<RigidBody>();
    floorBody->scale = glm::vec3(50.0f, 0.5f, 50.0f);
    floorBody->position = glm::vec3(0.0f, floorBody->scale.y * 0.5f, 0.0f);
    floorBody->mass = 0.0f;
    floorBody->isStatic = true;
    physicsWorld.addBody(floorBody);

    SimpleMesh floorMesh;
    floorMesh.createCube(floorBody->scale.x, floorBody->scale.y, floorBody->scale.z);

    auto cubeBody = std::make_shared<RigidBody>();
    cubeBody->scale = glm::vec3(0.5f);
    cubeBody->position = glm::vec3(2.0f, 5.0f, 0.0f);
    cubeBody->mass = 1.0f;
    cubeBody->isModel = true;
    cubeBody->isStatic = false;
    physicsWorld.addBody(cubeBody);

    SimpleMesh cubeMesh;
    cubeMesh.createCube(cubeBody->scale.x, cubeBody->scale.y, cubeBody->scale.z);

    auto playerBody = std::make_shared<RigidBody>();

    // Auto-fit model to ~10 units tall (more visible)
    glm::vec3 size = character.GetSize();
    float targetHeight = 10.0f;
    float autoScale = targetHeight / size.y;
    std::cout << "Model original size: " << glm::to_string(size) << ", scale factor: " << autoScale << "\n";

    float modelScale = autoScale;
    // Scale the physics body separately from the visual model
    playerBody->scale = glm::vec3(0.5f, 1.8f, 0.5f); // Keep physics body reasonable
    playerBody->position = glm::vec3(0.0f, floorBody->scale.y + playerBody->scale.y * 0.5f, 0.0f);
    playerBody->mass = 1.0f;
    playerBody->isModel = true;
    playerBody->isStatic = false;
    physicsWorld.addBody(playerBody);

    CharacterController playerController(playerBody, &physicsWorld);

    float lastTime = (float)glfwGetTime();
    FootLock leftFootLock, rightFootLock;
    glm::vec3 prevRootPos(0.0f);

    // Initialize new systems
    LightingSystem lightingSystem;
    
    // Add some lights
    Light pointLight(LightType::POINT, glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f, 0.8f, 0.3f), 1.0f);
    pointLight.constant = 1.0f;
    pointLight.linear = 0.09f;
    pointLight.quadratic = 0.032f;
    lightingSystem.addLight(pointLight);
    
    // Add a directional light
    Light dirLight(LightType::DIRECTIONAL, glm::vec3(0.0f), glm::vec3(-0.2f, -1.0f, -0.3f), glm::vec3(0.8f, 0.8f, 0.7f), 0.5f);
    lightingSystem.addLight(dirLight);
    
    // Create a constraint between two objects (for demonstration)
    auto constraint = new DistanceConstraint(cubeBody, playerBody, glm::vec3(0.0f), glm::vec3(0.0f), 5.0f);
    physicsWorld.addConstraint(constraint);

    // ================= MAIN LOOP =================
    while (!glfwWindowShouldClose(window))
    {
        float now = (float)glfwGetTime();
        float dt = now - lastTime;
        lastTime = now;
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);

        // ---------------- INPUT ----------------
        glm::vec3 moveInput(0.0f);
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
            moveInput = glm::vec3(1.0f);
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
            moveInput.z -= 0.5f;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
            moveInput.z += 0.5f;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
            moveInput.x -= 0.5f;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
            moveInput.x += 0.5f;
        playerController.setMoveInput(moveInput);

        // Test new character controller features
        if(glfwGetKey(window,GLFW_KEY_LEFT_SHIFT)==GLFW_PRESS) {
            playerController.setSprint(true);
        } else {
            playerController.setSprint(false);
        }
        
        if(glfwGetKey(window,GLFW_KEY_C)==GLFW_PRESS) {
            static float crouchTimer = 0.0f;
            crouchTimer += dt;
            if(crouchTimer > 0.5f) { // Debounce
                playerController.crouch(!playerController.isCrouching());
                crouchTimer = 0.0f;
            }
        }
        
        if(glfwGetKey(window,GLFW_KEY_X)==GLFW_PRESS) {
            playerController.slide();
        }

        glm::vec3 playerPos = playerBody->position;

        float speed = glm::length(glm::vec2(moveInput.x, moveInput.z));
        fsm.Update(dt, speed);
        animator.Update(dt);
        
        // Test camera shake when pressing 'K'
        if(glfwGetKey(window,GLFW_KEY_K)==GLFW_PRESS) {
            camera.AddShake(0.1f, 0.5f, glm::vec3(1.0f));
        }
        
        // Update camera shake
        camera.UpdateShake(dt);

        // ---------------- ROOT BONE OFFSET ----------------
        int hipsIdx = skeleton.GetBoneIndex("hips");
        if (hipsIdx == -1)
            hipsIdx = skeleton.rootBoneIndex;
        glm::vec3 hipsPos = (hipsIdx >= 0) ? animator.currBoneWorldPos[hipsIdx] : glm::vec3(0.0f);
        std::cout << "[MAIN_LOOP] Hips index: " << hipsIdx << ", Hips position: " << glm::to_string(hipsPos) << "\n";

        // Calculate model position - use only the horizontal movement from animation, keep vertical at player level
        glm::vec3 modelPosition = playerPos;
        // Only apply X/Z translation from animation, keep Y at player level
        modelPosition.x = playerPos.x - hipsPos.x;
        modelPosition.z = playerPos.z - hipsPos.z;

        // Apply 180-degree rotation around X-axis to make the character upright
        // This fixes the handstand issue by rotating the model to proper orientation
        //  glm::mat4 rotationFix = glm::rotate(glm::mat4(1.0f), glm::radians(180.0f), glm::vec3(1.0f, 0.0f, 0.0f));

        // Adjust position to compensate for the rotation (move up to keep feet at ground level)
        glm::vec3 adjustedPosition = modelPosition;
        adjustedPosition.y += 5.0f * modelScale; // Compensate for the vertical flip

        glm::mat4 modelMat = glm::translate(glm::mat4(1.0f), adjustedPosition) * glm::scale(glm::mat4(1.0f), glm::vec3(modelScale));
        std::cout << "[MAIN_LOOP] Player position: " << glm::to_string(playerPos) << "\n";
        std::cout << "[MAIN_LOOP] Model matrix translation: " << glm::to_string(glm::vec3(modelMat[3])) << "\n";

        // ---------------- FOOT IK ----------------
        auto processFoot = [&](int bone, FootLock &lock)
        {
            if (bone < 0 || bone >= (int)animator.GetFinalBoneMatrices().size())
                return;
            glm::vec3 footWorld = animator.GetBoneWorldPosition(bone, modelMat);
            bool planted = animator.IsFootPlanted(bone);
            if (planted)
            {
                if (!lock.locked)
                {
                    glm::vec3 hit, normal;
                    if (physicsWorld.raycastDown(footWorld, 1.0f, hit, normal))
                    {
                        lock.locked = true;
                        lock.worldPos = hit;
                    }
                }
                lock.weight = std::min(lock.weight + dt * 8.0f, 1.0f);
            }
            else
            {
                lock.weight = std::max(lock.weight - dt * 8.0f, 0.0f);
                if (lock.weight == 0.0f)
                    lock.locked = false;
            }
            if (lock.locked)
            {
                glm::vec3 correction = lock.worldPos - footWorld;
                glm::mat4 boneGlobal = animator.globalBoneMatrices[bone];
                glm::vec3 boneOffset = glm::vec3(glm::inverse(boneGlobal) * glm::vec4(correction, 0.0f));
                animator.AddIKOffset(bone, boneOffset, lock.weight);
            }
        };
        if (leftFootBone != -1)
            processFoot(leftFootBone, leftFootLock);
        if (rightFootBone != -1)
            processFoot(rightFootBone, rightFootLock);

        // ---------------- ROOT MOTION ----------------
        glm::vec3 rootMotion(0.0f);
        if (hipsIdx >= 0 && hipsIdx < (int)animator.currBoneWorldPos.size())
        {
            glm::vec3 hipsWorld = animator.currBoneWorldPos[hipsIdx];
            rootMotion = hipsWorld - prevRootPos;
            rootMotion.y = 0.0f;
            rootMotion *= modelScale;
            prevRootPos = hipsWorld;
            playerController.applyRootMotion(playerBody, rootMotion, dt);
        }

        // DEBUG: Print some bone positions to understand skeleton orientation
        static int debugCounter = 0;
        if (debugCounter < 5)
        { // Only print first few frames
            if (skeleton.GetBoneIndex("hips") >= 0 && skeleton.GetBoneIndex("leftfoot") >= 0)
            {
                int hipsBone = skeleton.GetBoneIndex("hips");
                int leftFootBone = skeleton.GetBoneIndex("leftfoot");

                if (hipsBone < animator.currBoneWorldPos.size() && leftFootBone < animator.currBoneWorldPos.size())
                {
                    glm::vec3 hipsPos = animator.currBoneWorldPos[hipsBone];
                    glm::vec3 footPos = animator.currBoneWorldPos[leftFootBone];

                    std::cout << "[DEBUG] Frame " << debugCounter << " - Hips pos: " << glm::to_string(hipsPos)
                              << ", Left foot pos: " << glm::to_string(footPos)
                              << ", Difference (foot-hips): " << glm::to_string(footPos - hipsPos) << "\n";
                }
            }
            debugCounter++;
        }

        // ---------------- PHYSICS ----------------
        physicsWorld.step(dt);

        // ---------------- RENDER ----------------
        std::cout << "[RENDER] Starting render frame\n";
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::mat4 projection = glm::perspective(glm::radians(45.0f), 1280.f / 720.f, 0.1f, 1000.f);
        glm::mat4 view = camera.GetViewMatrix();
        std::cout << "[RENDER] View matrix: " << glm::to_string(view[3]) << "\n"; // Print camera position

        // Floor
        glm::mat4 floorMat = glm::translate(glm::mat4(1.0f), floorBody->position) * glm::scale(glm::mat4(1.0f), floorBody->scale);
        flatShader.use();
        flatShader.setMat4("projection", projection);
        flatShader.setMat4("view", view);
        flatShader.setMat4("model", floorMat);
        flatShader.setVec3("color", glm::vec3(0.4f));
        floorMesh.Draw();

        // Cube
        glm::mat4 cubeMat = glm::translate(glm::mat4(1.0f), cubeBody->position) * glm::scale(glm::mat4(1.0f), cubeBody->scale);
        flatShader.setMat4("model", cubeMat);
        flatShader.setVec3("color", glm::vec3(0.8f, 0.2f, 0.2f));
        cubeMesh.Draw();

        // Character
        std::cout << "[RENDER] About to draw character\n";
        skinnedShader.use();
        skinnedShader.setMat4("projection", projection);
        skinnedShader.setMat4("view", view);
        skinnedShader.setMat4("model", modelMat);
        skinnedShader.setVec3("viewPos", camera.Position); // Set the view position for lighting
        std::cout << "[RENDER] Skinned shader uniforms set, about to call character.Draw\n";
        character.Draw(skinnedShader, animator);
        std::cout << "[RENDER] Character draw completed\n";

        // Debug draw skeleton
        const auto &finalBones = animator.GetFinalBoneMatrices();
        const Skeleton &skel = character.GetSkeleton();
        std::vector<DebugLine> skeletonLines;
        BuildSkeletonLines(skel.rootNode, modelMat, -1, skel, finalBones, skeletonLines);

        // TODO: Implement skeleton visualization to debug orientation

        // CAMERA FOLLOW
        camera.FollowPlayerSmooth(playerPos, dt);
        glfwSwapBuffers(window);
        std::cout << "[RENDER] Frame swap completed\n";
    }

    glfwTerminate();
    return 0;
}
