#include "shader_manager.h"
#include <iostream>

namespace ShaderManager {

static GLuint g_mainShaderProg = 0;
static GLuint g_gizmoShaderProg = 0;
static GLuint g_unlitShaderProg = 0;
static GLuint g_normalsShaderProg = 0;

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
    // Schlick Fresnel (N·V form). NOTE: the original referenced `reflectDir`,
    // which was only declared later (inside the per-light loop body) — a use-
    // before-declaration that is a hard GLSL compile error on every driver
    // (glslc: "'reflectDir' : undeclared identifier"). Since the Fresnel term
    // needs only the view direction, use the standard N·V Schlick approximation.
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(viewDir, norm), 0.0), 5.0);
    
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

    {
        GLint mainLinkOk = 0;
        glGetProgramiv(g_mainShaderProg, GL_LINK_STATUS, &mainLinkOk);
        if (mainLinkOk == GL_FALSE) {
            char log[1024];
            glGetProgramInfoLog(g_mainShaderProg, sizeof(log), nullptr, log);
            std::cerr << "[ShaderManager] Main PBR shader link failed: " << log
                      << "\nGetMainShaderProgram() will return 0; the editor "
                      << "falls back to the next available flat program.\n";
            glDeleteProgram(g_mainShaderProg);
            g_mainShaderProg = 0;
        }
    }

    glDeleteShader(vsObj);
    glDeleteShader(fsObj);

    // Gizmo shader (AAA aesthetic: fake cylindrical shading, Fresnel rim,
    // hover glow, dual-pass X-Ray occlusion). forGizmo.txt Step 2/3.
    const char* gizmoVS = R"(
#version 430 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal; // Optional for future 3D tube upgrades

uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uModel;

out vec3 vFragPos;
out vec3 vLocalPos;
out vec3 vViewDir;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vFragPos = worldPos.xyz;
    vLocalPos = aPos;

    mat4 invView = inverse(uView);
    vec3 camPos = invView[3].xyz;
    vViewDir = normalize(camPos - worldPos.xyz);

    gl_Position = uProjection * uView * worldPos;
}
)";
    
    const char* gizmoFS = R"(
#version 430 core
in vec3 vFragPos;
in vec3 vLocalPos;
in vec3 vViewDir;

uniform vec3 uColor;
uniform float uHoverGlow = 0.0;
uniform float uIsOccludedPass = 0.0;

out vec4 FragColor;

void main() {
    vec3 viewDir = normalize(vViewDir);

    vec3 fakeNormal = normalize(cross(viewDir, vec3(0.0, 1.0, 0.0)));
    if(length(fakeNormal) < 0.1) fakeNormal = vec3(1.0, 0.0, 0.0);
    float lightIntensity = max(dot(fakeNormal, viewDir), 0.0);
    float pseudoSpec = pow(lightIntensity, 16.0);

    float fresnel = 1.0 - max(dot(fakeNormal, viewDir), 0.0);
    fresnel = pow(fresnel, 3.0) * 0.4;

    vec3 coreColor = mix(uColor, uColor * 1.3 + vec3(0.15), uHoverGlow);
    vec3 finalColor = coreColor * (0.6 + 0.4 * lightIntensity) + (pseudoSpec * 0.3) + (fresnel * coreColor);
    finalColor += (uHoverGlow * 0.25) * vec3(1.0);

    if (uIsOccludedPass > 0.5) {
        float luminance = dot(finalColor, vec3(0.2126, 0.7152, 0.0722));
        vec3 desaturatedColor = mix(vec3(luminance), uColor, 0.2);
        if (mod(gl_FragCoord.x + gl_FragCoord.y, 8.0) > 3.0) {
            discard;
        }
        FragColor = vec4(desaturatedColor, 0.35);
    } else {
        FragColor = vec4(finalColor, 1.0);
    }
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
    
    { GLint gizmoLinkOk = 0;
      glGetProgramiv(g_gizmoShaderProg, GL_LINK_STATUS, &gizmoLinkOk);
      if (gizmoLinkOk == GL_FALSE) {
          std::cerr << "[ShaderManager] Gizmo shader link failed; falling back to flat unlit.\n";
          glDeleteProgram(g_gizmoShaderProg);
          g_gizmoShaderProg = 0;
      } }
    
    glDeleteShader(gvs);
    glDeleteShader(gfs);

    // Unlit (Viewport -> Unlit mode): a flat, lighting-free forward shader that
    // reuses the Renderer's CameraBlock UBO (binding 0, same layout as the main
    // PBR shader) + the "model" uniform (Renderer::SetModelUniform, name-based)
    // + albedo at location 0 (Renderer hardcodes glUniform3f(0, batch.albedo)).
    // That lets the batched forward renderer draw static meshes without a
    // separate code path. Link-failure is recoverable: fall back to the main
    // shader in renderSceneOnly if this program is 0.
    const char* unlitVS = R"(
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
    int pad;
} camera;
void main() {
    gl_Position = camera.projection * camera.view * model * vec4(aPos, 1.0);
}
)";

    const char* unlitFS = R"(
