#define GLM_ENABLE_EXPERIMENTAL

#include "link/Shader.h"
#include "link/Model.h"
#include "link/flyCamera.h"
#include "link/Animation.h"
#include "link/Animator.h"
#include "link/AnimationStateMachine.h"
#include "link/AssimpAnimationLoader.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/string_cast.hpp>


#include <iostream>

// ================= CAMERA =================
flyCamera camera(
    glm::vec3(0.0f, 3.0f, 10.0f),
    glm::vec3(0, 1, 0),
    -90.0f, 20.0f, 8.0f
);

void framebuffer_size_callback(GLFWwindow*, int w, int h)
{
    glViewport(0, 0, w, h);
}
void mouse_callback(GLFWwindow*, double x, double y)
{
    camera.ProcessMouseMovement((float)x, (float)y);
}
void scroll_callback(GLFWwindow*, double, double y)
{
    camera.ProcessMouseScroll((float)y);
}


// ================= MAIN =================
int main()
{
    // ---------- GLFW ----------
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window =
        glfwCreateWindow(1280, 720, "Animation Debug", nullptr, nullptr);
    glfwMakeContextCurrent(window);

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    glEnable(GL_DEPTH_TEST);

    Shader shader("link/VS.glsl", "link/FS.glsl");
// =====================================================
// LOAD SKINNED MODEL (MIXAMO)
// =====================================================
Model character("assets/bot.fbx");
const Skeleton& skeleton = character.GetSkeleton();

std::cout << "==== Skeleton Debug ====\n";
std::cout << "Bones: " << skeleton.bones.size() << "\n";

if (!skeleton.boneMapping.count("mixamorig:Hips"))
{
    std::cerr << "ERROR: Skeleton missing mixamorig:Hips\n";
    return -1;
}

std::cout << "ROOT FOUND: mixamorig:Hips -> "
          << skeleton.boneMapping.at("mixamorig:Hips") << "\n";


// =====================================================
// LOAD ANIMATIONS (NO SKIN)
// =====================================================
Assimp::Importer importer;

auto LoadAnim = [&](const std::string& path) -> Animation
{
    const aiScene* scene =
        importer.ReadFile(path, aiProcess_Triangulate);

    if (!scene || !scene->HasAnimations())
    {
        std::cerr << "Failed to load animation: " << path << "\n";
        return Animation("Empty", 0.0f, 25.0f);
    }

    return AssimpAnimationLoader::LoadAnimation(
        scene, scene->mAnimations[0]);
};

Animation idleAnim = LoadAnim("assets/idle.fbx");
Animation walkAnim = LoadAnim("assets/walk.fbx");
Animation runAnim  = LoadAnim("assets/run.fbx");

idleAnim.DebugPrintBoneNames();

if (idleAnim.GetDuration() <= 0.0f)
{
    std::cerr << "ERROR: Idle animation has zero duration\n";
    return -1;
}


// =====================================================
// DEBUG: ANIMATION ↔ SKELETON MATCH
// =====================================================
std::cout << "==== Animation ↔ Skeleton Match Test ====\n";

int matched = 0;
for (const auto& [boneName, id] : skeleton.boneMapping)
{
    std::string clean = boneName;
    size_t p = clean.find('|');
    if (p != std::string::npos) clean = clean.substr(p + 1);
    p = clean.find(':');
    if (p != std::string::npos) clean = clean.substr(p + 1);

    if (idleAnim.GetBoneAnimation(clean))
        matched++;
}

std::cout << "Matched animation channels: " << matched << "\n";


// =====================================================
// ANIMATOR + STATE MACHINE
// =====================================================
Animator animator(&skeleton);

 
animator.Play(&idleAnim);

// Force evaluation BEFORE render loop (kills T-pose)
animator.Update(0.0f);
animator.Upload(shader);

AnimationStateMachine sm(&animator);
sm.SetAnimations(&idleAnim, &walkAnim, &runAnim);


// =====================================================
// TRANSFORM / TIME
// =====================================================
glm::vec3 playerPos(0.0f);
glm::vec3 scale(0.01f);

float lastTime = static_cast<float>(glfwGetTime());

    // ================= LOOP =================
    while(!glfwWindowShouldClose(window))
    {
        float now = (float)glfwGetTime();
        float dt = now - lastTime;
        lastTime = now;

        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);

        // ----- Player input -----
        glm::vec3 move(0.0f);
        if(glfwGetKey(window, GLFW_KEY_W)==GLFW_PRESS) move.z -= 1;
        if(glfwGetKey(window, GLFW_KEY_S)==GLFW_PRESS) move.z += 1;
        if(glfwGetKey(window, GLFW_KEY_A)==GLFW_PRESS) move.x -= 1;
        if(glfwGetKey(window, GLFW_KEY_D)==GLFW_PRESS) move.x += 1;

        float speed = glm::length(glm::vec2(move.x, move.z));
        if(speed > 0.0f) move /= speed;

        // ----- Update animation -----
        sm.Update(dt, speed);
       animator.Update(dt);

        // ----- Apply root motion -----
        playerPos += animator.ConsumeRootMotion() * dt;
        for (int i = 0; i < 10; ++i)
{
    std::cout << "Bone[" << i << "] "
              << glm::to_string(animator.GetFinalBoneMatrices()[i])
              << "\n";
}
    auto& m = animator.GetFinalBoneMatrices()[0];
std::cout << m[3][0] << " " << m[3][1] << " " << m[3][2] << "\n";


        // ----- Camera -----
        camera.FollowPlayerSmooth(playerPos + glm::vec3(0,2,6), dt);

        // ----- Render -----
        glClearColor(0.1f,0.1f,0.15f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::mat4 projection = glm::perspective(glm::radians(45.0f), 1280.f/720.f, 0.1f, 100.f);
        glm::mat4 view = camera.GetViewMatrix();
        glm::mat4 modelMat = glm::translate(glm::mat4(1.0f), playerPos) *
                             glm::scale(glm::mat4(1.0f), scale);

        shader.use();
       // animator.Update(dt);
        shader.setMat4("projection", projection);
        shader.setMat4("view", view);
        shader.setMat4("model", modelMat);

        animator.Upload(shader);      // Upload bone matrices
        character.Draw(shader);       // Draw the model

        glfwSwapBuffers(window);
    }

    glfwTerminate();
    return 0;
}
