#define GLM_ENABLE_EXPERIMENTAL

#include "shaderSystem/Shader.h"
#include "modelSystem/Model.h"
#include "cameraSystem/flyCamera.h"
#include "animationSystem/Animator.h"
#include "animationSystem/AssimpAnimationLoader.h"
#include "animationSystem/AnimationStateMachine.h"
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

// ================= CAMERA =================
flyCamera camera(
    glm::vec3(0.0f, 3.0f, 10.0f),
    glm::vec3(0, 2, 0),
    -90.0f,
    0.0f,
    10.0f);

// Third-person camera settings
glm::vec3 cameraPivot(0.0f, 2.0f, 0.0f);  // Point camera looks at (character + offset)
float cameraDistance = 15.0f;              // Distance from pivot (increased for better view)
float cameraHeight = 5.0f;                 // Camera height offset (higher angle)
float cameraRotateSpeed = 3.0f;            // Mouse rotation speed
float cameraFollowSmooth = 3.0f;           // LOWER = smoother/more lag, HIGHER = tighter/snappier
float cameraZoomSpeed = 15.0f;             // Zoom speed
float cameraZoomMin = 8.0f;                // Minimum zoom distance (increased)
float cameraZoomMax = 30.0f;               // Maximum zoom distance (increased)
bool cameraFollowEnabled = true;
bool cameraFixedMode = false;              // Fixed behind camera (no orbit, more stable)
float cameraPivotSmooth = 2.0f;            // Even smoother pivot following (reduces jitter)

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
    if (!glfwInit()) { std::cerr << "GLFW init failed\n"; return -1; }
    
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window = glfwCreateWindow(1280, 720, "3D Engine - Skinned Model", nullptr, nullptr);
    if (!window) { std::cerr << "Window failed\n"; glfwTerminate(); return -1; }
    
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
        std::cerr << "GLAD failed\n"; return -1;
    }

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);  // Start in SOLID mode (not wireframe)

    // Skybox - enable for better visuals
    std::cout << "Loading skybox...\n";
    setupSkybox();
    std::vector<std::string> skyboxFaces = {
        "assets/skybox/day/right.jpg",
        "assets/skybox/day/left.jpg",
        "assets/skybox/day/top.jpg",
        "assets/skybox/day/bottom.jpg",
        "assets/skybox/day/back.jpg",
        "assets/skybox/day/front.jpg"
    };
    skyboxTexture = loadCubemap(skyboxFaces);
    std::cout << "Skybox loaded\n";

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
    
    // Helper lambda to load animation
    auto loadAnim = [&](const std::string& path, const std::string& name) -> Animation* {
        const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs);
        if (scene && scene->HasAnimations()) {
            Animation* anim = new Animation(AssimpAnimationLoader::LoadAnimationWithPoseCorrection(scene, scene->mAnimations[0]));
            std::cout << "  Loaded " << name << ": " << anim->name << " (" << anim->duration << "s, " 
                      << anim->boneAnimations.size() << " bones)\n";
            return anim;
        }
        std::cout << "  WARNING: Failed to load " << name << " from " << path << "\n";
        return nullptr;
    };
    
    // Load animations (using available files)
    idleAnim = loadAnim("assets/Idle.fbx", "Idle");
    walkAnim = loadAnim("assets/Walking.fbx", "Walk");
    runAnim = loadAnim("assets/Run.fbx", "Run");
    jumpAnim = loadAnim("assets/Jump.fbx", "Jump");
    fallAnim = loadAnim("assets/fall.fbx", "Fall");
    crouchAnim = loadAnim("assets/Crouching.fbx", "Crouch");
    crouchWalkAnim = loadAnim("assets/chrouchWalk.fbx", "CrouchWalk");

    // Load grass model for ground cover (optional - large file)
    Model* grassModel = nullptr;
    std::cout << "\nLoading grass model (optional, may take time)...\n";
    try {
        grassModel = new Model("assets/grass/grass.fbx");
        if (grassModel && grassModel->GetMeshCount() > 0) {
            std::cout << "✓ Grass loaded: " << grassModel->GetMeshCount() << " meshes (" 
                      << (grassModel->GetSize().x * grassModel->GetSize().y * grassModel->GetSize().z / 1000000.0f) 
                      << "M units³)\n";
        } else {
            std::cout << "Grass model empty, using terrain colors\n";
            if (grassModel) delete grassModel;
            grassModel = nullptr;
        }
    } catch (...) {
        std::cout << "Grass loading failed, using terrain colors\n";
        grassModel = nullptr;
    }

    // Debug: print animation root motion info
    auto printAnimInfo = [](Animation* anim, const std::string& name) {
        if (anim) {
            std::cout << "  " << name << ": duration=" << anim->duration
                      << "s, ticks/sec=" << anim->ticksPerSecond
                      << ", bones=" << anim->boneAnimations.size() << "\n";
        }
    };
    std::cout << "\n=== ANIMATION INFO ===\n";
    printAnimInfo(idleAnim, "Idle");
    printAnimInfo(walkAnim, "Walk");
    printAnimInfo(runAnim, "Run");
    printAnimInfo(jumpAnim, "Jump");
    printAnimInfo(fallAnim, "Fall");
    printAnimInfo(crouchAnim, "Crouch");
    printAnimInfo(crouchWalkAnim, "CrouchWalk");
    
    // Create animator
    Animator* animator = new Animator(&skeleton);
    
    // Create animation state machine
    AnimationStateMachine* stateMachine = new AnimationStateMachine(animator);
    
    // Register animations with state machine
    stateMachine->registerAnimations(
        idleAnim,   // IDLE
        walkAnim,   // WALK
        runAnim,    // RUN
        jumpAnim,   // JUMP (optional)
        fallAnim,   // FALL (optional)
        crouchAnim, // CROUCH (optional)
        crouchWalkAnim // CROUCH_WALK (optional)
    );
    
    // Set up default transitions
    stateMachine->setBlendDuration(0.25f);  // Smoother, less snappy (was 0.15f)
    stateMachine->setWalkRunBlendThreshold(0.5f, 0.8f);

    // Initialize with IDLE animation
    stateMachine->initialize();

    // ENABLE root motion - the animation drives the movement
    // We'll extract root motion and apply it in the camera-relative direction
    animator->SetLockRootPosition(false);
    std::cout << "Root motion ENABLED - will extract and apply camera-relative\n";

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
    
    // Floor height (where feet should plant)
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
        
        // Configure foot IK
        Animator::FootIKSettings ikSettings;
        ikSettings.enabled = true;
        ikSettings.floorHeight = floorHeight;
        ikSettings.ikStrength = 1.0f;
        ikSettings.footLockBlend = 0.9f;
        ikSettings.maxIKDistance = 0.2f;
        ikSettings.leftFootBone = leftFoot;
        ikSettings.rightFootBone = rightFoot;
        ikSettings.leftToeBone = leftToe;
        ikSettings.rightToeBone = rightToe;
        
        animator->SetFootIKSettings(ikSettings);
        std::cout << "Foot IK configured and enabled!\n";
    }
    
    // Create floor physics body
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
    std::cout << "========================================\n\n";

    // ============================================================
    // OPEN WORLD TERRAIN SYSTEM
    // ============================================================
    std::cout << "\n=== INITIALIZING OPEN WORLD TERRAIN ===\n";
    
    Terrain::TerrainConfig terrainConfig;
    terrainConfig.chunkSize = 100.0f;
    terrainConfig.chunkResolution = 64;
    terrainConfig.viewDistance = 4;  // Load 4 chunks in each direction
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
    
    // Terrain shader
    Shader* terrainShader = nullptr;
    Shader* waterShader = nullptr;
    Shader* treeShader = nullptr;
    Shader* grassShader = nullptr;
    GLuint terrainVAO = 0;
    
    std::cout << "========================================\n\n";

    // ============================================================
    // CHARACTER MOVEMENT STATE
    // ============================================================
    glm::vec3 characterPos(0.0f, 0.0f, 0.0f);  // Character world position
    glm::vec3 characterVelocity(0.0f);
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

    std::cout << "\n=== READY ===\n\n";
    std::cout << "Camera position: (" << camera.Position.x << ", " << camera.Position.y << ", " << camera.Position.z << ")\n";
    std::cout << "Character position: (0, 0, 0)\n";
    std::cout << "Press F to toggle wireframe/solid\n";
    std::cout << "Press B for bone debug visualization\n\n";
    
    while (running && !glfwWindowShouldClose(window)) {
        float now = (float)glfwGetTime();
        float dt = std::min(now - lastTime, 0.1f);
        lastTime = now;
        
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

        // ============================================================
        // THIRD-PERSON CAMERA FOLLOW
        // ============================================================
        if (cameraFollowEnabled) {
            // Smoothly follow character - pivot tracks character position
            // Use VERY smooth interpolation to eliminate jitter
            glm::vec3 targetPivot = characterPos + glm::vec3(0, 2.0f, 0);  // Look at character's upper body
            cameraPivot = glm::mix(cameraPivot, targetPivot, cameraPivotSmooth * dt);
            
            if (cameraFixedMode) {
                // FIXED MODE: Camera stays directly behind character (stable, no orbit)
                // Calculate direction character is facing
                glm::vec3 forward;
                forward.x = sin(glm::radians(rotationAngle));
                forward.z = cos(glm::radians(rotationAngle));
                forward.y = 0.0f;
                forward = glm::normalize(forward);
                
                // Camera position: behind character based on character's facing direction
                glm::vec3 targetCamPos = characterPos - forward * cameraDistance + glm::vec3(0, cameraHeight, 0);
                
                // Extra smooth camera position - reduces jitter significantly
                camera.Position = glm::mix(camera.Position, targetCamPos, cameraFollowSmooth * dt);
                camera.Target = cameraPivot;
            } else {
                // ORBIT MODE: Camera rotates around character using yaw/pitch
                float yawRad = glm::radians(camera.Yaw);
                float pitchRad = glm::radians(camera.Pitch);
                
                glm::vec3 camOffset;
                camOffset.x = cos(pitchRad) * sin(yawRad) * cameraDistance;
                camOffset.y = sin(pitchRad) * cameraDistance + cameraHeight;
                camOffset.z = cos(pitchRad) * cos(yawRad) * cameraDistance;
                
                glm::vec3 targetCamPos = cameraPivot + camOffset;
                camera.Position = glm::mix(camera.Position, targetCamPos, cameraFollowSmooth * dt);
                camera.Target = cameraPivot;
            }
        }

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
        charInput.jump = (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) && !lastJump;
        lastJump = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;

        // Grounded check based on terrain height
        float terrainHeight = terrain ? terrain->getHeightAt(characterPos.x, characterPos.z) : 0.0f;
        bool wasGrounded = (characterPos.y <= terrainHeight + 0.01f);
        charInput.grounded = wasGrounded;

        // Jump handling - set vertical velocity FIRST so state machine sees it
        static float jumpVelocity = 0.0f;
        if (charInput.jump && wasGrounded) {
            jumpVelocity = 5.0f;  // Initial jump impulse
            charInput.verticalVelocity = jumpVelocity;  // Set BEFORE grounded check
            charInput.grounded = false;  // Now set grounded to false
        } else if (!charInput.grounded) {
            jumpVelocity -= 12.0f * dt;  // Gravity
            charInput.verticalVelocity = jumpVelocity;

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
            std::cout << "=== FOOT IK STATUS ===\n";
            animator->DebugDrawFootIK();
        }
        lastG = glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS;
        
        // Print animation state
        static bool lastH = false;
        if (glfwGetKey(window, GLFW_KEY_H) == GLFW_PRESS && !lastH && stateMachine) {
            stateMachine->printState();
        }
        lastH = glfwGetKey(window, GLFW_KEY_H) == GLFW_PRESS;

        // Update animation state machine FIRST - input triggers animation state change
        if (stateMachine) {
            stateMachine->update(dt, charInput);
            
            // Debug: print input values every 30 frames
            static int dbg = 0;
            dbg++;
            if (dbg % 30 == 0) {
                std::cout << "[INPUT] moveMag=" << charInput.moveMagnitude
                          << " sprint=" << charInput.sprint
                          << " jump=" << charInput.jump
                          << " grounded=" << charInput.grounded
                          << " vVel=" << charInput.verticalVelocity
                          << " crouch=" << charInput.crouch << "\n";
            }
        }

        // Build model matrix with character position and rotation
        glm::mat4 modelMat = glm::translate(glm::mat4(1.0f), characterPos);
        modelMat *= glm::rotate(glm::mat4(1.0f), glm::radians(rotationAngle), glm::vec3(0.0f, 1.0f, 0.0f));
        modelMat *= glm::scale(glm::mat4(1.0f), glm::vec3(modelScale));

        // Extract root motion from animation and apply in camera-relative direction
        // Root motion DRIVES the movement, not the keyboard input
        if (animator) {
            glm::vec3 rootMotion = animator->ConsumeRootMotion();
            float motionMagnitude = glm::length(rootMotion);

            // Apply root motion only when there's movement input
            // The animation plays in the direction specified by input
            if (charInput.moveMagnitude > 0.01f && moveDirection != glm::vec3(0.0f)) {
                // Apply root motion in the camera-relative movement direction
                // Reduced multiplier (0.25f) to prevent sliding and make steps more visible
                float movementMultiplier = 0.25f;  // Adjust: lower = slower, more natural steps
                characterPos += moveDirection * motionMagnitude * movementMultiplier;
            }

            // Debug: print root motion every frame when moving
            if (charInput.moveMagnitude > 0.01f) {
                static int frameCounter = 0;
                frameCounter++;
                if (frameCounter % 30 == 0) {  // Print every 30 frames
                    std::cout << "[RootMotion] Mag: " << motionMagnitude
                              << " | Dir: (" << moveDirection.x << ", " << moveDirection.z << ")"
                              << " | Pos: (" << characterPos.x << ", " << characterPos.y << ", " << characterPos.z << ")\n";
                }
            }

            // Update foot IK - disable when moving fast (feet slide during motion)
            bool isMoving = (charInput.moveMagnitude > 0.1f);
            animator->UpdateFootIK(dt, modelMat, isMoving);
        }

        // Debug: print state changes
        if (stateMachine) {
            static AnimationState lastState = AnimationState::NONE;
            AnimationState curState = stateMachine->getCurrentState();
            if (curState != lastState) {
                std::cout << ">>> STATE CHANGE: " << AnimationStateToString(lastState) 
                          << " -> " << AnimationStateToString(curState) << "\n";
                std::cout << "    Speed: " << charInput.moveMagnitude
                          << " Sprint: " << charInput.sprint
                          << " Dir: (" << charInput.moveDirection.x << ", " << charInput.moveDirection.y << ")\n";
                lastState = curState;
            }
        }

        glClearColor(0.15f, 0.15f, 0.2f, 1.0f);  // Darker background
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::mat4 projection = glm::perspective(glm::radians(45.0f), 1280.f/720.f, 0.1f, 1000.f);
        glm::mat4 view = camera.GetViewMatrix();

        // Draw skybox FIRST (behind everything)
        drawSkybox(view, projection, skyboxTexture);

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
            
            terrain->render();
            
            // Render grass on terrain (if grass model loaded)
            if (grassModel) {
                // Would render grass instances here
                // For now, grass is handled by vegetation system
            }
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
                    
                    worldObjects->placeObject(rockType, placePos,
                                             rock.scale.x, rock.rotation);
                }
                rocksPlaced = true;
                std::cout << "[World] Placed " << vegetation->getRocks().size()
                          << " rocks using imported models (with physics)\n";
            }

            // Render all world objects (trees, rocks, etc.)
            if (worldObjects) {
                std::cout << "[World] Rendering " << worldObjects->getObjectCount() << " objects\n";
                worldObjects->render(view, projection, camera.Position);
            }
        }

        // Debug: print active chunks
        static int terrainDebug = 0;
        terrainDebug++;
        if (terrainDebug % 120 == 0 && terrain) {
            std::cout << "[Terrain] Active chunks: " << terrain->getActiveChunkCount() << "\n";
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
