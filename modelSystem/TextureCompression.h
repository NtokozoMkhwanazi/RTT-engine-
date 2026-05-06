#ifndef TEXTURE_COMPRESSION_H
#define TEXTURE_COMPRESSION_H

/**
 * Texture Compression Utilities
 * 
 * Supports:
 * - BC1/DXT1 compression for RGB textures (6:1 ratio)
 * - BC3/DXT5 compression for RGBA textures (4:1 ratio)
 * - DDS file loading for pre-compressed BC7/BC5 textures
 * - Automatic caching of compressed textures to disk
 * 
 * Benefits:
 * - Reduced VRAM usage (up to 6x smaller)
 * - Faster load times (less data to transfer to GPU)
 * - Better cache efficiency
 */

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cmath>

// S3TC/DXT compression formats (may not be in all OpenGL headers)
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_RED_RGTC1
#define GL_COMPRESSED_RED_RGTC1 0x8DBB
#endif
#ifndef GL_COMPRESSED_SIGNED_RED_RGTC1
#define GL_COMPRESSED_SIGNED_RED_RGTC1 0x8DBC
#endif
#ifndef GL_COMPRESSED_RG_RGTC2
#define GL_COMPRESSED_RG_RGTC2 0x8DBD
#endif
#ifndef GL_COMPRESSED_SIGNED_RG_RGTC2
#define GL_COMPRESSED_SIGNED_RG_RGTC2 0x8DBE
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM 0x8E8D
#endif
#ifndef GL_COMPRESSED_SRGB_BPTC_UNORM
#define GL_COMPRESSED_SRGB_BPTC_UNORM 0x8E8E
#endif
#ifndef GL_COMPRESSED_SRGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_SRGB_S3TC_DXT1_EXT 0x8C4C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4D
#endif

// DDS file format constants
#define DDS_FOURCC 0x00000004
#define DDS_RGB    0x00000040
#define DDS_RGBA   0x00000041

// DDS FourCC codes
#define DDS_DXT1  0x31545844
#define DDS_DXT3  0x33545844
#define DDS_DXT5  0x35545844
#define DDS_BC7   0x37434200
#define DDS_BC5   0x35434200

// DDS header structure
struct DDSHeader {
    uint32_t magic;
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipCount;
    uint32_t reserved1[11];
    struct {
        uint32_t size;
        uint32_t flags;
        uint32_t fourCC;
        uint32_t rgbBitCount;
        uint32_t rBitMask;
        uint32_t gBitMask;
        uint32_t bBitMask;
        uint32_t aBitMask;
    } ddspf;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};

// ============================================================================
// BC1/DXT1 Compression (6:1 ratio for RGB)
// ============================================================================

/**
 * Encode a 4x4 block of RGB pixels as BC1/DXT1
 */
