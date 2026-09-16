#pragma once
// ============================================================================
// DeferredRenderer — Full Deferred Rendering Pipeline
// ============================================================================
// 3-pass deferred renderer for the editor GL path:
//   1. G-Buffer Pass: render scene into position/normal/albedo/material MRT
//   2. Lighting Pass: compute PBR lighting for all lights in screen-space
//   3. Forward Pass: transparent objects rendered with depth test
//
// Benefits over forward rendering:
//   - O(N+L) instead of O(N*L) for N objects × L lights
//   - SSAO, SSR, volumetrics are trivial to add in screen-space
//   - Only one PBR evaluation per pixel (no overdraw waste)
//
// The G-Buffer layout:
//   - Attachment 0: position (RGBA16F) — view-space
//   - Attachment 1: normal   (RGBA16F) — view-space
//   - Attachment 2: albedo+metallic (RGBA8) — RGB = albedo, A = metallic
//   - Attachment 3: roughness+AO+emissive (RGBA8) — R = roughness, G = AO, B = emissive
//   - Depth: R32F (for depth testing + SSAO input)
// ============================================================================

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include "lighting/LightingEnvironment.h"   // canonical ambient / sun for the lighting pass
#include "lighting/LightData.h"            // GPULightData / LightUBO / makeGpuLight (suggestions.txt #1)
#include <vector>
#include <algorithm>

class DeferredRenderer {
public:
    bool Initialize(int width, int height) {
        m_width = width;
        m_height = height;

        // ---- G-Buffer FBO --------------------------------------------------
        glGenFramebuffers(1, &m_gBufferFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_gBufferFBO);

        // Position (RGBA16F) -- already sized, valid for glTextureStorage2D
        glCreateTextures(GL_TEXTURE_2D, 1, &m_gPosition);
        bindGBufferTexture(m_gPosition, GL_RGBA16F, GL_RGBA, GL_FLOAT, 0);

        // Normal (RGBA16F) -- already sized
        glCreateTextures(GL_TEXTURE_2D, 1, &m_gNormal);
        bindGBufferTexture(m_gNormal, GL_RGBA16F, GL_RGBA, GL_FLOAT, 1);

        // Albedo + Metallic (RGBA8) -- already sized
        glCreateTextures(GL_TEXTURE_2D, 1, &m_gAlbedoMetal);
        bindGBufferTexture(m_gAlbedoMetal, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 2);

        // Roughness + AO + Emissive (RGBA8) -- already sized
        glCreateTextures(GL_TEXTURE_2D, 1, &m_gRoughAOEmissive);
        bindGBufferTexture(m_gRoughAOEmissive, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 3);

        // Tell OpenGL the color attachments
        GLuint attachments[4] = {
            GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
            GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3
        };
        glDrawBuffers(4, attachments);

        // Depth buffer
        glGenRenderbuffers(1, &m_depthRBO);
        glBindRenderbuffer(GL_RENDERBUFFER, m_depthRBO);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthRBO);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[DeferredRenderer] G-Buffer FBO incomplete!\n";
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // ---- Lighting FBO (HDR output) -------------------------------------
        glGenFramebuffers(1, &m_lightingFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_lightingFBO);

        glCreateTextures(GL_TEXTURE_2D, 1, &m_lightingTex);
        glTextureStorage2D(m_lightingTex, 1, GL_RGBA16F, width, height);
        glTextureParameteri(m_lightingTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_lightingTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_lightingTex, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[DeferredRenderer] Lighting FBO incomplete!\n";
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // ---- Light buffer (std430 SSBO, suggestions.txt #1) ----------------
        glGenBuffers(1, &m_lightSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightSSBO);
        // Orphan once; per-frame content is updated with a single glBufferSubData.
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(LightUBO), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        m_initialized = true;
        return true;
    }

    void Shutdown() {
        auto delFBO = [](GLuint& fbo) { if (fbo) { glDeleteFramebuffers(1, &fbo); fbo = 0; } };
        auto delTex = [](GLuint& tex) { if (tex) { glDeleteTextures(1, &tex); tex = 0; } };
        auto delRBO = [](GLuint& rbo) { if (rbo) { glDeleteRenderbuffers(1, &rbo); rbo = 0; } };

        delFBO(m_gBufferFBO);
        delTex(m_gPosition);
        delTex(m_gNormal);
        delTex(m_gAlbedoMetal);
        delTex(m_gRoughAOEmissive);
        delRBO(m_depthRBO);

        delFBO(m_lightingFBO);
        delTex(m_lightingTex);
        if (m_lightSSBO) { glDeleteBuffers(1, &m_lightSSBO); m_lightSSBO = 0; }

        m_initialized = false;
    }

    void Resize(int width, int height) {
        if (!m_initialized) return;
        Shutdown();
        Initialize(width, height);
    }

    // ---- Pass Interface ----------------------------------------------------

