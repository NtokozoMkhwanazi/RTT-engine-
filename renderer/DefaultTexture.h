#pragma once

// ============================================================================
// Default Texture Generator
// ============================================================================
// Creates a default grey texture when model textures are missing
// NOTE: Requires OpenGL context! Returns 0 if no context available.
// ============================================================================

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <iostream>

class DefaultTexture {
public:
    static GLuint GetGreyTexture() {
        // Check if OpenGL context is available
        if (!glGenTextures) {
            std::cerr << "[DefaultTexture] WARNING: No OpenGL context available!\n";
            return 0;  // Return 0 (invalid texture ID) instead of crashing
        }
        
        if (greyTextureID == 0) {
            CreateGreyTexture();
        }
        return greyTextureID;
    }

    static GLuint GetWhiteTexture() {
        // Check if OpenGL context is available
        if (!glGenTextures) {
            std::cerr << "[DefaultTexture] WARNING: No OpenGL context available!\n";
            return 0;  // Return 0 (invalid texture ID) instead of crashing
        }
        
        if (whiteTextureID == 0) {
            CreateWhiteTexture();
        }
        return whiteTextureID;
    }

    static void Cleanup() {
        if (!glDeleteTextures) return;  // No GL context
        
        if (greyTextureID != 0) {
            glDeleteTextures(1, &greyTextureID);
            greyTextureID = 0;
        }
        if (whiteTextureID != 0) {
            glDeleteTextures(1, &whiteTextureID);
            whiteTextureID = 0;
        }
    }

private:
    static inline GLuint greyTextureID = 0;
    static inline GLuint whiteTextureID = 0;

    static void CreateGreyTexture() {
        // Double-check GL context
        if (!glGenTextures) {
            std::cerr << "[DefaultTexture] ERROR: Cannot create texture - no OpenGL context!\n";
            return;
        }
        
        // Create 1x1 grey pixel texture
        unsigned char greyPixel[3] = {128, 128, 128};  // RGB grey

        glGenTextures(1, &greyTextureID);
        glBindTexture(GL_TEXTURE_2D, greyTextureID);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, greyPixel);

        // Set texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, 0);

        std::cout << "[DefaultTexture] Created grey fallback texture\n";
    }

    static void CreateWhiteTexture() {
        // Double-check GL context
        if (!glGenTextures) {
            std::cerr << "[DefaultTexture] ERROR: Cannot create texture - no OpenGL context!\n";
            return;
        }
        
        // Create 1x1 white pixel texture
        unsigned char whitePixel[3] = {255, 255, 255};  // RGB white

        glGenTextures(1, &whiteTextureID);
        glBindTexture(GL_TEXTURE_2D, whiteTextureID);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, whitePixel);

        // Set texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, 0);

        std::cout << "[DefaultTexture] Created white fallback texture\n";
    }
};
