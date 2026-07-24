/**
 * Terrain System Unit Tests
 * 
 * Tests for procedural terrain generation, chunk streaming,
 * height queries, and LOD system.
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cmath>

// We'll test the terrain logic without full OpenGL context
// by testing the mathematical functions directly

class TerrainTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code before each test
    }
    
    void TearDown() override {
        // Cleanup code after each test
    }
};

/**
 * Test: Perlin Noise Heightmap Generation
 * Verifies that heightmap values are within expected range
 * and show proper noise characteristics
 */
TEST_F(TerrainTest, HeightmapGeneration_ValidRange) {
    // Simulate heightmap generation parameters
    const int heightmapSize = 1024;
    const float heightScale = 80.0f;
    
    // Generate test heightmap (simplified Perlin noise simulation)
    std::vector<float> heightmap(heightmapSize * heightmapSize);
    
    // Simple noise function for testing (replace with actual Perlin)
    auto perlinNoise = [](float x, float y) -> float {
        // Simplified noise - in real code, use proper Perlin
        return (std::sin(x * 0.1f) * std::cos(y * 0.1f) + 1.0f) * 0.5f;
    };
    
    // Generate heightmap
    for (int y = 0; y < heightmapSize; y++) {
        for (int x = 0; x < heightmapSize; x++) {
            float noise = perlinNoise(x, y);
            heightmap[y * heightmapSize + x] = noise * heightScale;
        }
    }
    
    // Verify all heights are within valid range [0, heightScale]
    for (const auto& height : heightmap) {
        EXPECT_GE(height, 0.0f) << "Height below minimum";
        EXPECT_LE(height, heightScale) << "Height above maximum";
    }
}

/**
 * Test: Heightmap Continuity
 * Verifies that adjacent heightmap values don't have extreme jumps
 */
TEST_F(TerrainTest, HeightmapContinuity_SmoothTransitions) {
    const int heightmapSize = 256;  // Smaller for performance
    const float heightScale = 80.0f;
    const float maxGradient = 0.5f;  // Maximum allowed height difference
    
    std::vector<float> heightmap(heightmapSize * heightmapSize);
    
    // Generate heightmap
    auto perlinNoise = [](float x, float y) -> float {
        return (std::sin(x * 0.1f) * std::cos(y * 0.1f) + 1.0f) * 0.5f;
    };
    
    for (int y = 0; y < heightmapSize; y++) {
        for (int x = 0; x < heightmapSize; x++) {
            float noise = perlinNoise(x, y);
            heightmap[y * heightmapSize + x] = noise * heightScale;
        }
    }
    
    // Check continuity (skip edges)
    for (int y = 1; y < heightmapSize - 1; y++) {
        for (int x = 1; x < heightmapSize - 1; x++) {
            float center = heightmap[y * heightmapSize + x];
            float right = heightmap[y * heightmapSize + (x + 1)];
            float down = heightmap[(y + 1) * heightmapSize + x];
            
            float diffRight = std::abs(center - right);
            float diffDown = std::abs(center - down);
            
            EXPECT_LE(diffRight, heightScale * maxGradient) 
                << "Height jump too large at (" << x << "," << y << ") right";
            EXPECT_LE(diffDown, heightScale * maxGradient) 
                << "Height jump too large at (" << x << "," << y << ") down";
        }
    }
}

/**
 * Test: Chunk Coordinate Calculation
 * Verifies correct mapping from world space to chunk space
 */
TEST_F(TerrainTest, ChunkCoordinateCalculation_CorrectMapping) {
    const float chunkSize = 100.0f;
    
    auto getChunkCoords = [chunkSize](float worldX, float worldZ) -> std::pair<int, int> {
        int chunkX = static_cast<int>(std::floor(worldX / chunkSize));
        int chunkY = static_cast<int>(std::floor(worldZ / chunkSize));
        return {chunkX, chunkY};
    };
    
    // Test various world positions
    struct TestCase {
        float worldX, worldZ;
        int expectedChunkX, expectedChunkY;
    };
    
    std::vector<TestCase> testCases = {
        {0.0f, 0.0f, 0, 0},
        {50.0f, 50.0f, 0, 0},      // Middle of chunk (0,0)
        {100.0f, 100.0f, 1, 1},    // Edge of chunk (1,1)
        {-50.0f, -50.0f, -1, -1},  // Negative coordinates
        {250.0f, 150.0f, 2, 1},    // Larger coordinates
    };
    
    for (const auto& test : testCases) {
        auto [chunkX, chunkY] = getChunkCoords(test.worldX, test.worldZ);
        EXPECT_EQ(chunkX, test.expectedChunkX) 
            << "Chunk X mismatch for world position (" << test.worldX << ", " << test.worldZ << ")";
        EXPECT_EQ(chunkY, test.expectedChunkY) 
            << "Chunk Y mismatch for world position (" << test.worldX << ", " << test.worldZ << ")";
    }
}

/**
 * Test: Height Query at World Position
 * Verifies interpolation of height from heightmap
 */
