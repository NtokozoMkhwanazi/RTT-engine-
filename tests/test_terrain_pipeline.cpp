// ============================================================================
//  Terrain GPU Pipeline Tests
// ============================================================================
//  Locks in the behavior of the Unreal-style terrain overhaul:
//    - 1-texel-per-meter heightmap mapping (regression: the old worldX/size
//      scaling collapsed the whole world onto texel 0, rendering flat terrain
//      and desyncing physics from rendering)
//    - bilinear heightfield sampling (physics matches the rendered surface)
//    - GPU heightmap texture + RVT material atlas creation and page baking
//    - material page recycling eviction
//
//  CPU tests run everywhere (test runner + engine self-check). GL tests create
//  their own hidden context and SKIP when none is available.
// ============================================================================

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <vector>
#include <algorithm>
#include <cmath>
#include <map>

#include "world/Terrain.h"   // Terrain + TerrainChunk + terrain:: heightfield helpers

// Compile-time contract: the static grid is position-only (12 bytes); the UV
// is derived from world position inside the vertex shader.
static_assert(sizeof(TerrainVertex) == 12,
              "TerrainVertex must be position-only (shader derives the UV)");

// ============================================================================
//  CPU heightfield tests (no GL required) - exercise the REAL functions the
//  physics system and the GPU displacement shader both use.
// ============================================================================
class TerrainHeightfieldTest : public ::testing::Test {};

// --- Heightmap texel mapping -------------------------------------------------

TEST_F(TerrainHeightfieldTest, WorldToTexel_OneTexelPerMeter) {
    // Regression: the old mapping (worldCoord / heightmapSize) turned every
    // world position into texel 0, flattening the entire terrain. Texel i must
    // sit at world coordinate i.
    EXPECT_EQ(terrain::worldToTexel(0.0f, 1024), 0);
    EXPECT_EQ(terrain::worldToTexel(1.0f, 1024), 1);
    EXPECT_EQ(terrain::worldToTexel(500.0f, 1024), 500);
    EXPECT_EQ(terrain::worldToTexel(1023.9f, 1024), 1023);
    // Out-of-range clamps to the edge texel (never out of bounds).
    EXPECT_EQ(terrain::worldToTexel(5000.0f, 1024), 1023);
    EXPECT_EQ(terrain::worldToTexel(-3.0f, 1024), 0);
}

TEST_F(TerrainHeightfieldTest, SampleBilinear_SpikeLocalization) {
    // A spike at texel 500 must be found at world (500, y) - NOT at (0, y).
    // The old worldX/size mapping returned 100 at (0,0) and 0 at (500,500).
    const int size = 1024;
    std::vector<float> h(size * size, 0.0f);
    h[500 * size + 500] = 100.0f;

    EXPECT_NEAR(terrain::sampleHeightBilinear(h.data(), size, 500.0f, 500.0f), 100.0f, 1e-3);
    EXPECT_NEAR(terrain::sampleHeightBilinear(h.data(), size, 0.0f, 0.0f), 0.0f, 1e-3);
    EXPECT_NEAR(terrain::sampleHeightBilinear(h.data(), size, 499.5f, 500.0f), 50.0f, 1e-3);
    EXPECT_NEAR(terrain::sampleHeightBilinear(h.data(), size, 500.25f, 500.0f), 75.0f, 1e-3);
}

// --- Bilinear interpolation ----------------------------------------------------

TEST_F(TerrainHeightfieldTest, SampleBilinear_ExactOnRamp) {
    // Bilinear sampling of a linear ramp is exact everywhere (continuous
    // surface - not blocky nearest-neighbor grid steps).
    const int size = 64;
    std::vector<float> h(size * size);
    for (int z = 0; z < size; ++z)
        for (int x = 0; x < size; ++x)
            h[z * size + x] = (float)x;

    EXPECT_NEAR(terrain::sampleHeightBilinear(h.data(), size, 0.0f, 5.0f), 0.0f, 1e-4);
    EXPECT_NEAR(terrain::sampleHeightBilinear(h.data(), size, 2.5f, 5.0f), 2.5f, 1e-4);
    EXPECT_NEAR(terrain::sampleHeightBilinear(h.data(), size, 31.25f, 3.5f), 31.25f, 1e-4);
    EXPECT_NEAR(terrain::sampleHeightBilinear(h.data(), size, 63.0f, 3.5f), 63.0f, 1e-4);
}

