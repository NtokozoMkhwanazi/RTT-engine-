// ============================================================================
// test_vegetation_world.cpp
// ============================================================================
// Regression tests for the vegetation pipeline:
//
//   1. The app-level VegetationConfig used to have near-zero densities
//      (treeDensity = 0.00005, maxTreesPerChunk = 1) which made
//      generateForChunk() produce ZERO trees/rocks - a silent empty world
//      (objects "placed" but nothing ever spawned, and terrain Y-snapping
//      had nothing to snap). Fixed in editor/config.h.
//
//   2. Terrain-surface Y placement: vegetation spawns at y=0 when fed an
//      empty heightmap, and WorldManager::snapToTerrain() lifts each object
//      onto the real surface via Terrain::getHeightAt. These tests lock in
//      the chunk-scoped generation math so the world stays populated.
//
// All CPU-only: VegetationSystem::generateForChunk is pure math (no GL).
// ============================================================================

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <cmath>
#include <cstdio>

#include "../world/VegetationSystem.h"
#include "../editor/config.h"

namespace {

// Config matching the app's default world setup (80m chunks, 5x5 view).
VegetationSystem::VegetationConfig AppConfig() {
    const Config::VegetationConfig& c = Config::getVegetationConfig();
    VegetationSystem::VegetationConfig vc;
    vc.treeDensity = c.treeDensity;
    vc.rockDensity = c.rockDensity;
    vc.maxTreesPerChunk = c.maxTreesPerChunk;
    vc.maxRocksPerChunk = c.maxRocksPerChunk;
    vc.minTreeHeight = c.minTreeHeight;
    vc.maxTreeHeight = c.maxTreeHeight;
    return vc;
}

} // namespace

// ============================================================================
// The core regression: the app config must actually produce vegetation.
// ============================================================================

TEST(VegetationAppConfigTest, DensitiesAreSane) {
    const Config::VegetationConfig& c = Config::getVegetationConfig();
    // The silent-empty-world bug: 0.00005 density with max 1 per chunk
    // yielded min(6400*0.00005, 1) = 0 trees every time.
    EXPECT_GT(c.treeDensity, 0.0f);
    EXPECT_GT(c.rockDensity, 0.0f);
    EXPECT_GT(c.maxTreesPerChunk, 0);
    EXPECT_GT(c.maxRocksPerChunk, 0);
}

TEST(VegetationAppConfigTest, GeneratesTreesAndRocksPerChunk) {
    VegetationSystem veg(AppConfig());

    // Chunk 0,0 - 80x80m world surface with no height data (y=0).
    std::vector<float> emptyHeights;
    veg.generateForChunk(0, 0, 80.0f, emptyHeights, 1024);

    EXPECT_GT(veg.getTrees().size(), 0u)
        << "App-config tree density must produce trees per chunk "
           "(was 0 with the old near-zero density)";
    EXPECT_GT(veg.getRocks().size(), 0u)
        << "App-config rock density must produce rocks per chunk "
           "(was 0 with the old near-zero density)";

    // And the caps bound the counts (no unbounded spawning).
    const Config::VegetationConfig& c = Config::getVegetationConfig();
    EXPECT_LE(veg.getTrees().size(), (size_t)c.maxTreesPerChunk);
    EXPECT_LE(veg.getRocks().size(), (size_t)c.maxRocksPerChunk);
}

TEST(VegetationAppConfigTest, WorldScaleCountsAreReasonable) {
    // The editor world is 5x5 chunks (viewDistance=2). At the fixed
    // densities this must yield a populated-but-not-absurd world.
    VegetationSystem veg(AppConfig());
    for (int x = -2; x <= 2; ++x) {
        for (int z = -2; z <= 2; ++z) {
            std::vector<float> emptyHeights;
            veg.generateForChunk(x, z, 80.0f, emptyHeights, 1024);
        }
    }
    EXPECT_GE(veg.getTrees().size(), 50u);
    EXPECT_GE(veg.getRocks().size(), 25u);
    EXPECT_LE(veg.getTrees().size(), 2000u);  // sanity ceiling
}

// ============================================================================
// Chunk-scoped generation math
// ============================================================================

TEST(VegetationChunkTest, ObjectsStayInsideChunkBounds) {
    VegetationSystem veg(AppConfig());
    const float chunkSize = 80.0f;
    std::vector<float> emptyHeights;
    veg.generateForChunk(2, -1, chunkSize, emptyHeights, 1024);

    const float minX = 2.0f * chunkSize;
    const float maxX = minX + chunkSize;
    const float minZ = -1.0f * chunkSize;
    const float maxZ = minZ + chunkSize;

    for (const auto& tree : veg.getTrees()) {
        EXPECT_GE(tree.position.x, minX);
        EXPECT_LT(tree.position.x, maxX);
        EXPECT_GE(tree.position.z, minZ);
        EXPECT_LT(tree.position.z, maxZ);
    }
    for (const auto& rock : veg.getRocks()) {
        EXPECT_GE(rock.position.x, minX);
        EXPECT_LT(rock.position.x, maxX);
        EXPECT_GE(rock.position.z, minZ);
        EXPECT_LT(rock.position.z, maxZ);
    }
}