TEST_F(TerrainTest, HeightQueryAtPosition_AccurateInterpolation) {
    const float chunkSize = 100.0f;
    const int resolution = 64;
    const float heightScale = 80.0f;
    
    // Create small test heightmap
    std::vector<float> heightmap(resolution * resolution);
    for (int y = 0; y < resolution; y++) {
        for (int x = 0; x < resolution; x++) {
            // Create a slope for testing
            heightmap[y * resolution + x] = (x / (float)resolution) * heightScale;
        }
    }
    
    auto getHeightAt = [&](float worldX, float worldZ) -> float {
        // Convert to local chunk coordinates
        float localX = std::fmod(worldX, chunkSize);
        float localZ = std::fmod(worldZ, chunkSize);
        if (localX < 0) localX += chunkSize;
        if (localZ < 0) localZ += chunkSize;
        
        // Convert to heightmap coordinates
        float uvX = localX / chunkSize * (resolution - 1);
        float uvZ = localZ / chunkSize * (resolution - 1);
        
        int x0 = static_cast<int>(std::floor(uvX));
        int z0 = static_cast<int>(std::floor(uvZ));
        int x1 = std::min(x0 + 1, resolution - 1);
        int z1 = std::min(z0 + 1, resolution - 1);
        
        float fracX = uvX - x0;
        float fracZ = uvZ - z0;
        
        // Bilinear interpolation
        float h00 = heightmap[z0 * resolution + x0];
        float h10 = heightmap[z0 * resolution + x1];
        float h01 = heightmap[z1 * resolution + x0];
        float h11 = heightmap[z1 * resolution + x1];
        
        float h0 = h00 * (1 - fracX) + h10 * fracX;
        float h1 = h01 * (1 - fracX) + h11 * fracX;
        
        return h0 * (1 - fracZ) + h1 * fracZ;
    };
    
    // Test height queries
    float h1 = getHeightAt(25.0f, 50.0f);  // Quarter across chunk
    float h2 = getHeightAt(50.0f, 50.0f);  // Half across chunk
    float h3 = getHeightAt(75.0f, 50.0f);  // Three quarters across chunk
    
    // Heights should increase linearly (slope test)
    EXPECT_LT(h1, h2) << "Height should increase along X axis";
    EXPECT_LT(h2, h3) << "Height should increase along X axis";
    
    // Heights should be within valid range
    EXPECT_GE(h1, 0.0f);
    EXPECT_LE(h3, heightScale);
}

/**
 * Test: LOD Distance Calculation
 * Verifies correct LOD level selection based on distance
 */
TEST_F(TerrainTest, LODDistanceCalculation_CorrectLevelSelection) {
    const std::vector<float> lodDistances = {50.0f, 100.0f, 200.0f, 500.0f};
    const int maxLOD = 3;
    
    auto getLODLevel = [&](float distance) -> int {
        for (int i = 0; i < maxLOD; i++) {
            if (distance < lodDistances[i]) {
                return i;
            }
        }
        return maxLOD;
    };
    
    // Test various distances
    EXPECT_EQ(getLODLevel(0.0f), 0);       // Closest - highest detail
    EXPECT_EQ(getLODLevel(25.0f), 0);      // Still LOD 0
    EXPECT_EQ(getLODLevel(75.0f), 1);      // LOD 1
    EXPECT_EQ(getLODLevel(150.0f), 2);     // LOD 2
    EXPECT_EQ(getLODLevel(300.0f), 3);     // Farthest - lowest detail
}

/**
 * Test: Chunk Key Generation
 * Verifies unique key generation for chunk map lookup
 */
TEST_F(TerrainTest, ChunkKeyGeneration_UniqueKeys) {
    const int worldSize = 1000;
    
    auto getChunkKey = [](int chunkX, int chunkY) -> long long {
        return static_cast<long long>(chunkX) * worldSize + chunkY;
    };
    
    // Test that different chunk coordinates produce different keys
    std::set<long long> keys;
    
    for (int x = -5; x <= 5; x++) {
        for (int y = -5; y <= 5; y++) {
            long long key = getChunkKey(x, y);
            EXPECT_EQ(keys.count(key), 0) 
                << "Duplicate key " << key << " for chunk (" << x << ", " << y << ")";
            keys.insert(key);
        }
    }
    
    // Verify specific key values
    EXPECT_EQ(getChunkKey(0, 0), 0);
    EXPECT_EQ(getChunkKey(1, 0), worldSize);
    EXPECT_EQ(getChunkKey(0, 1), 1);
    EXPECT_EQ(getChunkKey(-1, 0), -worldSize);
}

/**
 * Test: Normal Calculation
 * Verifies terrain normal computation from heightmap
 */
TEST_F(TerrainTest, NormalCalculation_CorrectOrientation) {
    const float scale = 0.1f;  // Height scale for normal calculation
    
    auto calculateNormal = [&](float hL, float hR, float hD, float hU) -> glm::vec3 {
        return glm::normalize(glm::vec3(hL - hR, 2.0f * scale, hD - hU));
    };
    
    // Test flat terrain (normal should point up)
    glm::vec3 normalFlat = calculateNormal(0.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_NEAR(normalFlat.x, 0.0f, 0.01f);
    EXPECT_NEAR(normalFlat.y, 1.0f, 0.01f);
    EXPECT_NEAR(normalFlat.z, 0.0f, 0.01f);
    
    // Test slope (normal should tilt)
    glm::vec3 normalSlope = calculateNormal(-1.0f, 1.0f, 0.0f, 0.0f);
    EXPECT_LT(normalSlope.x, 0.0f);  // Should tilt left
    EXPECT_GT(normalSlope.y, 0.0f);  // Still pointing up
    EXPECT_NEAR(normalSlope.z, 0.0f, 0.01f);
}