#version 430 core
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
    int pad;
} camera;
layout(location = 0) uniform vec3 albedo;
out vec4 FragColor;
void main() {
    FragColor = vec4(albedo, 1.0);
}
)";

    GLuint uvs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(uvs, 1, &unlitVS, nullptr);
    glCompileShader(uvs);
    GLuint ufs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(ufs, 1, &unlitFS, nullptr);
    glCompileShader(ufs);

    GLuint unlitProg = glCreateProgram();
    glAttachShader(unlitProg, uvs);
    glAttachShader(unlitProg, ufs);
    glLinkProgram(unlitProg);
    GLint unlitLinkOk = 0;
    glGetProgramiv(unlitProg, GL_LINK_STATUS, &unlitLinkOk);
    if (unlitLinkOk == GL_FALSE) {
        std::cerr << "[ShaderManager] Unlit shader link failed (glsl log below); "
                     "Unlit mode will fall back to the main PBR shader.\n";
        glDeleteProgram(unlitProg);
    } else {
        g_unlitShaderProg = unlitProg;
    }
    glDeleteShader(uvs);
    glDeleteShader(ufs);

    // Normals viewport mode (renderMode()==2): visualize mesh normals as RGB
    // (world-space normal * 0.5 + 0.5). Shares the same CameraBlock UBO (binding
    // 0) + "model" uniform as the main shader so the batched forward renderer
    // feeds it for free. Link-checked; falls back to main on failure.
    const char* normalsVS = R"(
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
    int pad;
} camera;
out vec3 vNormal;
void main() {
    vec4 wp = model * vec4(aPos, 1.0);
    vNormal = mat3(model) * aNormal;
    gl_Position = camera.projection * camera.view * wp;
}
)";

    const char* normalsFS = R"(
#version 430 core
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
    int pad;
} camera;
in vec3 vNormal;
out vec4 FragColor;
void main() {
    FragColor = vec4(normalize(vNormal) * 0.5 + 0.5, 1.0);
}
)";

    GLuint nvs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(nvs, 1, &normalsVS, nullptr);
    glCompileShader(nvs);
    GLuint nfs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(nfs, 1, &normalsFS, nullptr);
    glCompileShader(nfs);

    GLuint normalsProg = glCreateProgram();
    glAttachShader(normalsProg, nvs);
    glAttachShader(normalsProg, nfs);
    glLinkProgram(normalsProg);
    GLint normalsLinkOk = 0;
    glGetProgramiv(normalsProg, GL_LINK_STATUS, &normalsLinkOk);
    if (normalsLinkOk == GL_FALSE) {
        std::cerr << "[ShaderManager] Normals shader link failed; "
                     "Normals mode will fall back to the main PBR shader.\n";
        glDeleteProgram(normalsProg);
    } else {
        g_normalsShaderProg = normalsProg;
    }
    glDeleteShader(nvs);
    glDeleteShader(nfs);
}

void CleanupShaders() {
    if (g_mainShaderProg) glDeleteProgram(g_mainShaderProg);
    if (g_gizmoShaderProg) glDeleteProgram(g_gizmoShaderProg);
    if (g_unlitShaderProg) glDeleteProgram(g_unlitShaderProg);
    if (g_normalsShaderProg) glDeleteProgram(g_normalsShaderProg);
    g_mainShaderProg = 0;
    g_gizmoShaderProg = 0;
    g_unlitShaderProg = 0;
    g_normalsShaderProg = 0;
}

GLuint GetMainShaderProgram() { return g_mainShaderProg; }
GLuint GetGizmoShaderProgram() { return g_gizmoShaderProg; }
GLuint GetUnlitShaderProgram() { return g_unlitShaderProg; }
GLuint GetNormalsShaderProgram() { return g_normalsShaderProg; }

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

void SetFloat(GLuint program, const char* name, float v) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform1f(loc, v);
}

} // namespace ShaderManager
