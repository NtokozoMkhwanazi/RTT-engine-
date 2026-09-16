#pragma once
#include "TerrainChunk.h"
#include "shaderSystem/terrain_splat.h"
#include "lighting/LightingEnvironment.h"  // #3: thread env, not Instance()
#include <glm/glm.hpp>
#include <vector>
#include <array>
#include <memory>
#include <string>
#include <map>
#include <set>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <condition_variable>

// Forward declaration only — the full definition lives in renderer/ShadowMapper.h
// (a lower-level module). A pointer is all the render call-site needs.
class ShadowMapper;

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
        float chunkSize = 200.0f;
        int chunkResolution = 1080;
        int viewDistance = 4;
        float lodDistance = 120.0f;
        int heightmapSize = 2040;
        float heightScale = 100.0f;
    };

    Terrain();
    Terrain(const TerrainConfig& config);
    ~Terrain();

    void initialize();
    void update(const glm::vec3& cameraPos, float dt);
    void render(const glm::mat4& view, const glm::mat4& projection,
                const glm::vec3& cameraPos, float fovDegrees = 45.0f,
                float aspectRatio = 16.0f/9.0f, float nearPlane = 0.1f,
                float farPlane = 1000.0f,
                ShadowMapper* shadow = nullptr,
                const LightingEnvironment& lighting =
                    LightingEnvironment::Instance()) const;

    // Deferred path: render terrain into the G-Buffer (MRT output).
    // Uses the same frustum culling + LOD as the forward path, but
    // swaps to the G-Buffer shader so the deferred lighting pass
    // picks up terrain alongside opaque meshes.
    void renderGBuffer(const glm::mat4& view, const glm::mat4& projection,
                       const glm::vec3& cameraPos, float fovDegrees = 45.0f,
                       float aspectRatio = 16.0f/9.0f, float nearPlane = 0.1f,
                       float farPlane = 1000.0f) const;

    float getHeightAt(float worldX, float worldZ) const;
    glm::vec3 getNormalAt(float worldX, float worldZ) const;
    bool isLoadedAt(float worldX, float worldZ) const;
    const TerrainConfig& getConfig() const { return m_config; }
    size_t getActiveChunkCount() const { return m_chunks.size(); }

    // Opt-in authored heightmap (e.g. a grayscale displacement PNG/EXR) that
    // feeds the GL_R32F master heightmap driving the terrain_splat VERTEX
    // displacement. Empty by default -> the procedural Perlin heightmap
    // (generateHeightmap) is used, so Terrain tests / existing behaviour are
    // unchanged. Set before initialize() to override the procedural surface.
    void setHeightmapSource(const std::string& path) { m_heightmapAssetPath = path; }
    const std::string& heightmapSource() const { return m_heightmapAssetPath; }

    // Opt-in: render terrain through the modern height-based splat-map shader
    // (shaderSystem/terrain_splat.*) instead of the legacy embedded RVT shader.
    // Off by default → the existing rendering path is untouched.
    void setUseSplatShader(bool enable) { m_useSplatShader = enable; }
    bool useSplatShader() const { return m_useSplatShader; }

    // Frustum culling toggle — when disabled, ALL terrain chunks are sent to
    // the GPU (no view-frustum rejection). Useful for debugging LOD and for
    // scenes where the camera is inside a small enclosed space.
    void setFrustumCulling(bool enabled) { m_frustumCulling = enabled; }
    bool isFrustumCullingEnabled() const { return m_frustumCulling; }

    // Master heightmap texture (GPU displacement source).
    GLuint getHeightTexture() const { return m_heightTexture; }

    // RVT-style material cache: multi-layer auto-material baked into an atlas
    // once per chunk, then sampled per-pixel (no per-frame layer blending).
    static constexpr int kMaterialAtlasSize = 4096;    // virtual texture size
    static constexpr int kMaterialPageSize = 128;      // page (tile) size
    static constexpr int kMaterialPagesPerSide = kMaterialAtlasSize / kMaterialPageSize;
    GLuint getMaterialAtlas() const { return m_materialAtlas; }
    int getMaterialPageCount() const { return kMaterialPagesPerSide * kMaterialPagesPerSide; }

    // Packed PBR atlas (RGBA8): world normal (rgb = n*0.5+0.5) + roughness
    // (a). Baked in parallel with the albedo atlas - attachment 1 of the
    // bake FBO - so the main pass gets real micro-detail per pixel.
    GLuint getMaterialPbrAtlas() const { return m_materialPbrAtlas; }

    // Bake a chunk's auto-material into its atlas page (main thread, GL alive).
    // Returns the page index, or -1 if baking is unavailable.
    int bakeChunkMaterial(const TerrainChunk& chunk);

    // FBO the material atlas is rendered into (page readback / tests).
    GLuint getBakeFramebuffer() const { return m_bakeFBO; }

