#pragma once
// ============================================================================
// ShadowMapper — Cascaded Shadow Maps (CSM) with PCF
// ============================================================================
// 3-cascade CSM for large-world terrain rendering.
// Each cascade covers a fraction of the view frustum with increasing
// resolution for nearby geometry. Logarithmic split scheme (Practical
// Shadow Mapping, Engel 2006) gives high quality near the camera and
// acceptable quality at distance.
//
// Pipeline:
//   Begin(viewProj, lightDir) → for each cascade: bind slice FBO, render
//   depth → End() → Bind(textureUnit) binds the ARRAY texture so the PBR
//   shader can sample cascade 0/1/2 by layer.
// ============================================================================

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <iostream>
#include <array>
#include <vector>

// Canonical sun direction + the LightingEnvironment standard live in
// lighting/LightingEnvironment.h (lighting/ is the engine-wide source of
// truth for the sun; kSunDirection is defined there). ShadowMapper only needs
// its default value before Begin() runs.
#include "lighting/LightingEnvironment.h"

class ShadowMapper {
public:
    // Each cascade is 2048²; the 3 layers share a 2D-array texture
    static constexpr int SHADOW_SIZE  = 2048;
    static constexpr int NUM_CASCADES = 3;

    // Split scheme: lambda = 0.5 blends linear + logarithmic
    static constexpr float SPLIT_LAMBDA = 0.5f;

    bool Initialize() {
        // ---- Depth array texture (3 layers) ---------------------------------
        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_cascadeArrayTex);
        glTextureStorage3D(m_cascadeArrayTex, 1, GL_DEPTH_COMPONENT32F,
                           SHADOW_SIZE, SHADOW_SIZE, NUM_CASCADES);

        // glTextureStorage3D above already allocates all 3 layers immutably.
        // (The old glTexImage3D + per-layer glTexSubImage3D(NULL) loop was a
        // no-op -- NULL subimage leaves contents unchanged -- and per-cascade
        // glClear(GL_DEPTH_BUFFER_BIT) in BeginCascade() initializes them.)

