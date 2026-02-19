#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <random>

// Procedural vegetation system
class VegetationSystem {
public:
    struct VegetationConfig {
        float treeDensity = 0.02f;         // Trees per square meter
        float grassDensity = 0.5f;         // Grass clusters per square meter
        float rockDensity = 0.01f;         // Rocks per square meter
        float minTreeHeight = 3.0f;
        float maxTreeHeight = 8.0f;
        int maxTreesPerChunk = 50;
        int maxRocksPerChunk = 30;
    };

    struct Tree {
        glm::vec3 position;
        float height;
        float radius;
        int type;  // 0=pine, 1=oak, 2=birch
    };

    struct Rock {
        glm::vec3 position;
        glm::vec3 scale;
        float rotation;
        int type;  // 0=boulder, 1=stone, 2=cliff
    };

    VegetationSystem();
    VegetationSystem(const VegetationConfig& config);

    // Generate vegetation for a chunk
    void generateForChunk(int chunkX, int chunkY, float chunkSize, 
                          const std::vector<float>& heights, int heightmapSize);

    // Get trees for rendering
    const std::vector<Tree>& getTrees() const { return m_trees; }
    const std::vector<Rock>& getRocks() const { return m_rocks; }

    // Clear vegetation (for chunk unloading)
    void clear();

private:
    // Check if position is valid for vegetation
    bool isValidPosition(const glm::vec3& pos, const std::vector<float>& heights, 
                         int heightmapSize, float heightThreshold) const;

    // Generate random position in chunk
    glm::vec3 randomPositionInChunk(int chunkX, int chunkY, float chunkSize, 
                                    std::mt19937& rng) const;

    VegetationConfig m_config;
    std::vector<Tree> m_trees;
    std::vector<Rock> m_rocks;
    std::mt19937 m_rng;
};