private:
    void generateHeightmap();
    void loadHeightmapFromFile(const std::string& path);
    void getChunkCoords(float worldX, float worldZ, int& chunkX, int& chunkY) const;

    // Opt-in height-based splat render path (see shaderSystem/terrain_splat.*).
    void renderSplat(const glm::mat4& view, const glm::mat4& projection,
                     const glm::vec3& cameraPos, ShadowMapper* shadow,
                     const LightingEnvironment& lighting) const;

    void workerThread();
    PendingChunk generateChunkData(int chunkX, int chunkY);
    void commitChunks();

    TerrainConfig m_config;
    std::vector<float> m_heightmap;
    // Empty -> procedural Perlin heightmap (default). Set via setHeightmapSource()
    // to drive the GL_R32F vertex-displacement terrain from an authored asset.
    std::string m_heightmapAssetPath;
    std::map<ChunkCoord, std::unique_ptr<TerrainChunk>> m_chunks;

    GLuint m_heightTexture = 0;   // GL_R32F master heightmap (GPU displacement)
    GLuint m_desertTex = 0;       // tiling rock albedo (dry-desert material)
    GLuint m_desertNormalTex = 0; // PBR normal map (EXR, half float)
    GLuint m_desertRoughTex = 0;  // PBR roughness map (EXR, half float)

    // ---- True-PBR per-layer texture sets for the splat shader (splat path) --
    // Loaded alongside the legacy desert set (which the legacy RVT/bake path
    // still uses). Each layer maps to an elevation band; layers whose textures
    // failed to load (or that have no asset, e.g. snow peaks) fall back to
    // m_whiteTexture so the per-layer tint carries the colour. See
    // TerrainChunk.cpp for the asset paths and terrain_splat.h for the unit map.
    struct SplatLayerTextures {
        GLuint albedo = 0;   // GL_SRGB8 (sRGB albedo JPG)
        GLuint normal = 0;   // GL_RGBA16F (EXR world/GL normal)
        GLuint rough  = 0;   // GL_R8 (JPG) or GL_RGBA16F (EXR)
        GLuint ao     = 0;   // GL_R8 (JPG), or 0 (proxied from rough)
    };
    std::array<SplatLayerTextures, 4> m_splatLayers;
    GLuint m_whiteTexture = 0;        // 1x1 white fallback for tint-only / missing maps
    GLuint m_flatNormalTexture = 0;   // 1x1 flat-up (0.5,0.5,1.0) normal fallback

    // Material atlas + bake FBO (RVT). m_materialPbrAtlas packs the baked
    // world normal (rgb) + roughness (a) as COLOR_ATTACHMENT1 of m_bakeFBO.
    GLuint m_materialAtlas = 0;
    GLuint m_materialPbrAtlas = 0;
    GLuint m_bakeFBO = 0;
    int m_nextFreePage = 0;
    std::map<ChunkCoord, int> m_chunkPage;   // chunk -> atlas page

    ChunkCoord m_lastChunk{-9999, -9999};
    bool m_initialized = false;
    bool m_useSplatShader = true;  // height-based splat path active by default (true PBR)
    bool m_frustumCulling = true;  // toggle from the editor UI (World Settings)

    // LOD evaluation throttle: distances are only re-measured (and index
    // buffers only re-uploaded on a level change) when the camera has moved
    // meaningfully or a refresh interval elapsed - standing still must not
    // churn per-chunk LOD math + GL every frame.
    glm::vec3 m_lodEvalPos{0.0f};
    float m_lodEvalTimer = 0.0f;

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
