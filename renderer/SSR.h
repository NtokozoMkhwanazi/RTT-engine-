#pragma once
// ============================================================================
// SSR — Screen-Space Reflections
// ============================================================================
// Traces reflections in screen-space using the G-Buffer:
//   1. For each pixel, march along the reflected ray in view space
//   2. At each step, reproject to screen-space and compare depth
//   3. If the ray hits geometry, sample the color at that point
//   4. Apply edge fading, blur, and blend with the scene
//
// Input: G-Position, G-Normal, depth, scene color (from deferred lighting)
// Output: SSR composite (scene + reflections)
// ============================================================================

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <random>
#include <vector>

class SSR {
public:
    bool Initialize(int width, int height) {
        m_width  = width;
        m_height = height;

        // ---- SSR FBO (half-res for performance) ----------------------------
        int hw = width / 2, hh = height / 2;
        glGenFramebuffers(1, &m_ssrfbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssrfbo);

        glCreateTextures(GL_TEXTURE_2D, 1, &m_ssrTex);
        // GL_RGBA16F already sized; 1 level (LINEAR, no mipmaps); rendered into via FBO.
        glTextureStorage2D(m_ssrTex, 1, GL_RGBA16F, hw, hh);
        glTextureParameteri(m_ssrTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_ssrTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_ssrTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_ssrTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssrTex, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[SSR] SSR FBO incomplete!\n";
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // ---- Blur FBOs (separable Gaussian, half-res) ---------------------
        glGenFramebuffers(1, &m_blurFBO[0]);
        glBindFramebuffer(GL_FRAMEBUFFER, m_blurFBO[0]);
        glCreateTextures(GL_TEXTURE_2D, 1, &m_blurTex[0]);
        // GL_RGBA16F already sized; 1 level (LINEAR, no mipmaps); rendered into via FBO.
        glTextureStorage2D(m_blurTex[0], 1, GL_RGBA16F, hw, hh);
        glTextureParameteri(m_blurTex[0], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_blurTex[0], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_blurTex[0], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_blurTex[0], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_blurTex[0], 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glGenFramebuffers(1, &m_blurFBO[1]);
        glBindFramebuffer(GL_FRAMEBUFFER, m_blurFBO[1]);
        glCreateTextures(GL_TEXTURE_2D, 1, &m_blurTex[1]);
        // GL_RGBA16F already sized; 1 level (LINEAR, no mipmaps); rendered into via FBO.
        glTextureStorage2D(m_blurTex[1], 1, GL_RGBA16F, hw, hh);
        glTextureParameteri(m_blurTex[1], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_blurTex[1], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_blurTex[1], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_blurTex[1], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_blurTex[1], 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        m_initialized = true;
        return true;
    }

    void Shutdown() {
        auto del = [](GLuint& fbo, GLuint& tex) {
            if (fbo)  { glDeleteFramebuffers(1, &fbo);  fbo = 0; }
            if (tex)  { glDeleteTextures(1, &tex);       tex = 0; }
        };
        del(m_ssrfbo, m_ssrTex);
        del(m_blurFBO[0], m_blurTex[0]);
        del(m_blurFBO[1], m_blurTex[1]);
        m_initialized = false;
    }

    void Resize(int width, int height) {
        if (!m_initialized) return;
        Shutdown();
        Initialize(width, height);
    }

    // ---- Pass Interface ----------------------------------------------------

    // SSR ray march: reads G-Buffer, writes reflections to m_ssrTex
    void RenderSSR(GLuint ssrShader,
                   const glm::mat4& projection,
                   const glm::mat4& view,
                   GLuint gPositionTex,
                   GLuint gNormalTex,
                   GLuint sceneColorTex,
                   GLuint depthTex,
                   int maxSteps = 64,
                   float thickness = 0.5f) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssrfbo);
        int hw = m_width / 2, hh = m_height / 2;
        glViewport(0, 0, hw, hh);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(ssrShader);

        // G-Buffer textures
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gPositionTex);
        glUniform1i(glGetUniformLocation(ssrShader, "gPosition"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gNormalTex);
        glUniform1i(glGetUniformLocation(ssrShader, "gNormal"), 1);

        // Scene color (source of reflection samples)
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, sceneColorTex);
        glUniform1i(glGetUniformLocation(ssrShader, "uSceneColor"), 2);

        // Depth (for ray hit detection)
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, depthTex);
        glUniform1i(glGetUniformLocation(ssrShader, "uDepth"), 3);

        // Uniforms
        glUniformMatrix4fv(glGetUniformLocation(ssrShader, "uProjection"),
                           1, GL_FALSE, &projection[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(ssrShader, "uView"),
                           1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(ssrShader, "uInvProjection"),
                           1, GL_FALSE, &glm::inverse(projection)[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(ssrShader, "uInvView"),
                           1, GL_FALSE, &glm::inverse(view)[0][0]);
        glUniform2f(glGetUniformLocation(ssrShader, "uScreenSize"),
                    (float)m_width, (float)m_height);
        glUniform1i(glGetUniformLocation(ssrShader, "uMaxSteps"), maxSteps);
        glUniform1f(glGetUniformLocation(ssrShader, "uThickness"), thickness);

        drawFullscreenTriangle();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // Blur pass: smooth the raw SSR noise
    void RenderBlur(GLuint blurShader) {
        int hw = m_width / 2, hh = m_height / 2;

        // Horizontal
        glBindFramebuffer(GL_FRAMEBUFFER, m_blurFBO[0]);
        glViewport(0, 0, hw, hh);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(blurShader);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_ssrTex);
        glUniform1i(glGetUniformLocation(blurShader, "uImage"), 0);
        glUniform2f(glGetUniformLocation(blurShader, "uDirection"),
                    1.0f / (float)hw, 0.0f);
        drawFullscreenTriangle();

        // Vertical
        glBindFramebuffer(GL_FRAMEBUFFER, m_blurFBO[1]);
        glViewport(0, 0, hw, hh);
        glClear(GL_COLOR_BUFFER_BIT);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_blurTex[0]);
        glUniform2f(glGetUniformLocation(blurShader, "uDirection"),
                    0.0f, 1.0f / (float)hh);
        drawFullscreenTriangle();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // ---- Getters -----------------------------------------------------------
    GLuint GetSSRTexture()      const { return m_blurTex[1]; }  // blurred result
    GLuint GetRawSSRTexture()   const { return m_ssrTex; }
    bool   IsInitialized()      const { return m_initialized; }

private:
    bool m_initialized = false;
    int  m_width = 0, m_height = 0;

    // SSR
    GLuint m_ssrfbo   = 0;
    GLuint m_ssrTex   = 0;

    // Blur
    GLuint m_blurFBO[2] = {0, 0};
    GLuint m_blurTex[2] = {0, 0};

    void drawFullscreenTriangle() {
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
};
