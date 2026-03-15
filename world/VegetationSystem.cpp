#include "VegetationSystem.h"
#include <cmath>
#include <algorithm>

VegetationSystem::VegetationSystem()
    : m_config()
{
}

VegetationSystem::VegetationSystem(const VegetationConfig& config)
    : m_config(config), m_rng(42)
{
}

void VegetationSystem::clear() {
    m_trees.clear();
    m_rocks.clear();
    m_instanceBuffersDirty = true;
}

void VegetationSystem::buildInstanceData() {
    m_treeInstances.clear();
    m_rockInstances.clear();

    // Convert trees to instance data
    for (const auto& tree : m_trees) {
        TreeInstance inst;
        inst.position = tree.position;
        inst.scale = tree.height / 10.0f;  // Scale based on height
        inst.type = tree.type;
        m_treeInstances.push_back(inst);
    }

    // Convert rocks to instance data
    for (const auto& rock : m_rocks) {
        RockInstance inst;
        inst.position = rock.position;
        inst.rotation = rock.rotation;
        inst.scale = rock.scale;
        inst.type = rock.type;
        m_rockInstances.push_back(inst);
    }

    m_instanceBuffersDirty = true;
}

void VegetationSystem::createInstanceBuffers() {
    if (m_treeInstanceVBO == 0) {
        glGenBuffers(1, &m_treeInstanceVBO);
    }
    if (m_rockInstanceVBO == 0) {
        glGenBuffers(1, &m_rockInstanceVBO);
    }
    updateInstanceBuffers();
}

void VegetationSystem::updateInstanceBuffers() {
    if (!m_instanceBuffersDirty) return;

    buildInstanceData();

    // Update tree instance buffer
    if (m_treeInstanceVBO != 0 && !m_treeInstances.empty()) {
        glBindBuffer(GL_ARRAY_BUFFER, m_treeInstanceVBO);
        glBufferData(GL_ARRAY_BUFFER, m_treeInstances.size() * sizeof(TreeInstance),
                     m_treeInstances.data(), GL_STATIC_DRAW);
    }

    // Update rock instance buffer
    if (m_rockInstanceVBO != 0 && !m_rockInstances.empty()) {
        glBindBuffer(GL_ARRAY_BUFFER, m_rockInstanceVBO);
        glBufferData(GL_ARRAY_BUFFER, m_rockInstances.size() * sizeof(RockInstance),
                     m_rockInstances.data(), GL_STATIC_DRAW);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_instanceBuffersDirty = false;
}

void VegetationSystem::renderTreesInstanced(GLuint treeVAO) const {
    if (m_treeInstances.empty() || m_treeInstanceVBO == 0) return;

    glBindVertexArray(treeVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_treeInstanceVBO);

    // Set up instanced attribute pointers (location 3, 4, 5 for mat4-like data)
    // Position (location 3)
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(TreeInstance), (void*)0);
    glVertexAttribDivisor(3, 1);  // Per-instance

    // Scale and type (location 4)
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(TreeInstance),
                         (void*)(sizeof(float) * 3));
    glVertexAttribDivisor(4, 1);

    // Draw instanced
    glDrawElementsInstanced(GL_TRIANGLES, 0, GL_UNSIGNED_INT, 0, m_treeInstances.size());

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

void VegetationSystem::renderRocksInstanced(GLuint rockVAO) const {
    if (m_rockInstances.empty() || m_rockInstanceVBO == 0) return;

    glBindVertexArray(rockVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_rockInstanceVBO);

    // Set up instanced attribute pointers
    // Position (location 3)
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(RockInstance), (void*)0);
    glVertexAttribDivisor(3, 1);

    // Rotation (location 4)
    glEnableVertexAttribArray(4);
    glVertexAttrib1f(4, 0.0f);  // Use rotation from instance data in shader
    glVertexAttribDivisor(4, 0);

    // Scale (location 5)
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, sizeof(RockInstance),
                         (void*)(sizeof(float) * 4));
    glVertexAttribDivisor(5, 1);

    // Draw instanced
    glDrawElementsInstanced(GL_TRIANGLES, 0, GL_UNSIGNED_INT, 0, m_rockInstances.size());

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

void VegetationSystem::cleanupInstanceBuffers() {
    if (m_treeInstanceVBO != 0) {
        glDeleteBuffers(1, &m_treeInstanceVBO);
        m_treeInstanceVBO = 0;
    }
    if (m_rockInstanceVBO != 0) {
        glDeleteBuffers(1, &m_rockInstanceVBO);
        m_rockInstanceVBO = 0;
    }
}

