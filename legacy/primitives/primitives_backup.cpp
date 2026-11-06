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
#include "ecs/components/GeospatialComponent.h"
#include "ecs/components/PredictionComponent.h"
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
static GLuint g_gizmoShaderProg = 0;  // Simple unlit shader for gizmos
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

    // Create simple unlit shader for gizmos (position + color only)
    const char* gizmoVS = R"(
#version 430 core
layout(location=0) in vec3 aPos;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uModel;
void main() {
    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
}
)";
    const char* gizmoFS = R"(
#version 430 core
uniform vec3 uColor;
out vec4 FragColor;
void main() {
    FragColor = vec4(uColor, 1.0);
}
)";
    GLuint gvs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(gvs, 1, &gizmoVS, nullptr);
    glCompileShader(gvs);

    GLuint gfs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(gfs, 1, &gizmoFS, nullptr);
    glCompileShader(gfs);

    g_gizmoShaderProg = glCreateProgram();
    glAttachShader(g_gizmoShaderProg, gvs);
    glAttachShader(g_gizmoShaderProg, gfs);
    glLinkProgram(g_gizmoShaderProg);

    glDeleteShader(gvs);
    glDeleteShader(gfs);
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
// Procedural Mesh VAOs
// ============================================================================
static GLuint g_cubeVAO = 0, g_cubeVBO = 0, g_cubeEBO = 0;
static GLuint g_sphereVAO = 0, g_sphereVBO = 0, g_sphereEBO = 0;
static GLuint g_planeVAO = 0, g_planeVBO = 0, g_planeEBO = 0;
static GLuint g_cylinderVAO = 0, g_cylinderVBO = 0, g_cylinderEBO = 0;
static GLuint g_coneVAO = 0, g_coneVBO = 0, g_coneEBO = 0;
static GLuint g_torusVAO = 0, g_torusVBO = 0, g_torusEBO = 0;

static GLsizei g_cubeIndexCount = 36;
static GLsizei g_sphereIndexCount = 0;
static GLsizei g_planeIndexCount = 0;
static GLsizei g_cylinderIndexCount = 0;
static GLsizei g_coneIndexCount = 0;
static GLsizei g_torusIndexCount = 0;

static void initCube() {
