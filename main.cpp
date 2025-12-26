#define GLM_ENABLE_EXPERIMENTAL

#include "link/Shader.h"
#include "link/Model.h"
#include "link/Skybox.h"
#include "link/flyCamera.h"
#include "link/RigidBody.h"
#include "link/Physics.h"
#include "link/Entity.h"
#include "link/CharacterController.h"
#include "link/Animation.h"
#include "link/Animator.h"
#include "link/AnimationStateMachine.h"
#include "link/DebugSkeleton.h"
#include "link/DebugDraw.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <iostream>
#include <vector>
#include <memory>
#include <ctime>

// ------------------- CAMERA -------------------
flyCamera camera(glm::vec3(0.0f, 3.0f, 10.0f), glm::vec3(0.0f,1.0f,0.0f), -90.0f, 20.0f, 8.0f);

void framebuffer_size_callback(GLFWwindow* window, int width, int height) { glViewport(0, 0, width, height); }
void mouse_callback(GLFWwindow* window, double xpos, double ypos) { camera.ProcessMouseMovement((float)xpos, (float)ypos); }
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) { camera.ProcessMouseScroll((float)yoffset); }

static void UploadIdentityBones(Shader& shader)
{
    for (int i = 0; i < 100; ++i)
    {
        shader.setMat4("bones[" + std::to_string(i) + "]", glm::mat4(1.0f));
    }
}

// ------------------- Cube vertices -------------------
static float cubeVertices[] = {
    // positions         // normals        // uvs
   -0.5f,-0.5f,-0.5f,  0.0f,0.0f,-1.0f,  0.0f,0.0f,
    0.5f,-0.5f,-0.5f,  0.0f,0.0f,-1.0f,  1.0f,0.0f,
    0.5f, 0.5f,-0.5f,  0.0f,0.0f,-1.0f,  1.0f,1.0f,
    0.5f, 0.5f,-0.5f,  0.0f,0.0f,-1.0f,  1.0f,1.0f,
   -0.5f, 0.5f,-0.5f,  0.0f,0.0f,-1.0f,  0.0f,1.0f,
   -0.5f,-0.5f,-0.5f,  0.0f,0.0f,-1.0f,  0.0f,0.0f,

   -0.5f,-0.5f, 0.5f,  0.0f,0.0f,1.0f,   0.0f,0.0f,
    0.5f,-0.5f, 0.5f,  0.0f,0.0f,1.0f,   1.0f,0.0f,
    0.5f, 0.5f, 0.5f,  0.0f,0.0f,1.0f,   1.0f,1.0f,
    0.5f, 0.5f, 0.5f,  0.0f,0.0f,1.0f,   1.0f,1.0f,
   -0.5f, 0.5f, 0.5f,  0.0f,0.0f,1.0f,   0.0f,1.0f,
   -0.5f,-0.5f, 0.5f,  0.0f,0.0f,1.0f,   0.0f,0.0f,

   -0.5f, 0.5f, 0.5f, -1.0f,0.0f,0.0f,   1.0f,0.0f,
   -0.5f, 0.5f,-0.5f, -1.0f,0.0f,0.0f,   1.0f,1.0f,
   -0.5f,-0.5f,-0.5f, -1.0f,0.0f,0.0f,   0.0f,1.0f,
   -0.5f,-0.5f,-0.5f, -1.0f,0.0f,0.0f,   0.0f,1.0f,
   -0.5f,-0.5f, 0.5f, -1.0f,0.0f,0.0f,   0.0f,0.0f,
   -0.5f, 0.5f, 0.5f, -1.0f,0.0f,0.0f,   1.0f,0.0f,

    0.5f, 0.5f, 0.5f,  1.0f,0.0f,0.0f,   1.0f,0.0f,
    0.5f, 0.5f,-0.5f,  1.0f,0.0f,0.0f,   1.0f,1.0f,
    0.5f,-0.5f,-0.5f,  1.0f,0.0f,0.0f,   0.0f,1.0f,
    0.5f,-0.5f,-0.5f,  1.0f,0.0f,0.0f,   0.0f,1.0f,
    0.5f,-0.5f, 0.5f,  1.0f,0.0f,0.0f,   0.0f,0.0f,
    0.5f, 0.5f, 0.5f,  1.0f,0.0f,0.0f,   1.0f,0.0f,

   -0.5f,-0.5f,-0.5f,  0.0f,-1.0f,0.0f,  0.0f,1.0f,
    0.5f,-0.5f,-0.5f,  0.0f,-1.0f,0.0f,  1.0f,1.0f,
    0.5f,-0.5f, 0.5f,  0.0f,-1.0f,0.0f,  1.0f,0.0f,
    0.5f,-0.5f, 0.5f,  0.0f,-1.0f,0.0f,  1.0f,0.0f,
   -0.5f,-0.5f, 0.5f,  0.0f,-1.0f,0.0f,  0.0f,0.0f,
   -0.5f,-0.5f,-0.5f,  0.0f,-1.0f,0.0f,  0.0f,1.0f,

   -0.5f, 0.5f,-0.5f,  0.0f,1.0f,0.0f,   0.0f,1.0f,
    0.5f, 0.5f,-0.5f,  0.0f,1.0f,0.0f,   1.0f,1.0f,
    0.5f, 0.5f, 0.5f,  0.0f,1.0f,0.0f,   1.0f,0.0f,
    0.5f, 0.5f, 0.5f,  0.0f,1.0f,0.0f,   1.0f,0.0f,
   -0.5f, 0.5f, 0.5f,  0.0f,1.0f,0.0f,   0.0f,0.0f,
   -0.5f, 0.5f,-0.5f,  0.0f,1.0f,0.0f,   0.0f,1.0f
};

