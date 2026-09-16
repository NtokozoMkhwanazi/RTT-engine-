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

    // Flat tangent-space normal (0.5,0.5,1.0). Sampling this as a normal map
    // leaves geometry's shading identical to the legacy flat-lit path, so
    // meshes without a real normal map never regress. (Grey would unpack to
    // (0,0,0) = a degenerate/normalised NaN which breaks lighting.)
    static GLuint GetFlatNormalTexture() {
        if (!glGenTextures) return 0;
        if (flatNormalTextureID == 0) { CreateFlatNormalTexture(); }
        return flatNormalTextureID;
    }

    // Pure black. Used as the default metallic map so meshes without one read
    // metallic=0 (dielectric), which is correct for stone/wood/plants.
    static GLuint GetBlackTexture() {
        if (!glGenTextures) return 0;
        if (blackTextureID == 0) { CreateBlackTexture(); }
        return blackTextureID;
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
        if (flatNormalTextureID != 0) {
            glDeleteTextures(1, &flatNormalTextureID);
            flatNormalTextureID = 0;
        }
        if (blackTextureID != 0) {
            glDeleteTextures(1, &blackTextureID);
            blackTextureID = 0;
        }
    }

private:
    static inline GLuint greyTextureID = 0;
    static inline GLuint whiteTextureID = 0;
    static inline GLuint flatNormalTextureID = 0;
    static inline GLuint blackTextureID = 0;

    static void CreateFlatNormalTexture() {
        if (!glGenTextures) return;
        unsigned char flatPixel[3] = {128, 128, 255};  // (0.5,0.5,1.0) = up
        glCreateTextures(GL_TEXTURE_2D, 1, &flatNormalTextureID);
        glTextureStorage2D(flatNormalTextureID, 1, GL_RGB8, 1, 1);
        glTextureSubImage2D(flatNormalTextureID, 0, 0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, flatPixel);
        glTextureParameteri(flatNormalTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(flatNormalTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(flatNormalTextureID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(flatNormalTextureID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        std::cout << "[DefaultTexture] Created flat-normal fallback texture\n";
    }

    static void CreateBlackTexture() {
        if (!glGenTextures) return;
        unsigned char blackPixel[3] = {0, 0, 0};
        glCreateTextures(GL_TEXTURE_2D, 1, &blackTextureID);
        glTextureStorage2D(blackTextureID, 1, GL_RGB8, 1, 1);
        glTextureSubImage2D(blackTextureID, 0, 0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, blackPixel);
        glTextureParameteri(blackTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(blackTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(blackTextureID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(blackTextureID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        std::cout << "[DefaultTexture] Created black fallback texture\n";
    }

    static void CreateGreyTexture() {
        // Double-check GL context
        if (!glGenTextures) {
            std::cerr << "[DefaultTexture] ERROR: Cannot create texture - no OpenGL context!\n";
            return;
        }
        
        // Create 1x1 grey pixel texture
        unsigned char greyPixel[3] = {128, 128, 128};  // RGB grey

        // DSA: glCreateTextures yields a name with no storage/binding side effects.
        glCreateTextures(GL_TEXTURE_2D, 1, &greyTextureID);
        glTextureStorage2D(greyTextureID, 1, GL_RGB8, 1, 1);                       // unsized GL_RGB -> GL_RGB8
        glTextureSubImage2D(greyTextureID, 0, 0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, greyPixel);

        // Set texture parameters
        glTextureParameteri(greyTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(greyTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(greyTextureID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(greyTextureID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

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

        // DSA: glCreateTextures yields a name with no storage/binding side effects.
        glCreateTextures(GL_TEXTURE_2D, 1, &whiteTextureID);
        glTextureStorage2D(whiteTextureID, 1, GL_RGB8, 1, 1);                    // unsized GL_RGB -> GL_RGB8
        glTextureSubImage2D(whiteTextureID, 0, 0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, whitePixel);

        // Set texture parameters
        glTextureParameteri(whiteTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(whiteTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(whiteTextureID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(whiteTextureID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        std::cout << "[DefaultTexture] Created white fallback texture\n";
    }
};