    // Begin G-Buffer pass: bind MRT, clear all attachments
    void BeginGBuffer() {
        glBindFramebuffer(GL_FRAMEBUFFER, m_gBufferFBO);
        glViewport(0, 0, m_width, m_height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
    }

    // End G-Buffer pass
    void EndGBuffer() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // ---- Light data for multi-light accumulation ----
    struct LightData {
        glm::vec4 position;  // xyz + type (0=dir, 1=point, 2=spot)
        glm::vec4 color;     // rgb + intensity
    };

    // Lighting pass: fullscreen quad that reads G-Buffer + computes PBR
    // Writes to the lighting FBO (HDR). Supports multiple lights, SSAO, and SSR.
    void RenderLightingPass(GLuint lightingShader,
                            const glm::mat4& projection,
                            const glm::mat4& view,
                            const glm::vec3& cameraPos,
                            const glm::vec3& lightDir,
                            const glm::vec3& lightColor,
                            const LightingEnvironment& lighting,   // #3: explicit env ref (no Instance() in render path)
                            GLuint shadowMapTex = 0,
                            const glm::mat4& lightSpaceMatrix = glm::mat4(1.0f),
                            GLuint ssaoTex = 0,
                            const std::vector<LightData>& lights = {},
                            GLuint ssrTex = 0,
                            float reflectionStrength = 0.3f,
                            const float* cascadeSplits = nullptr,
                            const glm::mat4* cascadeMatrices = nullptr) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_lightingFBO);
        glViewport(0, 0, m_width, m_height);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST);

        glUseProgram(lightingShader);

        // Bind G-Buffer textures
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_gPosition);
        glUniform1i(glGetUniformLocation(lightingShader, "gPosition"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_gNormal);
        glUniform1i(glGetUniformLocation(lightingShader, "gNormal"), 1);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_gAlbedoMetal);
        glUniform1i(glGetUniformLocation(lightingShader, "gAlbedoMetal"), 2);

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, m_gRoughAOEmissive);
        glUniform1i(glGetUniformLocation(lightingShader, "gRoughAOEmissive"), 3);

        // Optional shadow map (CSM cascade ARRAY -> sampler2DArray uShadowMapArray)
        if (shadowMapTex) {
            glActiveTexture(GL_TEXTURE4);
            // ShadowMapper stores the cascades as a GL_TEXTURE_2D_ARRAY; the
            // lighting shader samples it via `uniform sampler2DArray
            // uShadowMapArray`. Binding to GL_TEXTURE_2D (or naming the uniform
            // "uShadowMap") is a type mismatch that leaves the array sampler
            // pointing at texture unit 0 (gPosition) — i.e. shadows are dead.
            glBindTexture(GL_TEXTURE_2D_ARRAY, shadowMapTex);
            glUniform1i(glGetUniformLocation(lightingShader, "uShadowMapArray"), 4);
            glUniform1i(glGetUniformLocation(lightingShader, "uShadowEnabled"), 1);
            glUniformMatrix4fv(glGetUniformLocation(lightingShader, "uLightSpaceMatrix"),
                               1, GL_FALSE, &lightSpaceMatrix[0][0]);
            glUniform1i(glGetUniformLocation(lightingShader, "uShadowEnabled"), 1);
        } else {
            glUniform1i(glGetUniformLocation(lightingShader, "uShadowEnabled"), 0);
        }

        // Optional SSAO
        if (ssaoTex) {
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, ssaoTex);
            glUniform1i(glGetUniformLocation(lightingShader, "uSSAO"), 5);
            glUniform1i(glGetUniformLocation(lightingShader, "uSSAOEnabled"), 1);
        } else {
            glUniform1i(glGetUniformLocation(lightingShader, "uSSAOEnabled"), 0);
        }

        // Shadow CSM uniforms
        if (shadowMapTex && cascadeSplits && cascadeMatrices) {
            for (int i = 0; i < 3; ++i) {
                std::string prefix = "uLightSpaceMatrix[" + std::to_string(i) + "]";
                glUniformMatrix4fv(glGetUniformLocation(lightingShader, prefix.c_str()),
                                   1, GL_FALSE, &cascadeMatrices[i][0][0]);
                prefix = "uCascadeSplits[" + std::to_string(i) + "]";
                glUniform1f(glGetUniformLocation(lightingShader, prefix.c_str()),
                            cascadeSplits[i]);
            }
        }

        // ---- Light accumulation (single SSBO upload, suggestions.txt #1) ----
        // Build a flat, std430 LightUBO from the caller's light list (or a
        // single directional sun fallback when empty) and push it to the GPU in
        // ONE glBufferSubData — replacing the previous 8x glUniform4f + 8x
        // glGetUniformLocation("uLightPositions[i]") per-draw string hashes.
        m_lightCache = LightUBO{};
        if (lights.empty()) {
            // Fallback: directional key light (type=0), surface->sun direction.
            m_lightCache.lightCount = 1u;
            m_lightCache.lights[0]  = makeDirectionalLight(lightDir, lightColor, 1.0f);
        } else {
            const uint32_t n = std::min((size_t)lights.size(), (size_t)kMaxGpuLights);
            m_lightCache.lightCount = n;
            for (uint32_t i = 0; i < n; ++i) {
                // LightData: position.xyz + type(.w) , color.rgb + intensity(.a)
                const glm::vec4& pos = lights[i].position;
                const glm::vec4& col = lights[i].color;
                m_lightCache.lights[i] = makeGpuLight(
                    static_cast<uint32_t>(pos.w), glm::vec3(pos.x, pos.y, pos.z),
                    glm::vec3(0.0f), glm::vec3(col.x, col.y, col.z), col.a);
            }
        }

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_lightSSBO); // binding = 0 (matches GLSL)
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(LightUBO), &m_lightCache);

        // Ambient / sky-light from the LightingEnvironment passed in by the
        // caller (#3: thread the env through a context instead of a global
        // Instance() lookup here), so mesh shading matches the terrain's sun +
        // fog tint instead of drifting.
        const LightingEnvironment& LEnv = lighting;
        glUniform3fv(glGetUniformLocation(lightingShader, "uGroundBounce"), 1, &LEnv.groundBounce[0]);
        glUniform3fv(glGetUniformLocation(lightingShader, "uSkyTint"), 1, &LEnv.skyLightColor[0]);
        glUniform1f(glGetUniformLocation(lightingShader, "uAmbientStrength"), LEnv.ambientStrength);
        glUniform3fv(glGetUniformLocation(lightingShader, "uSunDirectionWS"), 1, &LEnv.sunDirection[0]);
        glUniform1f(glGetUniformLocation(lightingShader, "uSkyLightStrength"), LEnv.skyLightStrength);

        glUniform3fv(glGetUniformLocation(lightingShader, "uCameraPos"), 1, &cameraPos[0]);
        glUniformMatrix4fv(glGetUniformLocation(lightingShader, "uProjection"),
                           1, GL_FALSE, &projection[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(lightingShader, "uView"),
                           1, GL_FALSE, &view[0][0]);

        // SSR uniforms
        if (ssrTex) {
            glActiveTexture(GL_TEXTURE6);
            glBindTexture(GL_TEXTURE_2D, ssrTex);
            glUniform1i(glGetUniformLocation(lightingShader, "uSSRTexture"), 6);
            glUniform1i(glGetUniformLocation(lightingShader, "uSSREnabled"), 1);
            glUniform1f(glGetUniformLocation(lightingShader, "uReflectionStrength"),
                        reflectionStrength);
        } else {
            glUniform1i(glGetUniformLocation(lightingShader, "uSSREnabled"), 0);
        }

        // Draw fullscreen triangle
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glEnable(GL_DEPTH_TEST);
    }

    // Getters
    GLuint GetGBufferFBO()       const { return m_gBufferFBO; }
    GLuint GetGPosition()        const { return m_gPosition; }
    GLuint GetGNormal()          const { return m_gNormal; }
    GLuint GetGAlbedoMetal()     const { return m_gAlbedoMetal; }
    GLuint GetGRoughAOEmissive() const { return m_gRoughAOEmissive; }
    GLuint GetLightingFBO()      const { return m_lightingFBO; }
    GLuint GetLightingTex()      const { return m_lightingTex; }
    GLuint GetDepthRBO()         const { return m_depthRBO; }
    GLuint GetDepthTex()         const { return m_depthRBO; }  // RBO, but accessible for blit
    bool   IsInitialized()       const { return m_initialized; }

