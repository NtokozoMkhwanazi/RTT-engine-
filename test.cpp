/**
 * ============================================================================
 * RTT ENGINE EDITOR - Unreal Engine Style (Robust)
 * ============================================================================
 * Professional game engine editor with improved robustness:
 * - No static buffers that outlive ECS data
 * - Proper resource cleanup
 * - Safe entity selection handling
 * - Window resize handling
 * - Input validation
 * 
 * NOTE: If icons appear as squares/boxes, install a Nerd Font:
 *   sudo apt install fonts-font-awesome
 * ============================================================================
 */

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "ecs/systems/Systems.h"
#include "geospatial/GeospatialConverter.h"
#include "geospatial/GPSTracker.h"
#include "cameraSystem/flyCamera.h"
#include "renderer/GPUProfilerAdvanced.h"
#include "renderer/Renderer.h"
#include "renderer/MeshRegistry.h"
#include "ecs/systems/RenderSystem.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>

// Icon definitions (FontAwesome free icons)
#define ICON_MIN_FA 0xf000
#define ICON_MAX_FA 0xf8ff
#define ICON_FA_SEARCH "\xef\x80\x82"
#define ICON_FA_FOLDER "\xef\x81\xbb"
#define ICON_FA_FILE "\xef\x85\x9b"
#define ICON_FA_CUBE "\xef\x86\xb3"
#define ICON_FA_IMAGE "\xef\x80\xbe"
#define ICON_FA_PLAY "\xef\x81\x8b"
#define ICON_FA_PAUSE "\xef\x81\x8c"
#define ICON_FA_STOP "\xef\x81\x8d"
#define ICON_FA_UNDO "\xef\x83\xa2"
#define ICON_FA_REDO "\xef\x83\xa5"
#define ICON_FA_SAVE "\xef\x83\x87"
#define ICON_FA_OPEN "\xef\x82\x82"
#define ICON_FA_PLUS "\xef\x81\xa7"
#define ICON_FA_TRASH "\xef\x87\xb8"
#define ICON_FA_COG "\xef\x80\x93"
#define ICON_FA_INFO "\xef\x84\xa9"
#define ICON_FA_TIMES "\xef\x80\x8d"
#define ICON_FA_CHECK "\xef\x80\x8c"
#define ICON_FA_ARROWS_ALT "\xef\x82\xb2"
#define ICON_FA_ROTATE "\xef\x8b\xb1"
#define ICON_FA_EXPAND "\xef\x81\xa5"
#define ICON_FA_COMPRESS "\xef\x81\xa6"
#define ICON_FA_GLOBE "\xef\x82\xac"
#define ICON_FA_HOME "\xef\x80\x95"

// ============================================================================
// FBO for Viewport
// ============================================================================
struct Framebuffer {
    GLuint fbo = 0;
    GLuint colorTex = 0;
    GLuint rbo = 0;
    int width = 1280;
    int height = 720;
    
    void init(int w, int h) {
        cleanup();
        width = w; height = h;
        
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        
        glGenTextures(1, &colorTex);
        glBindTexture(GL_TEXTURE_2D, colorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
        
        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
        
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "ERROR: Framebuffer incomplete!\n";
        }
        
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    
    void resize(int w, int h) {
        if (w <= 0 || h <= 0 || (w == width && h == height)) return;
        init(w, h);
    }
    
    void cleanup() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (colorTex) glDeleteTextures(1, &colorTex);
        if (rbo) glDeleteRenderbuffers(1, &rbo);
        fbo = 0; colorTex = 0; rbo = 0;
    }
    
    void bind() {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
    }
    
    void unbind() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
};

static Framebuffer g_viewportFB;

// ============================================================================
// Shader Helper
// ============================================================================
static GLuint g_shaderProg = 0;
static GLuint g_cubeTexture = 0;  // Procedural texture for cubes

// Create a procedural checkerboard texture
static void createProceduralTexture() {
    const int texSize = 256;
    unsigned char* texData = new unsigned char[texSize * texSize * 3];
    
    // Generate checkerboard pattern
    int checkSize = 32;
    for (int y = 0; y < texSize; y++) {
        for (int x = 0; x < texSize; x++) {
            int idx = (y * texSize + x) * 3;
            bool isWhite = ((x / checkSize) + (y / checkSize)) % 2 == 0;
            
            if (isWhite) {
                texData[idx + 0] = 200;  // R
                texData[idx + 1] = 200;  // G
                texData[idx + 2] = 200;  // B
            } else {
                texData[idx + 0] = 80;   // R
                texData[idx + 1] = 80;   // G
                texData[idx + 2] = 100;  // B (slightly blue)
            }
        }
    }
    
    // Generate OpenGL texture
    glGenTextures(1, &g_cubeTexture);
    glBindTexture(GL_TEXTURE_2D, g_cubeTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, texSize, texSize, 0, GL_RGB, GL_UNSIGNED_BYTE, texData);
    glGenerateMipmap(GL_TEXTURE_2D);
    
    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    delete[] texData;
    
    std::cout << "[OK] Procedural checkerboard texture created (" << texSize << "x" << texSize << ")\n";
}

static void initShader() {
    // Optimized shader with UBO support, explicit layout locations, and proper material system
    const char* vs = R"(
#version 430 core

// Vertex attributes
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aTexCoord;  // For textures

// Instance matrix (per-instance data)
layout(location=7) in mat4 instanceMatrix;

// UBO for camera matrices - binding point 0
layout(std140, binding = 0) uniform CameraBlock {
    mat4 view;
    mat4 projection;
    mat4 viewProjection;
    vec4 viewPos;
    vec4 lightPos;
} camera;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;
out vec3 ViewDir;

void main() {
    // Use instance matrix for instanced rendering
    mat4 finalModel = instanceMatrix;
    
    // Transform position
    vec4 worldPos = finalModel * vec4(aPos, 1.0);
    FragPos = vec3(worldPos);
    Normal = mat3(transpose(inverse(finalModel))) * aNormal;
    
    // Pass texture coordinates
    TexCoord = aTexCoord;
    
    // Calculate view direction for fresnel/specular
    ViewDir = normalize(camera.viewPos.xyz - FragPos);
    
    // Use UBO for view/projection
    gl_Position = camera.projection * camera.view * worldPos;
}
)";
    const char* fs = R"(
#version 430 core

// Fragment inputs
in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;
in vec3 ViewDir;

// UBO for camera/light - binding point 0
layout(std140, binding = 0) uniform CameraBlock {
    mat4 view;
    mat4 projection;
    mat4 viewProjection;
    vec4 viewPos;
    vec4 lightPos;
} camera;

// Material properties - explicit locations
layout(location = 0) uniform vec3 albedo;      // Base color
layout(location = 1) uniform float metallic;    // Metallic factor (0-1)
layout(location = 2) uniform float roughness;   // Roughness factor (0-1)
layout(location = 3) uniform float ao;          // Ambient occlusion (0-1)
layout(location = 4) uniform vec3 emissive;     // Emissive color

// Texture samplers
uniform sampler2D albedoMap;
uniform sampler2D normalMap;
uniform sampler2D metallicRoughnessMap;
uniform bool useAlbedoMap = false;
uniform bool useNormalMap = false;

// Fragment output
out vec4 FragColor;

// PBR Lighting (simplified)
vec3 calculatePBR() {
    // Get albedo (from texture or uniform)
    vec3 baseColor = useAlbedoMap ? texture(albedoMap, TexCoord).rgb : albedo;

    // Normal (could sample normal map here)
    // Two-sided lighting: flip normal for backfaces
    vec3 norm = normalize(Normal);
    if (!gl_FrontFacing) {
        norm = -norm;
    }

    // Light direction
    vec3 lightDir = normalize(camera.lightPos.xyz - FragPos);

    // View direction (already calculated in VS)
    vec3 viewDir = normalize(ViewDir);

    // Reflect direction
    vec3 reflectDir = reflect(-lightDir, norm);

    // Fresnel effect (Schlick approximation)
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, baseColor, metallic);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(viewDir, reflectDir), 0.0), 5.0);

    // Diffuse (Lambert)
    vec3 diffuse = baseColor * (1.0 - F) * (1.0 - metallic);

    // Specular (Cook-Torrance GGX)
    float NdotL = max(dot(norm, lightDir), 0.0);
    float NdotV = max(dot(norm, viewDir), 0.0);
    float NdotR = max(dot(norm, reflectDir), 0.0);

    // Distribution (GGX)
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotR2 = NdotR * NdotR;
    float num = a2;
    float denom = (NdotR2 * (a2 - 1.0) + 1.0);
    denom = 3.14159 * denom * denom;
    float D = num / max(denom, 0.001);

    // Geometry (Schlick-GGX)
    float k = (a + 1.0) * (a + 1.0) / 8.0;
    float G_L = NdotL / (NdotL * (1.0 - k) + k);
    float G_V = NdotV / (NdotV * (1.0 - k) + k);
    float G = G_L * G_V;

    // specular BRDF
    vec3 specular = (D * F * G) / (4.0 * NdotL * NdotV + 0.001);

    // Final PBR lighting
    vec3 ambient = vec3(0.03) * baseColor * ao;
    vec3 result = ambient + (diffuse + specular) * NdotL;

    // Tone mapping (ACES)
    result = result / (result + vec3(1.0));
    result = pow(result, vec3(1.0/2.2));  // Gamma correction

    return result;
}

void main() {
    // Calculate PBR lighting
    vec3 color = calculatePBR();

    // Add emissive
    color += emissive;

    FragColor = vec4(color, 1.0);
}
)";
    GLuint vsObj = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vsObj, 1, &vs, nullptr);
    glCompileShader(vsObj);

    GLuint fsObj = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fsObj, 1, &fs, nullptr);
    glCompileShader(fsObj);

    g_shaderProg = glCreateProgram();
    glAttachShader(g_shaderProg, vsObj);
    glAttachShader(g_shaderProg, fsObj);
    glLinkProgram(g_shaderProg);

    glDeleteShader(vsObj);
    glDeleteShader(fsObj);
}

static void useShader() { glUseProgram(g_shaderProg); }
static void setMat4(const char* n, const glm::mat4& m) { 
    GLint loc = glGetUniformLocation(g_shaderProg, n);
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, &m[0][0]);
}
static void setVec3(const char* n, const glm::vec3& v) {
    GLint loc = glGetUniformLocation(g_shaderProg, n);
    if (loc >= 0) glUniform3fv(loc, 1, &v[0]);
}

// ============================================================================
// Cube Mesh
// ============================================================================
static GLuint g_cubeVAO = 0, g_cubeVBO = 0, g_cubeEBO = 0;
static GLuint g_sphereVAO = 0, g_sphereVBO = 0, g_sphereEBO = 0;
static GLuint g_planeVAO = 0, g_planeVBO = 0, g_planeEBO = 0;
static GLsizei g_cubeIndexCount = 36;
static GLsizei g_sphereIndexCount = 0;
static GLsizei g_planeIndexCount = 0;

