#include "TextureAtlas.h"
#include <iostream>
#include <algorithm>
#include <cstring>

// ============================================================================
// TEXTURE ATLAS IMPLEMENTATION
// ============================================================================

TextureAtlas::TextureAtlas() : m_config() {}

TextureAtlas::TextureAtlas(const AtlasConfig& config) : m_config(config) {}

TextureAtlas::~TextureAtlas() {
    cleanup();
}

bool TextureAtlas::initialize() {
    if (m_atlasTexture != 0) {
        return true;  // Already initialized
    }

    glGenTextures(1, &m_atlasTexture);
    if (m_atlasTexture == 0) {
        std::cerr << "[TextureAtlas] Failed to generate texture!\n";
        return false;
    }

    // Allocate atlas texture
    glBindTexture(GL_TEXTURE_2D, m_atlasTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_config.atlasSize, m_config.atlasSize, 
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    
    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glBindTexture(GL_TEXTURE_2D, 0);

    std::cout << "[TextureAtlas] Initialized: " << m_config.atlasSize << "x" 
              << m_config.atlasSize << ", max " << m_config.maxTextures << " textures\n";

    return true;
}

int TextureAtlas::addTexture(const std::string& path) {
    // For now, just create a placeholder region
    // In full implementation, would load texture and pack into atlas
    
    TextureEntry entry;
    entry.name = path;
    entry.width = m_config.defaultCellSize;
    entry.height = m_config.defaultCellSize;
    entry.regionId = m_regions.size();

    TextureRegion region;
    region.textureId = m_textures.size();
    region.cellSize = m_config.defaultCellSize;
    region.valid = true;
    // UV coordinates will be set during packing

    m_nameToId[path] = m_textures.size();
    m_textures.push_back(entry);
    m_regions.push_back(region);
    m_dirty = true;

    return entry.regionId;
}

int TextureAtlas::addTexture(const std::string& name, GLuint textureId, int width, int height) {
    TextureEntry entry;
    entry.name = name;
    entry.originalTextureId = textureId;
    entry.width = width;
    entry.height = height;
    entry.regionId = m_regions.size();

    TextureRegion region;
    region.textureId = m_textures.size();
    region.cellSize = std::max(width, height);
    region.valid = true;

    m_nameToId[name] = m_textures.size();
    m_textures.push_back(entry);
    m_regions.push_back(region);
    m_dirty = true;

    return entry.regionId;
}

const TextureAtlas::TextureRegion* TextureAtlas::getRegion(int textureId) const {
    if (textureId < 0 || textureId >= (int)m_regions.size()) {
        return nullptr;
    }
    return &m_regions[textureId];
}

const TextureAtlas::TextureRegion* TextureAtlas::getRegion(const std::string& name) const {
    auto it = m_nameToId.find(name);
    if (it == m_nameToId.end()) {
        return nullptr;
    }
    return getRegion(it->second);
}

void TextureAtlas::bind(GLenum textureUnit) const {
    if (m_atlasTexture == 0) return;

    glActiveTexture(textureUnit);
    glBindTexture(GL_TEXTURE_2D, m_atlasTexture);
}

void TextureAtlas::unbind() const {
    glBindTexture(GL_TEXTURE_2D, 0);
}

bool TextureAtlas::packTextures() {
    if (m_textures.empty()) return true;

    // Simple grid packing
    int cellSize = m_config.defaultCellSize;
    int cols = m_config.atlasSize / cellSize;
    int rows = m_config.atlasSize / cellSize;

    int textureIndex = 0;
    for (int row = 0; row < rows && textureIndex < (int)m_textures.size(); row++) {
        for (int col = 0; col < cols && textureIndex < (int)m_textures.size(); col++) {
            TextureRegion& region = m_regions[textureIndex];
            
            region.u = (float)(col * cellSize) / m_config.atlasSize;
            region.v = (float)(row * cellSize) / m_config.atlasSize;
            region.u2 = region.u + (float)cellSize / m_config.atlasSize;
            region.v2 = region.v + (float)cellSize / m_config.atlasSize;
            
            textureIndex++;
        }
    }

    m_dirty = false;
    return true;
}

bool TextureAtlas::uploadToGPU() {
    if (m_atlasTexture == 0 || m_textures.empty()) return false;

    // Create temporary buffer for atlas
    std::vector<unsigned char> atlasData(m_config.atlasSize * m_config.atlasSize * 4);

    // For each texture, copy into atlas
    // In full implementation, would read from original textures
    // For now, just fill with placeholder colors

    int cellSize = m_config.defaultCellSize;
    int cols = m_config.atlasSize / cellSize;

    for (size_t i = 0; i < m_textures.size(); i++) {
        int col = i % cols;
        int row = i / cols;
        int startX = col * cellSize;
        int startY = row * cellSize;

        // Generate placeholder color based on texture index
        unsigned char r = (i * 37) % 256;
        unsigned char g = (i * 53) % 256;
        unsigned char b = (i * 71) % 256;

        // Fill cell with color
        for (int y = 0; y < cellSize && startY + y < m_config.atlasSize; y++) {
            for (int x = 0; x < cellSize && startX + x < m_config.atlasSize; x++) {
                int atlasIdx = ((startY + y) * m_config.atlasSize + (startX + x)) * 4;
                atlasData[atlasIdx + 0] = r;
                atlasData[atlasIdx + 1] = g;
                atlasData[atlasIdx + 2] = b;
                atlasData[atlasIdx + 3] = 255;
            }
        }
    }

    // Upload to GPU
    glBindTexture(GL_TEXTURE_2D, m_atlasTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_config.atlasSize, m_config.atlasSize,
                    GL_RGBA, GL_UNSIGNED_BYTE, atlasData.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

size_t TextureAtlas::getUsedArea() const {
    return m_regions.size() * m_config.defaultCellSize * m_config.defaultCellSize;
}

float TextureAtlas::getUtilization() const {
    float totalArea = m_config.atlasSize * m_config.atlasSize;
    return getUsedArea() / totalArea * 100.0f;
}

void TextureAtlas::cleanup() {
    if (m_atlasTexture != 0) {
        glDeleteTextures(1, &m_atlasTexture);
        m_atlasTexture = 0;
    }
    m_regions.clear();
    m_textures.clear();
    m_nameToId.clear();
}

// ============================================================================
// MATERIAL IMPLEMENTATION
// ============================================================================

Material::Material() {}

Material::Material(const MaterialData& data) : m_data(data) {}

void Material::setAtlasRegion(int regionId) {
    m_data.atlasRegionId = regionId;
}

void Material::setAtlasRegion(const TextureAtlas& atlas, const std::string& textureName) {
    const TextureAtlas::TextureRegion* region = atlas.getRegion(textureName);
    if (region) {
        m_data.atlasRegionId = region->textureId;
    }
}

void Material::setAlbedo(const glm::vec3& color) {
    m_data.albedo = color;
    m_data.atlasRegionId = -1;  // No texture
}

void Material::bind(const TextureAtlas& atlas, GLuint shaderProgram) const {
    // Set material uniforms
    GLint loc = glGetUniformLocation(shaderProgram, "material.albedo");
    if (loc != -1) glUniform3fv(loc, 1, &m_data.albedo[0]);

    loc = glGetUniformLocation(shaderProgram, "material.metallic");
    if (loc != -1) glUniform1f(loc, m_data.metallic);

    loc = glGetUniformLocation(shaderProgram, "material.roughness");
    if (loc != -1) glUniform1f(loc, m_data.roughness);

    // Set texture UVs if using atlas
    if (m_data.atlasRegionId >= 0) {
        const TextureAtlas::TextureRegion* region = atlas.getRegion(m_data.atlasRegionId);
        if (region && region->valid) {
            loc = glGetUniformLocation(shaderProgram, "material.uvOffset");
            if (loc != -1) glUniform2f(loc, region->u, region->v);
            
            loc = glGetUniformLocation(shaderProgram, "material.useTexture");
            if (loc != -1) glUniform1i(loc, 1);
        }
    } else {
        GLint loc = glGetUniformLocation(shaderProgram, "material.useTexture");
        if (loc != -1) glUniform1i(loc, 0);
    }
}

// ============================================================================
// ATLAS MANAGER IMPLEMENTATION
// ============================================================================

AtlasManager& AtlasManager::getInstance() {
    static AtlasManager instance;
    return instance;
}

AtlasManager::AtlasManager() {}

AtlasManager::~AtlasManager() {
    cleanup();
}

TextureAtlas& AtlasManager::getTerrainAtlas() {
    return m_terrainAtlas;
}

TextureAtlas& AtlasManager::getVegetationAtlas() {
    return m_vegetationAtlas;
}

TextureAtlas& AtlasManager::getRockAtlas() {
    return m_rockAtlas;
}

int AtlasManager::addTerrainTexture(const std::string& path) {
    return m_terrainAtlas.addTexture(path);
}

int AtlasManager::addVegetationTexture(const std::string& path) {
    return m_vegetationAtlas.addTexture(path);
}

int AtlasManager::addRockTexture(const std::string& path) {
    return m_rockAtlas.addTexture(path);
}

void AtlasManager::bindAll() {
    m_terrainAtlas.bind(GL_TEXTURE0);
    m_vegetationAtlas.bind(GL_TEXTURE1);
    m_rockAtlas.bind(GL_TEXTURE2);
}

void AtlasManager::cleanup() {
    m_terrainAtlas.cleanup();
    m_vegetationAtlas.cleanup();
    m_rockAtlas.cleanup();
}