// ------------------- Shaders -------------------
const char* cubeVertexShaderSrc = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
void main(){ gl_Position = projection * view * model * vec4(aPos,1.0); })";

const char* cubeFragShaderSrc = R"(#version 330 core
out vec4 FragColor;
uniform vec3 diffuseColor;
void main(){ FragColor = vec4(diffuseColor,1.0); })";

const char* modelVertexShaderSrc = R"(#version 330 core
#define MAX_BONES 100

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in ivec4 aBoneIDs;
layout(location = 4) in vec4 aWeights;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat4 bones[MAX_BONES];

out vec2 TexCoords;
out vec3 FragPos;
out vec3 Normal;

void main()
{
    mat4 boneTransform = mat4(1.0);

    for(int i = 0; i < 4; ++i) {
        int id = aBoneIDs[i];
        float w = aWeights[i];
        if(id >= 0) // skip invalid bone IDs
            boneTransform += w * (bones[id] - mat4(0.0));
    }

    vec4 localPos = boneTransform * vec4(aPos, 1.0);
    FragPos = vec3(model * localPos);
    Normal = mat3(transpose(inverse(model))) * aNormal;
    TexCoords = aTexCoords;

    gl_Position = projection * view * model * localPos;
}


)";

const char* modelFragShaderSrc = R"(#version 330 core
out vec4 FragColor;
in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoords;

uniform sampler2D texture_diffuse1;

uniform vec3 lightPos;
uniform vec3 viewPos;

void main(){
    vec3 color = texture(texture_diffuse1, TexCoords).rgb;

    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);

    vec3 ambient = 0.3 * color;
    vec3 diffuse = 3.0 * diff * color;

    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    float spec = pow(max(dot(viewDir, reflectDir),0.0),32.0);
    vec3 specular = vec3(1.0) * spec;

    vec3 result = ambient + diffuse + specular;
    FragColor = vec4(result,1.0);
}
)";


const char* debugVertexShader = R"(#version 330 core
layout(location=0) in vec3 aPos;
uniform mat4 VP;
void main() {
    gl_Position = VP * vec4(aPos,1.0);
}
)";

const char* debugFragShader = R"(#version 330 core
out vec4 FragColor;
void main() {
    FragColor = vec4(1,0,0,1);
}
)";
 
