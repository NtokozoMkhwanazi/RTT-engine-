#pragma once

// forUI.txt: Asynchronous Texture Compressed Profiling.
// Tracks raw VRAM footprints of staged GL textures (block-compressed + linear)
// so the editor ImGui dashboard can surface per-file memory costs and a 512 MB
// budget bar. Registration is thread-safe (called from loader + polling tasks).
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstddef>
#include <glad/glad.h>

class TextureProfiler {
public:
    struct TextureMetadata {
        std::string filePath;
        size_t memorySizeInBytes = 0;
        int width = 0;
        int height = 0;
        std::string glFormatString = "Unknown";
    };
private:
    std::unordered_map<unsigned int, TextureMetadata> m_activeTextures;
    std::mutex m_profileMutex;
    std::atomic<size_t> m_totalVRAMAllocatedBytes{0};

    TextureProfiler() = default;
public:
    static TextureProfiler& Instance() {
        static TextureProfiler instance;
        return instance;
    }

    // Calculates raw block-compression memory footprints before allocating GL handles.
    size_t CalculateFootprint(int width, int height, int glInternalFormat) {
        size_t pixels = static_cast<size_t>(width) * height;
        switch (glInternalFormat) {
            case 0x8C43: // GL_SRGB8_ALPHA8
            case GL_RGBA8:
                return pixels * 4;
            case GL_RGB8:
            case 0x8C41: // GL_SRGB8
                return pixels * 3;
            case GL_RGBA16F:
                return pixels * 8; // Half-float channels
            case GL_R8:
                return pixels * 1;
            case 0x83F1: // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                return ((width + 3) / 4) * ((height + 3) / 4) * 8;
            case 0x83F3: // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
                return ((width + 3) / 4) * ((height + 3) / 4) * 16;
            default:
                return pixels * 4; // worst-case linear assumption
        }
    }

    void RegisterTexture(unsigned int glTextureID, const std::string& path,
                         int width, int height, int internalFormat) {
        std::lock_guard<std::mutex> lock(m_profileMutex);
        if (m_activeTextures.count(glTextureID)) return;

        TextureMetadata meta;
        meta.filePath = path;
        meta.width = width;
        meta.height = height;
        meta.memorySizeInBytes = CalculateFootprint(width, height, internalFormat);

        if (internalFormat == GL_RGBA16F) meta.glFormatString = "RGBA16F (HDR Linear)";
        else if (internalFormat == 0x8C41 || internalFormat == 0x8C43) meta.glFormatString = "sRGB Compressed (PBR Albedo)";
        else if (internalFormat == 0x83F3) meta.glFormatString = "BC3 / DXT5 (Compressed Normal/ARM)";
        else meta.glFormatString = "Linear R8/RGB8 Channel";

        m_activeTextures[glTextureID] = meta;
        m_totalVRAMAllocatedBytes.fetch_add(meta.memorySizeInBytes);
    }

    void UnregisterTexture(unsigned int glTextureID) {
        std::lock_guard<std::mutex> lock(m_profileMutex);
        auto it = m_activeTextures.find(glTextureID);
        if (it != m_activeTextures.end()) {
            m_totalVRAMAllocatedBytes.fetch_sub(it->second.memorySizeInBytes);
            m_activeTextures.erase(it);
        }
    }

    size_t GetTotalVRAMAllocated() const { return m_totalVRAMAllocatedBytes.load(); }

    std::unordered_map<unsigned int, TextureMetadata> GetTextureSnapshot() {
        std::lock_guard<std::mutex> lock(m_profileMutex);
        return m_activeTextures;
    }
};
