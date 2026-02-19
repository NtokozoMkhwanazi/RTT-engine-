#pragma once
#include "TerrainChunk.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <map>

// Terrain system - manages all chunks and streaming
class Terrain {
public:
    struct TerrainConfig {
        float chunkSize = 100.0f;          // Size of each chunk in world units
        int chunkResolution = 64;           // Vertices per chunk side
        int viewDistance = 4;               // How many chunks to load in each direction
        float lodDistance = 50.0f;          // Distance for LOD transitions
        int heightmapSize = 1024;           // Size of heightmap texture
        float heightScale = 50.0f;          // Maximum terrain height
    };

    Terrain();
    Terrain(const TerrainConfig& config);
    ~Terrain();

    // Initialize terrain (generate or load heightmap)
    void initialize();
    
    // Update terrain (stream chunks based on camera position)
    void update(const glm::vec3& cameraPos, float dt);
    
    // Render all visible chunks
    void render() const;
    
    // Get height at world position (for collision)
    float getHeightAt(float worldX, float worldZ) const;
    
    // Get normal at world position (for physics)
    glm::vec3 getNormalAt(float worldX, float worldZ) const;
    
    // Check if position is loaded
    bool isLoadedAt(float worldX, float worldZ) const;
    
    // Get configuration
    const TerrainConfig& getConfig() const { return m_config; }
    
    // Get number of active chunks
    size_t getActiveChunkCount() const { return m_chunks.size(); }

private:
    // Generate procedural heightmap (Perlin noise)
    void generateHeightmap();
    
    // Get chunk key for map lookup
    int getChunkKey(int chunkX, int chunkY) const;
    
    // Get or create chunk
    TerrainChunk* getOrCreateChunk(int chunkX, int chunkY);
    
    // Remove chunk
    void removeChunk(int chunkX, int chunkY);
    
    // Get chunk coordinates from world position
    void getChunkCoords(float worldX, float worldZ, int& chunkX, int& chunkY) const;

    TerrainConfig m_config;
    std::vector<float> m_heightmap;        // Global heightmap data
    std::map<int, std::unique_ptr<TerrainChunk>> m_chunks;  // Active chunks
    
    int m_lastChunkX = 0, m_lastChunkY = 0;  // Last center chunk
    bool m_initialized = false;
};
