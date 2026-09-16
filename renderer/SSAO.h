#pragma once
// ============================================================================
// SSAO — Screen-Space Ambient Occlusion
// ============================================================================
// Generates SSAO using a kernel-based approach:
//   1. G-Position Pass: render scene positions to a depth/position texture
//   2. SSAO Pass: for each pixel, sample a hemisphere kernel in view space,
//      compare depth to determine occlusion
//   3. Blur Pass: separable Gaussian blur to smooth noise
//   4. Composite: multiply scene color by SSAO factor
//
// Uses a 256-sample random rotation noise texture to decorrelate samples.
// ============================================================================

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <random>
#include <vector>

class SSAO {
public:
    bool Initialize(int width, int height) {
        m_width  = width;
        m_height = height;

        // ---- Generate sample kernel (64 hemisphere samples) -----------------
        generateKernel();

        // ---- Generate noise texture (4×4 random rotation vectors) ----------
        generateNoise();

        // ---- G-Position FBO (depth + view-space position) ------------------
        createGBufferFBO();

        // ---- SSAO FBO (half-res for performance) ---------------------------
        int hw = width / 2, hh = height / 2;
        glGenFramebuffers(1, &m_ssaoFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoFBO);

        glCreateTextures(GL_TEXTURE_2D, 1, &m_ssaoTex);
        // GL_RED (unsized) -> GL_R8 (sized), required by glTextureStorage2D.
        // 1 level (NEAREST, no mipmaps): rendered into via the FBO below.
        glTextureStorage2D(m_ssaoTex, 1, GL_R8, hw, hh);
        glTextureParameteri(m_ssaoTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_ssaoTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssaoTex, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[SSAO] SSAO FBO incomplete!\n";
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // ---- Blur FBO (same size as SSAO) ----------------------------------
        glGenFramebuffers(1, &m_blurFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_blurFBO);

        glCreateTextures(GL_TEXTURE_2D, 1, &m_blurTex);
        // GL_RED -> GL_R8 (sized); 1 level; rendered into via the FBO below.
        glTextureStorage2D(m_blurTex, 1, GL_R8, hw, hh);
        glTextureParameteri(m_blurTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_blurTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_blurTex, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[SSAO] Blur FBO incomplete!\n";
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        m_initialized = true;
        return true;
    }

    void Shutdown() {
        auto del = [](GLuint& fbo, GLuint& tex) {
            if (fbo)  { glDeleteFramebuffers(1, &fbo);  fbo = 0; }
            if (tex)  { glDeleteTextures(1, &tex);       tex = 0; }
        };
        del(m_gBufferFBO, m_gPositionTex);
        glDeleteTextures(1, &m_gNormalTex);
        glDeleteTextures(1, &m_noiseTex);
        glDeleteTextures(1, &m_depthRBO);
        del(m_ssaoFBO, m_ssaoTex);
        del(m_blurFBO, m_blurTex);
        m_gNormalTex = 0;
        m_noiseTex = 0;
        m_depthRBO = 0;
        m_initialized = false;
    }

    void Resize(int width, int height) {
        if (!m_initialized) return;
        Shutdown();
        Initialize(width, height);
    }

    // ---- Pass Interface ----------------------------------------------------

    // Begin G-Position pass: render scene positions to gPosition + gNormal
    void BeginGBuffer() {
        glBindFramebuffer(GL_FRAMEBUFFER, m_gBufferFBO);
        glViewport(0, 0, m_width, m_height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    // End G-Position pass
    void EndGBuffer() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // SSAO pass: reads G-Buffer, writes occlusion to m_ssaoTex
    void RenderSSAO(GLuint ssaoShader,
                    const glm::mat4& projection,
                    const glm::mat4& view,
                    int kernelSize = 64,
                    float radius = 0.5f,
                    float bias = 0.025f) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoFBO);
        int hw = m_width / 2, hh = m_height / 2;
        glViewport(0, 0, hw, hh);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(ssaoShader);

        // Bind G-Buffer textures
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_gPositionTex);
        glUniform1i(glGetUniformLocation(ssaoShader, "gPosition"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_gNormalTex);
        glUniform1i(glGetUniformLocation(ssaoShader, "gNormal"), 1);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_noiseTex);
        glUniform1i(glGetUniformLocation(ssaoShader, "texNoise"), 2);

        // Upload kernel
        for (int i = 0; i < kernelSize && i < (int)m_kernel.size(); ++i) {
            std::string name = "samples[" + std::to_string(i) + "]";
            glUniform3f(glGetUniformLocation(ssaoShader, name.c_str()),
                        m_kernel[i].x, m_kernel[i].y, m_kernel[i].z);
        }
        glUniform1i(glGetUniformLocation(ssaoShader, "kernelSize"), std::min(kernelSize, (int)m_kernel.size()));
        glUniform1f(glGetUniformLocation(ssaoShader, "radius"), radius);
        glUniform1f(glGetUniformLocation(ssaoShader, "bias"), bias);
        glUniform2f(glGetUniformLocation(ssaoShader, "noiseScale"),
                    (float)m_width / 4.0f, (float)m_height / 4.0f);

        drawFullscreenTriangle();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // Blur pass: smooth the raw SSAO noise
    void RenderBlur(GLuint blurShader) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_blurFBO);
        int hw = m_width / 2, hh = m_height / 2;
        glViewport(0, 0, hw, hh);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(blurShader);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_ssaoTex);
        glUniform1i(glGetUniformLocation(blurShader, "uImage"), 0);
        glUniform2f(glGetUniformLocation(blurShader, "uDirection"),
                    1.0f / (float)hw, 0.0f);