        glTextureParameteri(m_cascadeArrayTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_cascadeArrayTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_cascadeArrayTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTextureParameteri(m_cascadeArrayTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        float border[] = {1.0f, 1.0f, 1.0f, 1.0f};
        glTextureParameterfv(m_cascadeArrayTex, GL_TEXTURE_BORDER_COLOR, border);
        glTextureParameteri(m_cascadeArrayTex, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTextureParameteri(m_cascadeArrayTex, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

        // ---- Per-cascade FBOs (one per layer) ------------------------------
        for (int i = 0; i < NUM_CASCADES; ++i) {
            glGenFramebuffers(1, &m_cascadeFBO[i]);
            glBindFramebuffer(GL_FRAMEBUFFER, m_cascadeFBO[i]);
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                      m_cascadeArrayTex, 0, i);
            glDrawBuffer(GL_NONE);
            glReadBuffer(GL_NONE);

            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                std::cerr << "[CSM] FBO layer " << i << " incomplete!\n";
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                return false;
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        m_initialized = true;
        return true;
    }

    void Shutdown() {
        for (int i = 0; i < NUM_CASCADES; ++i) {
            if (m_cascadeFBO[i]) { glDeleteFramebuffers(1, &m_cascadeFBO[i]); m_cascadeFBO[i] = 0; }
        }
        if (m_cascadeArrayTex) { glDeleteTextures(1, &m_cascadeArrayTex); m_cascadeArrayTex = 0; }
        m_initialized = false;
    }

    // -----------------------------------------------------------------------
    // Begin: compute cascade splits and bind the full array for a multi-pass
    // shadow render. The caller renders the scene NUM_CASCADES times, each
    // time calling BeginCascade(i) to switch the active layer.
    // -----------------------------------------------------------------------
    void Begin(const glm::mat4& viewProj, const glm::vec3& lightDir,
               float nearClip = 0.1f, float farClip = 500.0f) {
        if (!m_initialized) return;

        m_lightDir = glm::normalize(lightDir);

        // Extract frustum split distances (log-linear blend)
        float range   = farClip - nearClip;
        float ratio   = farClip / nearClip;
        for (int i = 0; i < NUM_CASCADES; ++i) {
            float p = (float)(i + 1) / (float)NUM_CASCADES;
            float logSplit  = nearClip * std::pow(ratio, p);
            float linSplit = nearClip + range * p;
            float splitDist = SPLIT_LAMBDA * logSplit + (1.0f - SPLIT_LAMBDA) * linSplit;

            m_splits[i] = (i == 0) ? nearClip : m_splits[i - 1];
            m_splitDistances[i] = splitDist;
        }

        // Compute light-space matrices for each cascade
        computeLightMatrices(viewProj, nearClip, farClip);

        // Enable front-face culling to reduce shadow acne
        glCullFace(GL_FRONT);
    }

    // Bind FBO for cascade index i and set the viewport
    void BeginCascade(int i) {
        if (i < 0 || i >= NUM_CASCADES) return;
        glBindFramebuffer(GL_FRAMEBUFFER, m_cascadeFBO[i]);
        glViewport(0, 0, SHADOW_SIZE, SHADOW_SIZE);
        glClear(GL_DEPTH_BUFFER_BIT);
        m_activeCascade = i;
    }

    void End() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glCullFace(GL_BACK);
        m_activeCascade = -1;
    }

    // Bind the array texture to a texture unit (default: unit 4)
    void Bind(GLuint textureUnit = 4) {
        glActiveTexture(GL_TEXTURE0 + textureUnit);
        glBindTexture(GL_TEXTURE_2D_ARRAY, m_cascadeArrayTex);
    }

    // Getters
    bool IsInitialized() const { return m_initialized; }
    GLuint GetCascadeArrayTex() const { return m_cascadeArrayTex; }
    int GetActiveCascade() const { return m_activeCascade; }

    const glm::mat4& GetLightSpaceMatrix(int i) const { return m_lightSpaceMatrices[i]; }
    const glm::mat4& GetLightView() const { return m_lightView; }
    float GetSplitDistance(int i) const { return m_splitDistances[i]; }

    // The currently-active sun direction (surface -> sun). Set by Begin();
    // useful for hosts that want the same sun the shadow cascades were built for.
    glm::vec3 GetLightDir() const { return m_lightDir; }

    // Return all cascade light-space matrices as a contiguous array for uniform upload
    const glm::mat4* GetLightSpaceMatricesPtr() const { return m_lightSpaceMatrices.data(); }

private:
    bool m_initialized = false;

    GLuint m_cascadeArrayTex = 0;
    GLuint m_cascadeFBO[NUM_CASCADES] = {};

    int m_activeCascade = -1;

    glm::vec3 m_lightDir{kSunDirection};
    glm::mat4 m_lightView;

    std::array<glm::mat4, NUM_CASCADES> m_lightSpaceMatrices{};
    std::array<float, NUM_CASCADES> m_splits{};
    std::array<float, NUM_CASCADES> m_splitDistances{};

    // -------------------------------------------------------------------
    // Compute the orthographic projection + light view for each cascade.
    // Each cascade's frustum slice is fitted to a tight ortho box.
    // -------------------------------------------------------------------
    void computeLightMatrices(const glm::mat4& viewProj, float nearClip, float farClip) {
        // Light view: the sun sits in the +surfaceToSun direction (m_lightDir),
        // so the shadow camera looks from ABOVE the scene DOWN onto it. (The old
        // '-m_lightDir' placed the camera below, looking up through the terrain's
        // underside, which inverted every shadow. Flipped to match the canonical
        // surface->sun convention used by terrain_splat + deferred_lighting.)
        glm::vec3 lightPos = m_lightDir * 100.0f;
        m_lightView = glm::lookAt(lightPos, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

        // Reconstruct 8 frustum corners from the VP matrix (inverse)
        glm::mat4 invVP = glm::inverse(viewProj);
        std::array<glm::vec4, 8> corners = {
            invVP * glm::vec4(-1, -1, -1, 1),
            invVP * glm::vec4( 1, -1, -1, 1),
            invVP * glm::vec4(-1,  1, -1, 1),
            invVP * glm::vec4( 1,  1, -1, 1),
            invVP * glm::vec4(-1, -1,  1, 1),
            invVP * glm::vec4( 1, -1,  1, 1),
            invVP * glm::vec4(-1,  1,  1, 1),
            invVP * glm::vec4( 1,  1,  1, 1)
        };
        // Perspective divide
        for (auto& c : corners) c /= c.w;

        // Direction from near to far corner pair
        glm::vec3 ray = glm::normalize(glm::vec3(corners[4]) - glm::vec3(corners[0]));
        float totalDist = glm::length(glm::vec3(corners[4]) - glm::vec3(corners[0]));

        for (int i = 0; i < NUM_CASCADES; ++i) {
            float splitStart = (i == 0) ? 0.0f : m_splitDistances[i - 1];
            float splitEnd   = m_splitDistances[i];

            // 4 near corners + 4 far corners for this cascade slice
            std::array<glm::vec3, 8> sliceCorners;
            for (int j = 0; j < 4; ++j) {
                float tNear = (splitStart - nearClip) / (farClip - nearClip);
                float tFar  = (splitEnd   - nearClip) / (farClip - nearClip);
                sliceCorners[j]     = glm::mix(glm::vec3(corners[j]), glm::vec3(corners[j + 4]), tNear);
                sliceCorners[j + 4] = glm::mix(glm::vec3(corners[j]), glm::vec3(corners[j + 4]), tFar);
            }

            // Transform all 8 corners to light space
            glm::vec3 center(0.0f);
            for (auto& c : sliceCorners) {
                glm::vec3 lc = glm::vec3(m_lightView * glm::vec4(c, 1.0f));
                center += lc;
                c = lc;
            }
            center /= 8.0f;

            // Find tight AABB in light space
            float minX = 1e10f, maxX = -1e10f;
            float minY = 1e10f, maxY = -1e10f;
            float minZ = 1e10f, maxZ = -1e10f;
            for (auto& c : sliceCorners) {
                minX = std::min(minX, c.x); maxX = std::max(maxX, c.x);
                minY = std::min(minY, c.y); maxY = std::max(maxY, c.y);
                minZ = std::min(minZ, c.z); maxZ = std::max(maxZ, c.z);
            }

            // Add padding to prevent shadow edge shimmer
            float scaleX = (maxX - minX) * 1.05f;
            float scaleY = (maxY - minY) * 1.05f;
            float maxExt = std::max(scaleX, scaleY);

            // Snap to texel increments to prevent sub-pixel shimmer
            float worldUnitsPerTexel = maxExt / (float)SHADOW_SIZE;
            center.x = std::floor(center.x / worldUnitsPerTexel) * worldUnitsPerTexel;
            center.y = std::floor(center.y / worldUnitsPerTexel) * worldUnitsPerTexel;

            // Orthographic projection centered on the cascade
            float halfW = maxExt * 0.5f;
            float halfH = maxExt * 0.5f;
            m_lightSpaceMatrices[i] = glm::ortho(-halfW, halfW, -halfH, halfH,
                                                  minZ - 1.0f, maxZ + 1.0f)
                                    * glm::lookAt(center + m_lightDir * 10.0f,
                                                  center,
                                                  glm::vec3(0.0f, 1.0f, 0.0f));
        }
    }
};
