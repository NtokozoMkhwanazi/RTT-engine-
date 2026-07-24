// Minimal test: load bot.fbx + animation and render into a viewport-style FBO.
// This bypasses the full editor/geospatial/world systems to verify the model
// rendering path quickly.

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <vector>

#include "modelSystem/Model.h"
#include "animationSystem/Animator.h"
#include "animationSystem/Animation.h"
#include "shaderSystem/Shader.h"

int main() {
    if (!glfwInit()) {
        std::cerr << "Failed to init GLFW\n";
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "bot viewport minimal", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create window\n";
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to init GLAD\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    std::cout << "OpenGL: " << glGetString(GL_VERSION) << "\n";

    // Viewport FBO
    GLuint fbo, colorTex, rbo;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &colorTex);
    glGenRenderbuffers(1, &rbo);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1280, 720, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1280, 720);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "FBO incomplete\n";
        return 1;
    }

    // Load model (heap allocated so we can destroy before GLFW teardown)
    std::cout << "[BotMinimal] Loading assets/bot.fbx ...\n";
    Model* bot = new Model("assets/bot.fbx");
    if (bot->GetMeshCount() == 0) {
        std::cerr << "[BotMinimal] Failed to load bot.fbx\n";
        delete bot;
        return 1;
    }
    std::cout << "[BotMinimal] Loaded: meshes=" << bot->GetMeshCount()
              << " animations=" << bot->GetAnimationCount() << "\n";

    Animator* animator = new Animator(&bot->GetSkeleton());
    if (bot->GetAnimationCount() > 0) {
        animator->Play(bot->GetAnimation(0));
    }

    // Load shaders
    Shader shader("shaderSystem/VS.glsl", "shaderSystem/FS.glsl");
    if (shader.ID == 0) {
        std::cerr << "[BotMinimal] Failed to load shader\n";
        delete animator;
        delete bot;
        return 1;
    }

    glm::mat4 projection = glm::perspective(glm::radians(45.0f), 1280.0f / 720.0f, 0.1f, 1000.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 1.5f, 4.0f),
                                 glm::vec3(0.0f, 1.0f, 0.0f),
                                 glm::vec3(0.0f, 1.0f, 0.0f));

    // Dummy bone UBO: VS.glsl declares a uniform block at binding=3, so a buffer
    // must always be bound there even when uBoneBufferEnabled=0.
    GLuint dummyBoneUBO = 0;
    {
        glm::mat4 identity = glm::mat4(1.0f);
        glGenBuffers(1, &dummyBoneUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, dummyBoneUBO);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(identity), &identity[0][0], GL_STATIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 3, dummyBoneUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

    glEnable(GL_DEPTH_TEST);

    float lastTime = static_cast<float>(glfwGetTime());
    for (int frame = 0; frame < 10; ++frame) {
        float now = static_cast<float>(glfwGetTime());
        float dt = now - lastTime;
        lastTime = now;

        // Drain stale errors
        while (glGetError() != GL_NO_ERROR) {}

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, 1280, 720);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Position the bot in front of the camera, scale up so it is visible.
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.95f, 0.0f));
        model = glm::scale(model, glm::vec3(0.08f));
        model = glm::rotate(model, glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));

        shader.use();
        GLenum errAfterUse = glGetError();
        shader.setMat4("projection", projection);
        shader.setMat4("view", view);
        shader.setMat4("model", model);
        shader.setInt("uDisableInstancing", 1);
        shader.setInt("uDisableSkinning", 1);  // disable skinning for visibility test
        shader.setInt("uBoneBufferEnabled", 0); // use texture fallback path (no UBO)
        shader.setInt("uPaletteSize", 256);
        shader.setInt("uShowDebug", 1);  // ignore lighting, show geometry color
        shader.setVec3("lightPos", glm::vec3(10.0f, 10.0f, 10.0f));
        shader.setVec3("viewPos", glm::vec3(0.0f, 1.5f, 4.0f));

        GLenum errBeforeDraw = glGetError();

        if (bot->GetAnimationCount() > 0) {
            animator->Update(dt);
            bot->Draw(shader, *animator);
        } else {
            bot->DrawStatic(shader);
        }

        GLenum err = glGetError();
        std::cout << "[BotMinimal] frame=" << frame
                  << " errUse=0x" << std::hex << errAfterUse
                  << " errBeforeDraw=0x" << errBeforeDraw
                  << " errAfterDraw=0x" << err
                  << std::dec
                  << " meshes=" << bot->GetMeshCount() << "\n";

        // Read center pixel
        std::vector<unsigned char> px(4, 0);
        glReadPixels(640, 360, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        std::cout << "[BotMinimal] center pixel=(" << (int)px[0] << "," << (int)px[1]
                  << "," << (int)px[2] << "," << (int)px[3] << ")\n";
        std::cout.flush();

        glfwPollEvents();
    }

    // Destroy model/animator while GL context is still alive.
    // Model::~Model calls BoneMatrixBuffer::Shutdown which issues GL calls.
    delete animator;
    delete bot;

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &colorTex);
    glDeleteRenderbuffers(1, &rbo);
    glDeleteBuffers(1, &dummyBoneUBO);
    glfwDestroyWindow(window);
    glfwTerminate();
    std::cout << "[BotMinimal] Done\n";
    return 0;
}
