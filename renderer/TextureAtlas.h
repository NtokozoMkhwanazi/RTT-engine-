#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>

// Texture Atlas - combines multiple textures into one for efficient rendering
class TextureAtlas {
public:
    struct AtlasConfig {
        int atlasSize = 4096;          // Atlas texture size (power of 2)
        int maxTextures = 64;          // Maximum textures in atlas
        int defaultCellSize = 256;     // Default cell size
    };

    struct TextureRegion {
        int textureId{-1};             // Original texture ID
        float u{0}, v{0};              // UV offset in atlas
        float u2{1}, v2{1};            // UV end in atlas
        int cellSize{256};             // Size of this region
        bool valid{false};             // Is this region valid
    };

    TextureAtlas();
    TextureAtlas(const AtlasConfig& config);
    ~TextureAtlas();

    // Initialize atlas texture
    bool initialize();

    // Add texture to atlas (returns region UVs)
    int addTexture(const std::string& path);
    int addTexture(const std::string& name, GLuint textureId, int width, int height);

    // Get texture region by ID or name
    const TextureRegion* getRegion(int textureId) const;
    const TextureRegion* getRegion(const std::string& name) const;

    // Bind atlas texture
    void bind(GLenum textureUnit = GL_TEXTURE0) const;
    void unbind() const;

    // Get atlas texture ID
    GLuint getAtlasTextureId() const { return m_atlasTexture; }

    // Get statistics
    size_t getTextureCount() const { return m_regions.size(); }
    size_t getUsedArea() const;
    float getUtilization() const;

    // Cleanup
    void cleanup();

private:
    struct TextureEntry {
        std::string name;
        GLuint originalTextureId{0};
        int width{0}, height{0};
        int regionId{-1};
    };

    // Pack textures into atlas (simple grid packing)
    bool packTextures();

    // Upload packed textures to GPU
    bool uploadToGPU();

    AtlasConfig m_config;
    GLuint m_atlasTexture{0};
    std::vector<TextureRegion> m_regions;
    std::vector<TextureEntry> m_textures;
    std::unordered_map<std::string, int> m_nameToId;
    bool m_dirty{true};  // Needs repacking
};

// ============================================================================
// MATERIAL SYSTEM WITH ATLAS SUPPORT
// ============================================================================

class Material {
public:
    struct MaterialData {
        glm::vec3 albedo{1.0f};
        float metallic{0.0f};
        float roughness{1.0f};
        float ao{1.0f};
        int atlasRegionId{-1};  // -1 = use solid color
    };

    Material();
    Material(const MaterialData& data);

    // Set texture from atlas
    void setAtlasRegion(int regionId);
    void setAtlasRegion(const TextureAtlas& atlas, const std::string& textureName);

    // Set solid color (no texture)
    void setAlbedo(const glm::vec3& color);

    // Bind material for rendering
    void bind(const TextureAtlas& atlas, GLuint shaderProgram) const;

    const MaterialData& getData() const { return m_data; }

private:
    MaterialData m_data;
};

// ============================================================================
// ATLAS MANAGER - Global texture atlas management
// ============================================================================

class AtlasManager {
public:
    static AtlasManager& getInstance();

    // Get global terrain atlas
    TextureAtlas& getTerrainAtlas();

    // Get global vegetation atlas
    TextureAtlas& getVegetationAtlas();

    // Get global rock atlas
    TextureAtlas& getRockAtlas();

    // Add texture to appropriate atlas
    int addTerrainTexture(const std::string& path);
    int addVegetationTexture(const std::string& path);
    int addRockTexture(const std::string& path);

    // Bind all atlases
    void bindAll();

    // Cleanup
    void cleanup();

private:
    AtlasManager();
    ~AtlasManager();
    AtlasManager(const AtlasManager&) = delete;
    AtlasManager& operator=(const AtlasManager&) = delete;

    TextureAtlas m_terrainAtlas;
    TextureAtlas m_vegetationAtlas;
    TextureAtlas m_rockAtlas;
    bool m_initialized{false};
};
