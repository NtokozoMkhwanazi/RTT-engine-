#include "shader_manager.h"
#include <iostream>

namespace ShaderManager {

static GLuint g_mainShaderProg = 0;
static GLuint g_gizmoShaderProg = 0;

void InitShaders() {
    // Main PBR shader. The model transform comes from a UNIFORM (uModel/model),
    // not an instanced attribute: the renderer's batched path feeds transforms
    // through uniforms because the editor mesh VAOs don't define instance
    // attributes. The grid + ECS primitives/models all draw through this shader.
    const char* vs = R"(
#version 430 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aTexCoord;

uniform mat4 model;

layout(std140, binding = 0) uniform CameraBlock {
    mat4 view;
    mat4 projection;
    mat4 viewProjection;
    vec4 viewPos;
    vec4 lightPositions[8];
    vec4 lightColors[8];
    int lightCount;
    int pad0;
    int pad1;
    int pad2;
} camera;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;
out vec3 ViewDir;

void main() {
    mat4 finalModel = model;
    vec4 worldPos = finalModel * vec4(aPos, 1.0);
    FragPos = vec3(worldPos);
    Normal = mat3(transpose(inverse(finalModel))) * aNormal;
    TexCoord = aTexCoord;
    ViewDir = normalize(camera.viewPos.xyz - FragPos);
    gl_Position = camera.projection * camera.view * worldPos;
}
)";
    
    const char* fs = R"(
#version 430 core
in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;
in vec3 ViewDir;

layout(std140, binding = 0) uniform CameraBlock {
    mat4 view;
    mat4 projection;
    mat4 viewProjection;
    vec4 viewPos;
    vec4 lightPositions[8];
    vec4 lightColors[8];
    int lightCount;
    int pad0;
    int pad1;
    int pad2;
} camera;

layout(location = 0) uniform vec3 albedo;
layout(location = 1) uniform float metallic;
layout(location = 2) uniform float roughness;
layout(location = 3) uniform float ao;
layout(location = 4) uniform vec3 emissive;

uniform sampler2D albedoMap;
uniform bool useAlbedoMap = false;

out vec4 FragColor;

vec3 calculatePBR() {
    vec3 baseColor = useAlbedoMap ? texture(albedoMap, TexCoord).rgb : albedo;
    vec3 norm = normalize(Normal);
    if (!gl_FrontFacing) norm = -norm;
    
    vec3 viewDir = normalize(ViewDir);
    
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, baseColor, metallic);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(viewDir, reflectDir), 0.0), 5.0);
    
    vec3 diffuse = baseColor * (1.0 - F) * (1.0 - metallic);
    
    float a = roughness * roughness;
    float a2 = a * a;
    float k = (a + 1.0) * (a + 1.0) / 8.0;
    
    // Sum all lights: each contributes diffuse + specular weighted by its
    // premultiplied color (color * intensity) and a distance falloff so a
    // scene with several editor point lights doesn't just add the brightest.
    vec3 lighting = vec3(0.0);
    for (int i = 0; i < camera.lightCount && i < 8; ++i) {
        vec3 lightPos = camera.lightPositions[i].xyz;
        vec3 lightColor = camera.lightColors[i].rgb;
        vec3 toLight = lightPos - FragPos;
        float dist = max(length(toLight), 0.0001);
        vec3 lightDir = toLight / dist;
        vec3 reflectDir = reflect(-lightDir, norm);
        
        float NdotL = max(dot(norm, lightDir), 0.0);
        float NdotV = max(dot(norm, viewDir), 0.0);
        float NdotR = max(dot(norm, reflectDir), 0.0);
        
        float NdotR2 = NdotR * NdotR;
        float num = a2;
        float denom = (NdotR2 * (a2 - 1.0) + 1.0);
        denom = 3.14159 * denom * denom;
        float D = num / max(denom, 0.001);
        
        float G_L = NdotL / (NdotL * (1.0 - k) + k);
        float G_V = NdotV / (NdotV * (1.0 - k) + k);
        float G = G_L * G_V;
        
        vec3 specular = (D * F * G) / (4.0 * NdotL * NdotV + 0.001);
        
        // Distance attenuation (point-light style). Keeps total light sane.
        float atten = 1.0 / (1.0 + 0.09 * dist + 0.032 * dist * dist);
        lighting += (diffuse + specular) * NdotL * lightColor * atten;
    }
    
    vec3 ambient = vec3(0.15) * baseColor * ao;
    vec3 result = ambient + lighting;
    
    result = result / (result + vec3(1.0));
    result = pow(result, vec3(1.0/2.2));
    
    return result;
}

void main() {
    vec3 color = calculatePBR();
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
    
    g_mainShaderProg = glCreateProgram();
    glAttachShader(g_mainShaderProg, vsObj);
    glAttachShader(g_mainShaderProg, fsObj);
    glLinkProgram(g_mainShaderProg);
    
    glDeleteShader(vsObj);
    glDeleteShader(fsObj);
    
    // Gizmo shader (unlit)
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

void CleanupShaders() {
    if (g_mainShaderProg) glDeleteProgram(g_mainShaderProg);
    if (g_gizmoShaderProg) glDeleteProgram(g_gizmoShaderProg);
    g_mainShaderProg = 0;
    g_gizmoShaderProg = 0;
}

GLuint GetMainShaderProgram() { return g_mainShaderProg; }
GLuint GetGizmoShaderProgram() { return g_gizmoShaderProg; }

void UseMainShader() { glUseProgram(g_mainShaderProg); }
void UseGizmoShader() { glUseProgram(g_gizmoShaderProg); }

void SetMat4(GLuint program, const char* name, const glm::mat4& mat) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, &mat[0][0]);
}

void SetVec3(GLuint program, const char* name, const glm::vec3& vec) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform3fv(loc, 1, &vec[0]);
}

} // namespace ShaderManager