void VegetationSystem::generateForChunk(int chunkX, int chunkY, float chunkSize,
                                         const std::vector<float>& heights, int heightmapSize) {
    clear();
    
    float worldStartX = chunkX * chunkSize;
    float worldStartZ = chunkY * chunkSize;
    
    // Distribution for random positions
    std::uniform_real_distribution<float> posDist(0.0f, chunkSize);
    std::uniform_real_distribution<float> heightDist(m_config.minTreeHeight, m_config.maxTreeHeight);
    std::uniform_int_distribution<int> typeDist(0, 2);
    std::uniform_real_distribution<float> scaleDist(0.5f, 2.0f);
    std::uniform_real_distribution<float> rotDist(0.0f, 360.0f);
    
    // Generate trees
    int numTrees = (int)(chunkSize * chunkSize * m_config.treeDensity);
    numTrees = std::min(numTrees, m_config.maxTreesPerChunk);
    
    for (int i = 0; i < numTrees; i++) {
        glm::vec3 pos;
        pos.x = worldStartX + posDist(m_rng);
        pos.z = worldStartZ + posDist(m_rng);
        
        // Get height at position
        float hx = pos.x / heightmapSize;
        float hz = pos.z / heightmapSize;
        int hxInt = std::clamp((int)hx, 0, heightmapSize - 1);
        int hzInt = std::clamp((int)hz, 0, heightmapSize - 1);
        pos.y = heights[hzInt * heightmapSize + hxInt];
        
        // Check if valid (not too steep, not underwater)
        if (isValidPosition(pos, heights, heightmapSize, 3.0f)) {
            Tree tree;
            tree.position = pos;
            tree.height = heightDist(m_rng);
            tree.radius = tree.height * 0.15f;
            tree.type = typeDist(m_rng);
            m_trees.push_back(tree);
        }
    }
    
    // Generate rocks
    int numRocks = (int)(chunkSize * chunkSize * m_config.rockDensity);
    numRocks = std::min(numRocks, m_config.maxRocksPerChunk);
    
    for (int i = 0; i < numRocks; i++) {
        glm::vec3 pos;
        pos.x = worldStartX + posDist(m_rng);
        pos.z = worldStartZ + posDist(m_rng);
        
        float hx = pos.x / heightmapSize;
        float hz = pos.z / heightmapSize;
        int hxInt = std::clamp((int)hx, 0, heightmapSize - 1);
        int hzInt = std::clamp((int)hz, 0, heightmapSize - 1);
        pos.y = heights[hzInt * heightmapSize + hxInt];
        
        // Rocks prefer higher elevations and steep areas
        if (pos.y > 15.0f || isValidPosition(pos, heights, heightmapSize, 20.0f)) {
            Rock rock;
            rock.position = pos;
            rock.scale = glm::vec3(scaleDist(m_rng), scaleDist(m_rng) * 0.6f, scaleDist(m_rng));
            rock.rotation = rotDist(m_rng);
            rock.type = typeDist(m_rng);
            m_rocks.push_back(rock);
        }
    }
}

bool VegetationSystem::isValidPosition(const glm::vec3& pos, const std::vector<float>& heights,
                                        int heightmapSize, float heightThreshold) const {
    // Check slope (don't place on steep cliffs)
    float delta = 2.0f;
    float hL = 0, hR = 0, hD = 0, hU = 0;
    
    // Sample heights around position
    auto sampleHeight = [&](float x, float z) -> float {
        float hx = x / heightmapSize;
        float hz = z / heightmapSize;
        int hxInt = std::clamp((int)hx, 0, heightmapSize - 1);
        int hzInt = std::clamp((int)hz, 0, heightmapSize - 1);
        return heights[hzInt * heightmapSize + hxInt];
    };
    
    hL = sampleHeight(pos.x - delta, pos.z);
    hR = sampleHeight(pos.x + delta, pos.z);
    hD = sampleHeight(pos.x, pos.z - delta);
    hU = sampleHeight(pos.x, pos.z + delta);
    
    float slope = std::abs(hL - hR) + std::abs(hD - hU);
    
    // Too steep
    if (slope > heightThreshold) return false;
    
    // Not underwater
    if (pos.y < 3.0f) return false;
    
    return true;
}

glm::vec3 VegetationSystem::randomPositionInChunk(int chunkX, int chunkY, float chunkSize,
                                                   std::mt19937& rng) const {
    std::uniform_real_distribution<float> dist(0.0f, chunkSize);
    return glm::vec3(
        chunkX * chunkSize + dist(rng),
        0.0f,
        chunkY * chunkSize + dist(rng)
    );
}