TEST_F(TerrainHeightfieldTest, SampleBilinear_ContinuousAcrossTexels) {
    // A step function in the heightmap must be smoothed by interpolation: the
    // midpoint of the step is the average, not a hard jump.
    const int size = 64;
    std::vector<float> h(size * size, 10.0f);
    for (int x = 0; x < 10; ++x) h[x] = 0.0f;

    EXPECT_NEAR(terrain::sampleHeightBilinear(h.data(), size, 9.5f, 0.0f), 5.0f, 1e-3);
    EXPECT_NEAR(terrain::sampleHeightBilinear(h.data(), size, 9.25f, 0.0f), 2.5f, 1e-3);
    EXPECT_NEAR(terrain::sampleHeightBilinear(h.data(), size, 10.0f, 0.0f), 10.0f, 1e-3);
}

TEST_F(TerrainHeightfieldTest, SampleBilinear_ClampsAtWorldEdges) {
    const int size = 16;
    std::vector<float> h(size * size, 7.0f);
    // Negative and out-of-range coords clamp to the edge texels, never OOB.
    EXPECT_NEAR(terrain::sampleHeightBilinear(h.data(), size, -500.0f, 3.0f), 7.0f, 1e-4);
    EXPECT_NEAR(terrain::sampleHeightBilinear(h.data(), size, 9999.0f, -2.0f), 7.0f, 1e-4);
}

// --- Chunk-local sampling -------------------------------------------------------

TEST_F(TerrainHeightfieldTest, SampleChunkBilinear_ExactOnRamp) {
    const int resolution = 16;
    const float chunkSize = 64.0f;   // grid step = 64/16 = 4m
    std::vector<float> h((resolution + 1) * (resolution + 1));
    for (int z = 0; z <= resolution; ++z)
        for (int x = 0; x <= resolution; ++x)
            h[z * (resolution + 1) + x] = x * 2.0f;   // 0..32 across the chunk

    EXPECT_NEAR(terrain::sampleChunkBilinear(h.data(), resolution, 0.0f, 8.0f, chunkSize), 0.0f, 1e-4);
    EXPECT_NEAR(terrain::sampleChunkBilinear(h.data(), resolution, 2.0f, 8.0f, chunkSize), 1.0f, 1e-4);
    EXPECT_NEAR(terrain::sampleChunkBilinear(h.data(), resolution, 10.0f, 8.0f, chunkSize), 5.0f, 1e-4);
    EXPECT_NEAR(terrain::sampleChunkBilinear(h.data(), resolution, 62.0f, 8.0f, chunkSize), 31.0f, 1e-4);
}

TEST_F(TerrainHeightfieldTest, SampleChunkBilinear_ClampsBeyondChunk) {
    const int resolution = 16;
    const float chunkSize = 64.0f;
    std::vector<float> h((resolution + 1) * (resolution + 1));
    for (int z = 0; z <= resolution; ++z)
        for (int x = 0; x <= resolution; ++x)
            h[z * (resolution + 1) + x] = x * 2.0f;

    // Beyond the chunk: clamps to the edge height instead of reading OOB.
    EXPECT_NEAR(terrain::sampleChunkBilinear(h.data(), resolution, 100.0f, 8.0f, chunkSize), 32.0f, 1e-3);
    EXPECT_NEAR(terrain::sampleChunkBilinear(h.data(), resolution, -20.0f, 8.0f, chunkSize), 0.0f, 1e-3);
}

// --- TerrainChunk / Terrain surface API ------------------------------------------

TEST_F(TerrainHeightfieldTest, EmptyChunk_HeightQueryIsZero) {
    TerrainChunk chunk(0, 0, 80.0f, 32);   // pure construction - no GL touched
    EXPECT_FALSE(chunk.isLoaded());
    EXPECT_EQ(chunk.getHeightAt(5.0f, 5.0f), 0.0f);
}

