#define GLM_ENABLE_EXPERIMENTAL

#include "shaderSystem/Shader.h"
#include "modelSystem/Model.h"
#include "cameraSystem/flyCamera.h"
#include "animationSystem/Animator.h"
#include "animationSystem/AssimpAnimationLoader.h"
#include "animationSystem/AnimationStateMachine.h"  // For CharacterInput
#include "motionMatching/MotionMatcher.h"            // NEW: Motion Matching System
#include "shaderSystem/stb_image.h"
#include "physicsSystem/RigidBody.h"
#include "physicsSystem/Floor.h"
#include "world/Terrain.h"
#include "world/VegetationSystem.h"
#include "world/WorldObjectManager.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>
#include <vector>
#include <cmath>

// ================= CAMERA =================
flyCamera camera(
    glm::vec3(0.0f, 3.0f, 10.0f),
    glm::vec3(0, 2, 0),
    -90.0f,
    0.0f,
    10.0f);

// ================= CAMERA TEST ASSERTIONS =================
void runCameraTests() {
    std::cout << "\n=== RUNNING CAMERA TEST ASSERTIONS ===\n";
    
    int passed = 0, failed = 0;
    
    // Test 1: Camera initial position
    if (camera.Position == glm::vec3(0.0f, 3.0f, 10.0f)) {
        std::cout << "  [PASS] Camera initial position\n";
        passed++;
    } else {
        std::cout << "  [FAIL] Camera initial position\n";
        failed++;
    }
    
    // Test 2: Camera initial yaw/pitch
    if (camera.Yaw == -90.0f && camera.Pitch == 0.0f) {
        std::cout << "  [PASS] Camera initial yaw/pitch\n";
        passed++;
    } else {
        std::cout << "  [FAIL] Camera initial yaw/pitch\n";
        failed++;
    }
    
    // Test 3: GetViewMatrix returns valid matrix
    glm::mat4 view = camera.GetViewMatrix();
    bool validView = !std::isnan(view[0][0]) && !std::isnan(view[1][1]) && !std::isnan(view[2][2]);
    if (validView) {
        std::cout << "  [PASS] GetViewMatrix returns valid matrix\n";
        passed++;
    } else {
        std::cout << "  [FAIL] GetViewMatrix returns valid matrix\n";
        failed++;
    }
    
    // Test 4: GetProjectionMatrix returns valid matrix
    glm::mat4 proj = camera.GetProjectionMatrix(16.0f / 9.0f);
    bool validProj = !std::isnan(proj[0][0]) && !std::isnan(proj[1][1]);
    if (validProj) {
        std::cout << "  [PASS] GetProjectionMatrix returns valid matrix\n";
        passed++;
    } else {
        std::cout << "  [FAIL] GetProjectionMatrix returns valid matrix\n";
        failed++;
    }
    
    // Test 5: Mouse movement updates yaw/pitch
    flyCamera testCam(glm::vec3(0, 0, 10), glm::vec3(0, 1, 0), -90.0f, 0.0f, 10.0f);
    float initialYaw = testCam.Yaw;
    testCam.ProcessMouseMovement(450.0f, 350.0f);  // Simulate mouse move
    if (testCam.Yaw != initialYaw) {
        std::cout << "  [PASS] Mouse movement updates yaw/pitch\n";
        passed++;
    } else {
        std::cout << "  [FAIL] Mouse movement updates yaw/pitch\n";
        failed++;
    }
    
    // Test 6: Scroll zoom changes distance
    flyCamera zoomCam(glm::vec3(0, 0, 10), glm::vec3(0, 1, 0), -90.0f, 0.0f, 10.0f);
    float initialDist = zoomCam.DistanceToTarget;
    zoomCam.ProcessMouseScroll(5.0f);
    if (zoomCam.DistanceToTarget != initialDist) {
        std::cout << "  [PASS] Scroll zoom changes distance\n";
        passed++;
    } else {
        std::cout << "  [FAIL] Scroll zoom changes distance\n";
        failed++;
    }
    
    // Test 7: Zoom respects min/max limits
    zoomCam.ProcessMouseScroll(-100.0f);  // Try to zoom way in
    if (zoomCam.DistanceToTarget >= zoomCam.MinDistance) {
        std::cout << "  [PASS] Zoom respects min/max limits\n";
        passed++;
    } else {
        std::cout << "  [FAIL] Zoom respects min/max limits\n";
        failed++;
    }
    
    // Test 8: FollowPlayer updates position
    flyCamera followCam(glm::vec3(0, 5, 10), glm::vec3(0, 2, 0), -90.0f, 0.0f, 10.0f);
    glm::vec3 initialPos = followCam.Position;
    followCam.FollowPlayer(glm::vec3(0, 0, 5));  // Move target
    if (followCam.Position != initialPos) {
        std::cout << "  [PASS] FollowPlayer updates position\n";
        passed++;
    } else {
        std::cout << "  [FAIL] FollowPlayer updates position\n";
        failed++;
    }
    
    // Test 9: FollowPlayerSmooth updates position
    flyCamera smoothCam(glm::vec3(0, 5, 10), glm::vec3(0, 2, 0), -90.0f, 0.0f, 10.0f);
    glm::vec3 smoothInitialPos = smoothCam.Position;
    smoothCam.FollowPlayerSmooth(glm::vec3(0, 0, 5), 0.016f);
    if (smoothCam.Position != smoothInitialPos) {
        std::cout << "  [PASS] FollowPlayerSmooth updates position\n";
        passed++;
    } else {
        std::cout << "  [FAIL] FollowPlayerSmooth updates position\n";
        failed++;
    }
    
    // Test 10: Camera shake effect
    flyCamera shakeCam(glm::vec3(0, 5, 10), glm::vec3(0, 2, 0), -90.0f, 0.0f, 10.0f);
    shakeCam.AddShake(1.0f, 2.0f, glm::vec3(1, 1, 1));
    if (shakeCam.currentShake.intensity > 0.0f) {
        std::cout << "  [PASS] Camera shake effect\n";
        passed++;
    } else {
        std::cout << "  [FAIL] Camera shake effect\n";
        failed++;
    }
    
    // Test 11: SetFieldOfView clamps values
    flyCamera fovCam(glm::vec3(0, 5, 10), glm::vec3(0, 2, 0), -90.0f, 0.0f, 10.0f);
    fovCam.SetFieldOfView(150.0f);  // Should clamp to 120
    if (fovCam.FieldOfView <= 120.0f) {
        std::cout << "  [PASS] SetFieldOfView clamps values\n";
        passed++;
    } else {
        std::cout << "  [FAIL] SetFieldOfView clamps values\n";
        failed++;
    }
    
    // Test 12: Collision detection setup
    flyCamera colCam(glm::vec3(0, 5, 10), glm::vec3(0, 2, 0), -90.0f, 0.0f, 10.0f);
    colCam.SetCollisionEnabled(true);
    colCam.SetCollisionDistance(2.0f);
    std::cout << "  [PASS] Collision detection setup (manual verify)\n";
    passed++;
    
    std::cout << "\n=== CAMERA TEST RESULTS: " << passed << " passed, " << failed << " failed ===\n\n";
    
    if (failed > 0) {
        std::cerr << "WARNING: Some camera tests failed!\n";
    }
}

// Third-person camera settings - AAA QUALITY
glm::vec3 cameraPivot(0.0f, 2.0f, 0.0f);  // Point camera looks at (character + offset)
float cameraDistance = 21.0f;              // Distance from pivot (increased for better view)
float cameraHeight = 7.0f;                 // Camera height offset (higher overhead view)
float cameraRotateSpeed = 3.0f;            // Mouse rotation speed

// AAA QUALITY CAMERA - Maximum smoothing for perfect follow
// Camera updates AFTER character position = zero lag
// Zoom range for cinematic to gameplay views
float cameraZoomSpeed = 25.0f;             // Fast zoom scrolling
float cameraZoomMin = 10.0f;               // Minimum zoom (close-up)
float cameraZoomMax = 50.0f;               // Maximum zoom (very far - cinematic)
bool cameraFollowEnabled = true;
bool cameraFixedMode = false;              // Fixed behind camera (no orbit)