        drawFullscreenTriangle();

        // Second pass (vertical)
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoFBO);
        glViewport(0, 0, hw, hh);
        glClear(GL_COLOR_BUFFER_BIT);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_blurTex);
        glUniform2f(glGetUniformLocation(blurShader, "uDirection"),
                    0.0f, 1.0f / (float)hh);

        drawFullscreenTriangle();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // ---- Getters -----------------------------------------------------------
    GLuint GetSSAOTexture()     const { return m_blurTex; }  // return the blurred result
    GLuint GetGPositionTex()    const { return m_gPositionTex; }
    GLuint GetGNormalTex()      const { return m_gNormalTex; }
    GLuint GetNoiseTexture()    const { return m_noiseTex; }
    GLuint GetGPositionFBO()    const { return m_gBufferFBO; }  // for deferred G-Buffer blit
    bool   IsInitialized()      const { return m_initialized; }
    int    GetKernelSize()      const { return (int)m_kernel.size(); }

private:
    bool m_initialized = false;
    int  m_width = 0, m_height = 0;

    // G-Buffer
    GLuint m_gBufferFBO   = 0;
    GLuint m_gPositionTex = 0;
    GLuint m_gNormalTex   = 0;
    GLuint m_depthRBO     = 0;

    // SSAO
    GLuint m_ssaoFBO  = 0;
    GLuint m_ssaoTex  = 0;
    GLuint m_blurFBO  = 0;
    GLuint m_blurTex  = 0;

    // Noise + kernel
    std::vector<glm::vec3> m_kernel;
    GLuint m_noiseTex = 0;

    // -------------------------------------------------------------------
    void generateKernel() {
        m_kernel.resize(64);
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        std::default_random_engine rng(42);

        for (int i = 0; i < 64; ++i) {
            // Hemisphere sample (z >= 0)
            glm::vec3 sample(
                dist(rng) * 2.0f - 1.0f,
                dist(rng) * 2.0f - 1.0f,
                dist(rng)               // z ∈ [0, 1]
            );
            sample = glm::normalize(sample);
            sample *= dist(rng);

            // Accelerating interpolation: samples closer to origin have more
            // weight (most occlusion comes from nearby geometry)
            float scale = (float)i / 64.0f;
            scale = 0.1f + (1.0f - 0.1f) * (scale * scale);
            sample *= scale;
            m_kernel[i] = sample;
        }
    }

    void generateNoise() {
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        std::default_random_engine rng(12345);

        std::vector<glm::vec3> noise(16);
        for (int i = 0; i < 16; ++i) {
            noise[i] = glm::vec3(dist(rng) * 2.0f - 1.0f,
                                  dist(rng) * 2.0f - 1.0f,
                                  0.0f);  // tangent-space rotation only
        }

        glCreateTextures(GL_TEXTURE_2D, 1, &m_noiseTex);
        // GL_RGB16F already sized; 1 level (NEAREST, no mipmaps).
        glTextureStorage2D(m_noiseTex, 1, GL_RGB16F, 4, 4);
        glTextureSubImage2D(m_noiseTex, 0, 0, 0, 4, 4,
                            GL_RGB, GL_FLOAT, noise.data());
        glTextureParameteri(m_noiseTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_noiseTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(m_noiseTex, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_noiseTex, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    void createGBufferFBO() {
        glGenFramebuffers(1, &m_gBufferFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_gBufferFBO);

        // Position buffer (RGBA16F for precision)
        glCreateTextures(GL_TEXTURE_2D, 1, &m_gPositionTex);
        // GL_RGBA16F already sized; 1 level; rendered into via the FBO below.
        glTextureStorage2D(m_gPositionTex, 1, GL_RGBA16F, m_width, m_height);
        glTextureParameteri(m_gPositionTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_gPositionTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gPositionTex, 0);

        // Normal buffer (RGBA16F)
        glCreateTextures(GL_TEXTURE_2D, 1, &m_gNormalTex);
        // GL_RGBA16F already sized; 1 level; rendered into via the FBO below.
        glTextureStorage2D(m_gNormalTex, 1, GL_RGBA16F, m_width, m_height);
        glTextureParameteri(m_gNormalTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_gNormalTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_gNormalTex, 0);

        // Tell OpenGL which color attachments to draw
        GLuint attachments[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, attachments);

        // Depth RBO
        glGenRenderbuffers(1, &m_depthRBO);
        glBindRenderbuffer(GL_RENDERBUFFER, m_depthRBO);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_width, m_height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthRBO);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[SSAO] G-Buffer FBO incomplete!\n";
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void drawFullscreenTriangle() {
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
};