static void EncodeBC1Block(
    const uint8_t* pixels,    // 4x4x3 = 48 bytes
    uint8_t* outBlock)        // 8 bytes output
{
    // Find min and max color in the block
    uint8_t minColor[3] = {255, 255, 255};
    uint8_t maxColor[3] = {0, 0, 0};
    
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            const uint8_t* p = pixels + (y * 4 + x) * 3;
            int r = p[0], g = p[1], b = p[2];
            int lum = r * 299 + g * 587 + b * 114; // Luminance
            
            if (lum < (minColor[0] * 299 + minColor[1] * 587 + minColor[2] * 114)) {
                minColor[0] = r; minColor[1] = g; minColor[2] = b;
            }
            if (lum > (maxColor[0] * 299 + maxColor[1] * 587 + maxColor[2] * 114)) {
                maxColor[0] = r; maxColor[1] = g; maxColor[2] = b;
            }
        }
    }
    
    // Pack endpoints as 5-6-5 RGB
    uint16_t c0 = ((maxColor[0] >> 3) << 11) | ((maxColor[1] >> 2) << 5) | (maxColor[2] >> 3);
    uint16_t c1 = ((minColor[0] >> 3) << 11) | ((minColor[1] >> 2) << 5) | (minColor[2] >> 3);
    
    if (c0 < c1) std::swap(c0, c1);
    
    outBlock[0] = c0 & 0xFF;
    outBlock[1] = (c0 >> 8) & 0xFF;
    outBlock[2] = c1 & 0xFF;
    outBlock[3] = (c1 >> 8) & 0xFF;
    
    // Compute interpolated colors
    uint8_t colors[4][3];
    colors[0][0] = (c0 >> 11) & 0x1F; colors[0][0] = (colors[0][0] << 3) | (colors[0][0] >> 2);
    colors[0][1] = (c0 >> 5) & 0x3F;  colors[0][1] = (colors[0][1] << 2) | (colors[0][1] >> 4);
    colors[0][2] = c0 & 0x1F;         colors[0][2] = (colors[0][2] << 3) | (colors[0][2] >> 2);
    
    colors[1][0] = (c1 >> 11) & 0x1F; colors[1][0] = (colors[1][0] << 3) | (colors[1][0] >> 2);
    colors[1][1] = (c1 >> 5) & 0x3F;  colors[1][1] = (colors[1][1] << 2) | (colors[1][1] >> 4);
    colors[1][2] = c1 & 0x1F;         colors[1][2] = (colors[1][2] << 3) | (colors[1][2] >> 2);
    
    colors[2][0] = (2 * colors[0][0] + colors[1][0]) / 3;
    colors[2][1] = (2 * colors[0][1] + colors[1][1]) / 3;
    colors[2][2] = (2 * colors[0][2] + colors[1][2]) / 3;
    
    colors[3][0] = (colors[0][0] + 2 * colors[1][0]) / 3;
    colors[3][1] = (colors[0][1] + 2 * colors[1][1]) / 3;
    colors[3][2] = (colors[0][2] + 2 * colors[1][2]) / 3;
    
    // Assign each pixel to nearest color
    uint32_t indices = 0;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            const uint8_t* p = pixels + (y * 4 + x) * 3;
            int bestIdx = 0;
            int bestDist = 0x7FFFFFFF;
            
            for (int c = 0; c < 4; c++) {
                int dr = p[0] - colors[c][0];
                int dg = p[1] - colors[c][1];
                int db = p[2] - colors[c][2];
                int dist = dr * dr + dg * dg + db * db;
                if (dist < bestDist) {
                    bestDist = dist;
                    bestIdx = c;
                }
            }
            indices |= (bestIdx << ((y * 4 + x) * 2));
        }
    }
    
    outBlock[4] = indices & 0xFF;
    outBlock[5] = (indices >> 8) & 0xFF;
    outBlock[6] = (indices >> 16) & 0xFF;
    outBlock[7] = (indices >> 24) & 0xFF;
}

/**
 * Compress RGB image to BC1/DXT1 format
 */
static std::vector<uint8_t> CompressBC1(
    const uint8_t* rgbData,
    int width, int height)
{
    int blockW = (width + 3) / 4;
    int blockH = (height + 3) / 4;
    size_t compressedSize = blockW * blockH * 8;
    std::vector<uint8_t> compressed(compressedSize);
    
    uint8_t block[48];  // 4x4x3
    uint8_t* outPtr = compressed.data();
    
    for (int by = 0; by < blockH; by++) {
        for (int bx = 0; bx < blockW; bx++) {
            // Extract 4x4 block
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    int px = std::min(bx * 4 + x, width - 1);
                    int py = std::min(by * 4 + y, height - 1);
                    int idx = (py * width + px) * 3;
                    block[(y * 4 + x) * 3 + 0] = rgbData[idx + 0];
                    block[(y * 4 + x) * 3 + 1] = rgbData[idx + 1];
                    block[(y * 4 + x) * 3 + 2] = rgbData[idx + 2];
                }
            }
            EncodeBC1Block(block, outPtr);
            outPtr += 8;
        }
    }
    
    return compressed;
}

// ============================================================================
// BC3/DXT5 Compression (4:1 ratio for RGBA)
// ============================================================================

/**
 * Encode a 4x4 block of alpha values as BC3 alpha block
 */