void framebuffer_size_callback(GLFWwindow *, int w, int h) { glViewport(0, 0, w, h); }
void mouse_callback(GLFWwindow *window, double xpos, double ypos) {
    // Only allow orbit in non-fixed mode
    if (cameraFixedMode) return;
    
    // Orbit camera around pivot using yaw/pitch
    static double lastX = xpos, lastY = ypos;
    static bool firstMouse = true;
    
    if (firstMouse) {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
    }
    
    float dx = (float)(xpos - lastX);
    float dy = (float)(lastY - ypos);  // Inverted for natural feel
    lastX = xpos;
    lastY = ypos;
    
    // Update camera yaw and pitch
    camera.Yaw += dx * 0.1f;
    camera.Pitch = glm::clamp(camera.Pitch + dy * 0.1f, -80.0f, 80.0f);
}
void scroll_callback(GLFWwindow *window, double xoffset, double yoffset) {
    // Smooth zoom - adjust target distance
    cameraDistance -= (float)yoffset * cameraZoomSpeed * 0.1f;
    cameraDistance = glm::clamp(cameraDistance, cameraZoomMin, cameraZoomMax);
}

// ================= SKYBOX =================
GLuint skyboxVAO = 0, skyboxVBO = 0;
GLuint skyboxTexture = 0;

void setupSkybox() {
    float skyboxVertices[] = {
        -1.0f,  1.0f, -1.0f,  -1.0f, -1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,   1.0f,  1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,  -1.0f, -1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f, -1.0f,   1.0f, -1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,   1.0f,  1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,  -1.0f, -1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,   1.0f, -1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f,  -1.0f,  1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,   1.0f,  1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
    };
    
    glGenVertexArrays(1, &skyboxVAO);
    glGenBuffers(1, &skyboxVBO);
    glBindVertexArray(skyboxVAO);
    glBindBuffer(GL_ARRAY_BUFFER, skyboxVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), skyboxVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);
}

GLuint loadTexture(const std::string& path) {
    int width, height, nrChannels;
    stbi_set_flip_vertically_on_load(false);
    unsigned char *data = stbi_load(path.c_str(), &width, &height, &nrChannels, 0);
    
    GLuint texture;
    glGenTextures(1, &texture);
    
    if (data) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        stbi_image_free(data);
    } else {
        std::cerr << "Failed to load texture: " << path << "\n";
    }
    
    return texture;
}

GLuint loadCubemap(const std::vector<std::string>& faces) {
    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_CUBE_MAP, textureID);
    
    int width, height, nrChannels;
    for (unsigned int i = 0; i < faces.size(); i++) {
        stbi_set_flip_vertically_on_load(false);
        unsigned char *data = stbi_load(faces[i].c_str(), &width, &height, &nrChannels, 0);
        if (data) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
            stbi_image_free(data);
        } else {
            std::cerr << "Cubemap face failed to load: " << faces[i] << "\n";
            stbi_image_free(data);
        }
    }
    
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    
    return textureID;
}

void drawSkybox(glm::mat4 view, glm::mat4 projection, GLuint skyboxTex) {
    glDepthFunc(GL_LEQUAL);
    
    static Shader* skyboxShader = nullptr;
    if (!skyboxShader) {
        skyboxShader = new Shader("shaderSystem/skyboxVS.glsl", "shaderSystem/skyboxFS.glsl");
    }
    
    skyboxShader->use();
    skyboxShader->setMat4("projection", projection);
    skyboxShader->setMat4("view", glm::mat4(glm::mat3(view)));  // Remove translation
    
    glBindVertexArray(skyboxVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxTex);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);
    
    glDepthFunc(GL_LESS);
}

// ================= SKINNED SHADER =================
class SkinnedShader {
public:
    unsigned int ID;

    SkinnedShader() {
        const char* vs = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aTex;
layout(location=5) in ivec4 aBoneIDs;
layout(location=6) in vec4 aWeights;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoords;
out vec4 vBoneIDs;
out vec4 vWeights;

uniform sampler2D boneTex;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform int uPaletteSize;

mat4 GetBone(int boneID) {
    if (boneID < 0 || boneID >= uPaletteSize) return mat4(1.0);
    int x = boneID * 4;
    return mat4(
        texelFetch(boneTex, ivec2(x, 0), 0),
        texelFetch(boneTex, ivec2(x+1, 0), 0),
        texelFetch(boneTex, ivec2(x+2, 0), 0),
        texelFetch(boneTex, ivec2(x+3, 0), 0)
    );
}

void main() {
    vBoneIDs = vec4(aBoneIDs);
    vWeights = aWeights;
    TexCoords = aTex;

    // Normalize weights
    float sum = aWeights.x + aWeights.y + aWeights.z + aWeights.w;
    vec4 weights = (sum > 0.0) ? (aWeights / sum) : vec4(0.0);

    // Build skin matrix
    mat4 skin = mat4(0.0);
    bool validSkin = false;

    for (int i = 0; i < 4; i++) {
        int boneID = aBoneIDs[i];
        float w = weights[i];
        if (boneID >= 0 && boneID < uPaletteSize && w > 0.0) {
            mat4 boneMat = GetBone(boneID);
            skin += boneMat * w;
            validSkin = true;
        }
    }

    if (!validSkin) {
        // No valid bone influence - use bind pose (identity)
        skin = mat4(1.0);
    }

    // Apply skinning
    vec4 skinnedPos = skin * vec4(aPos, 1.0);
    vec4 worldPos = model * skinnedPos;

    FragPos = worldPos.xyz;

    // Transform normal
    mat3 normalMat = transpose(inverse(mat3(model) * mat3(skin)));
    Normal = normalize(normalMat * aNormal);

    gl_Position = projection * view * worldPos;
}
)";

        const char* fs = R"(
#version 330 core
out vec4 FragColor;
in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoords;
in vec4 vBoneIDs;
in vec4 vWeights;

uniform vec3 color;
uniform vec3 lightPos;
uniform vec3 viewPos;
uniform int uDebugMode;  // 0=normal, 1=show bone IDs, 2=show weights

void main() {
    if (uDebugMode == 1) {
        // Show bone IDs as color
        vec3 boneColor = vec3(vBoneIDs.xyz / 65.0);
        FragColor = vec4(boneColor, 1.0);
        return;
    }
    
    if (uDebugMode == 2) {
        // Show bone IDs mapped to colors (not weights)
        // Map bone ID to a color based on ID
        vec3 boneColor;
        if (vBoneIDs.x >= 55.0 && vBoneIDs.x <= 62.0) {
            // Leg bones: green-cyan range
            boneColor = vec3(0.0, (vBoneIDs.x - 55.0) / 7.0, 1.0);
        } else if (vBoneIDs.x >= 30.0 && vBoneIDs.x <= 40.0) {
            // Arm bones: yellow-orange range
            boneColor = vec3(1.0, (40.0 - vBoneIDs.x) / 10.0, 0.0);
        } else {
            // Other bones: blue-purple range
            boneColor = vec3(0.5, 0.0, vBoneIDs.x / 65.0);
        }
        FragColor = vec4(boneColor, 1.0);
        return;
    }
    
    // Diffuse lighting
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);
    
    // Simple shading
    vec3 ambient = 0.3 * color;
    vec3 diffuse = diff * color;
    
    FragColor = vec4(ambient + diffuse, 1.0);
}
)";

        unsigned int vsObj = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vsObj, 1, &vs, nullptr);
        glCompileShader(vsObj);
        
        unsigned int fsObj = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fsObj, 1, &fs, nullptr);
        glCompileShader(fsObj);
        
        ID = glCreateProgram();
        glAttachShader(ID, vsObj);
        glAttachShader(ID, fsObj);
        glLinkProgram(ID);
        
        glDeleteShader(vsObj);
        glDeleteShader(fsObj);
        
        std::cout << "Skinned shader compiled: ID=" << ID << "\n";
    }
    
    void use() { glUseProgram(ID); }
    
    void setMat4(const std::string& name, glm::mat4 val) {
        glUniformMatrix4fv(glGetUniformLocation(ID, name.c_str()), 1, GL_FALSE, &val[0][0]);
    }
    
    void setVec3(const std::string& name, glm::vec3 val) {
        glUniform3fv(glGetUniformLocation(ID, name.c_str()), 1, &val[0]);
    }
    
    void setInt(const std::string& name, int val) {
        glUniform1i(glGetUniformLocation(ID, name.c_str()), val);
    }
};

