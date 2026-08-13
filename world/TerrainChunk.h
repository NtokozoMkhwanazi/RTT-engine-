#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

// A single vertex of the static terrain grid. Y is displaced in the vertex
// shader by sampling the master heightmap texture (Unreal-style GPU terrain);
// the shader derives the heightmap UV from the world XZ, so position-only.
struct TerrainVertex {
    glm::vec3 position;  // world-space grid position (y unused)
};

// Terrain shader (shared across all chunks, initialized once)
void initTerrainShader();
extern GLuint g_terrainShader;

// Named uniform locations (kept in sync with initTerrainShader below)
enum TerrainUniform {
    TU_VIEW = 0,
    TU_PROJECTION,
    TU_LIGHT_DIR,
    TU_VIEW_POS,
    TU_FOG_COLOR,
    TU_FOG_NEAR,
    TU_FOG_FAR,
    TU_HEIGHT_MAP,      // sampler2D - master heightmap texture
    TU_HEIGHT_MAP_SIZE, // vec2 - heightmap texels per axis (world meters covered)
    TU_LOD_DISTANCE,    // float - base LOD transition distance
    TU_WATER_LEVEL,     // float - water level for auto-material
    // RVT material cache (used by the main pass)
    TU_MATERIAL_ATLAS,  // sampler2D - baked multi-layer material atlas
    TU_ATLAS_PAGE,      // vec2 - this chunk's page origin in atlas UV space
    TU_CHUNK_ORIGIN,    // vec2 - chunk world-space min corner (XZ)
    TU_CHUNK_SIZE,      // float - chunk size in world units
    TU_USE_ATLAS,       // float - 1 = sample baked atlas, 0 = compute layers live
    // Desert rock albedo texture (dry-desert terrain look)
    TU_DESERT_TEX,      // sampler2D - tiling rock/sand albedo
    TU_TEX_SCALE,       // float - world meters per texture tile
    TU_COUNT
};
extern GLint g_terrainUniforms[TU_COUNT];

// Bake program: desert albedo sampler lives on unit 2 (heightmap=0, atlas=1).
extern GLint g_terrainBakeDesertTex;

// ============================================================
// Heightfield sampling (shared by physics + GPU displacement)
// ============================================================
// The heightmap is a continuous surface with 1 texel per world meter
// (texel i sits at world coordinate i). This matches the vertex-shader
// displacement UV ((aPos.xz + 0.5) / uHeightMapSize) sampled with GL_LINEAR
// and the chunk generator's floor(worldCoord) texel lookup, so CPU physics
// and the rendered surface agree exactly. Never reintroduce the old
// worldCoord / size scaling - it collapsed the whole world onto texel 0
// (flat terrain) and desynced physics from rendering.
namespace terrain {

// Map a world-space coordinate (meters) to a heightmap texel index, clamped
// to [0, size-1]. Negative and out-of-range coords clamp to the edge texel.
inline int worldToTexel(float worldCoord, int size) {
    return std::clamp((int)std::floor(worldCoord), 0, size - 1);
}

// Bilinear sample of the master heightfield at a world position (meters).
// Matches the GPU's linearly-filtered texture sample exactly.
inline float sampleHeightBilinear(const float* heights, int size,
                                  float worldX, float worldZ) {
    const int x0 = worldToTexel(worldX, size);
    const int z0 = worldToTexel(worldZ, size);
    const int x1 = std::min(x0 + 1, size - 1);
    const int z1 = std::min(z0 + 1, size - 1);
    const float tx = std::clamp(worldX - (float)x0, 0.0f, 1.0f);
    const float tz = std::clamp(worldZ - (float)z0, 0.0f, 1.0f);
    const float h00 = heights[z0 * size + x0];
    const float h10 = heights[z0 * size + x1];
    const float h01 = heights[z1 * size + x0];
    const float h11 = heights[z1 * size + x1];
    const float top = h00 * (1.0f - tx) + h10 * tx;
    const float bottom = h01 * (1.0f - tx) + h11 * tx;
    return top * (1.0f - tz) + bottom * tz;
}

// Bilinear sample over a chunk-local height grid. localX/localZ are meters
// inside the chunk; the grid step is chunkSize / resolution.
inline float sampleChunkBilinear(const float* heights, int resolution,
                                 float localX, float localZ, float chunkSize) {
    const int vertsPerSide = resolution + 1;
    const float step = chunkSize / (float)resolution;
    const float fx = localX / step;
    const float fz = localZ / step;
    const int x0 = std::clamp((int)std::floor(fx), 0, resolution);
    const int z0 = std::clamp((int)std::floor(fz), 0, resolution);
    const int x1 = std::min(x0 + 1, resolution);
    const int z1 = std::min(z0 + 1, resolution);
    const float tx = std::clamp(fx - (float)x0, 0.0f, 1.0f);
    const float tz = std::clamp(fz - (float)z0, 0.0f, 1.0f);
    const float h00 = heights[z0 * vertsPerSide + x0];
    const float h10 = heights[z0 * vertsPerSide + x1];
    const float h01 = heights[z1 * vertsPerSide + x0];
    const float h11 = heights[z1 * vertsPerSide + x1];
    const float top = h00 * (1.0f - tx) + h10 * tx;
    const float bottom = h01 * (1.0f - tx) + h11 * tx;
    return top * (1.0f - tz) + bottom * tz;
}

} // namespace terrain

// Bake program: renders the multi-layer auto-material into the RVT atlas page
// for a chunk once (shared displacement VS, material-only FS).
extern GLuint g_terrainBakeProgram;
// Bake-program uniform locations (uHeightMap/uHeightMapSize/water shared via
// the same names, resolved per program).
extern GLint g_terrainBakeView, g_terrainBakeProj, g_terrainBakeHeightMap,
            g_terrainBakeHeightMapSize, g_terrainBakeWaterLevel;

// Terrain chunk - one section of the world terrain
class TerrainChunk {
public:
    TerrainChunk(int chunkX, int chunkY, float chunkSize, int resolution);
    ~TerrainChunk();

    // Generate terrain geometry from heightmap
    void generateHeightmap(const std::vector<float>& heightmap, int heightmapSize);

    // Generate terrain geometry from pre-computed height data (for async loading)
    void generateHeightmapFromData(std::vector<float> heights);

    // Update LOD based on distance to camera
    void updateLOD(const glm::vec3& cameraPos, float lodDistance);

    // Render the chunk (no shader setup - call Terrain::renderBatch first)
    void draw() const;

    // Get chunk world position
    glm::vec3 getWorldPosition() const;

    // Get height at local coordinates (bilinear over the chunk's height grid)
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
    void updateBounds();
    void generateIndices(int lod);

    int m_chunkX, m_chunkY;
    float m_chunkSize;
    int m_resolution;

    std::vector<float> m_heights;
    std::vector<TerrainVertex> m_vertices;
    std::vector<unsigned int> m_indices;

    GLuint m_VAO = 0, m_VBO = 0, m_EBO = 0;

    glm::vec3 m_boundsMin;
    glm::vec3 m_boundsMax;

    bool m_loaded = false;
    int m_lod = 0;
    float m_distanceToCamera = 0.0f;
};