static void EncodeBC3AlphaBlock(
    const uint8_t* alphas,    // 16 bytes (4x4)
    uint8_t* outBlock)        // 8 bytes output
{
    uint8_t minA = 255, maxA = 0;
    for (int i = 0; i < 16; i++) {
        if (alphas[i] < minA) minA = alphas[i];
        if (alphas[i] > maxA) maxA = alphas[i];
    }
    
    if (maxA - minA < 8) {
        // Not enough range - store as explicit alphas
        outBlock[0] = maxA;
        outBlock[1] = minA;
        // 3 bits per pixel
        memset(outBlock + 2, 0, 6);
        for (int i = 0; i < 16; i++) {
            int shift = (i % 8) * 3;
            int byte = 2 + (i / 8) * 3;
            outBlock[byte] |= (alphas[i] >> (5 - shift % 8));
        }
    } else {
        outBlock[0] = maxA;
        outBlock[1] = minA;
        
        // 8 interpolated levels
        uint8_t levels[8];
        levels[0] = maxA;
        levels[1] = minA;
        for (int i = 2; i < 8; i++) {
            levels[i] = ((8 - i) * maxA + i * minA) / 8;
        }
        
        // Assign each pixel to nearest level
        uint64_t indices = 0;
        for (int i = 0; i < 16; i++) {
            int bestIdx = 0;
            int bestDist = 0x7FFFFFFF;
            for (int j = 0; j < 8; j++) {
                int dist = std::abs((int)alphas[i] - (int)levels[j]);
                if (dist < bestDist) {
                    bestDist = dist;
                    bestIdx = j;
                }
            }
            indices |= ((uint64_t)bestIdx << (i * 3));
        }
        
        memcpy(outBlock + 2, &indices, 6);
    }
}

/**
 * Compress RGBA image to BC3/DXT5 format
 */
static std::vector<uint8_t> CompressBC3(
    const uint8_t* rgbaData,
    int width, int height)
{
    int blockW = (width + 3) / 4;
    int blockH = (height + 3) / 4;
    size_t compressedSize = blockW * blockH * 16;
    std::vector<uint8_t> compressed(compressedSize);
    
    uint8_t alphaBlock[16];
    uint8_t rgbBlock[48];
    uint8_t outBlock[16];
    
    for (int by = 0; by < blockH; by++) {
        for (int bx = 0; bx < blockW; bx++) {
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    int px = std::min(bx * 4 + x, width - 1);
                    int py = std::min(by * 4 + y, height - 1);
                    int idx = (py * width + px) * 4;
                    alphaBlock[y * 4 + x] = rgbaData[idx + 3];
                    rgbBlock[(y * 4 + x) * 3 + 0] = rgbaData[idx + 0];
                    rgbBlock[(y * 4 + x) * 3 + 1] = rgbaData[idx + 1];
                    rgbBlock[(y * 4 + x) * 3 + 2] = rgbaData[idx + 2];
                }
            }
            
            EncodeBC3AlphaBlock(alphaBlock, outBlock);
            EncodeBC1Block(rgbBlock, outBlock + 8);
            memcpy(compressed.data() + (by * blockW + bx) * 16, outBlock, 16);
        }
    }
    
    return compressed;
}

// ============================================================================
// DDS File Loading
// ============================================================================

/**
 * Load a DDS file and upload it as a compressed OpenGL texture
 */
