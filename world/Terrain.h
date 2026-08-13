#pragma once
#include "TerrainChunk.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <condition_variable>

struct PendingChunk {
    int chunkX;
    int chunkY;
    std::vector<float> heights;
    int resolution;
    float chunkSize;
};

struct ChunkCoord {
    int x, y;
    bool operator<(const ChunkCoord& o) const {
        if (x != o.x) return x < o.x;
        return y < o.y;
    }
    bool operator==(const ChunkCoord& o) const { return x == o.x && y == o.y; }
};

class Terrain {
public:
    struct TerrainConfig {
        float chunkSize = 80.0f;
        int chunkResolution = 32;
        int viewDistance = 2;
        float lodDistance = 60.0f;
        int heightmapSize = 1024;
        float heightScale = 25.0f;
    };

    Terrain();
    Terrain(const TerrainConfig& config);
    ~Terrain();

    void initialize();
    void update(const glm::vec3& cameraPos, float dt);
    void render(const glm::mat4& view, const glm::mat4& projection,
                const glm::vec3& cameraPos, float fovDegrees = 45.0f,
                float aspectRatio = 16.0f/9.0f, float nearPlane = 0.1f,
                float farPlane = 1000.0f) const;

    float getHeightAt(float worldX, float worldZ) const;
    glm::vec3 getNormalAt(float worldX, float worldZ) const;
    bool isLoadedAt(float worldX, float worldZ) const;
    const TerrainConfig& getConfig() const { return m_config; }
    size_t getActiveChunkCount() const { return m_chunks.size(); }

    // Master heightmap texture (GPU displacement source).
    GLuint getHeightTexture() const { return m_heightTexture; }

    // RVT-style material cache: multi-layer auto-material baked into an atlas
    // once per chunk, then sampled per-pixel (no per-frame layer blending).
    static constexpr int kMaterialAtlasSize = 4096;    // virtual texture size
    static constexpr int kMaterialPageSize = 128;      // page (tile) size
    static constexpr int kMaterialPagesPerSide = kMaterialAtlasSize / kMaterialPageSize;
    GLuint getMaterialAtlas() const { return m_materialAtlas; }
    int getMaterialPageCount() const { return kMaterialPagesPerSide * kMaterialPagesPerSide; }

    // Bake a chunk's auto-material into its atlas page (main thread, GL alive).
    // Returns the page index, or -1 if baking is unavailable.
    int bakeChunkMaterial(const TerrainChunk& chunk);

    // FBO the material atlas is rendered into (page readback / tests).
    GLuint getBakeFramebuffer() const { return m_bakeFBO; }

private:
    void generateHeightmap();
    void getChunkCoords(float worldX, float worldZ, int& chunkX, int& chunkY) const;

    void workerThread();
    PendingChunk generateChunkData(int chunkX, int chunkY);
    void commitChunks();

    TerrainConfig m_config;
    std::vector<float> m_heightmap;
    std::map<ChunkCoord, std::unique_ptr<TerrainChunk>> m_chunks;

    GLuint m_heightTexture = 0;   // GL_R32F master heightmap (GPU displacement)
    GLuint m_desertTex = 0;       // tiling rock albedo (dry-desert material)

    // Material atlas + bake FBO (RVT).
    GLuint m_materialAtlas = 0;
    GLuint m_bakeFBO = 0;
    int m_nextFreePage = 0;
    std::map<ChunkCoord, int> m_chunkPage;   // chunk -> atlas page

    ChunkCoord m_lastChunk{-9999, -9999};
    bool m_initialized = false;

    std::thread m_workerThread;
    std::atomic<bool> m_running{false};
    std::queue<ChunkCoord> m_pendingRequests;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCV;

    std::queue<PendingChunk> m_completedChunks;
    std::mutex m_completedMutex;

    std::set<ChunkCoord> m_knownChunks;
    std::mutex m_knownMutex;
};