int main()
{
    srand((unsigned int)time(nullptr));

    // ============================================================
    // GLFW + GLAD
    // ============================================================
    if (!glfwInit()) {
        std::cerr << "GLFW init failed\n";
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "ComplexEngine", nullptr, nullptr);
    if (!window) {
        std::cerr << "Window creation failed\n";
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "GLAD init failed\n";
        return -1;
    }

    glEnable(GL_DEPTH_TEST);

    // ============================================================
    // Shaders
    // ============================================================
    Shader cubeShader(cubeVertexShaderSrc, cubeFragShaderSrc);
    Shader modelShader(modelVertexShaderSrc, modelFragShaderSrc);
    Shader debugShader(debugVertexShader, debugFragShader);

    // ============================================================
    // Skybox
    // ============================================================
    std::vector<std::string> faces = {
        "assets/skybox/day/right.jpg",
        "assets/skybox/day/left.jpg",
        "assets/skybox/day/top.jpg",
        "assets/skybox/day/bottom.jpg",
        "assets/skybox/day/front.jpg",
        "assets/skybox/day/back.jpg"
    };
    Skybox sky(faces, faces);

    // ============================================================
    // Physics World
    // ============================================================
    PhysicsWorld physicsWorld;
    physicsWorld.gravity = glm::vec3(0.0f, -9.81f, 0.0f);

    // ============================================================
    // Models
    // ============================================================
    Model boulderModel("models/boulder/BOULDER.gltf");

    Model celandineModel("assets/models/walking.fbx");

    if (!boulderModel.GetScene() || !celandineModel.GetScene()) {
        std::cerr << "Model loading failed\n";
        return -1;
    }

    // ============================================================
    // Animator
    // ============================================================
    Animator animator(&celandineModel.GetSkeleton());
    AnimationStateMachine animSM(&animator);

    animSM.SetAnimations(
        celandineModel.GetAnimation(0),
        celandineModel.GetAnimation(1),
        celandineModel.GetAnimation(2)
    );

    // ============================================================
    // World Bodies
    // ============================================================
    auto floor = std::make_shared<RigidBody>();
    floor->position = {0.0f, -0.5f, 0.0f};
    floor->scale    = {50.0f, 1.0f, 50.0f};
    floor->isStatic = true;
    physicsWorld.addBody(floor);

    auto boulder = std::make_shared<RigidBody>();
    boulder->position = {0.0f, 0.0f, 0.0f};
    boulder->scale    = {1.5f,1.5f,1.5f};
    boulder->mass     = 10.0f;
    boulder->isStatic = true;
    boulder->isModel  = true;
    physicsWorld.addBody(boulder);

    auto celandine = std::make_shared<RigidBody>();
    celandine->position = {5.0f, 0.0f, 0.0f};
    celandine->scale    = {0.1f,0.1f,0.1f};
    celandine->mass     = 10.0f;
    celandine->isModel  = true;
    physicsWorld.addBody(celandine);

    auto playerBody = std::make_shared<RigidBody>(
        glm::vec3(2.0f, 0.31f, 2.0f),
        glm::vec3(0.6f),
        80.0f,
        false
    );
    playerBody->isPlayer = true;
    playerBody->isModel  = true;
    physicsWorld.addBody(playerBody);

    CharacterController character(playerBody, &physicsWorld);

    // ============================================================
    // Cube Geometry
    // ============================================================
    GLuint cubeVAO, cubeVBO;
    glGenVertexArrays(1, &cubeVAO);
    glGenBuffers(1, &cubeVBO);

    glBindVertexArray(cubeVAO);
    glBindBuffer(GL_ARRAY_BUFFER, cubeVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVertices), cubeVertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));

    glBindVertexArray(0);

    // ============================================================
    // Debug Skeleton Lines
    // ============================================================
    GLuint debugVAO, debugVBO;
    glGenVertexArrays(1, &debugVAO);
    glGenBuffers(1, &debugVBO);

    glBindVertexArray(debugVAO);
    glBindBuffer(GL_ARRAY_BUFFER, debugVBO);
    glBufferData(GL_ARRAY_BUFFER, 10000 * sizeof(DebugLine), nullptr, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(DebugLine), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(DebugLine), (void*)(sizeof(glm::vec3)));

    glBindVertexArray(0);

    // ============================================================
    // Timing
    // ============================================================
    float lastTime = (float)glfwGetTime();
    float accumulator = 0.0f;
    constexpr float fixedDt = 1.0f / 120.0f;
    bool DEBUG_ANIMATOR = false;

    // ============================================================
    // MAIN LOOP
    // ============================================================
    while (!glfwWindowShouldClose(window))
    {
        float now = (float)glfwGetTime();
        float dt  = glm::min(now - lastTime, 0.05f);
        lastTime  = now;

        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);

        // ---------------- Input ----------------
        glm::vec3 moveDir(0.0f);
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) moveDir.z -= 1.0f;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) moveDir.z += 1.0f;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) moveDir.x -= 1.0f;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) moveDir.x += 1.0f;

        character.setMoveInput(moveDir);
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
            character.jump();

        // ---------------- Fixed Update ----------------
        accumulator += dt;
        while (accumulator >= fixedDt)
        {
            character.update(fixedDt);

            float speed = glm::length(glm::vec2(
                playerBody->velocity.x,
                playerBody->velocity.z
            ));

            animSM.Update(fixedDt, speed);
            animator.Update(fixedDt);

            if (!DEBUG_ANIMATOR) {
                glm::vec3 rootDelta = animator.ConsumeRootMotion();
                if (glm::length(rootDelta) > 0.0f)
                    celandine->velocity += rootDelta / fixedDt;
            }

            physicsWorld.step(fixedDt);
            accumulator -= fixedDt;
        }

        float alpha = accumulator / fixedDt;

        // ---------------- Camera ----------------
        glm::vec3 camPos = glm::mix(
            playerBody->prevPosition,
            playerBody->position,
            alpha
        );
        camera.FollowPlayerSmooth(camPos, fixedDt);

        glm::mat4 proj = glm::perspective(
            glm::radians(camera.Zoom),
            1280.f / 720.f,
            0.1f,
            200.f
        );
        glm::mat4 view = camera.GetViewMatrix();

        // ---------------- Render ----------------
        glClearColor(0.12f, 0.14f, 0.17f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        sky.render(view, proj);

        // ---------------- Debug Skeleton ----------------
        std::vector<DebugLine> boneLines;
        
        auto& skel = celandineModel.GetSkeleton();
        std::cout<< "ROOT NAME:"<< skel.rootNode.name << std::endl;
        std::cout<<"ROOT CHILD COUNT:"<<skel.rootNode.children.size()<<std::endl;

        glm::mat4 modelMat =
        glm::translate(glm::mat4(1.0f), celandine->position) *
        glm::scale(glm::mat4(1.0f), celandine->scale);
    
    BuildSkeletonLines(
        skel.rootNode,
        modelMat,
        -1,
        skel,
        animator.GetFinalBoneMatrices(),
        boneLines
    );


    std::cout<<"boneLines size after build :" <<boneLines.size() <<std::endl;
      // 1. Correct the empty check logic
if (boneLines.empty()) {
    std::cout << "boneLines : Empty" << std::endl; 
} else {
    std::cout << "boneLines : Not Empty, Size: " << boneLines.size() << std::endl;

    // 2. Access the individual members a and b
    for (const auto& line : boneLines) {
        // 'line' is a single DebugLine struct, not a vector.
        // Use .a and .b to access its vec3 positions.
        std::cout << "Line Start: (" << line.a.x << ", " << line.a.y << ", " << line.a.z << ") "
                  << "End: (" << line.b.x << ", " << line.b.y << ", " << line.b.z << ")" 
                  << std::endl;
    }
}



        glBindVertexArray(debugVAO);
        glBufferData(GL_ARRAY_BUFFER,
            boneLines.size() * sizeof(DebugLine),
            boneLines.data(),
            GL_DYNAMIC_DRAW);

        debugShader.use();
        debugShader.setMat4("VP", proj * view);
        glDrawArrays(GL_LINES, 0, (GLsizei)boneLines.size() * 2);

        // ---------------- World ----------------
        auto interp = [&](const std::shared_ptr<RigidBody>& b) {
            glm::vec3 p = glm::mix(b->prevPosition, b->position, alpha);
            glm::quat r = glm::normalize(glm::slerp(b->prevRotation, b->rotation, alpha));
            return glm::translate(glm::mat4(1), p) *
                   glm::mat4_cast(r) *
                   glm::scale(glm::mat4(1), b->scale);
        };

        cubeShader.use();
        cubeShader.setMat4("view", view);
        cubeShader.setMat4("projection", proj);

        cubeShader.setVec3("diffuseColor", {0.3f, 0.8f, 0.3f});
        cubeShader.setMat4("model", interp(floor));
        glBindVertexArray(cubeVAO);
        glDrawArrays(GL_TRIANGLES, 0, 36);

        cubeShader.setVec3("diffuseColor", {0.8f, 0.3f, 0.3f});
        cubeShader.setMat4("model", interp(playerBody));
        glDrawArrays(GL_TRIANGLES, 0, 36);

        // ---------------- Models ----------------
        modelShader.use();
        modelShader.setMat4("view", view);
        modelShader.setMat4("projection", proj);
        modelShader.setVec3("viewPos", camera.Position);
        modelShader.setVec3("lightPos", {20, 40, 20});

        UploadIdentityBones(modelShader);
        modelShader.setMat4("model", interp(boulder));
        boulderModel.Draw(modelShader);

        animator.Upload(modelShader);
        modelShader.setMat4("model",
            glm::translate(glm::mat4(1), celandine->position));
        celandineModel.Draw(modelShader);

        glfwSwapBuffers(window);
    }

    // ============================================================
    // Cleanup
    // ============================================================
    sky.cleanup();
    glDeleteVertexArrays(1, &cubeVAO);
    glDeleteBuffers(1, &cubeVBO);
    glfwDestroyWindow(window);
    glfwTerminate();
}