private:
    bool m_initialized = false;
    int  m_width = 0, m_height = 0;

    // G-Buffer
    GLuint m_gBufferFBO = 0;
    GLuint m_gPosition = 0;
    GLuint m_gNormal = 0;
    GLuint m_gAlbedoMetal = 0;
    GLuint m_gRoughAOEmissive = 0;
    GLuint m_depthRBO = 0;

    // Lighting output
    GLuint m_lightingFBO = 0;
    GLuint m_lightingTex = 0;

    // Light SSBO (std430 GPULightData[] — suggestions.txt #1). Allocated once
    // in Initialize(); the per-draw light set is pushed with a single
    // glBufferSubData instead of 8x glUniform4f(glGetUniformLocation(...)) lookups.
    GLuint m_lightSSBO = 0;
    LightUBO m_lightCache{};            // CPU mirror; uploaded when dirty

    void bindGBufferTexture(GLuint tex, GLenum internalFmt, GLenum fmt, GLenum type, GLuint colorIdx) {
        // DSA: allocate immutable storage (1 level -- single-level render
        // target, GL_NEAREST, no mipmaps). internalFmt is already sized
        // (GL_RGBA16F / GL_RGBA8) so glTextureStorage2D is legal; no bind needed.
        glTextureStorage2D(tex, 1, internalFmt, m_width, m_height);
        glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + colorIdx,
                               GL_TEXTURE_2D, tex, 0);
    }
};