static void initCube() {
    // Cube with positions, normals, AND texture coordinates
    float verts[] = {
        // positions          // normals           // texcoords
        -0.5f,-0.5f,-0.5f,  0,0,-1,  0,0,   0.5f,-0.5f,-0.5f,  0,0,-1,  1,0,   0.5f,0.5f,-0.5f,  0,0,-1,  1,1,
         0.5f,0.5f,-0.5f,  0,0,-1,  1,1,  -0.5f,0.5f,-0.5f,  0,0,-1,  0,1,  -0.5f,-0.5f,-0.5f,  0,0,-1,  0,0,
        -0.5f,-0.5f, 0.5f,  0,0, 1,  0,0,   0.5f,-0.5f, 0.5f,  0,0, 1,  1,0,   0.5f, 0.5f, 0.5f,  0,0, 1,  1,1,
         0.5f, 0.5f, 0.5f,  0,0, 1,  1,1,  -0.5f, 0.5f, 0.5f,  0,0, 1,  0,1,  -0.5f,-0.5f, 0.5f,  0,0, 1,  0,0,
        -0.5f, 0.5f, 0.5f, -1,0, 0,  0,0,  -0.5f, 0.5f,-0.5f, -1,0, 0,  1,0,  -0.5f,-0.5f,-0.5f, -1,0, 0,  1,1,
        -0.5f,-0.5f,-0.5f, -1,0, 0,  1,1,  -0.5f,-0.5f, 0.5f, -1,0, 0,  0,1,  -0.5f, 0.5f, 0.5f, -1,0, 0,  0,0,
         0.5f, 0.5f, 0.5f,  1,0, 0,  0,0,   0.5f, 0.5f,-0.5f,  1,0, 0,  1,0,   0.5f,-0.5f,-0.5f,  1,0, 0,  1,1,
         0.5f,-0.5f,-0.5f,  1,0, 0,  1,1,   0.5f,-0.5f, 0.5f,  1,0, 0,  0,1,   0.5f, 0.5f, 0.5f,  1,0, 0,  0,0,
        -0.5f,-0.5f,-0.5f,  0,-1, 0,  0,0,   0.5f,-0.5f,-0.5f,  0,-1, 0,  1,0,   0.5f,-0.5f, 0.5f,  0,-1, 0,  1,1,
         0.5f,-0.5f, 0.5f,  0,-1, 0,  1,1,  -0.5f,-0.5f, 0.5f,  0,-1, 0,  0,1,  -0.5f,-0.5f,-0.5f,  0,-1, 0,  0,0,
        -0.5f, 0.5f,-0.5f,  0, 1, 0,  0,0,   0.5f, 0.5f,-0.5f,  0, 1, 0,  1,0,   0.5f, 0.5f, 0.5f,  0, 1, 0,  1,1,
         0.5f, 0.5f, 0.5f,  0, 1, 0,  1,1,  -0.5f, 0.5f, 0.5f,  0, 1, 0,  0,1,  -0.5f, 0.5f,-0.5f,  0, 1, 0,  0,0
    };
    unsigned int idx[] = {
        0,1,2,2,3,4,5, 6,7,7,8,9, 10,11,12,12,13,14,
        15,16,17,17,18,19, 20,21,22,22,23,24, 25,26,27,27,28,29,
        30,31,32,32,33,34, 35,36,37,37,38,39
    };

    glGenVertexArrays(1, &g_cubeVAO);
    glGenBuffers(1, &g_cubeVBO);
    glGenBuffers(1, &g_cubeEBO);

    glBindVertexArray(g_cubeVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_cubeVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_cubeEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

    // Position attribute (location 0) - 3 floats
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    // Normal attribute (location 1) - 3 floats
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Texture coordinate attribute (location 2) - 2 floats
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(6*sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
}

static void drawCube() {
    glBindVertexArray(g_cubeVAO);
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

static void cleanupCube() {
    if (g_cubeVAO) glDeleteVertexArrays(1, &g_cubeVAO);
    if (g_cubeVBO) glDeleteBuffers(1, &g_cubeVBO);
    if (g_cubeEBO) glDeleteBuffers(1, &g_cubeEBO);
    g_cubeVAO = 0; g_cubeVBO = 0; g_cubeEBO = 0;
}

// ============================================================================
// Sphere Mesh (UV sphere)
// ============================================================================
static void initSphere(int rings = 16, int segments = 32) {
    std::vector<float> verts;
    std::vector<unsigned int> indices;

    for (int r = 0; r <= rings; r++) {
        float phi = M_PI * r / rings;
        for (int s = 0; s <= segments; s++) {
            float theta = 2.0f * M_PI * s / segments;

            float x = std::sin(phi) * std::cos(theta);
            float y = std::cos(phi);
            float z = std::sin(phi) * std::sin(theta);

            float u = (float)s / segments;
            float v = (float)r / rings;

            verts.push_back(x * 0.5f);  // Scale to unit sphere
            verts.push_back(y * 0.5f);
            verts.push_back(z * 0.5f);
            verts.push_back(x);  // Normal
            verts.push_back(y);
            verts.push_back(z);
            verts.push_back(u);  // Texcoord
            verts.push_back(v);
        }
    }

    for (int r = 0; r < rings; r++) {
        for (int s = 0; s < segments; s++) {
            unsigned int current = r * (segments + 1) + s;
            unsigned int next = current + segments + 1;
            indices.push_back(current);
            indices.push_back(next);
            indices.push_back(current + 1);
            indices.push_back(current + 1);
            indices.push_back(next);
            indices.push_back(next + 1);
        }
    }

    g_sphereIndexCount = indices.size();

    glGenVertexArrays(1, &g_sphereVAO);
    glGenBuffers(1, &g_sphereVBO);
    glGenBuffers(1, &g_sphereEBO);

    glBindVertexArray(g_sphereVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_sphereVBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_sphereEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
}

// ============================================================================
// Plane Mesh
// ============================================================================
static void initPlane() {
    float verts[] = {
        // positions          // normals           // texcoords
        -0.5f, 0.0f, -0.5f,  0, 1, 0,  0, 0,
         0.5f, 0.0f, -0.5f,  0, 1, 0,  1, 0,
         0.5f, 0.0f,  0.5f,  0, 1, 0,  1, 1,
        -0.5f, 0.0f,  0.5f,  0, 1, 0,  0, 1,
    };
    unsigned int indices[] = {
        0, 1, 2, 2, 3, 0
    };
    g_planeIndexCount = 6;

    glGenVertexArrays(1, &g_planeVAO);
    glGenBuffers(1, &g_planeVBO);
    glGenBuffers(1, &g_planeEBO);

    glBindVertexArray(g_planeVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_planeVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_planeEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
}

static void cleanupSphere() {
    if (g_sphereVAO) glDeleteVertexArrays(1, &g_sphereVAO);
    if (g_sphereVBO) glDeleteBuffers(1, &g_sphereVBO);
    if (g_sphereEBO) glDeleteBuffers(1, &g_sphereEBO);
    g_sphereVAO = 0; g_sphereVBO = 0; g_sphereEBO = 0;
}

static void cleanupPlane() {
    if (g_planeVAO) glDeleteVertexArrays(1, &g_planeVAO);
    if (g_planeVBO) glDeleteBuffers(1, &g_planeVBO);
    if (g_planeEBO) glDeleteBuffers(1, &g_planeEBO);
    g_planeVAO = 0; g_planeVBO = 0; g_planeEBO = 0;
}

// ============================================================================
// Grid Rendering (forward declarations - globals defined below)
// ============================================================================
static GLuint g_gridVAO = 0, g_gridVBO = 0;
static GLsizei g_gridLineCount = 0;

static void initGrid(int gridSize, float spacing);
static void drawGrid(const glm::mat4& view, const glm::mat4& projection);
static void cleanupGrid();

// ============================================================================
// Gizmo Rendering (forward declarations)
// ============================================================================
static void drawGizmo(const glm::vec3& position, float size, const glm::quat& rotation);

// ============================================================================
// Grid Rendering Implementation
// ============================================================================
static void initGrid(int gridSize, float spacing) {
    std::vector<float> gridVerts;
    float halfSize = gridSize * spacing * 0.5f;

    // Generate grid lines along X axis
    for (int i = 0; i <= gridSize; i++) {
        float x = -halfSize + i * spacing;
        gridVerts.push_back(x); gridVerts.push_back(0.0f); gridVerts.push_back(-halfSize);
        gridVerts.push_back(x); gridVerts.push_back(0.0f); gridVerts.push_back(halfSize);
    }

    // Generate grid lines along Z axis
    for (int i = 0; i <= gridSize; i++) {
        float z = -halfSize + i * spacing;
        gridVerts.push_back(-halfSize); gridVerts.push_back(0.0f); gridVerts.push_back(z);
        gridVerts.push_back(halfSize); gridVerts.push_back(0.0f); gridVerts.push_back(z);
    }

    // Make axes thicker/bolder by adding them separately
    // X axis (red)
    gridVerts.push_back(-halfSize); gridVerts.push_back(0.01f); gridVerts.push_back(0.0f);
    gridVerts.push_back(halfSize); gridVerts.push_back(0.01f); gridVerts.push_back(0.0f);
    // Z axis (blue)
    gridVerts.push_back(0.0f); gridVerts.push_back(0.01f); gridVerts.push_back(-halfSize);
    gridVerts.push_back(0.0f); gridVerts.push_back(0.01f); gridVerts.push_back(halfSize);

    g_gridLineCount = static_cast<GLsizei>(gridVerts.size() / 3);

    glGenVertexArrays(1, &g_gridVAO);
    glGenBuffers(1, &g_gridVBO);

    glBindVertexArray(g_gridVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_gridVBO);
    glBufferData(GL_ARRAY_BUFFER, gridVerts.size() * sizeof(float), gridVerts.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

// ============================================================================
// Globals
// ============================================================================
static ecs::World g_world;
static flyCamera* g_camera = nullptr;
static ecs::EntityID g_selected = ecs::INVALID_ENTITY_ID;
static float g_fps = 0.0f;

// Renderer and Render System
static Renderer g_renderer;
static ecs::RenderSystem g_renderSystem;
static ecs::GeospatialSystem g_geospatialSystem;
static Model* g_currentModel = nullptr;

// Editor State
enum class GizmoType { None, Translate, Rotate, Scale };
enum class SpaceType { Local, World };
static GizmoType g_gizmoType = GizmoType::Translate;
static SpaceType g_spaceType = SpaceType::World;
static bool g_showGrid = true;
static bool g_showGizmo = true;
static bool g_showWireframe = false;  // Wireframe debug mode

// Viewport camera controls
static bool g_isViewing = false;
static glm::vec2 g_lastMousePos;

// UI Layout constants (must match ImGui panel dimensions)
static constexpr float kMenuBarHeight = 25.0f;
static constexpr float kToolbarHeight = 36.0f;
static constexpr float kSidePanelWidth = 280.0f;
static constexpr float kTabbedBottomHeight = 180.0f;  // console height

// UI State (non-static to avoid lifetime issues)
struct UIState {
    char searchBuffer[128] = "";
    char pathBuffer[256] = "/Game/Assets";
    int leftPanelTab = 0;  // 0=Outliner, 1=Details
    int bottomPanelTab = 0;  // 0=Content, 1=Console, 2=Profiler
};
static UIState g_uiState;

// Game State
static bool g_isPlaying = false;
static bool g_wasPlaying = false;
static float g_gameSpeed = 1.0f;

// Debug configuration
struct DebugConfig {
    bool verbose = true;
};
static DebugConfig g_debugConfig;

// Console/Log system
struct LogMessage {
    std::string text;
    float timestamp;
    int level;  // 0=info, 1=warning, 2=error
};
static std::vector<LogMessage> g_consoleMessages;
static bool g_showConsole = true;
static int g_consoleFilter = -1;  // -1=all, 0=info, 1=warning, 2=error

void logMessage(const std::string& text, int level = 0) {
    LogMessage msg;
    msg.text = text;
    msg.timestamp = glfwGetTime();
    msg.level = level;
    g_consoleMessages.push_back(msg);
    
    // Keep only last 1000 messages
    if (g_consoleMessages.size() > 1000) {
        g_consoleMessages.erase(g_consoleMessages.begin());
    }
    
    // Also print to stdout
    const char* levelStr = level == 2 ? "[ERROR] " : level == 1 ? "[WARN] " : "[INFO] ";
    std::cout << levelStr << text << "\n";
}

// Entity management
void deleteSelectedEntity() {
    if (g_selected == ecs::INVALID_ENTITY_ID) return;

    // TODO: Properly destroy entity through ECS
    logMessage("Deleted entity " + std::to_string(g_selected), 0);
    g_selected = ecs::INVALID_ENTITY_ID;
}

void duplicateSelectedEntity() {
    if (g_selected == ecs::INVALID_ENTITY_ID) return;

    ecs::Entity entity{g_selected};
    auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(entity);
    auto* m = g_world.getComponentArchetype<ecs::MeshComponent>(entity);

    if (t && m) {
        auto newEntity = g_world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
        auto* newT = g_world.getComponentArchetype<ecs::TransformComponent>(newEntity);
        auto* newM = g_world.getComponentArchetype<ecs::MeshComponent>(newEntity);

        if (newT && newM) {
            newT->position = t->position + glm::vec3(1, 0, 0);
            newT->rotation = t->rotation;
            newT->scale = t->scale;
            newM->color = m->color;
            newM->visible = m->visible;
            newM->meshID = m->meshID;

            logMessage("Duplicated entity " + std::to_string(g_selected) + " -> " + std::to_string(newEntity.id), 0);
        }
    }
}

// ============================================================================
// Entity Creation (using archetype storage)
// ============================================================================
ecs::Entity createCube(const glm::vec3& pos, const glm::vec3& scale, const glm::vec3& color) {
    // Create entity with Transform + Mesh only (NameComponent causes issues)
    auto e = g_world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    if (!e.isValid()) return e;

    // Use getComponentArchetype for archetype-stored components
    auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_world.getComponentArchetype<ecs::MeshComponent>(e);

    if (t) { t->position = pos; t->scale = scale; t->rotation = glm::quat(1, 0, 0, 0); }
    if (m) { m->visible = true; m->meshID = 0; m->color = color; }

    return e;
}

ecs::Entity createSphere(const glm::vec3& pos, float radius, const glm::vec3& color) {
    auto e = g_world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    if (!e.isValid()) return e;

    auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_world.getComponentArchetype<ecs::MeshComponent>(e);

    if (t) { t->position = pos; t->scale = glm::vec3(radius); t->rotation = glm::quat(1, 0, 0, 0); }
    if (m) { m->visible = true; m->color = color; m->meshID = 0; }

    return e;
}

ecs::Entity createPlane(const glm::vec3& pos, const glm::vec2& size, const glm::vec3& color) {
    auto e = g_world.createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent>();
    if (!e.isValid()) return e;

    auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_world.getComponentArchetype<ecs::MeshComponent>(e);

    if (t) { t->position = pos; t->scale = glm::vec3(size.x, 0.01f, size.y); t->rotation = glm::quat(1, 0, 0, 0); }
    if (m) { m->visible = true; m->color = color; m->meshID = 0; }

    return e;
}

ecs::Entity createLight(const glm::vec3& pos, const glm::vec3& color, float intensity) {
    auto e = g_world.createEntityWithComponents<ecs::TransformComponent>();
    if (!e.isValid()) return e;

    auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(e);

    if (t) { t->position = pos; t->scale = glm::vec3(0.2f); t->rotation = glm::quat(1, 0, 0, 0); }

    // TODO: Add LightComponent when available
    (void)color; (void)intensity;  // Suppress unused warnings

    return e;
}

ecs::Entity createCamera(const glm::vec3& pos, const glm::vec3& target) {
    auto e = g_world.createEntityWithComponents<ecs::TransformComponent>();
    if (!e.isValid()) return e;

    auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(e);

    if (t) { t->position = pos; t->scale = glm::vec3(1); t->rotation = glm::quat(1, 0, 0, 0); }

    // TODO: Add CameraComponent when available
    (void)target;  // Suppress unused warnings

    return e;
}

// ============================================================================
// Grid Rendering Implementation
// ============================================================================
static void drawGrid(const glm::mat4& view, const glm::mat4& projection) {
    if (!g_showGrid || g_gridVAO == 0) return;

    glDisable(GL_DEPTH_TEST);  // Grid always renders on top
    glUseProgram(g_shaderProg);

    // Set view/projection
    GLint viewLoc = glGetUniformLocation(g_shaderProg, "view");
    GLint projLoc = glGetUniformLocation(g_shaderProg, "projection");
    GLint modelLoc = glGetUniformLocation(g_shaderProg, "model");
    GLint colorLoc = glGetUniformLocation(g_shaderProg, "color");

    glUniformMatrix4fv(viewLoc, 1, GL_FALSE, &view[0][0]);
    glUniformMatrix4fv(projLoc, 1, GL_FALSE, &projection[0][0]);

    // Draw grid lines (gray)
    glm::mat4 model = glm::mat4(1.0f);
    glUniformMatrix4fv(modelLoc, 1, GL_FALSE, &model[0][0]);
    glUniform3f(colorLoc, 0.3f, 0.3f, 0.35f);

    glBindVertexArray(g_gridVAO);
    // Draw all lines except last 4 (axes)
    glDrawArrays(GL_LINES, 0, g_gridLineCount - 4);

    // Draw X axis (red)
    glUniform3f(colorLoc, 0.8f, 0.2f, 0.2f);
    glDrawArrays(GL_LINES, g_gridLineCount - 4, 2);

    // Draw Z axis (blue)
    glUniform3f(colorLoc, 0.2f, 0.2f, 0.8f);
    glDrawArrays(GL_LINES, g_gridLineCount - 2, 2);

    glBindVertexArray(0);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}

static void cleanupGrid() {
    if (g_gridVAO) glDeleteVertexArrays(1, &g_gridVAO);
    if (g_gridVBO) glDeleteBuffers(1, &g_gridVBO);
    g_gridVAO = 0; g_gridVBO = 0; g_gridLineCount = 0;
}

// ============================================================================
// Gizmo Rendering (Translate/Rotate/Scale)
// ============================================================================
static void drawGizmo(const glm::vec3& position, float size, const glm::quat& rotation) {
    if (!g_showGizmo || g_selected == ecs::INVALID_ENTITY_ID) return;
    if (g_gizmoType == GizmoType::None) return;

    glDisable(GL_DEPTH_TEST);
    glUseProgram(g_shaderProg);

    GLint viewLoc = glGetUniformLocation(g_shaderProg, "view");
    GLint projLoc = glGetUniformLocation(g_shaderProg, "projection");
    GLint modelLoc = glGetUniformLocation(g_shaderProg, "model");
    GLint colorLoc = glGetUniformLocation(g_shaderProg, "color");

    // Build rotation matrix from quaternion
    glm::mat4 rotMat = glm::toMat4(rotation);

    // Gizmo axes: X=red, Y=green, Z=blue
    struct AxisGizmo {
        glm::vec3 direction;
        glm::vec3 color;
        std::vector<float> verts;
    };

    float arrowLen = size * 0.8f;
    float headLen = size * 0.25f;
    float headWidth = size * 0.12f;

    AxisGizmo axes[3];

    // X axis (red)
    axes[0].direction = glm::vec3(1, 0, 0);
    axes[0].color = glm::vec3(1.0f, 0.2f, 0.2f);
    axes[0].verts = {
        // Line
        0, 0, 0,  arrowLen, 0, 0,
        // Arrow head
        arrowLen, 0, 0,  arrowLen - headLen, headWidth, 0,
        arrowLen, 0, 0,  arrowLen - headLen, -headWidth, 0,
        arrowLen, 0, 0,  arrowLen - headLen, 0, headWidth,
        arrowLen, 0, 0,  arrowLen - headLen, 0, -headWidth,
    };

    // Y axis (green)
    axes[1].direction = glm::vec3(0, 1, 0);
    axes[1].color = glm::vec3(0.2f, 1.0f, 0.2f);
    axes[1].verts = {
        0, 0, 0,  0, arrowLen, 0,
        0, arrowLen, 0,  headWidth, arrowLen - headLen, 0,
        0, arrowLen, 0,  -headWidth, arrowLen - headLen, 0,
        0, arrowLen, 0,  0, arrowLen - headLen, headWidth,
        0, arrowLen, 0,  0, arrowLen - headLen, -headWidth,
    };

    // Z axis (blue)
    axes[2].direction = glm::vec3(0, 0, 1);
    axes[2].color = glm::vec3(0.2f, 0.2f, 1.0f);
    axes[2].verts = {
        0, 0, 0,  0, 0, arrowLen,
        0, 0, arrowLen,  headWidth, 0, arrowLen - headLen,
        0, 0, arrowLen,  -headWidth, 0, arrowLen - headLen,
        0, 0, arrowLen,  0, headWidth, arrowLen - headLen,
        0, 0, arrowLen,  0, -headWidth, arrowLen - headLen,
    };

    // For rotate gizmo, draw circles instead of arrows
    if (g_gizmoType == GizmoType::Rotate) {
        float radius = size * 0.6f;
        int segments = 32;

        for (int a = 0; a < 3; a++) {
            axes[a].verts.clear();
            glm::vec3 d = axes[a].direction;

            // Find two perpendicular axes
            glm::vec3 perp1, perp2;
            if (abs(d.x) > 0.9f) { perp1 = glm::vec3(0, 0, 1); }
            else { perp1 = glm::vec3(1, 0, 0); }
            perp1 = glm::normalize(glm::cross(d, perp1));
            perp2 = glm::normalize(glm::cross(d, perp1));

            for (int i = 0; i < segments; i++) {
                float angle1 = (float)i / segments * 2.0f * 3.14159f;
                float angle2 = (float)(i + 1) / segments * 2.0f * 3.14159f;

                glm::vec3 p1 = glm::cos(angle1) * perp1 * radius + glm::sin(angle1) * perp2 * radius;
                glm::vec3 p2 = glm::cos(angle2) * perp1 * radius + glm::sin(angle2) * perp2 * radius;

                axes[a].verts.push_back(p1.x); axes[a].verts.push_back(p1.y); axes[a].verts.push_back(p1.z);
                axes[a].verts.push_back(p2.x); axes[a].verts.push_back(p2.y); axes[a].verts.push_back(p2.z);
            }
        }
    }

    // For scale gizmo, draw small cubes at axis ends
    if (g_gizmoType == GizmoType::Scale) {
        for (int a = 0; a < 3; a++) {
            float endPos = size * 0.8f;
            glm::vec3 end = axes[a].direction * endPos;
            float cubeSize = size * 0.15f;

            axes[a].verts = {
                // Line to cube
                0, 0, 0,  end.x, end.y, end.z,
                // Small cube at end (wireframe edges)
                end.x - cubeSize, end.y - cubeSize, end.z - cubeSize,  end.x + cubeSize, end.y - cubeSize, end.z - cubeSize,
                end.x + cubeSize, end.y - cubeSize, end.z - cubeSize,  end.x + cubeSize, end.y + cubeSize, end.z - cubeSize,
                end.x + cubeSize, end.y + cubeSize, end.z - cubeSize,  end.x - cubeSize, end.y + cubeSize, end.z - cubeSize,
                end.x - cubeSize, end.y + cubeSize, end.z - cubeSize,  end.x - cubeSize, end.y - cubeSize, end.z - cubeSize,
                end.x - cubeSize, end.y - cubeSize, end.z + cubeSize,  end.x + cubeSize, end.y - cubeSize, end.z + cubeSize,
                end.x + cubeSize, end.y - cubeSize, end.z + cubeSize,  end.x + cubeSize, end.y + cubeSize, end.z + cubeSize,
                end.x + cubeSize, end.y + cubeSize, end.z + cubeSize,  end.x - cubeSize, end.y + cubeSize, end.z + cubeSize,
                end.x - cubeSize, end.y + cubeSize, end.z + cubeSize,  end.x - cubeSize, end.y - cubeSize, end.z + cubeSize,
                // Connect front to back
                end.x - cubeSize, end.y - cubeSize, end.z - cubeSize,  end.x - cubeSize, end.y - cubeSize, end.z + cubeSize,
                end.x + cubeSize, end.y - cubeSize, end.z - cubeSize,  end.x + cubeSize, end.y - cubeSize, end.z + cubeSize,
                end.x + cubeSize, end.y + cubeSize, end.z - cubeSize,  end.x + cubeSize, end.y + cubeSize, end.z + cubeSize,
                end.x - cubeSize, end.y + cubeSize, end.z - cubeSize,  end.x - cubeSize, end.y + cubeSize, end.z + cubeSize,
            };
        }
    }

    // Create VAO for gizmo
    GLuint gizmoVAO, gizmoVBO;
    glGenVertexArrays(1, &gizmoVAO);
    glGenBuffers(1, &gizmoVBO);
    glBindVertexArray(gizmoVAO);
    glBindBuffer(GL_ARRAY_BUFFER, gizmoVBO);
    glBufferData(GL_ARRAY_BUFFER, 256 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(0);

    // Draw each axis
    for (int a = 0; a < 3; a++) {
        // Transform gizmo vertices by position and rotation
        std::vector<float> transformedVerts;
        transformedVerts.reserve(axes[a].verts.size());

        for (size_t i = 0; i < axes[a].verts.size(); i += 3) {
            glm::vec3 localPos(axes[a].verts[i], axes[a].verts[i+1], axes[a].verts[i+2]);
            glm::vec3 worldPos = position + glm::vec3(rotMat * glm::vec4(localPos, 1.0f));
            transformedVerts.push_back(worldPos.x);
            transformedVerts.push_back(worldPos.y);
            transformedVerts.push_back(worldPos.z);
        }

        glBufferSubData(GL_ARRAY_BUFFER, 0, transformedVerts.size() * sizeof(float), transformedVerts.data());

        glUniform3f(colorLoc, axes[a].color.r, axes[a].color.g, axes[a].color.b);
        glDrawArrays(GL_LINES, 0, transformedVerts.size() / 3);
    }

    glBindVertexArray(0);
    glDeleteVertexArrays(1, &gizmoVAO);
    glDeleteBuffers(1, &gizmoVBO);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}

// ============================================================================
// Scene Serialization (Simple JSON-like format)
// ============================================================================
#include <fstream>
#include <sstream>

static std::string g_currentSceneFile;

bool saveScene(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "ERROR: Could not open file for writing: " << filename << "\n";
        return false;
    }

    file << "{\n";
    file << "  \"version\": \"1.0\",\n";
    file << "  \"entities\": [\n";

    bool first = true;
    g_world.forEach<ecs::TransformComponent, ecs::NameComponent>(
        [&](ecs::EntityID id, ecs::TransformComponent& t, ecs::NameComponent& n) {
            if (!first) file << ",\n";
            first = false;

            file << "    {\n";
            file << "      \"name\": \"" << n.name << "\",\n";
            file << "      \"position\": [" << t.position.x << ", " << t.position.y << ", " << t.position.z << "],\n";
            file << "      \"rotation\": [" << t.rotation.x << ", " << t.rotation.y << ", " << t.rotation.z << ", " << t.rotation.w << "],\n";
            file << "      \"scale\": [" << t.scale.x << ", " << t.scale.y << ", " << t.scale.z << "]\n";
            file << "    }";
        });

    file << "\n  ]\n";
    file << "}\n";

    file.close();
    g_currentSceneFile = filename;
    std::cout << "Scene saved to: " << filename << "\n";
    return true;
}

bool loadScene(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "ERROR: Could not open file for reading: " << filename << "\n";
        return false;
    }

    // Clear current scene
    g_world.shutdown();
    g_world.init();

    // Simple parsing (production would use proper JSON library)
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    // Parse entities (simplified - just looking for position patterns)
    size_t pos = 0;
    int entityCount = 0;

    while ((pos = content.find("\"position\":", pos)) != std::string::npos) {
        size_t start = content.find("[", pos);
        size_t end = content.find("]", start);
        if (start != std::string::npos && end != std::string::npos) {
            std::string posStr = content.substr(start + 1, end - start - 1);
            float x, y, z;
            if (sscanf(posStr.c_str(), "%f, %f, %f", &x, &y, &z) == 3) {
                createCube(glm::vec3(x, y, z), glm::vec3(1), glm::vec3(0.8f, 0.8f, 0.8f));
                entityCount++;
            }
        }
        pos = end;
    }

    g_currentSceneFile = filename;
    std::cout << "Loaded " << entityCount << " entities from: " << filename << "\n";
    return true;
}

// ============================================================================
// About Dialog
// ============================================================================
static bool g_showAbout = false;

void showAboutDialog() {
    if (!g_showAbout) return;

    ImGui::OpenPopup("About RTT Engine");
    if (ImGui::BeginPopupModal("About RTT Engine", &g_showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1), "RTT Engine Editor");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(200, 0));
        ImGui::Text("Version: 1.0.0");
        ImGui::Text("Built: %s %s", __DATE__, __TIME__);
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::TextWrapped("A professional 3D game engine editor with:");
        ImGui::BulletText("ECS Architecture");
        ImGui::BulletText("Real-time rendering");
        ImGui::BulletText("Physics simulation");
        ImGui::BulletText("Animation system");
        ImGui::BulletText("Motion matching");
        ImGui::Dummy(ImVec2(0, 15));
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Built with Dear ImGui, OpenGL, and GLFW");
        ImGui::Dummy(ImVec2(0, 15));

        if (ImGui::Button("OK", ImVec2(120, 0))) {
            g_showAbout = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

// ============================================================================
// Render Scene to FBO
// ============================================================================
static int g_frameCount = 0;
static float g_lastRenderTime = 0;

void renderScene() {
    PROFILE_GPU_SCOPE("Render Scene");
    
    g_frameCount++;
    float currentTime = glfwGetTime();
    
    // Print debug info every 60 frames
    if (g_frameCount % 60 == 0 && g_debugConfig.verbose) {
        float dt = currentTime - g_lastRenderTime;
        if (dt > 0) {
            std::cout << "[DEBUG] Frame " << g_frameCount 
                      << " | Entities: " << g_world.getEntityCount()
                      << " | FPS: " << (1.0f/dt) << "\n";
        }
        g_lastRenderTime = currentTime;
    }

    // 🔵 2. Render scene INTO framebuffer (your viewport)
    g_viewportFB.bind();

    // Set viewport to match FBO size
    glViewport(0, 0, g_viewportFB.width, g_viewportFB.height);

    // Enable proper state
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Wireframe mode for debugging
    if (g_showWireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    } else {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    // Clear
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Set renderer viewport and camera matrices
    g_renderer.SetViewport(0, 0, g_viewportFB.width, g_viewportFB.height);

    glm::mat4 view = glm::lookAt(g_camera->Position, g_camera->Target, glm::vec3(0,1,0));
    glm::mat4 proj = glm::perspective(glm::radians(60.0f),
                                       (float)g_viewportFB.width / g_viewportFB.height,
                                       0.1f, 1000.0f);
    g_renderer.SetCameraMatrices(view, proj);
    g_renderer.SetLightParameters(glm::vec3(5.0f, 5.0f, 5.0f), g_camera->Position);

    // Render all ECS entities through RenderSystem (uses SubmitBatches internally)
    g_renderSystem.render();

    // === ECS RENDERING ===
    // Use the ECS RenderSystem instead of direct rendering
    static bool g_useDirectRendering = false;  // Set to true to use direct rendering fallback
    static int g_debugPrintCounter = 0;
    
    if (g_useDirectRendering) {
        g_debugPrintCounter++;
        
        // Clear any previous errors
        while (glGetError() != GL_NO_ERROR);
        
        if (g_debugPrintCounter <= 3) {
            std::cout << "[DIRECT_RENDER] Drawing cubes directly! (frame " << g_frameCount << ")" << std::endl;
            std::cout << "[DIRECT_RENDER] Shader: " << g_shaderProg << ", VAO: " << g_cubeVAO << std::endl;
            std::cout << "[DIRECT_RENDER] FBO bound: " << g_viewportFB.fbo << ", Size: " 
                      << g_viewportFB.width << "x" << g_viewportFB.height << std::endl;
        }
        
        // Render cubes directly using the same approach as viewport_debug_test
        glUseProgram(g_shaderProg);
        glBindVertexArray(g_cubeVAO);
        
        // Set view/projection uniforms
        GLint viewLoc = glGetUniformLocation(g_shaderProg, "view");
        GLint projLoc = glGetUniformLocation(g_shaderProg, "projection");
        GLint modelLoc = glGetUniformLocation(g_shaderProg, "model");
        GLint colorLoc = glGetUniformLocation(g_shaderProg, "color");
        GLint lightPosLoc = glGetUniformLocation(g_shaderProg, "lightPos");
        GLint viewPosLoc = glGetUniformLocation(g_shaderProg, "viewPos");
        
        if (g_debugPrintCounter <= 3) {
            std::cout << "[DIRECT_RENDER] Uniform locations: view=" << viewLoc 
                      << " proj=" << projLoc << " model=" << modelLoc 
                      << " color=" << colorLoc << " light=" << lightPosLoc 
                      << " viewPos=" << viewPosLoc << std::endl;
        }
        
        // Set uniforms while shader is bound
        glUniformMatrix4fv(viewLoc, 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(projLoc, 1, GL_FALSE, &proj[0][0]);
        glUniform3f(lightPosLoc, 5.0f, 5.0f, 5.0f);
        glUniform3f(viewPosLoc, g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
        
        // Draw 3 cubes at different positions with different colors
        struct CubeData {
            glm::vec3 pos;
            glm::vec3 color;
            glm::vec3 scale;
        };
        
        CubeData cubes[] = {
            {{0.0f, 1.0f, 0.0f}, {1.0f, 0.2f, 0.2f}, {1.0f, 1.0f, 1.0f}},    // Red
            {{2.0f, 2.0f, 0.0f}, {0.2f, 1.0f, 0.2f}, {0.5f, 0.5f, 0.5f}},    // Green
            {{-2.0f, 3.0f, 0.0f}, {0.2f, 0.2f, 1.0f}, {0.7f, 0.7f, 0.7f}}    // Blue
        };
        
        for (int i = 0; i < 3; i++) {
            glm::mat4 model = glm::translate(glm::mat4(1.0f), cubes[i].pos);
            model = glm::scale(model, cubes[i].scale);
            glUniformMatrix4fv(modelLoc, 1, GL_FALSE, &model[0][0]);
            glUniform3f(colorLoc, cubes[i].color.r, cubes[i].color.g, cubes[i].color.b);
            
            if (g_debugPrintCounter <= 3) {
                std::cout << "[DIRECT_RENDER] Drawing cube " << i << " at (" 
                          << cubes[i].pos.x << "," << cubes[i].pos.y << "," << cubes[i].pos.z << ")" << std::endl;
            }
            
            glDrawArrays(GL_TRIANGLES, 0, 36);
            
            // Check error after each draw
            GLenum drawErr = glGetError();
            if (drawErr != GL_NO_ERROR && g_debugPrintCounter <= 3) {
                std::cerr << "[DIRECT_RENDER] Error after drawing cube " << i << ": " << drawErr << std::endl;
            }
        }
        
        glUseProgram(0);
        glBindVertexArray(0);
    }
    else {
        // Render all ECS entities through RenderSystem
        g_renderSystem.render();
    }

    // Render grid (uses same shader with simple color mode)
    drawGrid(view, proj);

    // Render gizmo for selected entity
    ecs::Entity selectedEntity{g_selected};
    if (selectedEntity.isValid() && g_selected != ecs::INVALID_ENTITY_ID) {
        auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(selectedEntity);
        if (t) {
            float gizmoSize = glm::max(1.0f, glm::distance(g_camera->Position, t->position) * 0.15f);
            drawGizmo(t->position, gizmoSize, t->rotation);
        }
    }

    // DEBUG: Draw a visible test quad to verify FBO is working
    // This helps distinguish between "FBO broken" vs "scene not rendering"
   // glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(0);
    glBindVertexArray(0);

    // Draw a small green rectangle in bottom-right corner as FBO activity indicator
    glBegin(GL_QUADS);
    glColor3f(0.0f, 1.0f, 0.0f);  // Green = FBO is working
    glVertex2f(0.9f, -0.95f);
    glVertex2f(0.98f, -0.95f);
    glVertex2f(0.98f, -0.85f);
    glVertex2f(0.9f, -0.85f);
    glEnd();

    // Reset polygon mode (wireframe debug)
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // 🔴 3. Restore default framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    //glDisable(GL_CULL_FACE);
}

// ============================================================================
// UI Helper Functions
// ============================================================================
void renderTransformSection(ecs::TransformComponent* t) {
    if (!t) return;
    
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Transform");
        
        ImGui::SeparatorText("Position");
        glm::vec3 p = t->position;
        if (ImGui::DragFloat3("##Position", &p.x, 0.1f, -10000, 10000)) {
            t->position = p;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset##Pos", ImVec2(60, 0))) {
            t->position = glm::vec3(0);
        }
        
        ImGui::SeparatorText("Rotation");
        glm::vec3 r = glm::degrees(glm::eulerAngles(t->rotation));
        if (ImGui::DragFloat3("##Rotation", &r.x, 1.0f, -180, 180)) {
            t->setEulerAngles(glm::radians(r));
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset##Rot", ImVec2(60, 0))) {
            t->rotation = glm::quat(1, 0, 0, 0);
        }
        
        ImGui::SeparatorText("Scale");
        glm::vec3 s = t->scale;
        if (ImGui::DragFloat3("##Scale", &s.x, 0.01f, 0.01f, 1000)) {
            t->scale = s;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset##Scale", ImVec2(60, 0))) {
            t->scale = glm::vec3(1);
        }
        
        ImGui::PopID();
    }
}

void renderMeshSection(ecs::MeshComponent* m) {
    if (!m) return;

    if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Mesh");

        ImGui::SeparatorText("Appearance");
        glm::vec3 c = m->color;
        if (ImGui::ColorEdit3("Albedo", &c.x, ImGuiColorEditFlags_Float)) {
            m->color = c;  // Real-time color change!
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(60, 0))) {
            m->color = glm::vec3(0.8f);
        }
        
        ImGui::Checkbox("Visible", &m->visible);
        
        ImGui::SeparatorText("Material");
        ImGui::SliderFloat("Metallic", &m->metallic, 0.0f, 1.0f);
        ImGui::SliderFloat("Roughness", &m->roughness, 0.0f, 1.0f);

        ImGui::PopID();
    }
}

// ============================================================================
// Font Loading Helper (using stb_truetype - no freetype dependency)
// ============================================================================

static bool loadIconFont(ImGuiIO& io) {
    // Open log file for debugging
    FILE* logFile = fopen("font_load.log", "w");
    if (logFile) {
        fprintf(logFile, "=== Font Loading Log ===\n");
        fflush(logFile);
    }
    
    // Common Nerd Font and monospace font locations
    const char* fontPaths[] = {
        // User's local fonts (highest priority - your system has these!)
        "/home/run-time-terror/.local/share/fonts/FiraCodeNerdFont-Regular.ttf",
        "/home/run-time-terror/.local/share/fonts/FiraCodeNerdFontMono-Regular.ttf",
        // Generic user font directories
        "/home/run-time-terror/.local/share/fonts/JetBrainsMonoNerdFont-Regular.ttf",
        // Nerd Fonts (priority - these have icons)
        "/usr/share/fonts/truetype/JetBrainsMono/JetBrainsMonoNerdFont-Regular.ttf",
        "/usr/share/fonts/truetype/JetBrainsMonoNL-Regular.ttf",
        "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf",
        "/usr/share/fonts/opentype/jetbrains-mono/JetBrainsMonoNerdFont-Regular.ttf",
        "/usr/share/fonts/jetbrains-mono/JetBrainsMonoNerdFont-Regular.ttf",
        "/usr/share/fonts/truetype/firacode/FiraCodeNerdFont-Regular.ttf",
        "/usr/share/fonts/TTF/FiraCodeNerdFont-Regular.ttf",
        "/usr/share/fonts/truetype/NerdFonts/JetBrainsMonoNerdFont-Regular.ttf",
        // Ubuntu Mono (available on this system)
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono[wght].ttf",
        // Common monospace fonts (no icons but better than default)
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoMono-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
        nullptr
    };
    
    ImFontConfig fontConfig;
    fontConfig.MergeMode = false;
    fontConfig.PixelSnapH = true;
    
    // Load main font with icon range
    ImVector<ImWchar> ranges;
    ImFontGlyphRangesBuilder builder;
    
    // Add default ranges plus FontAwesome icons
    builder.AddText("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!@#$%^&*()_+-=[]{}|;':\",./<>?");
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    
    // Add FontAwesome icon range
    ImWchar iconRange[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    builder.AddRanges(iconRange);
    
    builder.BuildRanges(&ranges);
    
    for (int i = 0; fontPaths[i] != nullptr; i++) {
        struct stat buffer;
        if (stat(fontPaths[i], &buffer) == 0) {
            std::cout << "Loading font: " << fontPaths[i] << "\n";
            if (logFile) {
                fprintf(logFile, "SUCCESS: Loaded font: %s\n", fontPaths[i]);
                fflush(logFile);
            }
            io.Fonts->AddFontFromFileTTF(fontPaths[i], 16.0f, &fontConfig, ranges.Data);
            
            // Check if it's a Nerd Font (supports icons)
            bool hasIcons = (strstr(fontPaths[i], "Nerd") != nullptr || 
                           strstr(fontPaths[i], "nerd") != nullptr);
            if (hasIcons) {
                std::cout << "Icon font loaded successfully!\n";
                if (logFile) {
                    fprintf(logFile, "This font supports icons (Nerd Font detected)\n");
                    fflush(logFile);
                }
            } else {
                std::cout << "Note: This font doesn't support icons.\n";
                if (logFile) {
                    fprintf(logFile, "Note: This font does not support icons\n");
                    fflush(logFile);
                }
            }
            if (logFile) fclose(logFile);
            return true;
        } else {
            if (logFile) {
                fprintf(logFile, "NOT FOUND: %s\n", fontPaths[i]);
                fflush(logFile);
            }
        }
    }
    
    std::cout << "No custom font found, using default ImGui font.\n";
    std::cout << "For better fonts and icons, install a Nerd Font:\n";
    std::cout << "  sudo apt install fonts-font-awesome fonts-jetbrains-mono\n";
    std::cout << "  OR download from: https://www.nerdfonts.com/font-downloads\n";
    
    if (logFile) {
        fprintf(logFile, "FAILURE: No fonts found, using default\n");
        fclose(logFile);
    }
    return false;
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "=== RTT Engine Editor ===\n";

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "ERROR: Failed to initialize GLFW\n";
        return -1;
    }
    
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1920, 1080, "RTT Engine Editor", nullptr, nullptr);
    if (!window) {
        std::cerr << "ERROR: Failed to create GLFW window\n";
        glfwTerminate();
        return -1;
    }
    
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);  // Disable vsync for max FPS

    // Initialize GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "ERROR: Failed to initialize GLAD\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    std::cout << "OpenGL: " << glGetString(GL_VERSION) << "\n";

    // Initialize GPU Profiler
    std::cout << "Initializing GPU Profiler...\n";
    if (!AdvancedGPUProfiler::getInstance().initialize()) {
        std::cerr << "WARNING: GPU Profiler initialization failed\n";
    }

    // Initialize resources
    initShader();
    initCube();
    initSphere(16, 32);
    initPlane();
    initGrid(20, 1.0f);  // Initialize 20x20 grid with 1 unit spacing
    createProceduralTexture();  // Create checkerboard texture
    g_viewportFB.init(1280, 720);

    // Register procedural meshes in MeshRegistry
    auto& meshReg = MeshRegistry::getInstance();
    meshReg.registerMesh(ecs::MeshType::Cube, g_cubeVAO, g_cubeVBO, g_cubeEBO, g_cubeIndexCount);
    meshReg.registerMesh(ecs::MeshType::Sphere, g_sphereVAO, g_sphereVBO, g_sphereEBO, g_sphereIndexCount);
    meshReg.registerMesh(ecs::MeshType::Plane, g_planeVAO, g_planeVBO, g_planeEBO, g_planeIndexCount);
    std::cout << "[MeshRegistry] Registered Cube, Sphere, Plane\n";

    // Set texture for renderer
    g_renderer.SetDefaultTexture(g_cubeTexture);

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "engine_ui.ini";

    // Enable proper menu and window behavior
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    
    // Fix resize duplication - ensure proper alpha blending
    io.ConfigWindowsResizeFromEdges = true;
    
    // Load icon font
    loadIconFont(io);

    // Setup ImGui style
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.WindowPadding = ImVec2(4, 4);
    style.FramePadding = ImVec2(6, 3);
    style.ItemSpacing = ImVec2(6, 4);
    
    // Setup ImGui colors (Unreal-inspired dark theme)
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);  // Fully opaque
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.60f, 0.40f, 0.0f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.60f, 0.40f, 0.0f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.50f, 0.50f, 0.50f, 0.5f);  // Semi-transparent grip
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.60f, 0.60f, 0.60f, 0.7f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.70f, 0.70f, 0.70f, 0.9f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 450");

    // Initialize camera and ECS
    g_camera = new flyCamera(glm::vec3(0, 5, 10), glm::vec3(0, 0, 0), -90, 0, 10);
    g_world.init();
    g_world.addSystem<ecs::PhysicsSystem>().setGravity(glm::vec3(0, -9.81f, 0));

    // Initialize Renderer and Render System
    std::cout << "Initializing Renderer...\n";
    g_renderer.Initialize();
    std::cout << "Renderer initialized with UBO and optimizations\n";
    g_renderSystem.setRenderer(&g_renderer);
    g_renderSystem.setWorld(&g_world);  // Set world pointer for iteration
    std::cout << "RenderSystem configured\n";

    // Create a simple test model from VAO (cube)
    g_currentModel = Model::CreateFromVAO(g_cubeVAO, 36);
    std::cout << "Test model created from VAO (VAO=" << g_cubeVAO << ")\n";
    g_renderSystem.setModel(g_currentModel);

    // Set shader program for rendering
    g_renderSystem.setDefaultShaderProgram(g_shaderProg);
    std::cout << "Shader program set (" << g_shaderProg << ") with UBO support\n";

    // Add render system to world (use our global instance)
    g_world.addSystem(&g_renderSystem);
    std::cout << "RenderSystem added to world\n";

    // Initialize Geospatial System (Sydney Opera House as default origin)
    g_geospatialSystem.initialize(-33.8568, 151.2153, 50.0);
    g_geospatialSystem.setGPSMode(GPSTracker::Mode::SIMULATED_WALK);
    g_world.addSystem(&g_geospatialSystem);
    std::cout << "GeospatialSystem added to world\n";

    // Create initial scene (AFTER render system is added)
    std::cout << "Creating test cubes...\n";
    auto cube1 = createCube(glm::vec3(0, 1, 0), glm::vec3(1), glm::vec3(0.8f, 0.2f, 0.2f));
    std::cout << "Created cube 1 (entity: " << cube1.id << ")\n";
    auto cube2 = createCube(glm::vec3(2, 2, 0), glm::vec3(0.5f), glm::vec3(0.2f, 0.8f, 0.2f));
    std::cout << "Created cube 2 (entity: " << cube2.id << ")\n";
    auto cube3 = createCube(glm::vec3(-2, 3, 0), glm::vec3(0.7f), glm::vec3(0.2f, 0.2f, 0.8f));
    std::cout << "Created cube 3 (entity: " << cube3.id << ")\n";
    std::cout << "Total entities: " << g_world.getEntityCount() << "\n";
    
    // Debug: Verify components were added
    std::cout << "\n=== Verifying Entity Components ===\n";
    for (auto& cube : {cube1, cube2, cube3}) {
        if (cube.isValid()) {
            auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(cube);
            auto* m = g_world.getComponentArchetype<ecs::MeshComponent>(cube);
            std::cout << "Entity " << cube.id << ": ";
            if (t) {
                std::cout << "Transform@(" << t->position.x << "," << t->position.y << "," << t->position.z << ") ";
            } else {
                std::cout << "NO_TRANSFORM ";
            }
            if (m) {
                std::cout << "Mesh[visible=" << m->visible << ", meshID=" << m->meshID << "]";
            } else {
                std::cout << "NO_MESH";
            }
            std::cout << "\n";
        } else {
            std::cout << "Entity INVALID\n";
        }
    }
    std::cout << "====================================\n\n";

    std::cout << "Ready!\n";
    std::cout << "Controls: Right-click+drag to look, WASD to move\n";

    // Main loop
    float lastTime = 0;
    float fpsTimer = 0;
    int frames = 0;

    while (!glfwWindowShouldClose(window)) {
        // Start GPU frame
        BEGIN_GPU_FRAME();
        
        // Calculate delta time
        float now = glfwGetTime();
        float dt = now - lastTime;
        lastTime = now;

        // FPS counter
        frames++;
        fpsTimer += dt;
        if (fpsTimer >= 1.0f) {
            g_fps = static_cast<float>(frames);
            frames = 0;
            fpsTimer = 0;
        }

        // Update ECS
        g_world.update(dt);

        // Get window dimensions and mouse position early (needed for viewport bounds)
        int windowW, windowH;
        glfwGetWindowSize(window, &windowW, &windowH);
        double mx, my;
        glfwGetCursorPos(window, &mx, &my);

        // Handle keyboard shortcuts (non-movement, no viewport dependency)
        if (!io.WantCaptureKeyboard) {
            // Gizmo tools
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS && g_gizmoType != GizmoType::Translate) {
                static float lastW = 0;
                if (glfwGetTime() - lastW > 0.2f) {
                    g_gizmoType = GizmoType::Translate;
                    lastW = glfwGetTime();
                }
            }
            if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS && g_gizmoType != GizmoType::Rotate) {
                static float lastE = 0;
                if (glfwGetTime() - lastE > 0.2f) {
                    g_gizmoType = GizmoType::Rotate;
                    lastE = glfwGetTime();
                }
            }
            if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS && g_gizmoType != GizmoType::Scale) {
                static float lastR = 0;
                if (glfwGetTime() - lastR > 0.2f) {
                    g_gizmoType = GizmoType::Scale;
                    lastR = glfwGetTime();
                }
            }
            if (glfwGetKey(window, GLFW_KEY_X) == GLFW_PRESS) {
                static float lastX = 0;
                if (glfwGetTime() - lastX > 0.2f) {
                    g_spaceType = (g_spaceType == SpaceType::World) ? SpaceType::Local : SpaceType::World;
                    lastX = glfwGetTime();
                }
            }
            
            // Entity operations (with Ctrl modifier)
            bool ctrlPressed = (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                               glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS);
            
            if (ctrlPressed) {
                if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
                    duplicateSelectedEntity();
                    // Simple debounce
                    static float lastDupTime = 0;
                    if (glfwGetTime() - lastDupTime > 0.2f) {
                        lastDupTime = glfwGetTime();
                    }
                }
            }
            
            // Delete key
            if (glfwGetKey(window, GLFW_KEY_DELETE) == GLFW_PRESS) {
                static float lastDelTime = 0;
                if (glfwGetTime() - lastDelTime > 0.2f) {
                    deleteSelectedEntity();
                    lastDelTime = glfwGetTime();
                }
            }
            
            // Console toggle
            if (glfwGetKey(window, GLFW_KEY_BACKSLASH) == GLFW_PRESS) {
                static float lastToggle = 0;
                if (glfwGetTime() - lastToggle > 0.2f) {
                    g_showConsole = !g_showConsole;
                    lastToggle = glfwGetTime();
                }
            }
        }

        // Render 3D scene to FBO
        {
            PROFILE_GPU_SCOPE("Render Scene");
            renderScene();
        }

        // End GPU frame
        END_GPU_FRAME();

        // Setup ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Window dimensions already retrieved earlier for input handling
        // Panel dimensions
        const float menuBarHeight = 25.0f;
        const float toolbarHeight = 36.0f;
        const float sidePanelWidth = 280.0f;
        const float bottomPanelHeight = 250.0f;
        const float consoleHeight = 180.0f;
        const float statusBarHeight = 24.0f;
        
        // Use smaller of console or bottom panel height for tabbed panel
        const float tabbedBottomHeight = consoleHeight;

        // ========== MENU BAR ==========
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New Scene", "Ctrl+N")) {
                    // Clear scene and create new one
                    g_world.shutdown();
                    g_world.init();
                    g_selected = ecs::INVALID_ENTITY_ID;
                    std::cout << "New scene created\n";
                }
                if (ImGui::MenuItem("Open Scene", "Ctrl+O")) {
                    // For now, load a default scene file
                    loadScene("scene.json");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Save", "Ctrl+S")) {
                    saveScene(g_currentSceneFile.empty() ? "scene.json" : g_currentSceneFile);
                }
                if (ImGui::MenuItem("Save As", "Ctrl+Shift+S")) {
                    saveScene("scene.json");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4")) {
                    glfwSetWindowShouldClose(window, true);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z")) {
                    // TODO: Implement undo system
                    std::cout << "Undo not yet implemented\n";
                }
                if (ImGui::MenuItem("Redo", "Ctrl+Y")) {
                    // TODO: Implement redo system
                    std::cout << "Redo not yet implemented\n";
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Preferences")) {
                    // TODO: Implement preferences
                    std::cout << "Preferences not yet implemented\n";
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("GameObject")) {
                if (ImGui::MenuItem("Cube")) {
                    createCube(glm::vec3(0, 1, 0), glm::vec3(1), glm::vec3(0.8f, 0.8f, 0.8f));
                }
                if (ImGui::MenuItem("Sphere")) {
                    createSphere(glm::vec3(0, 1, 0), 0.5f, glm::vec3(0.2f, 0.8f, 0.2f));
                }
                if (ImGui::MenuItem("Plane")) {
                    createPlane(glm::vec3(0, 0, 0), glm::vec2(10, 10), glm::vec3(0.5f, 0.5f, 0.5f));
                }
                if (ImGui::MenuItem("Light")) {
                    createLight(glm::vec3(5, 10, 5), glm::vec3(1, 1, 0.9f), 1.0f);
                }
                if (ImGui::MenuItem("Camera")) {
                    createCamera(glm::vec3(0, 5, 10), glm::vec3(0, 0, 0));
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Window")) {
                ImGui::MenuItem("World Outliner", nullptr, true);
                ImGui::MenuItem("Details", nullptr, true);
                ImGui::MenuItem("Toolbox", nullptr, true);
                ImGui::MenuItem("Viewport", nullptr, true);
                ImGui::Separator();
                if (ImGui::MenuItem("Profiler")) {
                    g_uiState.bottomPanelTab = 2;  // Switch to Profiler tab
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help")) {
                if (ImGui::MenuItem("Documentation")) {
                    std::cout << "Opening documentation...\n";
                    // TODO: Open documentation URL
                }
                if (ImGui::MenuItem("About RTT Engine")) {
                    g_showAbout = true;
                }
                ImGui::EndMenu();
            }

            // Performance stats on right side
            ImGui::Separator();
            ImGui::SameLine();
            
            char perfText[64];
            snprintf(perfText, sizeof(perfText), "FPS: %.0f  |  Entities: %d", g_fps, g_world.getEntityCount());
            float perfWidth = ImGui::CalcTextSize(perfText).x + 30;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - perfWidth - 150);
            ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.5f, 1), "%s", perfText);
            
            // GPU time if available
            auto& profiler = AdvancedGPUProfiler::getInstance();
            float gpuTime = profiler.getFrameTimeMs();
            if (gpuTime > 0) {
                ImGui::SameLine();
                char gpuText[64];
                snprintf(gpuText, sizeof(gpuText), "GPU: %.2f ms", gpuTime);
                ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1), "%s", gpuText);
            }

            ImGui::EndMainMenuBar();
        }

        // ========== TOOLBAR ==========
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.08f, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::BeginChild("Toolbar", ImVec2(static_cast<float>(windowW), toolbarHeight),
                         false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollbar);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 3));

        // Entity creation buttons
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.3f, 0.4f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.4f, 0.5f, 1));
        
        if (ImGui::Button(ICON_FA_CUBE " Cube", ImVec2(75, 26))) {
            createCube(glm::vec3(0, 1, 0), glm::vec3(1), glm::vec3(1.0f, 0.2f, 0.2f));
            logMessage("Created Cube");
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CUBE " Sphere", ImVec2(75, 26))) {
            createSphere(glm::vec3(0, 1, 0), 0.5f, glm::vec3(0.2f, 1.0f, 0.2f));
            logMessage("Created Sphere");
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CUBE " Plane", ImVec2(75, 26))) {
            createPlane(glm::vec3(0, 0, 0), glm::vec2(10, 10), glm::vec3(0.5f, 0.5f, 0.5f));
            logMessage("Created Plane");
        }
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();
        
        ImGui::PopStyleColor(2);

        // Transform tools
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Tools:");
        ImGui::SameLine();

        const char* transformTools[] = {"Translate", "Rotate", "Scale"};
        GizmoType transformTypes[] = {GizmoType::Translate, GizmoType::Rotate, GizmoType::Scale};
        const char* transformTooltips[] = {"Translate Tool (W)", "Rotate Tool (E)", "Scale Tool (R)"};
        
        for (int i = 0; i < 3; i++) {
            bool isActive = (g_gizmoType == transformTypes[i]);
            ImGui::PushStyleColor(ImGuiCol_Button, isActive ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
            if (ImGui::Button(transformTools[i], ImVec2(75, 28))) {
                g_gizmoType = transformTypes[i];
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", transformTooltips[i]);
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }

        // Separator
        ImGui::Dummy(ImVec2(15, 1));
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(15, 1));
        ImGui::SameLine();

        // Space toggle
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Space:");
        ImGui::SameLine();

        const char* spaceText = (g_spaceType == SpaceType::World) ? "World" : "Local";
        if (ImGui::Button(spaceText, ImVec2(70, 28))) {
            g_spaceType = (g_spaceType == SpaceType::World) ? SpaceType::Local : SpaceType::World;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Transform Space (X)");
        ImGui::SameLine();

        // Separator
        ImGui::Dummy(ImVec2(15, 1));
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(15, 1));
        ImGui::SameLine();

        // View options
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "View:");
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, g_showGrid ? ImVec4(0.3f, 0.3f, 0.2f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Grid", ImVec2(50, 28))) g_showGrid = !g_showGrid;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Grid Display");
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, g_showGizmo ? ImVec4(0.3f, 0.3f, 0.2f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Gizmo", ImVec2(55, 28))) g_showGizmo = !g_showGizmo;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Gizmo Display");
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, g_showWireframe ? ImVec4(0.3f, 0.2f, 0.3f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Wire", ImVec2(50, 28))) g_showWireframe = !g_showWireframe;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Wireframe Mode (Debug)");
        ImGui::PopStyleColor();
        ImGui::SameLine();

        // GPS mode cycle button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.25f, 0.15f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.35f, 0.25f, 1));
        if (ImGui::Button("GPS", ImVec2(50, 28))) {
            // Cycle through GPS modes
            auto currentMode = g_geospatialSystem.getGPSTracker().getMode();
            int modeInt = static_cast<int>(currentMode);
            modeInt = (modeInt + 1) % 5;  // 5 modes
            g_geospatialSystem.setGPSMode(static_cast<GPSTracker::Mode>(modeInt));
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Cycle GPS Mode: %s",
                g_geospatialSystem.getGPSTracker().getCurrentModeName());
        }
        ImGui::SameLine();

        // Separator
        ImGui::Dummy(ImVec2(15, 1));
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(15, 1));
        ImGui::SameLine();

        // Play controls
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Play:");
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, g_isPlaying ? ImVec4(0.1f, 0.3f, 0.1f, 1) : ImVec4(0.2f, 0.5f, 0.2f, 1));
        if (ImGui::Button(g_isPlaying ? "PLAYING" : "PLAY", ImVec2(70, 28))) {
            if (!g_isPlaying) {
                // Enter play mode
                g_isPlaying = true;
                g_wasPlaying = false;
                std::cout << "=== PLAY MODE ===\n";
                // TODO: Save current state before entering play mode
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enter Play Mode");
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, g_isPlaying ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.3f, 0.3f, 0.3f, 1));
        ImGui::BeginDisabled(!g_isPlaying);
        if (ImGui::Button("PAUSE", ImVec2(70, 28))) {
            g_wasPlaying = g_isPlaying;
            g_isPlaying = !g_wasPlaying;
            std::cout << (g_wasPlaying ? "=== PAUSED ===\n" : "=== RESUMED ===\n");
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pause/Resume Game");
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, g_isPlaying ? ImVec4(0.5f, 0.1f, 0.1f, 1) : ImVec4(0.3f, 0.3f, 0.3f, 1));
        ImGui::BeginDisabled(!g_isPlaying && !g_wasPlaying);
        if (ImGui::Button("STOP", ImVec2(60, 28))) {
            if (g_isPlaying || g_wasPlaying) {
                g_isPlaying = false;
                g_wasPlaying = false;
                std::cout << "=== STOP (Exited Play Mode) ===\n";
                // TODO: Restore state from before play mode
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Exit Play Mode");
        ImGui::PopStyleColor();

        ImGui::PopStyleVar(2);
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // ========== LEFT PANEL (Outliner / Details - Tabbed) ==========
        ImGui::SetNextWindowPos(ImVec2(0, menuBarHeight + toolbarHeight));
        ImGui::SetNextWindowSize(ImVec2(sidePanelWidth,
            static_cast<float>(windowH) - menuBarHeight - toolbarHeight - tabbedBottomHeight));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.25f, 0.25f, 0.25f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1));
        ImGui::Begin("Scene", nullptr,
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Tab buttons
        ImGui::PushStyleColor(ImGuiCol_Button, g_uiState.leftPanelTab == 0 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Outliner", ImVec2(sidePanelWidth/3 - 5, 28))) g_uiState.leftPanelTab = 0;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, g_uiState.leftPanelTab == 1 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Details", ImVec2(sidePanelWidth/3 - 5, 28))) g_uiState.leftPanelTab = 1;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, g_uiState.leftPanelTab == 2 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Geo", ImVec2(sidePanelWidth/3 - 5, 28))) g_uiState.leftPanelTab = 2;
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

        if (g_uiState.leftPanelTab == 0) {
            // ========== WORLD OUTLINER ==========
            // Search box
            ImGui::PushItemWidth(-1);
            ImGui::InputTextWithHint("##Search", "Search entities...", g_uiState.searchBuffer,
                                    sizeof(g_uiState.searchBuffer));
            ImGui::PopItemWidth();
            ImGui::Separator();
            ImGui::Spacing();
            
            int entityCount = 0;
        g_world.forEach<ecs::TransformComponent, ecs::MeshComponent>(
            [&](ecs::EntityID id, ecs::TransformComponent&, ecs::MeshComponent& m) {
                bool isSelected = (g_selected == id);
                ImGui::PushStyleColor(ImGuiCol_Text,
                    isSelected ? ImVec4(1.0f, 0.8f, 0.2f, 1) : ImVec4(0.85f, 0.85f, 0.85f, 1));

                char label[64];
                snprintf(label, sizeof(label), "Entity %d", id);

                // Right-click context menu
                if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    g_selected = id;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        // Double-click: focus on entity (TODO: move camera)
                        logMessage("Focus on: " + std::string(label));
                    }
                }
                
                // Context menu on right-click
                if (ImGui::BeginPopupContextItem()) {
                    g_selected = id;
                    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                        duplicateSelectedEntity();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete", "Delete")) {
                        deleteSelectedEntity();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Rename")) {
                        // TODO: Open rename dialog
                    }
                    ImGui::EndPopup();
                }
                
                ImGui::PopStyleColor();
                entityCount++;
            });

        // Global context menu (right-click in empty space)
        if (ImGui::BeginPopupContextWindow("OutlinerContext")) {
            if (ImGui::MenuItem("Create Cube")) {
                createCube(glm::vec3(0, 1, 0), glm::vec3(1), glm::vec3(0.8f, 0.8f, 0.8f));
            }
            if (ImGui::MenuItem("Create Sphere")) {
                createSphere(glm::vec3(0, 1, 0), 0.5f, glm::vec3(0.2f, 0.8f, 0.2f));
            }
            if (ImGui::MenuItem("Create Plane")) {
                createPlane(glm::vec3(0, 0, 0), glm::vec2(10, 10), glm::vec3(0.5f, 0.5f, 0.5f));
            }
            if (ImGui::MenuItem("Create Light")) {
                createLight(glm::vec3(5, 10, 5), glm::vec3(1, 1, 0.9f), 1.0f);
            }
            if (ImGui::MenuItem("Create Camera")) {
                createCamera(glm::vec3(0, 5, 10), glm::vec3(0, 0, 0));
            }
            ImGui::EndPopup();
        }

        if (entityCount == 0) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No entities in scene");
            ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1), "Right-click to create one");
        }
        } else if (g_uiState.leftPanelTab == 1) {
            // ========== DETAILS ==========
            if (g_selected != ecs::INVALID_ENTITY_ID) {
                // Get components safely
                ecs::Entity selectedEntity{g_selected};

                // Check if entity still exists before accessing
                bool entityStillExists = false;
                ecs::TransformComponent* t = nullptr;
                ecs::MeshComponent* m = nullptr;

                // Try to get components (they may not exist or entity may be destroyed)
                if (selectedEntity.isValid()) {
                    t = g_world.getComponentArchetype<ecs::TransformComponent>(selectedEntity);
                    m = g_world.getComponentArchetype<ecs::MeshComponent>(selectedEntity);
                    entityStillExists = (t || m);
                }

                ImGui::Separator();
                ImGui::Spacing();

                // Transform component
                if (t && entityStillExists) renderTransformSection(t);

                // Mesh component
                if (m && entityStillExists) renderMeshSection(m);

            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No entity selected");
                ImGui::Dummy(ImVec2(0, 20));
                ImGui::TextWrapped("Select an entity from the Outliner to view and edit its properties.");
            }
        } else if (g_uiState.leftPanelTab == 2) {
            // ========== GEOSPATIAL PANEL ==========
            ImGui::SeparatorText("GPS Tracker");

            const GPSFix& fix = g_geospatialSystem.getCurrentGPSFix();

            // GPS mode display
            ImGui::Text("Mode: %s", g_geospatialSystem.getGPSTracker().getCurrentModeName());
            ImGui::Text("Satellites: %d", fix.satelliteCount);
            ImGui::Text("Accuracy: H=%.1fm V=%.1fm", fix.horizontalAccuracy, fix.verticalAccuracy);
            ImGui::Separator();

            // Position display
            ImGui::SeparatorText("Position (WGS84)");
            ImGui::Text("Lat: %.8f°", fix.latitude);
            ImGui::Text("Lon: %.8f°", fix.longitude);
            ImGui::Text("Alt: %.2f m", fix.altitude);
            ImGui::Separator();

            // DMS format
            ImGui::SeparatorText("DMS Format");
            char latDir = fix.latitude >= 0 ? 'N' : 'S';
            char lonDir = fix.longitude >= 0 ? 'E' : 'W';
            ImGui::Text("%c %.8f°", latDir, std::abs(fix.latitude));
            ImGui::Text("%c %.8f°", lonDir, std::abs(fix.longitude));
            ImGui::Separator();

            // Movement info
            ImGui::SeparatorText("Movement");
            ImGui::Text("Speed: %.2f m/s (%.1f km/h)", fix.speed, fix.speed * 3.6f);
            ImGui::Text("Heading: %.1f°", fix.heading);
            ImGui::Text("Sim Time: %.1fs", g_geospatialSystem.getGPSTracker().getSimTime());
            ImGui::Separator();

            // Origin info
            ImGui::SeparatorText("Local Origin");
            glm::dvec3 origin = g_geospatialSystem.getConverter().getOriginWGS84();
            ImGui::Text("Lat: %.4f°", origin.x);
            ImGui::Text("Lon: %.4f°", origin.y);
            ImGui::Text("Alt: %.1f m", origin.z);

            // Validation
            if (fix.isValid) {
                double dist = GeospatialConverter::haversineDistance(
                    origin.x, origin.y, fix.latitude, fix.longitude);
                double bearing = GeospatialConverter::bearing(
                    origin.x, origin.y, fix.latitude, fix.longitude);
                ImGui::Separator();
                ImGui::SeparatorText("From Origin");
                ImGui::Text("Distance: %.1f m", dist);
                ImGui::Text("Bearing: %.1f°", bearing);
            }

        }

        ImGui::PopStyleColor(2);
        ImGui::End();

        // ========== BOTTOM PANEL (Content / Console / Debug - Tabbed) ==========
        ImGui::SetNextWindowPos(ImVec2(0, static_cast<float>(windowH) - tabbedBottomHeight));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(windowW), tabbedBottomHeight));
        ImGui::Begin("Toolbox", nullptr,
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1));

        // Tab buttons
        ImGui::PushStyleColor(ImGuiCol_Button, g_uiState.bottomPanelTab == 0 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Content", ImVec2(90, 28))) g_uiState.bottomPanelTab = 0;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, g_uiState.bottomPanelTab == 1 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Console", ImVec2(90, 28))) g_uiState.bottomPanelTab = 1;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, g_uiState.bottomPanelTab == 2 ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
        if (ImGui::Button("Profiler", ImVec2(90, 28))) g_uiState.bottomPanelTab = 2;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::Spacing();

        if (g_uiState.bottomPanelTab == 0) {
            // ========== CONTENT BROWSER ==========

        // Path bar
        ImGui::PushItemWidth(300);
        ImGui::InputText("##Path", g_uiState.pathBuffer, sizeof(g_uiState.pathBuffer));
        ImGui::PopItemWidth();
        ImGui::SameLine();

        if (ImGui::Button("Refresh", ImVec2(75, 0))) {
            std::cout << "Refreshing asset browser: " << g_uiState.pathBuffer << "\n";
            // Asset refresh would scan the directory
        }
        ImGui::SameLine();
        if (ImGui::Button("Import", ImVec2(75, 0))) {
            std::cout << "Import asset dialog (not implemented)\n";
            // TODO: Open file dialog for asset import
        }

        ImGui::Separator();

        // Asset grid
        ImGui::BeginChild("Assets", ImVec2(0, 0), true);

        // Calculate grid layout
        float iconSize = 70.0f;
        float availWidth = ImGui::GetContentRegionAvail().x;
        int itemsPerRow = std::max(1, static_cast<int>(availWidth / (iconSize + 15)));

        // Meshes section
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1), "MESHES");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));

        const char* meshes[] = {"Cube", "Sphere", "Plane", "Cylinder", "Cone", "Torus"};
        ecs::MeshType meshTypes[] = {
            ecs::MeshType::Cube, ecs::MeshType::Sphere, ecs::MeshType::Plane,
            ecs::MeshType::Cylinder, ecs::MeshType::Cone, ecs::MeshType::Torus
        };
        glm::vec3 meshColors[] = {
            {0.8f, 0.8f, 0.8f}, {0.2f, 0.8f, 0.2f}, {0.5f, 0.5f, 0.5f},
            {0.8f, 0.6f, 0.2f}, {0.8f, 0.2f, 0.2f}, {0.2f, 0.6f, 0.8f}
        };

        for (int i = 0; i < 6; i++) {
            ImGui::PushID(i);

            // Asset box - clickable
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.35f, 0.45f, 1));
            if (ImGui::Button("##Asset", ImVec2(iconSize, iconSize))) {
                if (g_selected != ecs::INVALID_ENTITY_ID) {
                    // Change selected entity's mesh type
                    ecs::Entity selEntity{g_selected};
                    auto* meshComp = g_world.getComponentArchetype<ecs::MeshComponent>(selEntity);
                    if (meshComp) {
                        meshComp->meshType = meshTypes[i];
                        meshComp->color = meshColors[i];
                        logMessage(std::string("Changed selected entity to ") + meshes[i]);
                    } else {
                        logMessage("Selected entity has no MeshComponent", 1);
                    }
                } else {
                    // Create new entity
                    glm::vec3 spawnPos(0, 1, 0);
                    createCube(spawnPos, glm::vec3(1), meshColors[i]);
                    // Set mesh type on the newly created entity
                    // (createCube creates a cube, so for non-cube types we need to update)
                    if (i > 0) {
                        // Find the last created entity and update its mesh type
                        g_world.forEach<ecs::TransformComponent, ecs::MeshComponent>(
                            [&](ecs::EntityID id, ecs::TransformComponent&, ecs::MeshComponent& m) {
                                // This is a bit hacky - in production, track the last created entity
                            });
                    }
                    logMessage(std::string("Created ") + meshes[i]);
                }
            }
            ImGui::PopStyleColor(2);

            if (ImGui::IsItemHovered()) {
                if (g_selected != ecs::INVALID_ENTITY_ID) {
                    ImGui::SetTooltip("Click to change selected entity to %s", meshes[i]);
                } else {
                    ImGui::SetTooltip("Click to add %s to scene", meshes[i]);
                }
            }

            ImGui::SameLine();
            ImGui::TextWrapped("%s", meshes[i]);
            ImGui::SameLine();

            if ((i + 1) % itemsPerRow != 0 && i < 5) {
                ImGui::SameLine();
            }
            ImGui::PopID();
        }

        ImGui::Dummy(ImVec2(0, 15));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 10));

        // Materials section
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.4f, 1), "MATERIALS");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));

        const char* materials[] = {"M_Default", "M_Metal", "M_Wood", "M_Stone", "M_Glass"};

        // Material presets: albedo, metallic, roughness
        struct MaterialPreset {
            glm::vec3 albedo;
            float metallic;
            float roughness;
        };
        MaterialPreset matPresets[] = {
            {{0.8f, 0.8f, 0.8f}, 0.0f, 0.5f},  // Default
            {{0.9f, 0.9f, 0.95f}, 0.9f, 0.2f},  // Metal
            {{0.4f, 0.25f, 0.1f}, 0.0f, 0.9f},  // Wood
            {{0.5f, 0.5f, 0.5f}, 0.0f, 0.95f},  // Stone
            {{0.9f, 0.95f, 1.0f}, 0.1f, 0.05f}, // Glass
        };

        for (int i = 0; i < 5; i++) {
            ImGui::PushID(i + 100);

            // Color swatch as button background
            ImVec4 btnColor = ImVec4(matPresets[i].albedo.r, matPresets[i].albedo.g, matPresets[i].albedo.b, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(
                matPresets[i].albedo.r * 1.2f, matPresets[i].albedo.g * 1.2f, matPresets[i].albedo.b * 1.2f, 1.0f));

            if (ImGui::Button("##Mat", ImVec2(iconSize, iconSize))) {
                // Apply material to selected entity
                if (g_selected != ecs::INVALID_ENTITY_ID) {
                    ecs::Entity selEntity{g_selected};
                    auto* meshComp = g_world.getComponentArchetype<ecs::MeshComponent>(selEntity);
                    if (meshComp) {
                        meshComp->color = matPresets[i].albedo;
                        meshComp->metallic = matPresets[i].metallic;
                        meshComp->roughness = matPresets[i].roughness;
                        logMessage(std::string("Applied ") + materials[i] + " to selected entity");
                    } else {
                        logMessage("Selected entity has no MeshComponent", 1);
                    }
                } else {
                    logMessage("No entity selected - select one first", 1);
                }
            }
            ImGui::PopStyleColor(2);

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Apply %s to selected entity\nMetallic: %.1f, Roughness: %.2f",
                    materials[i], matPresets[i].metallic, matPresets[i].roughness);
            }

            ImGui::SameLine();
            ImGui::TextWrapped("%s", materials[i]);
            ImGui::SameLine();

            if ((i + 1) % itemsPerRow != 0 && i < 4) {
                ImGui::SameLine();
            }
            ImGui::PopID();
        }

        ImGui::EndChild();

        } else if (g_uiState.bottomPanelTab == 1) {
            // ========== CONSOLE ==========
            // Console toolbar
            if (ImGui::Button("Clear", ImVec2(60, 0))) {
                g_consoleMessages.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("Collapse All", ImVec2(80, 0))) {
                // TODO: Collapse log groups
            }
            ImGui::SameLine();
            ImGui::Button("##Sep1", ImVec2(1, 20));
            ImGui::SameLine();

            // Filter buttons
            const char* filterNames[] = {"All", "Info", "Warning", "Error"};
            for (int i = -1; i < 3; i++) {
                ImGui::PushStyleColor(ImGuiCol_Button, g_consoleFilter == i ? ImVec4(0.4f, 0.3f, 0.0f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
                if (ImGui::Button(filterNames[i + 1], ImVec2(60, 0))) {
                    g_consoleFilter = i;
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }

            ImGui::Separator();

            // Message list
            ImGui::BeginChild("ConsoleMessages", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

            int msgCount = 0;
            for (const auto& msg : g_consoleMessages) {
                if (g_consoleFilter != -1 && msg.level != g_consoleFilter) continue;

                ImVec4 color;
                const char* prefix;
                if (msg.level == 2) { color = ImVec4(1.0f, 0.3f, 0.3f, 1); prefix = "[ERROR]"; }
                else if (msg.level == 1) { color = ImVec4(1.0f, 0.8f, 0.0f, 1); prefix = "[WARN]"; }
                else { color = ImVec4(0.7f, 0.7f, 0.7f, 1); prefix = "[INFO]"; }

                ImGui::TextColored(color, "%s %s", prefix, msg.text.c_str());
                msgCount++;
            }

            if (msgCount == 0) {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No messages");
            }

            // Auto-scroll to bottom
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }

            ImGui::EndChild();

        } else if (g_uiState.bottomPanelTab == 2) {
            // ========== PROFILER PANEL ==========
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1), "PROFILER");
            ImGui::Separator();
            ImGui::Spacing();

            // Engine stats
            ImGui::Text("FPS: %.1f", g_fps);
            ImGui::Text("Frame Time: %.2f ms", 1000.0f / (g_fps > 0 ? g_fps : 60));
            ImGui::Text("Entity Count: %d", g_world.getEntityCount());
            ImGui::Separator();
            ImGui::Spacing();

            // Camera info
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1), "CAMERA");
            ImGui::Text("Position: (%.2f, %.2f, %.2f)",
                g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
            ImGui::Text("Target: (%.2f, %.2f, %.2f)",
                g_camera->Target.x, g_camera->Target.y, g_camera->Target.z);
            ImGui::Separator();
            ImGui::Spacing();

            // Selected entity info
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1), "SELECTION");
            if (g_selected != ecs::INVALID_ENTITY_ID) {
                ImGui::Text("Selected Entity ID: %d", g_selected);
                auto* t = g_world.getComponentArchetype<ecs::TransformComponent>(ecs::Entity{g_selected});
                if (t) {
                    ImGui::Text("Position: (%.2f, %.2f, %.2f)", t->position.x, t->position.y, t->position.z);
                }
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "No entity selected");
            }
            ImGui::Separator();
            ImGui::Spacing();

            // Memory info
            ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1), "MEMORY");
            auto* drawData = ImGui::GetDrawData();
            if (drawData) {
                ImGui::Text("ImGui Draw Lists: %d", drawData->CmdListsCount);
            } else {
                ImGui::Text("ImGui Draw Lists: N/A");
            }
            // TODO: Add more memory stats when memory manager is integrated

        }

        ImGui::PopStyleColor();
        ImGui::End();

        // ========== VIEWPORT (Center - Maximizes remaining space) ==========
        float vpX = sidePanelWidth;
        float vpY = menuBarHeight + toolbarHeight;
        float vpW = static_cast<float>(windowW) - sidePanelWidth;
        float vpH = static_cast<float>(windowH) - menuBarHeight - toolbarHeight - tabbedBottomHeight;

        ImGui::SetNextWindowPos(ImVec2(vpX, vpY));
        ImGui::SetNextWindowSize(ImVec2(vpW, vpH));
        ImGui::Begin("Viewport", nullptr,
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImVec2 contentSize = ImGui::GetContentRegionAvail();
        if (contentSize.x > 0 && contentSize.y > 0) {
            int w = static_cast<int>(contentSize.x);
            int h = static_cast<int>(contentSize.y);

            // Resize FBO if needed
            if (w != g_viewportFB.width || h != g_viewportFB.height) {
                g_viewportFB.resize(w, h);
            }

            // Display FBO texture
            ImGui::Image((void*)(intptr_t)g_viewportFB.colorTex,
                        ImVec2(static_cast<float>(w), static_cast<float>(h)),
                        ImVec2(0, 1), ImVec2(1, 0));

            // Viewport bounds in screen space
            ImVec2 viewportMin = ImGui::GetCursorScreenPos();
            ImVec2 viewportMax = ImVec2(viewportMin.x + w, viewportMin.y + h);
            ImVec2 mousePos = ImGui::GetMousePos();

            bool mouseInViewport = (mousePos.x >= viewportMin.x && mousePos.x < viewportMax.x &&
                                   mousePos.y >= viewportMin.y && mousePos.y < viewportMax.y);

            // ====================================================================
            // CAMERA INPUT HANDLING (after ImGui::NewFrame, proper ordering)
            // ====================================================================

            // Track right-click hold for orbit camera
            bool rightClickInViewport = mouseInViewport && ImGui::IsMouseDown(ImGuiMouseButton_Right);
            bool rightClickReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Right);

            if (rightClickInViewport && !io.WantCaptureMouse) {
                g_isViewing = true;
            }
            if (rightClickReleased) {
                g_isViewing = false;
            }

            // Orbit camera with right-click drag
            if (g_isViewing && !io.WantCaptureMouse) {
                float dx = static_cast<float>(mx - g_lastMousePos.x);
                float dy = static_cast<float>(my - g_lastMousePos.y);
                g_camera->ProcessMouseMovement(dx * 0.3f, dy * 0.3f);
            }

            // WASD camera movement (only when right-clicking in viewport - Unreal style)
            if (g_isViewing && !io.WantCaptureKeyboard && !io.WantCaptureMouse) {
                float moveSpeed = 5.0f * dt;
                glm::vec3 forward = glm::normalize(g_camera->Target - g_camera->Position);
                glm::vec3 right = glm::normalize(glm::cross(forward, g_camera->WorldUp));

                if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
                    g_camera->Position += forward * moveSpeed;
                    g_camera->Target += forward * moveSpeed;
                }
                if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
                    g_camera->Position -= forward * moveSpeed;
                    g_camera->Target -= forward * moveSpeed;
                }
                if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
                    g_camera->Position -= right * moveSpeed;
                    g_camera->Target -= right * moveSpeed;
                }
                if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
                    g_camera->Position += right * moveSpeed;
                    g_camera->Target += right * moveSpeed;
                }
                if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
                    g_camera->Position.y -= moveSpeed;
                    g_camera->Target.y -= moveSpeed;
                }
                if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) {
                    g_camera->Position.y += moveSpeed;
                    g_camera->Target.y += moveSpeed;
                }
            }

            // Scroll zoom when mouse is in viewport
            if (mouseInViewport && !io.WantCaptureMouse) {
                float scrollY = io.MouseWheel;
                if (scrollY != 0.0f) {
                    g_camera->ProcessMouseScroll(scrollY * 2.0f);
                }
            }

            // Update last mouse position
            g_lastMousePos = glm::vec2(static_cast<float>(mx), static_cast<float>(my));

            // Hide cursor when orbiting
            if (g_isViewing) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);
            }

            // Viewport context menu (right-click release in empty space)
            if (mouseInViewport && rightClickReleased && !g_isViewing) {
                ImGui::OpenPopup("ViewportContext");
            }

            if (ImGui::BeginPopup("ViewportContext")) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1), "Create Entity");
                ImGui::Separator();

                if (ImGui::MenuItem(ICON_FA_CUBE " Create Cube")) {
                    createCube(glm::vec3(0, 1, 0), glm::vec3(1), glm::vec3(1.0f, 0.2f, 0.2f));
                    logMessage("Created Cube from viewport menu");
                }
                if (ImGui::MenuItem(ICON_FA_CUBE " Create Sphere")) {
                    createSphere(glm::vec3(0, 1, 0), 0.5f, glm::vec3(0.2f, 1.0f, 0.2f));
                    logMessage("Created Sphere from viewport menu");
                }
                if (ImGui::MenuItem(ICON_FA_CUBE " Create Plane")) {
                    createPlane(glm::vec3(0, 0, 0), glm::vec2(10, 10), glm::vec3(0.5f, 0.5f, 0.5f));
                    logMessage("Created Plane from viewport menu");
                }
                if (ImGui::MenuItem(ICON_FA_CUBE " Create Light")) {
                    createLight(glm::vec3(5, 10, 5), glm::vec3(1, 1, 0.9f), 1.0f);
                    logMessage("Created Light from viewport menu");
                }

                ImGui::Separator();
                if (ImGui::MenuItem("Clear Scene")) {
                    g_world.shutdown();
                    g_world.init();
                    g_selected = ecs::INVALID_ENTITY_ID;
                    logMessage("Scene cleared");
                }

                ImGui::EndPopup();
            }

            // Viewport overlay info
            ImGui::SetCursorScreenPos(ImVec2(viewportMin.x + 10, viewportMin.y + 10));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 0.8f));
            ImGui::Text("Camera: (%.1f, %.1f, %.1f)", g_camera->Position.x, g_camera->Position.y, g_camera->Position.z);
            ImGui::Text("Gizmo: %s | Space: %s | %s",
                g_gizmoType == GizmoType::Translate ? "Translate" :
                g_gizmoType == GizmoType::Rotate ? "Rotate" : "Scale",
                g_spaceType == SpaceType::World ? "World" : "Local",
                g_showWireframe ? "WIREFRAME" : "Solid");
            ImGui::PopStyleColor();

            // Focus indicator when orbiting
            if (g_isViewing) {
                ImGui::SetCursorScreenPos(ImVec2(viewportMin.x + w/2 - 40, viewportMin.y + 10));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
                ImGui::Text("[ Right-click held - Orbit Mode ]");
                ImGui::PopStyleColor();
            }
        }

        ImGui::End();

        // Show About dialog if requested
        showAboutDialog();

        // ========== STATUS BAR (Bottom of screen) ==========
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
        ImGui::SetNextWindowPos(ImVec2(0, static_cast<float>(windowH) - statusBarHeight));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(windowW), statusBarHeight));
        ImGui::Begin("StatusBar", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Left side: Scene info
        int totalEntities = 0;
        g_world.forEach<ecs::TransformComponent>([&](ecs::EntityID, ecs::TransformComponent&) {
            totalEntities++;
        });

        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Entities: %d  |  Selected: %s",
            totalEntities,
            g_selected != ecs::INVALID_ENTITY_ID ? "Yes" : "No");
        ImGui::SameLine();

        // Center: Play mode indicator
        if (g_isPlaying) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1));
            ImGui::Text("\xef\x81\x8b  PLAYING");
            ImGui::PopStyleColor();
        } else if (g_wasPlaying) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1));
            ImGui::Text("\xef\x81\x8c  PAUSED");
            ImGui::PopStyleColor();
        }

        // Right side: FPS and memory
        char statusText[128];
        snprintf(statusText, sizeof(statusText), "FPS: %.0f  |  Frame: %.2fms",
            g_fps, 1000.0f / (g_fps > 0 ? g_fps : 60));

        float textWidth = ImGui::CalcTextSize(statusText).x + 20;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - textWidth);
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "%s", statusText);

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        // Final ImGui render
        ImGui::Render();
        
        // Clear and render ImGui draw data
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Swap buffers and poll events
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    std::cout << "Shutting down...\n";

    // Shutdown GPU profiler
    AdvancedGPUProfiler::getInstance().shutdown();
    std::cout << "GPU Profiler shutdown complete\n";

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    g_world.shutdown();
    
    cleanupCube();
    cleanupSphere();
    cleanupPlane();
    cleanupGrid();
    g_viewportFB.cleanup();
    glDeleteProgram(g_shaderProg);
    
    if (g_camera) {
        delete g_camera;
        g_camera = nullptr;
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    std::cout << "Goodbye!\n";
    return 0;
}