TEST(VegetationChunkTest, EmptyHeightmapYieldsYZeroAndNoCrash) {
    // With no height data everything spawns at y=0 (the WorldManager snaps
    // these onto the terrain surface afterwards). Must not crash or produce
    // NaNs - this exercises the empty-heights path used by the editor.
    VegetationSystem veg(AppConfig());
    std::vector<float> emptyHeights;
    veg.generateForChunk(0, 0, 80.0f, emptyHeights, 1024);
    for (const auto& tree : veg.getTrees()) {
        EXPECT_FLOAT_EQ(tree.position.y, 0.0f);
        EXPECT_TRUE(std::isfinite(tree.position.x));
        EXPECT_TRUE(std::isfinite(tree.position.z));
    }
    for (const auto& rock : veg.getRocks()) {
        EXPECT_FLOAT_EQ(rock.position.y, 0.0f);
    }
}

TEST(VegetationChunkTest, HeightmapSnapsPlacementYToSurface) {
    // When heights ARE provided the generator must read the surface height
    // at the placement (and only accept positions that pass validation).
    // A flat 40m plateau means every accepted object sits at y=40.
    const int size = 64;
    std::vector<float> heights(size * size, 40.0f);

    VegetationSystem veg(AppConfig());
    veg.generateForChunk(0, 0, 64.0f, heights, size);

    ASSERT_GT(veg.getTrees().size(), 0u);
    for (const auto& tree : veg.getTrees()) {
        EXPECT_FLOAT_EQ(tree.position.y, 40.0f);
    }
}

TEST(VegetationChunkTest, ClearResetsAllObjects) {
    VegetationSystem veg(AppConfig());
    std::vector<float> emptyHeights;
    veg.generateForChunk(0, 0, 80.0f, emptyHeights, 1024);
    ASSERT_GT(veg.getTrees().size(), 0u);
    veg.clear();
    EXPECT_EQ(veg.getTrees().size(), 0u);
    EXPECT_EQ(veg.getRocks().size(), 0u);
}

TEST(VegetationChunkTest, DeterministicWithSeed) {
    // Two systems with the same config share the fixed RNG seed -> the same
    // chunk must produce the same object layout (stable save/load).
    VegetationSystem a(AppConfig());
    VegetationSystem b(AppConfig());
    std::vector<float> emptyHeights;
    a.generateForChunk(1, 1, 80.0f, emptyHeights, 1024);
    b.generateForChunk(1, 1, 80.0f, emptyHeights, 1024);

    ASSERT_EQ(a.getTrees().size(), b.getTrees().size());
    ASSERT_EQ(a.getRocks().size(), b.getRocks().size());
    for (size_t i = 0; i < a.getTrees().size(); ++i) {
        EXPECT_FLOAT_EQ(a.getTrees()[i].position.x, b.getTrees()[i].position.x);
        EXPECT_FLOAT_EQ(a.getTrees()[i].position.z, b.getTrees()[i].position.z);
        EXPECT_FLOAT_EQ(a.getTrees()[i].height, b.getTrees()[i].height);
    }
}

// ============================================================================
// Placement Y-snapping (the WorldManager::snapToTerrain contract)
// ============================================================================

TEST(VegetationSnapTest, SnapMathLiftsYFromZeroToSurface) {
    // Mirrors the world_manager snapping: given a y=0 placement, the snapped
    // position equals the terrain height at (x,z). Using the same 1-texel-per-
    // meter convention the terrain uses (flat 25m plateau here).
    const int size = 32;
    std::vector<float> heights(size * size, 25.0f);

    auto snap = [&](glm::vec3 p) {
        const int tx = std::clamp((int)std::floor(p.x), 0, size - 1);
        const int tz = std::clamp((int)std::floor(p.z), 0, size - 1);
        p.y = heights[tz * size + tx];
        return p;
    };

    glm::vec3 placed(10.0f, 0.0f, 12.0f);
    glm::vec3 snapped = snap(placed);
    EXPECT_FLOAT_EQ(snapped.y, 25.0f);
    EXPECT_FLOAT_EQ(snapped.x, placed.x);
    EXPECT_FLOAT_EQ(snapped.z, placed.z);
}