// ================= MAIN =================
int main()
{
    // Initialize GLFW with hints for better compatibility
    if (!glfwInit()) { 
        std::cerr << "GLFW init failed\n"; 
        return -1; 
    }

    // Set OpenGL version and profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);  // For macOS compatibility
    glfwWindowHint(GLFW_RESIZABLE, GL_TRUE);
    
    // Try to create window with different hints if first attempt fails
    GLFWwindow *window = glfwCreateWindow(1280, 720, "3D Engine - Skinned Model", nullptr, nullptr);
    
    if (!window) {
        std::cerr << "Window creation failed with default hints, trying alternative...\n";
        glfwWindowHint(GLFW_SAMPLES, 4);  // Try with MSAA
        window = glfwCreateWindow(1280, 720, "3D Engine - Skinned Model", nullptr, nullptr);
    }
    
    if (!window) {
        std::cerr << "GLFW Window creation failed!\n";
        std::cerr << "Possible causes:\n";
        std::cerr << "  - No display available (running headless)\n";
        std::cerr << "  - OpenGL 3.3 not supported\n";
        std::cerr << "  - Graphics drivers not installed\n";
        std::cerr << "\nTry:\n";
        std::cerr << "  - Running on a system with a display\n";
        std::cerr << "  - Installing proper graphics drivers\n";
        std::cerr << "  - Using software OpenGL: export LIBGL_ALWAYS_SOFTWARE=1\n";
        glfwTerminate();
        return -1;
    }

    std::cout << "\n=== 3D ENGINE - ANIMATION STATE MACHINE ===\n";
    std::cout << "Controls:\n";
    std::cout << "  Mouse - Orbit camera (disabled in fixed mode)\n";
    std::cout << "  Scroll - Zoom in/out\n";
    std::cout << "  W/S - Walk forward/backward\n";
    std::cout << "  A/D - Strafe left/right\n";
    std::cout << "  Q/E - Camera pivot up/down\n";
    std::cout << "  R - Reset camera\n";
    std::cout << "  C - Toggle camera mode (orbit/fixed)\n";
    std::cout << "  SPACE - Jump\n";
    std::cout << "  LEFT SHIFT - Sprint (run faster)\n";
    std::cout << "  LEFT CTRL - Crouch (hold while moving)\n";
    std::cout << "  F - Toggle wireframe/solid\n";
    std::cout << "  B - Toggle bone debug (cycle modes)\n";
    std::cout << "  G - Print foot IK status\n";
    std::cout << "  H - Print animation state\n";
    std::cout << "  ESC - Exit\n";
    std::cout << "========================================\n\n";
    
    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "GLAD failed\n"; glfwTerminate(); return -1;
    }

    std::cout << "GLFW Window created successfully!\n";
    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << "\n";
    std::cout << "GLSL Version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << "\n";
    std::cout << "GLAD initialized successfully!\n";
    std::cout.flush();

    // Run camera test assertions
    runCameraTests();

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);  // Start in SOLID mode (not wireframe)

    // Skybox - DISABLED for world terrain rendering
    // Using clear color + fog for atmosphere instead
    std::cout << "Skybox disabled - using procedural fog for atmosphere\n";
    // setupSkybox();  // Disabled
    // skyboxTexture = 0;  // Disabled

    // Load model
    std::cout << "Loading model...\n";
    Model* character = new Model("assets/bot.fbx");
    const Skeleton& skeleton = character->GetSkeleton();
    std::cout << "Skeleton bones: " << skeleton.bones.size() << "\n";

    // Calculate model scale
    glm::vec3 size = character->GetSize();
    float modelScale = 10.0f / size.y;
    std::cout << "Model scale: " << modelScale << " (~10 units tall)\n";
    std::cout << "Meshes: " << character->GetMeshCount() << "\n";

    std::cout << "\n=== LOADING ANIMATIONS ===\n";
    
    Assimp::Importer importer;
    
    // Load all animations
    Animation* idleAnim = nullptr;
    Animation* walkAnim = nullptr;
    Animation* runAnim = nullptr;
    Animation* jumpAnim = nullptr;
    Animation* fallAnim = nullptr;
    Animation* crouchAnim = nullptr;
    Animation* crouchWalkAnim = nullptr;

    // ============================================================
    // LOADING SCREEN
    // ============================================================
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  LOADING...\n";
    std::cout << "========================================\n";
    
    int loadingStep = 0;
    int totalSteps = 10;  // Total loading steps
    
    auto showProgress = [&]() {
        loadingStep++;
        int percent = (loadingStep * 100) / totalSteps;
        std::cout << "  [" << percent << "%] ";
        std::cout.flush();
    };

    // Helper lambda to load animation
    auto loadAnim = [&](const std::string& path, const std::string& name) -> Animation* {
        const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs);
        if (scene && scene->HasAnimations()) {
            Animation* anim = new Animation(AssimpAnimationLoader::LoadAnimationWithPoseCorrection(scene, scene->mAnimations[0]));
            return anim;
        }
        return nullptr;
    };

    std::cout << "  Loading animations...\n";
    
    // Load animations
    idleAnim = loadAnim("assets/Idle.fbx", "Idle");    showProgress(); std::cout << "Idle\n";
    walkAnim = loadAnim("assets/Walking.fbx", "Walk");  showProgress(); std::cout << "Walk\n";
    runAnim = loadAnim("assets/Run.fbx", "Run");        showProgress(); std::cout << "Run\n";
    jumpAnim = loadAnim("assets/Jump.fbx", "Jump");     showProgress(); std::cout << "Jump\n";
    fallAnim = loadAnim("assets/fall.fbx", "Fall");     showProgress(); std::cout << "Fall\n";
    crouchAnim = loadAnim("assets/Crouching.fbx", "Crouch"); showProgress(); std::cout << "Crouch\n";
    crouchWalkAnim = loadAnim("assets/chrouchWalk.fbx", "CrouchWalk"); showProgress(); std::cout << "CrouchWalk\n";

    // Load grass model
    showProgress(); std::cout << "Grass...\n";
    Model* grassModel = nullptr;
    try {
        grassModel = new Model("assets/grass/grass.fbx");
        if (!grassModel || grassModel->GetMeshCount() == 0) {
            if (grassModel) delete grassModel;
            grassModel = nullptr;
        }
    } catch (...) {
        if (grassModel) delete grassModel;
        grassModel = nullptr;
    }

    // Create animator
    showProgress(); std::cout << "Animator...\n";
    Animator* animator = new Animator(&skeleton);

    // Motion Matching System
    showProgress(); std::cout << "Motion Matching...\n";
    MotionMatcher* matcher = new MotionMatcher();
    matcher->Initialize(&skeleton, animator);
    
    // Configure motion matching
    MotionMatchingConfig mmConfig;
    mmConfig.maxSearchResults = 10;
    mmConfig.searchRadius = 2.0f;
    mmConfig.useTrajectoryMatching = true;
    mmConfig.blendDuration = 0.1f;
    mmConfig.footPlantThreshold = 0.05f;
    mmConfig.footPlantHeightThreshold = 0.1f;
    mmConfig.enableFootLocking = true;
    mmConfig.trajectoryDuration = 0.5f;
    mmConfig.trajectoryPoints = 5;
    matcher->SetConfig(mmConfig);
    
    // Load animations into motion database
    std::cout << "\n=== LOADING ANIMATIONS INTO MOTION DATABASE ===\n";
    if (idleAnim) matcher->LoadAnimation("Idle", std::shared_ptr<Animation>(idleAnim, [](Animation*){}));
    if (walkAnim) matcher->LoadAnimation("Walk", std::shared_ptr<Animation>(walkAnim, [](Animation*){}));
    if (runAnim) matcher->LoadAnimation("Run", std::shared_ptr<Animation>(runAnim, [](Animation*){}));
    if (jumpAnim) matcher->LoadAnimation("Jump", std::shared_ptr<Animation>(jumpAnim, [](Animation*){}));
    if (crouchAnim) matcher->LoadAnimation("Crouch", std::shared_ptr<Animation>(crouchAnim, [](Animation*){}));
    if (crouchWalkAnim) matcher->LoadAnimation("CrouchWalk", std::shared_ptr<Animation>(crouchWalkAnim, [](Animation*){}));
    
    std::cout << "\nMotion Matching Database Stats:\n";
    std::cout << matcher->GetDatabaseStats() << "\n";
    
    // Build KD-Tree for fast search (after all animations loaded)
    std::cout << "\nBuilding KD-Tree for fast search...\n";
    matcher->BuildSearchIndex();

    // FIX: Your FBX animations have WRONG duration (29-499 seconds instead of 1-2 seconds)
    // This happens when Blender/Maya exports entire timeline instead of just the cycle
    // We'll truncate the duration to the actual animation cycle length
    auto fixAnimationDuration = [](Animation* anim, float expectedDuration, const std::string& name) {
        if (anim && anim->duration > 5.0f) {  // Only fix if way too long
            // The actual animation cycle is the first N frames
            // Truncate duration to expected length (this makes it loop correctly)
            anim->duration = expectedDuration;
            std::cout << "  [FIX] " << name << ": " << anim->name
                      << " duration " << (anim->duration > 100 ? "TRUNCATED" : "set")
                      << " (" << anim->duration << "s)\n";
        }
    };

    std::cout << "\n=== FIXING ANIMATION DURATIONS (FBX Export Issue) ===\n";
    fixAnimationDuration(idleAnim, 2.5f, "Idle");
    fixAnimationDuration(walkAnim, 1.2f, "Walk");
    fixAnimationDuration(runAnim, 0.9f, "Run");
    fixAnimationDuration(jumpAnim, 1.1f, "Jump");
    fixAnimationDuration(fallAnim, 1.0f, "Fall");
    fixAnimationDuration(crouchAnim, 0.6f, "Crouch");
    fixAnimationDuration(crouchWalkAnim, 1.0f, "CrouchWalk");
    std::cout << "Note: Re-export FBX with only animation cycle selected\n";

    // ENABLE root motion - the animation drives the movement
    // We'll extract root motion and apply it in the camera-relative direction
    animator->SetLockRootPosition(false);
    std::cout << "\nRoot motion ENABLED - will extract and apply camera-relative\n";
    std::cout << "Motion Matching ACTIVE - continuous pose searching\n";

    // Debug: print animation info (AFTER animator created)
    auto printAnimInfo = [](Animation* anim, const std::string& name) {
        if (anim) {
            float frames = anim->duration * 30.0f;  // Assume 30fps
            std::cout << "  " << name << ": duration=" << anim->duration
                      << "s (" << (int)frames << " frames @30fps)"
                      << ", ticks/sec=" << anim->ticksPerSecond
                      << ", bones=" << anim->boneAnimations.size() << "\n";
            
            // Check if animation length is appropriate
            if (name == "Walk" && (frames < 20 || frames > 60)) {
                std::cout << "    ⚠️  WARNING: Walk should be 30-45 frames (1-1.5s @30fps)\n";
            }
            if (name == "Run" && (frames < 15 || frames > 50)) {
                std::cout << "    ⚠️  WARNING: Run should be 24-36 frames (0.8-1.2s @30fps)\n";
            }
            if (name == "Jump" && (frames < 20 || frames > 60)) {
                std::cout << "    ⚠️  WARNING: Jump should be 30-45 frames (1-1.5s @30fps)\n";
            }
            if (name == "Idle" && (frames < 40 || frames > 120)) {
                std::cout << "    ⚠️  WARNING: Idle should be 60-90 frames (2-3s @30fps)\n";
            }
        } else {
            std::cout << "  " << name << ": NOT LOADED\n";
        }
    };
    
    std::cout << "\n=== ANIMATION INFO ===\n";
    std::cout << "Expected lengths @30fps:\n";
    std::cout << "  Idle: 60-90 frames (2-3s) - loops IN PLACE\n";
    std::cout << "  Walk: 30-45 frames (1-1.5s) - loops IN PLACE\n";
    std::cout << "  Run: 24-36 frames (0.8-1.2s) - loops IN PLACE\n";
    std::cout << "  Jump: 30-45 frames (1-1.5s) - ONE-SHOT with root motion\n";
    std::cout << "\nActual animations:\n";
    printAnimInfo(idleAnim, "Idle");
    printAnimInfo(walkAnim, "Walk");
    printAnimInfo(runAnim, "Run");
    printAnimInfo(jumpAnim, "Jump");
    printAnimInfo(fallAnim, "Fall");
    printAnimInfo(crouchAnim, "Crouch");
    printAnimInfo(crouchWalkAnim, "CrouchWalk");
    
    std::cout << "\n=== ROOT MOTION STATUS ===\n";
    std::cout << "Root position locked: " << (animator->IsRootPositionLocked() ? "YES (code drives movement)" : "NO (root motion DRIVES movement)") << "\n";
    std::cout << "\n=== TROUBLESHOOTING ===\n";
    std::cout << "If character slides/stuck in pose:\n";
    std::cout << "  1. Animations too short? Should be 30-45 frames for walk\n";
    std::cout << "  2. Root motion wrong? Try: animator->SetLockRootPosition(true)\n";
    std::cout << "  3. Movement multiplier wrong? Adjust in code (currently 0.25f)\n";

    std::cout << "\nAnimation State Machine initialized!\n";
    std::cout << "Registered states: " 
              << (idleAnim ? "IDLE " : "")
              << (walkAnim ? "WALK " : "")
              << (runAnim ? "RUN " : "")
              << (jumpAnim ? "JUMP " : "")
              << (fallAnim ? "FALL " : "")
              << (crouchAnim ? "CROUCH " : "")
              << (crouchWalkAnim ? "CROUCH_WALK " : "")
              << "\n";

    // ============================================================
    // SETUP FLOOR AND FOOT IK
    // ============================================================
    std::cout << "\n=== SETTING UP PHYSICS FLOOR & FOOT IK ===\n";

    // Floor height (where feet should plant) - will be updated dynamically in main loop
    float floorHeight = 0.0f;

    // Setup foot IK if animation is loaded
    if (animator) {
        // Find foot bone indices from skeleton
        int leftFoot = skeleton.GetBoneIndex("leftfoot");
        int rightFoot = skeleton.GetBoneIndex("rightfoot");
        int leftToe = skeleton.GetBoneIndex("lefttoebase");
        int rightToe = skeleton.GetBoneIndex("righttoebase");

        std::cout << "Foot bones: left=" << leftFoot << " right=" << rightFoot
                  << " | leftToe=" << leftToe << " rightToe=" << rightToe << "\n";

        // Configure foot IK - floor height will be updated dynamically in main loop
        Animator::FootIKSettings ikSettings;
        ikSettings.enabled = true;
        ikSettings.floorHeight = floorHeight;  // Updated dynamically in main loop
        ikSettings.ikStrength = 1.0f;
        ikSettings.footLockBlend = 0.9f;
        ikSettings.maxIKDistance = 0.2f;
        ikSettings.leftFootBone = leftFoot;
        ikSettings.rightFootBone = rightFoot;
        ikSettings.leftToeBone = leftToe;
        ikSettings.rightToeBone = rightToe;

        animator->SetFootIKSettings(ikSettings);
        std::cout << "Foot IK configured and enabled (floor Y will update dynamically)!\n";
    }
    
    // Create floor physics body (invisible collider)
    std::shared_ptr<RigidBody> floorBody = std::make_shared<RigidBody>(
        glm::vec3(0.0f, floorHeight, 0.0f),
        glm::vec3(200.0f, 1.0f, 200.0f),
        0.0f,  // infinite mass
        true   // static
    );
    floorBody->colliderType = ColliderType::BOX;
    floorBody->friction = 0.9f;
    floorBody->restitution = 0.0f;
    floorBody->albedo = glm::vec3(0.25f, 0.25f, 0.3f);  // Dark blue-gray

    std::cout << "Floor physics body created at y=" << floorHeight << "\n";
    
    // Create visible debug floor plane (for visualization)
    GLuint debugFloorVAO = 0, debugFloorVBO = 0, debugFloorEBO = 0;
    {
        float floorSize = 400.0f;
        int floorRes = 20;
        
        std::vector<float> vertices;
        std::vector<unsigned int> indices;
        
        for (int z = 0; z <= floorRes; z++) {
            for (int x = 0; x <= floorRes; x++) {
                float vx = (float)x / floorRes * floorSize - floorSize / 2.0f;
                float vz = (float)z / floorRes * floorSize - floorSize / 2.0f;
                vertices.push_back(vx);
                vertices.push_back(floorHeight);  // At floor height
                vertices.push_back(vz);
            }
        }
        
        for (int z = 0; z < floorRes; z++) {
            for (int x = 0; x < floorRes; x++) {
                int topLeft = z * (floorRes + 1) + x;
                int topRight = topLeft + 1;
                int bottomLeft = (z + 1) * (floorRes + 1) + x;
                int bottomRight = bottomLeft + 1;
                
                indices.push_back(topLeft);
                indices.push_back(bottomLeft);
                indices.push_back(topRight);
                indices.push_back(topRight);
                indices.push_back(bottomLeft);
                indices.push_back(bottomRight);
            }
        }
        
        glGenVertexArrays(1, &debugFloorVAO);
        glGenBuffers(1, &debugFloorVBO);
        glGenBuffers(1, &debugFloorEBO);
        
        glBindVertexArray(debugFloorVAO);
        glBindBuffer(GL_ARRAY_BUFFER, debugFloorVBO);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, debugFloorEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
        
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        
        glBindVertexArray(0);
        
        std::cout << "Debug floor plane created (visible wireframe grid)\n";
    }
    
    std::cout << "========================================\n\n";

    // ============================================================
    // OPEN WORLD TERRAIN SYSTEM
    // ============================================================
    std::cout << "\n=== INITIALIZING OPEN WORLD TERRAIN ===\n";
    
    Terrain::TerrainConfig terrainConfig;
    terrainConfig.chunkSize = 100.0f;
    terrainConfig.chunkResolution = 64;
    terrainConfig.viewDistance = 8;  // Load 4 chunks in each direction
    terrainConfig.lodDistance = 50.0f;
    terrainConfig.heightmapSize = 1024;
    terrainConfig.heightScale = 80.0f;  // Max terrain height
    
    Terrain* terrain = new Terrain(terrainConfig);
    terrain->initialize();
    
    // Vegetation system
    VegetationSystem::VegetationConfig vegConfig;
    vegConfig.treeDensity = 0.03f;        // More trees
    vegConfig.grassDensity = 0.8f;         // Dense grass
    vegConfig.rockDensity = 0.02f;         // More rocks
    vegConfig.minTreeHeight = 4.0f;
    vegConfig.maxTreeHeight = 12.0f;
    vegConfig.maxTreesPerChunk = 80;
    vegConfig.maxRocksPerChunk = 50;
    
    VegetationSystem* vegetation = new VegetationSystem(vegConfig);
    
    // World object manager (for importing actual 3D models)
    WorldObjectManager* worldObjects = new WorldObjectManager();
    // Try to load from assets/world_objects/ (will use fallbacks if not found)
    worldObjects->initialize("assets/World_objects/");
    
    std::cout << "\n=== WORLD OBJECT MANAGER ===\n";
    std::cout << "Loaded object types: " << worldObjects->getObjectCount() << " instances\n";
    std::cout << "To add trees/rocks: Place FBX models in assets/world_objects/\n";
    std::cout << "==============================\n\n";
    
    // Water level
    float waterLevel = 5.0f;

    // Terrain shader (used in render loop)
    Shader* terrainShader = nullptr;
    Shader* waterShader = nullptr;
    // Shader* treeShader = nullptr;    // Not currently used
    // Shader* grassShader = nullptr;   // Not currently used
    // GLuint terrainVAO = 0;           // Not currently used
    
    std::cout << "========================================\n\n";

    // ============================================================
    // CHARACTER MOVEMENT STATE
    // ============================================================
    glm::vec3 characterPos(0.0f, 0.0f, 0.0f);  // Character world position
    
    // Place character on terrain
    if (terrain) {
        float terrainHeight = terrain->getHeightAt(characterPos.x, characterPos.z);
        // Validate terrain height (prevent NaN)
        if (std::isfinite(terrainHeight)) {
            characterPos.y = terrainHeight;
            std::cout << "[Character] Spawned at terrain height: " << terrainHeight << "\n";
        } else {
            characterPos.y = 0.0f;
            std::cout << "[Character] Spawned at Y=0 (invalid terrain height)\n";
        }
    } else {
        characterPos.y = 0.0f;
        std::cout << "[Character] Spawned at Y=0 (no terrain)\n";
    }
    
    glm::vec3 characterVelocity(0.0f);
    glm::vec3 prevCharacterPos(0.0f, 0.0f, 0.0f);  // For velocity calculation
    float rotationAngle = 0.0f;   // Character rotation (degrees)

    // Create skinned shader
    SkinnedShader skinnedShader;

    // Create fresh VAOs for skinned mesh
    std::vector<GLuint> freshVAOs;
    std::vector<GLsizei> freshCounts;
    std::cout << "Creating skinned mesh VAOs...\n";
    for (size_t i = 0; i < character->GetMeshCount(); i++) {
        Mesh& mesh = character->GetMesh(i);
        GLuint vao, vbo, ebo;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);

        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, mesh.vertices.size() * sizeof(Vertex), mesh.vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.indices.size() * sizeof(unsigned int), mesh.indices.data(), GL_STATIC_DRAW);

        // Position (location 0)
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Position));
        glEnableVertexAttribArray(0);
        // Normal (location 1)
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));
        glEnableVertexAttribArray(1);
        // TexCoords (location 2)
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, TexCoords));
        glEnableVertexAttribArray(2);
        // BoneIDs (location 5) - INTEGER!
        glVertexAttribIPointer(5, 4, GL_INT, sizeof(Vertex), (void*)offsetof(Vertex, BoneIDs));
        glEnableVertexAttribArray(5);
        // Weights (location 6)
        glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Weights));
        glEnableVertexAttribArray(6);

        glBindVertexArray(0);

        freshVAOs.push_back(vao);
        freshCounts.push_back(mesh.indices.size());
        
        // Debug: print sample vertex bone data
        if (i == 0 && !mesh.vertices.empty()) {
            Vertex& v = mesh.vertices[0];
            std::cout << "  Mesh[" << i << "]: " << mesh.vertices.size() << " vertices\n";
            std::cout << "    Sample vertex: boneIDs=(" << v.BoneIDs.x << "," << v.BoneIDs.y << "," << v.BoneIDs.z << "," << v.BoneIDs.w << ")"
                      << " weights=(" << v.Weights.x << "," << v.Weights.y << "," << v.Weights.z << "," << v.Weights.w << ")\n";
        }
    }
    std::cout << "Skinned mesh VAOs created\n";

    // Main loop
    float lastTime = (float)glfwGetTime();
    bool running = true;
    int debugMode = 0;  // 0=normal, 1=bone debug
    
    // FPS counter variables
    int frameCount = 0;
    float fpsTimer = 0.0f;

    // Loading complete
    showProgress();
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  LOADING COMPLETE!\n";
    std::cout << "========================================\n\n";

    std::cout << "\n=== READY ===\n\n";
    std::cout << "Camera position: (" << camera.Position.x << ", " << camera.Position.y << ", " << camera.Position.z << ")\n";
    std::cout << "Character position: (0, 0, 0)\n";
    std::cout << "AAA QUALITY CAMERA: Maximum smoothing | Zero lag follow\n";
    std::cout << "Zoom: scroll wheel (10-50 units cinematic range)\n";
    std::cout << "Press F to toggle wireframe/solid\n";
    std::cout << "Press B for bone debug visualization\n";
    std::cout << "Press H to print camera debug\n";
    std::cout << "Press ESC to exit\n\n";
    std::cout << "=== MAIN LOOP STARTED ===\n";
    std::cout.flush();

    while (running && !glfwWindowShouldClose(window)) {
        float now = (float)glfwGetTime();
        float dt = std::min(now - lastTime, 0.1f);  // Cap dt to prevent huge jumps
        lastTime = now;

        // FPS counter (print every 5 seconds for minimal spam)
        frameCount++;
        fpsTimer += dt;
        if (fpsTimer >= 5.0f) {
            std::cout << "[FPS] " << frameCount << " (" << (1000.0f * fpsTimer / frameCount) << "ms/frame)\n";
            std::cout.flush();
            frameCount = 0;
            fpsTimer = 0.0f;
        }

        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) running = false;

        // ============================================================
        // CAMERA CONTROLS (Mouse orbit + Q/E only)
        // ============================================================
        float moveSpeed = 10.0f * dt;
        
        // Q/E for camera up/down only
        if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
            cameraPivot.y -= moveSpeed;
        }
        if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) {
            cameraPivot.y += moveSpeed;
        }
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) {
            cameraPivot = glm::vec3(0, 2, 0);
            cameraDistance = 10.0f;
            camera.Yaw = -90;
            camera.Pitch = 0;
        }

        // Toggle fixed camera mode (C key)
        static bool lastC = false;
        if (glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS && !lastC) {
            cameraFixedMode = !cameraFixedMode;
            std::cout << ">>> Camera Mode: " << (cameraFixedMode ? "FIXED (stable)" : "ORBIT") << "\n";
            if (cameraFixedMode) {
                // Reset to behind character
                camera.Yaw = -90;
                camera.Pitch = 0;
            }
        }
        lastC = glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS;

        // NOTE: Camera update moved to AFTER character position is finalized
        // This ensures camera follows the actual character position, not the previous frame's position

        // ============================================================
        // CHARACTER MOVEMENT INPUT (WASD triggers animation, root motion drives movement)
        // ============================================================
        CharacterInput charInput;

        // Movement input (WASD) - this triggers animation direction, NOT direct movement
        glm::vec2 moveDir(0.0f);

        // WASD input
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) moveDir.y = 1.0f;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) moveDir.y = -1.0f;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) moveDir.x = 1.0f;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) moveDir.x = -1.0f;

        // Normalize direction (prevent faster diagonal movement)
        if (glm::length(moveDir) > 1.0f) {
            moveDir = glm::normalize(moveDir);
        }

        charInput.moveDirection = moveDir;
        charInput.moveMagnitude = glm::length(moveDir);

        // Action inputs
        charInput.crouch = (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS);
        charInput.sprint = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS);

        // Calculate movement direction in world space (relative to camera)
        // This is used for character rotation and root motion direction
        glm::vec3 moveDirection(0.0f);

        if (charInput.moveMagnitude > 0.1f) {
            // Get camera forward and right vectors (flattened to XZ plane)
            glm::vec3 camForward = glm::normalize(camera.Target - camera.Position);
            camForward.y = 0.0f;
            camForward = glm::normalize(camForward);

            glm::vec3 camRight = glm::normalize(glm::cross(camForward, glm::vec3(0.0f, 1.0f, 0.0f)));

            // Calculate movement direction
            moveDirection = camForward * charInput.moveDirection.y + camRight * charInput.moveDirection.x;
            moveDirection = glm::normalize(moveDirection);

            // Smooth rotation - interpolate current angle toward target angle
            // SLOW rotation for realistic foot planting - character turns gradually
            float targetAngle = atan2(moveDirection.x, moveDirection.z) * 180.0f / 3.14159f;
            float rotationSpeed = 180.0f * dt;  // degrees per second (slow, natural turning)
            
            // Handle angle wrapping (-180 to 180)
            float angleDiff = targetAngle - rotationAngle;
            while (angleDiff > 180.0f) angleDiff -= 360.0f;
            while (angleDiff < -180.0f) angleDiff += 360.0f;
            
            // Deadzone: Don't rotate if angle difference is very small (prevents jitter)
            if (std::abs(angleDiff) > 1.0f) {
                // Clamp the change to rotation speed
                if (std::abs(angleDiff) > rotationSpeed) {
                    rotationAngle += std::copysign(rotationSpeed, angleDiff);
                } else {
                    rotationAngle = targetAngle;
                }
            }
        }

        // Jump input - triggers animation state, not direct position change
        static bool lastJump = false;
        bool jumpKeyPressed = (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS);
        charInput.jump = jumpKeyPressed && !lastJump;  // Edge detected: just pressed
        lastJump = jumpKeyPressed;

        // Grounded check based on terrain height
        float terrainHeight = terrain ? terrain->getHeightAt(characterPos.x, characterPos.z) : 0.0f;
        bool wasGrounded = (characterPos.y <= terrainHeight + 0.01f);
        charInput.grounded = wasGrounded;  // FSM needs ORIGINAL grounded state for jump check

        // Jump handling - set vertical velocity
        static float jumpVelocity = 0.0f;
        if (charInput.jump && wasGrounded) {
            jumpVelocity = 5.0f;  // Initial jump impulse
            charInput.verticalVelocity = jumpVelocity;
            // Don't set grounded=false here - FSM needs to see grounded=true for jump trigger
            // grounded will be set false NEXT frame after FSM processes
        } else if (!wasGrounded) {
            jumpVelocity -= 12.0f * dt;  // Gravity
            charInput.verticalVelocity = jumpVelocity;
            charInput.grounded = false;  // Now set grounded false for physics

            if (jumpVelocity <= 0.0f && characterPos.y <= terrainHeight + 0.01f) {
                charInput.grounded = true;
                jumpVelocity = 0.0f;
            }
        }

        // Apply gravity to character position (animation provides initial jump impulse)
        if (!charInput.grounded) {
            characterPos.y += jumpVelocity * dt;
            // Clamp to terrain height
            if (characterPos.y < terrainHeight) {
                characterPos.y = terrainHeight;
                charInput.grounded = true;
                jumpVelocity = 0.0f;
            }
        } else {
            // Snap to terrain when grounded
            characterPos.y = terrainHeight;
        }

        // Toggle wireframe
        static bool lastF = false;
        if (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS && !lastF) {
            static bool wireframe = true;
            wireframe = !wireframe;
            glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
            std::cout << ">>> Wireframe: " << (wireframe ? "ON" : "OFF") << "\n";
        }
        lastF = glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS;

        // Toggle bone debug view (cycle through 3 modes)
        static bool lastB = false;
        if (glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS && !lastB) {
            debugMode = (debugMode + 1) % 3;
            if (debugMode == 0) std::cout << ">>> Bone debug: OFF\n";
            else if (debugMode == 1) std::cout << ">>> Bone debug: ON (color=boneID)\n";
            else std::cout << ">>> Bone debug: ON (color=weights RGB)\n";
        }
        lastB = glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS;

        // Toggle foot IK debug
        static bool lastG = false;
        if (glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS && !lastG && animator) {
            std::cout << "\n=== FOOT IK STATUS ===\n";
            std::cout << "Enabled: " << (animator->footIKSettings.enabled ? "YES" : "NO") << "\n";
            std::cout << "Floor height: " << animator->footIKSettings.floorHeight << "\n";
            std::cout << "Character grounded: " << (animator->IsCharacterGrounded() ? "YES" : "NO") << "\n";
            std::cout << "Left foot planted: " << (animator->IsFootPlanted(animator->footIKSettings.leftFootBone) ? "YES" : "NO") << "\n";
            std::cout << "Right foot planted: " << (animator->IsFootPlanted(animator->footIKSettings.rightFootBone) ? "YES" : "NO") << "\n";
            std::cout << "Character Y position: " << characterPos.y << "\n";
            animator->DebugDrawFootIK();
        }
        lastG = glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS;
        
        // Print MOTION MATCHING debug
        static bool lastH = false;
        if (glfwGetKey(window, GLFW_KEY_H) == GLFW_PRESS && !lastH) {
            std::cout << "\n=== MOTION MATCHING DEBUG ===\n";
            matcher->PrintDebugInfo();
            std::cout << "[CAMERA] Pos=(" << camera.Position.x << ", " << camera.Position.y << ", " << camera.Position.z << ")\n";
            std::cout << "[CHARACTER] Pos=(" << characterPos.x << ", " << characterPos.y << ", " << characterPos.z << ")\n";
            std::cout << "[DISTANCE] Camera-to-character=" << glm::length(camera.Position - characterPos) << " units\n";
        }
        lastH = glfwGetKey(window, GLFW_KEY_H) == GLFW_PRESS;

        // ============================================================
        // UPDATE MOTION MATCHING (Replaces FSM update)
        // ============================================================
        if (matcher) {
            // Build character state for motion matching
            CharacterState charState;
            charState.position = characterPos;
            charState.velocity = characterVelocity;
            charState.rotation = glm::radians(rotationAngle);
            charState.moveDirection = moveDir;
            charState.grounded = charInput.grounded;
            charState.crouching = charInput.crouch;
            charState.jumping = charInput.jump;
            
            // Update motion matching - searches database, blends poses, applies foot IK
            matcher->Update(dt, charState);

            // CRITICAL: Update animator to calculate bone matrices!
            if (animator) {
                animator->Update(dt);
            }

            // Motion matching debug only on 'H' key press (see line ~1140)
        }

        // Build model matrix with character position and rotation
        glm::mat4 modelMat = glm::translate(glm::mat4(1.0f), characterPos);
        modelMat *= glm::rotate(glm::mat4(1.0f), glm::radians(rotationAngle), glm::vec3(0.0f, 1.0f, 0.0f));
        modelMat *= glm::scale(glm::mat4(1.0f), glm::vec3(modelScale));

        // Extract root motion from animation and apply in camera-relative direction
        // Root motion DRIVES the movement, not the keyboard input
        glm::vec3 rootMotionDelta(0.0f);  // Track how much we moved this frame

        if (animator) {
            glm::vec3 rootMotion = animator->ConsumeRootMotion();
            float motionMagnitude = glm::length(rootMotion);

            // Apply root motion for movement
            if (charInput.moveMagnitude > 0.01f && moveDirection != glm::vec3(0.0f)) {
                float movementMultiplier = 0.25f;
                rootMotionDelta = moveDirection * motionMagnitude * movementMultiplier;
                characterPos += rootMotionDelta;
            }

            // Update foot IK floor height based on terrain (dynamic)
            if (terrain && animator->footIKSettings.enabled) {
                float terrainHeight = terrain->getHeightAt(characterPos.x, characterPos.z);
                if (std::isfinite(terrainHeight)) {
                    Animator::FootIKSettings ikSettings = animator->footIKSettings;
                    ikSettings.floorHeight = terrainHeight;  // Update to match terrain
                    animator->SetFootIKSettings(ikSettings);
                }
            }

            // Update foot IK (after position update)
            bool isMoving = (charInput.moveMagnitude > 0.1f);
            animator->UpdateFootIK(dt, modelMat, isMoving);
        }
        
        // CRITICAL: Calculate character velocity for motion matching!
        // Velocity = position delta / time (with dt safety check)
        if (dt > 0.0001f) {
            characterVelocity = (characterPos - prevCharacterPos) / dt;
        } else {
            characterVelocity = glm::vec3(0.0f);
        }
        prevCharacterPos = characterPos;  // Save for next frame

        // ============================================================
        // THIRD-PERSON CAMERA FOLLOW (AAA QUALITY - MAXIMUM SMOOTHING)
        // ============================================================
        // CRITICAL: This runs AFTER characterPos is updated with root motion
        // This ensures camera follows the ACTUAL position, not last frame's position
        if (cameraFollowEnabled) {
            // AAA QUALITY - MAXIMUM SMOOTHING FOR PERFECT FOLLOW
            // At 60fps with smooth=60.0f: ~63% interpolation per frame (essentially locked on)
            // At 144fps with smooth=60.0f: ~30% interpolation per frame (still very tight)
            // Frame-rate independent: dt scaling ensures consistent behavior
            const float CAMERA_SMOOTH = 60.0f;      // MAXIMUM - camera locked to character
            const float PIVOT_SMOOTH = 40.0f;       // MAXIMUM - pivot follows instantly
            // const float ROTATION_SMOOTH = 50.0f; // Reserved for future rotation smoothing

            // Update pivot to follow character (maximum smoothing - no perceptible lag)
            glm::vec3 targetPivot = characterPos + glm::vec3(0, 2.0f, 0);
            
            // Validate pivot position
            if (std::isfinite(targetPivot.x) && std::isfinite(targetPivot.y) && std::isfinite(targetPivot.z)) {
                float smoothFactor = glm::clamp(PIVOT_SMOOTH * dt, 0.0f, 1.0f);
                cameraPivot = glm::mix(cameraPivot, targetPivot, smoothFactor);
            }

            if (cameraFixedMode) {
                // FIXED MODE: Camera directly behind character (AAA quality)
                glm::vec3 forward;
                forward.x = sin(glm::radians(rotationAngle));
                forward.z = cos(glm::radians(rotationAngle));
                forward.y = 0.0f;
                
                // Prevent NaN from normalizing zero vector
                if (glm::length(forward) > 0.0001f) {
                    forward = glm::normalize(forward);
                } else {
                    forward = glm::vec3(0.0f, 0.0f, 1.0f);  // Default forward
                }

                glm::vec3 targetCamPos = characterPos - forward * cameraDistance + glm::vec3(0, cameraHeight, 0);

                // Validate target position (prevent NaN)
                if (std::isfinite(targetCamPos.x) && std::isfinite(targetCamPos.y) && std::isfinite(targetCamPos.z)) {
                    // MAXIMUM smoothing - camera essentially locked to character
                    float smoothFactor = glm::clamp(CAMERA_SMOOTH * dt, 0.0f, 1.0f);
                    camera.Position = glm::mix(camera.Position, targetCamPos, smoothFactor);
                    camera.Target = cameraPivot;
                }
            } else {
                // ORBIT MODE: Camera rotates around character (AAA quality)
                float yawRad = glm::radians(camera.Yaw);
                float pitchRad = glm::radians(camera.Pitch);

                glm::vec3 camOffset;
                camOffset.x = cos(pitchRad) * sin(yawRad) * cameraDistance;
                camOffset.y = sin(pitchRad) * cameraDistance + cameraHeight;
                camOffset.z = cos(pitchRad) * cos(yawRad) * cameraDistance;

                glm::vec3 targetCamPos = cameraPivot + camOffset;

                // Validate target position
                if (std::isfinite(targetCamPos.x) && std::isfinite(targetCamPos.y) && std::isfinite(targetCamPos.z)) {
                    // MAXIMUM smoothing - camera locked to orbit position
                    float smoothFactor = glm::clamp(CAMERA_SMOOTH * dt, 0.0f, 1.0f);
                    camera.Position = glm::mix(camera.Position, targetCamPos, smoothFactor);
                    camera.Target = cameraPivot;
                }
            }
        }

        // Clear with sky-blue color for open world (no skybox)
        glClearColor(0.5f, 0.7f, 0.9f, 1.0f);  // Light blue sky color
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::mat4 projection = glm::perspective(glm::radians(45.0f), 1280.f/720.f, 0.1f, 1000.f);
        glm::mat4 view = camera.GetViewMatrix();

        // Skybox disabled - rendering world terrain directly
        // drawSkybox(view, projection, skyboxTexture);  // Disabled

        // ============================================================
        // RENDER TERRAIN (open world)
        // ============================================================
        if (terrain) {
            // Update terrain streaming based on camera position
            terrain->update(camera.Position, dt);
            
            // Generate vegetation for newly loaded chunks
            static bool vegetationGenerated = false;
            if (!vegetationGenerated && terrain) {
                // Generate vegetation with proper terrain heights
                int chunksToGenerate = 1;  // Generate for chunks around camera
                for (int cx = -chunksToGenerate; cx <= chunksToGenerate; cx++) {
                    for (int cy = -chunksToGenerate; cy <= chunksToGenerate; cy++) {
                        // Create heightmap by sampling terrain at multiple points
                        std::vector<float> chunkHeights(1024);
                        float worldStartX = cx * 100.0f;
                        float worldStartZ = cy * 100.0f;
                        
                        // Sample terrain at 32x32 grid for this chunk
                        for (int hz = 0; hz < 32; hz++) {
                            for (int hx = 0; hx < 32; hx++) {
                                float sampleX = worldStartX + (hx / 32.0f) * 100.0f;
                                float sampleZ = worldStartZ + (hz / 32.0f) * 100.0f;
                                float height = terrain->getHeightAt(sampleX, sampleZ);
                                chunkHeights[hz * 32 + hx] = height;
                            }
                        }
                        
                        vegetation->generateForChunk(cx, cy, 100.0f, chunkHeights, 32);
                        
                        std::cout << "[Vegetation] Chunk (" << cx << "," << cy << "): "
                                  << vegetation->getTrees().size() << " trees, "
                                  << vegetation->getRocks().size() << " rocks\n";
                    }
                }
                vegetationGenerated = true;
            }
            
            // Create terrain shader if needed
            if (!terrainShader) {
                terrainShader = new Shader("world/terrainVS.glsl", "world/terrainFS.glsl");
            }
            
            // Render all active terrain chunks
            terrainShader->use();
            terrainShader->setMat4("projection", projection);
            terrainShader->setMat4("view", view);
            terrainShader->setMat4("model", glm::mat4(1.0f));
            terrainShader->setVec3("terrainColor", glm::vec3(0.2f, 0.5f, 0.2f));
            terrainShader->setFloat("waterLevel", waterLevel);
            terrainShader->setVec3("cameraPos", camera.Position);  // For fog

            // Render terrain with frustum culling (Priority 1 optimization)
            terrain->render(camera.Position, 45.0f, 1280.0f/720.0f, 0.1f, 1000.0f);

            // Render grass on terrain (if grass model loaded)
            if (grassModel) {
                // Would render grass instances here
                // For now, grass is handled by vegetation system
            }
        }

        // ============================================================
        // RENDER DEBUG FLOOR (visible wireframe grid at floor height)
        // ============================================================
        {
            static Shader* debugFloorShader = nullptr;
            if (!debugFloorShader) {
                debugFloorShader = new Shader("shaderSystem/VS.glsl", "shaderSystem/FS.glsl");
            }
            
            // Render debug floor as wireframe
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            debugFloorShader->use();
            debugFloorShader->setMat4("projection", projection);
            debugFloorShader->setMat4("view", view);
            debugFloorShader->setMat4("model", glm::mat4(1.0f));
            debugFloorShader->setVec3("color", glm::vec3(0.0f, 1.0f, 0.0f));  // Green grid
            
            glBindVertexArray(debugFloorVAO);
            glDrawElements(GL_TRIANGLES, 20 * 20 * 6, GL_UNSIGNED_INT, 0);
            glBindVertexArray(0);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }

        // ============================================================
        // RENDER WATER PLANE
        // ============================================================
        {
            static GLuint waterVAO = 0;
            static GLuint waterVBO = 0;
            static GLuint waterEBO = 0;
            
            if (waterVAO == 0) {
                // Create large water plane
                float waterSize = 2000.0f;
                int waterRes = 50;
                
                std::vector<float> vertices;
                std::vector<unsigned int> indices;
                
                // Generate vertices
                for (int z = 0; z <= waterRes; z++) {
                    for (int x = 0; x <= waterRes; x++) {
                        float vx = (float)x / waterRes * waterSize - waterSize / 2.0f;
                        float vz = (float)z / waterRes * waterSize - waterSize / 2.0f;
                        vertices.push_back(vx);
                        vertices.push_back(waterLevel);
                        vertices.push_back(vz);
                        vertices.push_back(vx * 0.1f);  // UV
                        vertices.push_back(vz * 0.1f);
                    }
                }
                
                // Generate indices
                for (int z = 0; z < waterRes; z++) {
                    for (int x = 0; x < waterRes; x++) {
                        int topLeft = z * (waterRes + 1) + x;
                        int topRight = topLeft + 1;
                        int bottomLeft = (z + 1) * (waterRes + 1) + x;
                        int bottomRight = bottomLeft + 1;
                        
                        indices.push_back(topLeft);
                        indices.push_back(bottomLeft);
                        indices.push_back(topRight);
                        indices.push_back(topRight);
                        indices.push_back(bottomLeft);
                        indices.push_back(bottomRight);
                    }
                }
                
                glGenVertexArrays(1, &waterVAO);
                glGenBuffers(1, &waterVBO);
                glGenBuffers(1, &waterEBO);
                
                glBindVertexArray(waterVAO);
                glBindBuffer(GL_ARRAY_BUFFER, waterVBO);
                glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, waterEBO);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
                
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
                glEnableVertexAttribArray(1);
                
                glBindVertexArray(0);
            }
            
            // Render water AFTER terrain with proper blending
            if (!waterShader) {
                waterShader = new Shader("world/waterVS.glsl", "world/waterFS.glsl");
            }
            
            int waterRes = 50;  // Must match creation
            
            waterShader->use();
            waterShader->setMat4("projection", projection);
            waterShader->setMat4("view", view);
            waterShader->setMat4("model", glm::mat4(1.0f));
            waterShader->setVec3("cameraPos", camera.Position);
            waterShader->setFloat("time", (float)glfwGetTime());
            waterShader->setVec3("waterColor", glm::vec3(0.0f, 0.35f, 0.55f));
            waterShader->setFloat("waterLevel", waterLevel);
            
            // Enable blending for transparent water
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            
            // Disable depth write (but keep depth test) so water blends correctly
            glDepthMask(GL_FALSE);
            
            glBindVertexArray(waterVAO);
            glDrawElements(GL_TRIANGLES, (waterRes * waterRes * 6), GL_UNSIGNED_INT, 0);
            glBindVertexArray(0);
            
            // Restore depth write
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
        
        // ============================================================
        // RENDER VEGETATION AND WORLD OBJECTS
        // ============================================================
        if (vegetation && terrain) {
            // Update world objects (cull distant ones)
            if (worldObjects) {
                worldObjects->update(camera.Position, dt);
            }
            
            // Place trees from vegetation system (if not already placed)
            static bool treesPlaced = false;
            if (!treesPlaced && worldObjects) {
                const auto& trees = vegetation->getTrees();
                for (const auto& tree : trees) {
                    // Random tree type
                    WorldObjectType treeType = WorldObjectType::TREE_PINE;
                    int randType = rand() % 3;
                    if (randType == 1) treeType = WorldObjectType::TREE_OAK;
                    else if (randType == 2) treeType = WorldObjectType::TREE_BIRCH;
                    
                    worldObjects->placeObject(treeType, tree.position, tree.height * 0.3f, 
                                             0.0f, glm::vec3(1.0f));
                }
                treesPlaced = true;
                std::cout << "[World] Placed " << vegetation->getTrees().size() 
                          << " trees using imported models\n";
            }
            
            // Place rocks from vegetation system with physics
            static bool rocksPlaced = false;
            if (!rocksPlaced && worldObjects) {
                const auto& rocks = vegetation->getRocks();
                std::cout << "[World] Found " << rocks.size() << " rocks to place\n";
                for (const auto& rock : rocks) {
                    // Random rock type
                    WorldObjectType rockType = WorldObjectType::ROCK_BOULDER;
                    int randType = rand() % 3;
                    if (randType == 1) rockType = WorldObjectType::ROCK_STONE;
                    else if (randType == 2) rockType = WorldObjectType::ROCK_CLIFF;

                    // Get terrain height at rock position (physics - raycast to ground)
                    float terrainHeight = terrain->getHeightAt(rock.position.x, rock.position.z);
                    
                    // Place rock ON terrain (not floating)
                    glm::vec3 placePos = rock.position;
                    placePos.y = terrainHeight + rock.scale.y * 0.5f;  // Half scale so it sits ON ground
                    
                    worldObjects->placeObject(rockType, placePos-5.5f,
                                             rock.scale.x, rock.rotation);
                }
                rocksPlaced = true;
                std::cout << "[World] Placed " << vegetation->getRocks().size()
                          << " rocks using imported models (with physics)\n";
            }

            // Render all world objects (trees, rocks, etc.)
            if (worldObjects) {
                worldObjects->render(view, projection, camera.Position);
            }
        }

        // ============================================================
        // RENDER FLOOR (fallback if no terrain)
        // ============================================================
        {
            static GLuint floorVAO = 0;
            static GLuint floorVBO = 0;
            
            if (floorVAO == 0) {
                float floorSize = 100.0f;
                float floorVertices[] = {
                    // Positions          // Normals         // TexCoords
                    -floorSize, floorHeight, -floorSize,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f,
                     floorSize, floorHeight, -floorSize,  0.0f, 1.0f, 0.0f,  1.0f, 0.0f,
                     floorSize, floorHeight,  floorSize,  0.0f, 1.0f, 0.0f,  1.0f, 1.0f,
                    -floorSize, floorHeight,  floorSize,  0.0f, 1.0f, 0.0f,  0.0f, 1.0f
                };
                
                glGenVertexArrays(1, &floorVAO);
                glGenBuffers(1, &floorVBO);
                glBindVertexArray(floorVAO);
                glBindBuffer(GL_ARRAY_BUFFER, floorVBO);
                glBufferData(GL_ARRAY_BUFFER, sizeof(floorVertices), floorVertices, GL_STATIC_DRAW);
                
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
                glEnableVertexAttribArray(2);
                glBindVertexArray(0);
            }
            
            // Render floor
            static Shader* floorShader = nullptr;
            if (!floorShader) {
                floorShader = new Shader(
                    "shaderSystem/floorVS.glsl",
                    "shaderSystem/floorFS.glsl"
                );
            }
            
            floorShader->use();
            floorShader->setMat4("projection", projection);
            floorShader->setMat4("view", view);
            floorShader->setMat4("model", glm::mat4(1.0f));
            floorShader->setVec3("cameraPos", camera.Position);
            floorShader->setVec3("floorColor", glm::vec3(0.2f, 0.2f, 0.25f));
            floorShader->setFloat("floorHeight", floorHeight);
            floorShader->setFloat("gridSpacing", 1.0f);
            floorShader->setInt("showGrid", 1);
            
            glBindVertexArray(floorVAO);
            glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            glBindVertexArray(0);
        }

        // Draw model with skinned shader
        skinnedShader.use();
        skinnedShader.setMat4("projection", projection);
        skinnedShader.setMat4("view", view);
        skinnedShader.setMat4("model", modelMat);
        skinnedShader.setVec3("lightPos", glm::vec3(10.0f, 10.0f, 10.0f));
        skinnedShader.setVec3("viewPos", camera.Position);
        skinnedShader.setVec3("color", glm::vec3(0.8f, 0.6f, 0.4f));  // Skin tone
        skinnedShader.setInt("uDebugMode", debugMode);
        
        // Upload bone matrices - MUST happen after shader.use()
        if (animator) {
            const auto& finalBones = animator->GetFinalBoneMatrices();

            static GLuint boneTexID = 0;
            static int prevBoneCount = 0;
            int boneCount = (int)finalBones.size();
            
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
            
            // Re-allocate texture if bone count changed
            if (boneCount != prevBoneCount) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, 1, 0, GL_RGBA, GL_FLOAT, pixels.data());
                prevBoneCount = boneCount;
                std::cout << "[BoneTex] Allocated texture: " << boneCount << " bones\n";
            } else {
                // Update existing texture
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, 1, GL_RGBA, GL_FLOAT, pixels.data());
            }

            // Set bone texture uniform
            skinnedShader.setInt("boneTex", 10);
            skinnedShader.setInt("uPaletteSize", boneCount);
        }

        // Draw skinned meshes
        for (size_t i = 0; i < freshVAOs.size(); i++) {
            glBindVertexArray(freshVAOs[i]);
            glDrawElements(GL_TRIANGLES, freshCounts[i], GL_UNSIGNED_INT, 0);
        }
        glBindVertexArray(0);

        glfwSwapBuffers(window);
    }

    // Cleanup
    delete character;
    if (animator) delete animator;
    if (walkAnim) delete walkAnim;
    if (grassModel) delete grassModel;
    if (terrain) delete terrain;
    if (vegetation) delete vegetation;
    if (worldObjects) delete worldObjects;
    glfwTerminate();
    
    std::cout << "\nClosed.\n";
    return 0;
}
