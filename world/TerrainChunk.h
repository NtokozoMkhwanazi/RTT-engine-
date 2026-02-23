#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>

// Terrain chunk - one section of the world terrain
class TerrainChunk {
public:
    TerrainChunk(int chunkX, int chunkY, float chunkSize, int resolution);
    ~TerrainChunk();

    // Generate terrain geometry from heightmap
    void generateHeightmap(const std::vector<float>& heightmap, int heightmapSize);

    // Update LOD based on distance to camera
    void updateLOD(const glm::vec3& cameraPos, float lodDistance);

    // Render the chunk
    void render() const;

    // Get chunk world position
    glm::vec3 getWorldPosition() const;

    // Get height at local coordinates
    float getHeightAt(float localX, float localZ) const;

    // Check if chunk is loaded
    bool isLoaded() const { return m_loaded; }

    // Check if chunk is visible in frustum
    bool isVisibleInFrustum(const glm::vec3& cameraPos, float fovDegrees, 
                           float aspectRatio, float nearPlane, float farPlane) const;

    // Check if chunk is potentially visible (simple height-based occlusion)
    bool isPotentiallyVisible(const glm::vec3& cameraPos, const TerrainChunk* otherChunk) const;

    // Get chunk coordinates
    int getChunkX() const { return m_chunkX; }
    int getChunkY() const { return m_chunkY; }

    // Get distance to camera
    float getDistanceToCamera(const glm::vec3& cameraPos) const;

    // Get bounding box for culling
    const glm::vec3& getBoundsMin() const { return m_boundsMin; }
    const glm::vec3& getBoundsMax() const { return m_boundsMax; }

    // LOD level
    int getLOD() const { return m_lod; }

private:
    void createMesh();
    void updateMesh();
    void updateBounds();
    void generateIndices(int lod);  // Generate indices with LOD support

    int m_chunkX, m_chunkY;           // Chunk coordinates in world
    float m_chunkSize;                 // Size of chunk in world units
    int m_resolution;                  // Vertices per side (before LOD)

    std::vector<float> m_heights;      // Height values for this chunk
    std::vector<glm::vec3> m_vertices; // Generated vertices
    std::vector<unsigned int> m_indices; // Indices for rendering

    GLuint m_VAO = 0, m_VBO = 0, m_EBO = 0;

    // Bounding box for culling
    glm::vec3 m_boundsMin;
    glm::vec3 m_boundsMax;

    bool m_loaded = false;
    int m_lod = 0;                     // Current LOD level (0 = highest)
    float m_distanceToCamera = 0.0f;
};
