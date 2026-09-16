#pragma once
// ============================================================================
// PostProcess — Bloom + ACES Tone Mapping + Gamma Correction
// ============================================================================
// Multi-pass post-processing:
//   1. Extract bright pixels (luminance threshold)
//   2. Gaussian blur the bright pass (two-pass separable)
//   3. Composite: scene + bloom × strength → ACES tone map → gamma
//
// Uses the fullscreen-triangle shader (no VAO needed).
// ============================================================================

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <iostream>

class PostProcess {
public:
    bool Initialize(int width, int height) {
        m_width = width;
        m_height = height;

        // Create scene FBO (HDR color + depth)
        createFBO(m_sceneFBO, m_sceneColorTex, m_sceneDepthTex, width, height, GL_RGBA16F);

        // Create bloom FBOs (half-res for performance)
        int bw = width / 2, bh = height / 2;
        GLuint noDepth = 0;
        createFBO(m_blurFBO[0], m_blurTex[0], noDepth, bw, bh, GL_RGBA16F);
        createFBO(m_blurFBO[1], m_blurTex[1], noDepth, bw, bh, GL_RGBA16F);

        // ---- Auto-exposure luminance texture --------------------------------
        // GL_R32F with a full mip-chain; glGenerateTextureMipmap reduces the
        // per-pixel log(luminance) down to a 1x1 log-average in the coarsest
        // level. Rendered at 1/8 viewport resolution.
        {
            int lw = std::max(m_width / 8, 1);
            int lh = std::max(m_height / 8, 1);
            int levels = 1, tw = lw, th = lh;
            while (tw > 1 || th > 1) {
                tw = std::max(tw / 2, 1);
                th = std::max(th / 2, 1);
                ++levels;
            }
            glCreateTextures(GL_TEXTURE_2D, 1, &m_lumTex);
            glTextureStorage2D(m_lumTex, levels, GL_R32F, lw, lh);
            glTextureParameteri(m_lumTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTextureParameteri(m_lumTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(m_lumTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(m_lumTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            glGenFramebuffers(1, &m_lumFBO);
            glBindFramebuffer(GL_FRAMEBUFFER, m_lumFBO);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, m_lumTex, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                std::cerr << "[PostProcess] Luminance FBO incomplete!\n";
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        m_initialized = true;
        return true;
    }

    void Shutdown() {
        auto del = [](GLuint& fbo, GLuint& tex, GLuint& depth) {
            if (fbo)  { glDeleteFramebuffers(1, &fbo);  fbo = 0; }
            if (tex)  { glDeleteTextures(1, &tex);       tex = 0; }
            if (depth){ glDeleteTextures(1, &depth);     depth = 0; }
        };
        del(m_sceneFBO, m_sceneColorTex, m_sceneDepthTex);
        del(m_brightFBO, m_brightTex, dummy);
        del(m_blurFBO[0], m_blurTex[0], dummy);
        del(m_blurFBO[1], m_blurTex[1], dummy);
        del(m_lumFBO, m_lumTex, dummy);
        m_initialized = false;
    }

    void Resize(int width, int height) {
        if (!m_initialized) return;
        Shutdown();
        Initialize(width, height);
    }

    // ---- Pass interface -----------------------------------------------------

    // Call before scene rendering: binds the HDR scene FBO
    void BeginScene() {
        glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFBO);
        glViewport(0, 0, m_width, m_height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    // Call after scene rendering: runs auto-exposure + bloom + tone mapping.
    // outputFBO: target framebuffer for the final composite
    //   0            = default framebuffer (fullscreen app)
    //   viewportFBO  = editor's viewport FBO so ImGui picks it up
    // luminanceShader: optional auto-exposure pass (uScene). When non-zero and
    //   m_autoExposure is true, a 1x1 log-average luminance is generated and
    //   fed to the composite shader as uAvgLuminance. Pass 0 to use manual
    //   uExposure only.
    // minLum/maxLum: luminance clamp for the auto-exposure formula.
    void EndScene(GLuint extractShader, GLuint blurShader, GLuint compositeShader,
                  GLuint outputFBO = 0,
                  GLuint luminanceShader = 0,
                  float minLum = 0.0001f,
                  float maxLum = 8.0f) {
        glDisable(GL_DEPTH_TEST);

        const bool doAutoExposure = m_autoExposure && luminanceShader && m_lumTex;

        // (auto-exposure) Extract log-luminance, then mipmap-reduce to a 1x1
        // log-average (geometric-mean) luminance. Runs at 1/8 resolution.
        if (doAutoExposure) {
            int lw = std::max(m_width / 8, 1);
            int lh = std::max(m_height / 8, 1);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_sceneColorTex);
            glUseProgram(luminanceShader);
            glUniform1i(glGetUniformLocation(luminanceShader, "uScene"), 0);
            glBindFramebuffer(GL_FRAMEBUFFER, m_lumFBO);
            glViewport(0, 0, lw, lh);
            drawFullscreenTriangle();
            // GL_R32F mip-chain -> coarsest level = log-average luminance.
            glGenerateTextureMipmap(m_lumTex);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        // 1. Extract bright pixels
        glBindFramebuffer(GL_FRAMEBUFFER, m_brightFBO);
        glViewport(0, 0, m_width / 2, m_height / 2);
        glUseProgram(extractShader);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_sceneColorTex);
        glUniform1i(glGetUniformLocation(extractShader, "uScene"), 0);
        glUniform1f(glGetUniformLocation(extractShader, "uThreshold"), m_bloomThreshold);
        drawFullscreenTriangle();

        // 2. Gaussian blur (separable, iterated)
        glUseProgram(blurShader);
        for (int i = 0; i < m_bloomIterations; ++i) {
            // Horizontal
            glBindFramebuffer(GL_FRAMEBUFFER, m_blurFBO[0]);
            glViewport(0, 0, m_width / 2, m_height / 2);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, (i == 0) ? m_brightTex : m_blurTex[1]);
            glUniform1i(glGetUniformLocation(blurShader, "uImage"), 0);
            glUniform2f(glGetUniformLocation(blurShader, "uDirection"),
                        1.0f / (m_width / 2), 0.0f);
            drawFullscreenTriangle();

            // Vertical
            glBindFramebuffer(GL_FRAMEBUFFER, m_blurFBO[1]);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_blurTex[0]);
            glUniform2f(glGetUniformLocation(blurShader, "uDirection"),
                        0.0f, 1.0f / (m_height / 2));
            drawFullscreenTriangle();
        }

        // 3. Composite: scene + bloom + exposure -> ACES -> outputFBO
        glBindFramebuffer(GL_FRAMEBUFFER, outputFBO);
        glViewport(0, 0, m_width, m_height);
        glUseProgram(compositeShader);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_sceneColorTex);
        glUniform1i(glGetUniformLocation(compositeShader, "uScene"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_blurTex[1]);
        glUniform1i(glGetUniformLocation(compositeShader, "uBloom"), 1);

        glUniform1f(glGetUniformLocation(compositeShader, "uBloomStrength"), m_bloomStrength);
        glUniform1f(glGetUniformLocation(compositeShader, "uExposure"), m_exposure);

        if (doAutoExposure) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, m_lumTex);
            glUniform1i(glGetUniformLocation(compositeShader, "uAvgLuminance"), 2);
        }
        glUniform1i(glGetUniformLocation(compositeShader, "uAutoExposure"),
                    doAutoExposure ? 1 : 0);
        glUniform1f(glGetUniformLocation(compositeShader, "uMinLum"), minLum);
        glUniform1f(glGetUniformLocation(compositeShader, "uMaxLum"), maxLum);

        drawFullscreenTriangle();

        glEnable(GL_DEPTH_TEST);
    }

    // ---- Settings -----------------------------------------------------------
    void SetBloomThreshold(float t) { m_bloomThreshold = t; }
    void SetBloomStrength(float s)   { m_bloomStrength = s; }
    void SetBloomIterations(int n)   { m_bloomIterations = n; }
    void SetExposure(float e)        { m_exposure = e; }
    void SetAutoExposure(bool on)    { m_autoExposure = on; }
    void SetLuminanceRange(float minL, float maxL) { m_minLum = minL; m_maxLum = maxL; }

    GLuint GetSceneFBO()      const { return m_sceneFBO; }
    GLuint GetSceneColorTex() const { return m_sceneColorTex; }
    GLuint GetLuminanceTex()  const { return m_lumTex; }
    bool   IsInitialized()    const { return m_initialized; }

private:
    bool m_initialized = false;
    int  m_width = 0, m_height = 0;

    // Scene FBO (HDR)
    GLuint m_sceneFBO = 0, m_sceneColorTex = 0, m_sceneDepthTex = 0;

    // Bloom FBOs
    GLuint m_brightFBO = 0, m_brightTex = 0;
    GLuint m_blurFBO[2] = {0, 0};
    GLuint m_blurTex[2] = {0, 0};
    GLuint dummy = 0;  // for the del lambda

    // Auto-exposure (log-average luminance via mipmap reduction)
    GLuint m_lumFBO = 0;
    GLuint m_lumTex = 0;

    // Settings
    float m_bloomThreshold = 1.0f;
    float m_bloomStrength  = 0.15f;
    int   m_bloomIterations = 3;
    float m_exposure       = 1.0f;

    // Auto-exposure
    bool  m_autoExposure   = true;
    float m_minLum = 0.0001f;      // floor on log-average luminance
    float m_maxLum = 8.0f;        // ceiling on log-average luminance

    // ---- Helpers ------------------------------------------------------------

    void createFBO(GLuint& fbo, GLuint& tex, GLuint& depth,
                   int w, int h, GLenum internalFormat) {
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        glCreateTextures(GL_TEXTURE_2D, 1, &tex);
        // internalFormat comes from the caller and is always sized
        // (e.g. GL_RGBA16F); immutable storage, 1 level, rendered into via FBO.
        glTextureStorage2D(tex, 1, internalFormat, w, h);
        glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

        if (depth) {
            glCreateTextures(GL_TEXTURE_2D, 1, &depth);
            // GL_DEPTH_COMPONENT24 already sized; 1 level; rendered into via FBO.
            glTextureStorage2D(depth, 1, GL_DEPTH_COMPONENT24, w, h);
            glTextureParameteri(depth, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTextureParameteri(depth, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
        }

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            std::cerr << "[PostProcess] FBO incomplete!\n";

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void drawFullscreenTriangle() {
        // Single triangle, no VAO needed — vertex shader uses gl_VertexID
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
};