TEST_F(TerrainHeightfieldTest, HeightQueryBeforeInit_ReturnsZero) {
    Terrain terrain;                       // never initialized - no GL
    EXPECT_EQ(terrain.getHeightAt(10.0f, 10.0f), 0.0f);
    EXPECT_EQ(terrain.getNormalAt(0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}

TEST_F(TerrainHeightfieldTest, ChunkCoord_OrdersLexicographically) {
    std::map<ChunkCoord, int> m;
    m[{1, 0}] = 1;
    m[{0, 1}] = 2;
    m[{-1, 5}] = 3;
    EXPECT_EQ(m.at({1, 0}), 1);
    EXPECT_EQ(m.at({0, 1}), 2);
    EXPECT_EQ(m.at({-1, 5}), 3);
    EXPECT_TRUE((ChunkCoord{0, 5} < ChunkCoord{1, 0}));   // x is primary
    EXPECT_TRUE((ChunkCoord{1, 0} < ChunkCoord{1, 1}));
    EXPECT_FALSE((ChunkCoord{2, 2} < ChunkCoord{2, 2}));
}

TEST_F(TerrainHeightfieldTest, TerrainConfigAndPageConstants) {
    Terrain::TerrainConfig cfg;
    EXPECT_EQ(cfg.chunkSize, 80.0f);
    EXPECT_EQ(cfg.chunkResolution, 32);
    EXPECT_EQ(cfg.viewDistance, 2);
    EXPECT_EQ(cfg.heightmapSize, 1024);
    EXPECT_EQ(cfg.heightScale, 25.0f);

    // RVT material atlas: 4096x4096 RGBA8, 128px pages -> 32x32 = 1024 pages.
    EXPECT_EQ(Terrain::kMaterialAtlasSize, 4096);
    EXPECT_EQ(Terrain::kMaterialPageSize, 128);
    EXPECT_EQ(Terrain::kMaterialPagesPerSide, 32);
    Terrain terrain;
    EXPECT_EQ(terrain.getMaterialPageCount(), 1024);
    // Resources are 0 until initialize() runs (GL).
    EXPECT_EQ(terrain.getHeightTexture(), 0u);
    EXPECT_EQ(terrain.getMaterialAtlas(), 0u);
    EXPECT_EQ(terrain.getBakeFramebuffer(), 0u);
}

// ============================================================================
//  GL pipeline tests - create a hidden context once per process; SKIP cleanly
//  when no OpenGL context is available (headless CI boxes).
// ============================================================================
class TerrainPipelineGLTest : public ::testing::Test {
protected:
    static bool ensureGL() {
        static const bool ok = []() {
            if (!glfwInit()) return false;
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            s_window = glfwCreateWindow(64, 64, "terrain-pipeline-test", nullptr, nullptr);
            if (!s_window) { glfwTerminate(); return false; }
            glfwMakeContextCurrent(s_window);
            if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
                glfwDestroyWindow(s_window); s_window = nullptr;
                glfwTerminate(); return false;
            }
            return true;
        }();
        return ok;
    }

    void SetUp() override {
        if (!ensureGL()) GTEST_SKIP() << "No OpenGL context available";
    }

    void TearDown() override {
        // Program handles are context-local. Reset (and free) the shared terrain
        // shader globals so the engine (or the next test) recompiles them in its
        // own context instead of reusing handles that are invalid there.
        if (g_terrainShader != 0) { glDeleteProgram(g_terrainShader); g_terrainShader = 0; }
        if (g_terrainBakeProgram != 0) { glDeleteProgram(g_terrainBakeProgram); g_terrainBakeProgram = 0; }
    }

    // Small config keeps tests fast while exercising the real pipeline.
    static Terrain::TerrainConfig smallConfig() {
        Terrain::TerrainConfig cfg;
        cfg.heightmapSize = 256;
        cfg.chunkResolution = 16;
        cfg.chunkSize = 64.0f;
        cfg.viewDistance = 1;
        cfg.lodDistance = 30.0f;
        return cfg;
    }

    static GLFWwindow* s_window;
};
GLFWwindow* TerrainPipelineGLTest::s_window = nullptr;

TEST_F(TerrainPipelineGLTest, ShadersCompile_BothPrograms) {
    initTerrainShader();
    EXPECT_NE(g_terrainShader, 0u) << "Main terrain shader must compile + link";
    EXPECT_NE(g_terrainBakeProgram, 0u) << "Material bake program must compile + link";
    EXPECT_GE(g_terrainBakeView, 0);
    EXPECT_GE(g_terrainBakeProj, 0);
    EXPECT_GE(g_terrainBakeHeightMap, 0);
    EXPECT_GE(g_terrainBakeHeightMapSize, 0);
    EXPECT_GE(g_terrainBakeWaterLevel, 0);
}

TEST_F(TerrainPipelineGLTest, TerrainInitialize_CreatesGPUResources) {
    Terrain terrain(smallConfig());
    terrain.initialize();
    EXPECT_NE(terrain.getHeightTexture(), 0u) << "Master heightmap must be uploaded (R32F)";
    EXPECT_NE(terrain.getMaterialAtlas(), 0u) << "RVT material atlas must exist";
    EXPECT_NE(terrain.getBakeFramebuffer(), 0u);
    EXPECT_EQ(terrain.getMaterialPageCount(), 1024);

    // Update with a camera drives chunk creation (sync first fill + RVT bake).
    terrain.update(glm::vec3(32.0f, 0.0f, 32.0f), 1.0f / 60.0f);
    EXPECT_GT(terrain.getActiveChunkCount(), 0u);
}

TEST_F(TerrainPipelineGLTest, BakeChunkMaterial_ValidPageAndReuse) {
    Terrain terrain(smallConfig());
    terrain.initialize();
    initTerrainShader();

    TerrainChunk chunk(0, 0, 64.0f, 16);
    std::vector<float> h(17 * 17, 0.0f);
    chunk.generateHeightmapFromData(std::move(h));

    const int page = terrain.bakeChunkMaterial(chunk);
    ASSERT_GE(page, 0) << "Bake must produce a page";
    ASSERT_LT(page, 1024);
    // Re-baking the same chunk reuses its page (no page churn).
    EXPECT_EQ(terrain.bakeChunkMaterial(chunk), page);
}

TEST_F(TerrainPipelineGLTest, BakedPage_ReadbackHasMaterial) {
    Terrain terrain(smallConfig());
    terrain.initialize();
    initTerrainShader();

    // The bake samples the master heightmap (perlin, heights 0..25) at the
    // chunk's world footprint, so the page should show several material
    // bands (sand/grass/rock) instead of being empty or uniform.
    TerrainChunk chunk(0, 0, 64.0f, 16);
    std::vector<float> h(17 * 17, 0.0f);
    chunk.generateHeightmapFromData(std::move(h));
    const int page = terrain.bakeChunkMaterial(chunk);
    ASSERT_GE(page, 0);

    const int px = page % 32;
    const int py = page / 32;
    std::vector<unsigned char> buf(128 * 128 * 4, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, terrain.getBakeFramebuffer());
    glReadPixels(px * 128, py * 128, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    unsigned minV = 255, maxV = 0;
    int nonZero = 0;
    for (size_t i = 0; i < buf.size(); i += 4) {
        const unsigned r = buf[i], g = buf[i + 1], b = buf[i + 2];
        minV = std::min(minV, std::min(r, std::min(g, b)));
        maxV = std::max(maxV, std::max(r, std::max(g, b)));
        if (r || g || b) ++nonZero;
    }
    EXPECT_GT(nonZero, 0) << "bake produced an empty page";
    EXPECT_GT(maxV, 40) << "baked material must contain a bright layer (sand/grass)";
    EXPECT_LT(minV, maxV) << "baked material should vary across the page (height layers)";
}

TEST_F(TerrainPipelineGLTest, BakeRecycling_EvictsPreviousPageOwner) {
    Terrain terrain(smallConfig());
    terrain.initialize();
    initTerrainShader();

    // Bake more distinct chunks than pages exist (1024) so the page ring wraps
    // and recycles page 0 - evicting chunk (0,0), its original owner.
    for (int i = 0; i < 1025; ++i) {
        TerrainChunk chunk(i, 0, 64.0f, 8);
        std::vector<float> h(9 * 9, 1.0f);
        chunk.generateHeightmapFromData(std::move(h));
        ASSERT_GE(terrain.bakeChunkMaterial(chunk), 0);
    }

    // Chunk (0,0) must be re-assigned a fresh page, not the recycled page 0
    // (without eviction it would still map to page 0 and return 0 here).
    TerrainChunk chunk00(0, 0, 64.0f, 8);
    std::vector<float> h(9 * 9, 1.0f);
    chunk00.generateHeightmapFromData(std::move(h));
    EXPECT_NE(terrain.bakeChunkMaterial(chunk00), 0);
}

TEST_F(TerrainPipelineGLTest, ChunkGetHeightAt_DelegatesToHeightfieldHelper) {
    TerrainChunk chunk(0, 0, 64.0f, 16);
    std::vector<float> h(17 * 17);
    for (int z = 0; z < 17; ++z)
        for (int x = 0; x < 17; ++x)
            h[z * 17 + x] = x * 2.0f;   // ramp 0..32 across the chunk
    const std::vector<float> ref = h;   // keep a copy before the move
    chunk.generateHeightmapFromData(std::move(h));

    // Exact ground truth on the ramp (h = x*2, grid step 64/16 = 4m):
    // getHeightAt must reproduce known values, not just the helper it wraps.
    EXPECT_NEAR(chunk.getHeightAt(2.0f, 8.0f), 1.0f, 1e-4);
    EXPECT_NEAR(chunk.getHeightAt(10.0f, 8.0f), 5.0f, 1e-4);
    EXPECT_NEAR(chunk.getHeightAt(62.0f, 8.0f), 31.0f, 1e-4);

    // And it must match the shared helper on a dense grid of samples.
    for (float fx = 0.5f; fx <= 63.0f; fx += 5.0f)
        for (float fz = 0.5f; fz <= 63.0f; fz += 9.0f)
            EXPECT_NEAR(chunk.getHeightAt(fx, fz),
                        terrain::sampleChunkBilinear(ref.data(), 16, fx, fz, 64.0f), 1e-4);
}

TEST_F(TerrainPipelineGLTest, HeightTexture_MatchesCPUFallbackExactly) {
    // THE contract of the GPU terrain: the R32F heightmap texture uploaded for
    // vertex-shader displacement must contain exactly the same surface the CPU
    // physics queries sample. Read it back and compare texel-for-texel against
    // the (unloaded-chunk) fallback path, which bilinearly samples the same
    // master heightmap - integer texels match exactly.
    Terrain terrain(smallConfig());
    terrain.initialize();   // no chunks loaded -> getHeightAt uses the fallback

    std::vector<float> tex(256 * 256);
    glBindTexture(GL_TEXTURE_2D, terrain.getHeightTexture());
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, tex.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    const glm::ivec2 probes[] = {
        {0, 0}, {1, 0}, {127, 127}, {128, 63}, {200, 17}, {255, 255}
    };
    for (const auto& p : probes) {
        EXPECT_NEAR(tex[p.y * 256 + p.x],
                    terrain.getHeightAt((float)p.x, (float)p.y), 0.01f)
            << "GPU height texture texel (" << p.x << "," << p.y
            << ") disagrees with the CPU heightfield";
    }

    // The texture must carry the generated height range, not be empty/zeroed.
    float minV = 1e9f, maxV = -1e9f;
    for (float v : tex) { minV = std::min(minV, v); maxV = std::max(maxV, v); }
    EXPECT_GE(minV, 0.0f);
    EXPECT_LE(maxV, 25.1f);
    EXPECT_GT(maxV, 1.0f) << "heightmap texture looks empty";
}

TEST_F(TerrainPipelineGLTest, HeightQuery_ContinuousAcrossChunkBoundary) {
    Terrain terrain(smallConfig());
    terrain.initialize();
    terrain.update(glm::vec3(32.0f, 0.0f, 32.0f), 1.0f / 60.0f);   // loads chunks (0,0)..(1,1)

    // World 63.9 vs 64.1 straddles the boundary between chunk (0,0) and (1,0).
    // A continuous heightfield must not jump at the boundary.
    const float a = terrain.getHeightAt(63.9f, 10.0f);
    const float b = terrain.getHeightAt(64.1f, 10.0f);
    EXPECT_NEAR(a, b, 0.5f);
    EXPECT_GE(a, 0.0f);
    EXPECT_LE(a, 25.0f);
    EXPECT_GE(b, 0.0f);
    EXPECT_LE(b, 25.0f);
}

TEST_F(TerrainPipelineGLTest, HeightQuery_FallbackMatchesMappedMaster) {
    Terrain terrain(smallConfig());
    terrain.initialize();   // no chunks loaded -> master-heightmap fallback

    // A far point with no loaded chunk uses the fallback. Must be in range and
    // deterministic (the old worldX/size mapping returned texel-0 heights).
    const float h = terrain.getHeightAt(2000.0f, 2000.0f);
    EXPECT_GE(h, 0.0f);
    EXPECT_LE(h, 25.0f);
    EXPECT_EQ(h, terrain.getHeightAt(2000.0f, 2000.0f));
}

TEST_F(TerrainPipelineGLTest, NormalAt_ConsistentWithHeightGradient) {
    Terrain terrain(smallConfig());
    terrain.initialize();
    terrain.update(glm::vec3(32.0f, 0.0f, 32.0f), 1.0f / 60.0f);

    const glm::vec3 n = terrain.getNormalAt(30.0f, 30.0f);
    EXPECT_NEAR(glm::length(n), 1.0f, 1e-4) << "normals must be normalized";
    EXPECT_GT(n.y, 0.0f) << "terrain always faces up";
}