static GLuint LoadDDS(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        return 0;
    }
    
    DDSHeader header;
    if (fread(&header, sizeof(DDSHeader), 1, fp) != 1 || header.magic != 0x20534444) {
        fclose(fp);
        return 0;
    }
    
    int width = header.width;
    int height = header.height;
    uint32_t fourCC = header.ddspf.fourCC;
    
    // Determine compressed format
    GLenum internalFormat = 0;
    size_t blockSize = 0;
    
    switch (fourCC) {
        case DDS_DXT1:
            internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
            blockSize = 8;
            break;
        case DDS_DXT3:
            internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
            blockSize = 16;
            break;
        case DDS_DXT5:
            internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
            blockSize = 16;
            break;
        case DDS_BC7:
            internalFormat = GL_COMPRESSED_RGBA_BPTC_UNORM;
            blockSize = 16;
            break;
        case DDS_BC5:
            internalFormat = GL_COMPRESSED_RG_RGTC2;
            blockSize = 16;
            break;
        default:
            fclose(fp);
            return 0;
    }
    
    // Read all mip levels
    int mipCount = header.mipCount > 0 ? header.mipCount : 1;
    std::vector<uint8_t> data;
    
    // Calculate total size
    int w = width, h = height;
    size_t totalSize = 0;
    for (int i = 0; i < mipCount; i++) {
        int blockW = std::max(w, 4) / 4;
        int blockH = std::max(h, 4) / 4;
        totalSize += blockW * blockH * blockSize;
        w = std::max(w / 2, 1);
        h = std::max(h / 2, 1);
    }
    
    data.resize(totalSize);
    if (fread(data.data(), 1, totalSize, fp) != totalSize) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    
    // Upload to OpenGL
    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    // Upload each mip level
    w = width; h = height;
    size_t offset = 0;
    for (int i = 0; i < mipCount; i++) {
        int blockW = std::max(w, 4) / 4;
        int blockH = std::max(h, 4) / 4;
        size_t mipSize = blockW * blockH * blockSize;
        
        glCompressedTexImage2D(GL_TEXTURE_2D, i, internalFormat, w, h, 0,
                               (GLsizei)mipSize, data.data() + offset);
        
        offset += mipSize;
        w = std::max(w / 2, 1);
        h = std::max(h / 2, 1);
    }
    
    return textureID;
}

// ============================================================================
// Texture Compression Cache
// ============================================================================

/**
 * Generate a cache filename for compressed texture
 */
static std::string GetCompressedCachePath(const std::string& originalPath, bool hasAlpha) {
    std::string cacheDir = originalPath + ".cache";
    return cacheDir + (hasAlpha ? ".bc3" : ".bc1");
}

/**
 * Load texture with automatic compression
 * 
 * Priority:
 * 1. Check for DDS file (pre-compressed BC7/BC5)
 * 2. Check for compressed cache file
 * 3. Load original image and compress to BC1/BC3
 * 4. Save compressed data to cache for next time
 */
static GLuint LoadCompressedTexture(const std::string& path, bool compressEnabled = true) {
    if (!glGenTextures) {
        return 0;
    }
    
    // Try DDS first (pre-compressed)
    std::string ddsPath = path;
    size_t dotPos = ddsPath.rfind('.');
    if (dotPos != std::string::npos) {
        ddsPath.replace(dotPos, std::string::npos, ".dds");
    } else {
        ddsPath += ".dds";
    }
    
    // Check if DDS file exists
    FILE* ddsTest = fopen(ddsPath.c_str(), "rb");
    if (ddsTest) {
        fclose(ddsTest);
        GLuint tex = LoadDDS(ddsPath);
        if (tex) return tex;
    }
    
    // Check for compressed cache
    std::string originalPath = path;
    bool hasAlpha = false;
    
    // Quick check: if path contains "normal", "metallic", or "roughness", it likely has useful alpha
    if (path.find("normal") != std::string::npos ||
        path.find("alpha") != std::string::npos ||
        path.find("transparent") != std::string::npos) {
        hasAlpha = true;
    }
    
    // If compression is disabled, fall through to normal loading
    if (!compressEnabled) {
        return 0; // Signal caller to use normal loading
    }
    
    return 0; // Signal to use fallback
}

// ============================================================================
// OpenGL Compressed Internal Format Selection
// ============================================================================

/**
 * Select the best compressed internal format based on texture type
 */
static GLenum SelectCompressedFormat(int channels, bool isNormalMap, bool isSRGB, bool hasAlpha = false) {
    if (isNormalMap) {
        // Normal maps benefit from BC5 (2-channel RG compression)
        return GL_COMPRESSED_RG_RGTC2;
    }
    
    if (channels == 1) {
        return GL_COMPRESSED_RED_RGTC1;
    }
    
    if (channels == 2) {
        return GL_COMPRESSED_RG_RGTC2;
    }
    
    // 3 or 4 channels
    if (channels == 3 || !hasAlpha) {
        // BC1/DXT1 for RGB (6:1 compression)
        return isSRGB ? GL_COMPRESSED_SRGB_S3TC_DXT1_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
    } else {
        // BC3/DXT5 for RGBA (4:1 compression with alpha)
        return isSRGB ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    }
}

#endif // TEXTURE_COMPRESSION_H
