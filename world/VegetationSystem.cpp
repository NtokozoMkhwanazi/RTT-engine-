#include "VegetationSystem.h"
#include "TerrainChunk.h"
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
    m_pebbles.clear();
    m_plants.clear();
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

    // Global hard cap for performance: never place more than 5 trees scene-wide
    // total (overridable via VegetationConfig::treeDensity/maxTreesPerChunk for
    // production). The function-local static accumulates across chunks so the
    // cap is global, not per-chunk.
    static int s_totalTreesPlaced = 0;
    constexpr int kMaxTotalTrees = 5;
    numTrees = std::max(0, std::min(numTrees, kMaxTotalTrees - s_totalTreesPlaced));
    s_totalTreesPlaced += numTrees;
    
    for (int i = 0; i < numTrees; i++) {
        glm::vec3 pos;
        pos.x = worldStartX + posDist(m_rng);
        pos.z = worldStartZ + posDist(m_rng);
        
        // Get height at position using the standardized utility function
        // (terrain::sampleHeightBilinear from TerrainChunk.h) which correctly
        // maps world meters → texel coordinates, matching the GPU heightmap.
        if (!heights.empty() && heightmapSize > 0) {
            pos.y = terrain::sampleHeightBilinear(heights.data(), heightmapSize, pos.x, pos.z);
        } else {
            pos.y = 0.0f;
        }
        
        // Check if valid (not too steep, not underwater)
        if (heights.empty() || isValidPosition(pos, heights, heightmapSize, 3.0f)) {
            Tree tree;
            tree.position = pos;
            tree.height = heightDist(m_rng);
            tree.radius = tree.height * 0.15f;
            tree.type = typeDist(m_rng);
            m_trees.push_back(tree);
        }
    }
    
    // Generate rocks as small clusters (outcrops) instead of a uniform grid -
    // a scattering of 2-4 rocks hugging the same spot reads as a rocky outcrop
    // rather than evenly-sprinkled pebbles. The cluster count keeps the total
    // under maxRocksPerChunk; each rock jitters a few meters off the cluster
    // center (clamped to the chunk) so the group stays tight.
    int numRocks = (int)(chunkSize * chunkSize * m_config.rockDensity);
    numRocks = std::min(numRocks, m_config.maxRocksPerChunk);
    const int rocksPerCluster = 3;
    const int numClusters = std::max(1, (numRocks + rocksPerCluster - 1) / rocksPerCluster);
    std::uniform_real_distribution<float> clusterJitter(-4.0f, 4.0f);
    int rocksPlaced = 0;  // per-chunk count (m_rocks is global across chunks)

    for (int c = 0; c < numClusters && rocksPlaced < numRocks; c++) {
        glm::vec3 center;
        center.x = worldStartX + posDist(m_rng);
        center.z = worldStartZ + posDist(m_rng);
        const int here = std::min(rocksPerCluster, numRocks - rocksPlaced);
        for (int i = 0; i < here; i++) {
            glm::vec3 pos;
            pos.x = std::clamp(center.x + clusterJitter(m_rng), worldStartX, worldStartX + chunkSize - 0.01f);
            pos.z = std::clamp(center.z + clusterJitter(m_rng), worldStartZ, worldStartZ + chunkSize - 0.01f);
            
            if (!heights.empty() && heightmapSize > 0) {
                pos.y = terrain::sampleHeightBilinear(heights.data(), heightmapSize, pos.x, pos.z);
            } else {
                pos.y = 0.0f;
            }
            
            // Rocks prefer higher elevations and steep areas
            if (heights.empty() || pos.y > 15.0f || isValidPosition(pos, heights, heightmapSize, 20.0f)) {
                Rock rock;
                rock.position = pos;
                rock.scale = glm::vec3(scaleDist(m_rng), scaleDist(m_rng) * 0.6f, scaleDist(m_rng));
                rock.rotation = rotDist(m_rng);
                rock.type = typeDist(m_rng);
                m_rocks.push_back(rock);
                rocksPlaced++;
            }
        }
    }

    // Pebbles: a ground scatter of tiny stones (rock type 3) hugging the
    // surface - the "tiny rocks" that fill the negative space between the
    // larger boulder/stone/cliff outcrops. These reuse the stone model (stone.fbx)
    // scaled down to pebble size; WorldManager maps type 3 -> ROCK_STONE.
    int numPebbles = (int)(chunkSize * chunkSize * m_config.pebbleDensity);
    numPebbles = std::min(numPebbles, m_config.maxPebblesPerChunk);
    std::uniform_real_distribution<float> pebbleScaleDist(0.06f, 0.22f);
    for (int i = 0; i < numPebbles; i++) {
        glm::vec3 pos;
        pos.x = worldStartX + posDist(m_rng);
        pos.z = worldStartZ + posDist(m_rng);
        pos.y = 0.0f;
        if (heights.empty() || isValidPosition(pos, heights, heightmapSize, 0.5f)) {
            Rock rock;
            rock.position = pos;
            const float s = pebbleScaleDist(m_rng);
            rock.scale = glm::vec3(s * 0.9f, s * 0.5f, s);
            rock.rotation = rotDist(m_rng);
            rock.type = 3;   // pebble
            m_pebbles.push_back(rock);
        }
    }

    // Generate ground plants (grass clusters + low desert flora) - planted
    // MODELS on top of the boulder-textured terrain, so a high density reads
    // as a grassy field. grassDensity is per-m2; with the default 0.01 that's
    // ~64 per 80m chunk. Mostly bushy grass, with a few flowers/succulents
    // scattered among it. Low plants belong on gentle, dry ground.
    const int rawPlants = (int)(chunkSize * chunkSize * m_config.grassDensity);
    int numPlants = std::clamp(rawPlants, 20, 200);
    std::uniform_real_distribution<float> plantTypeDist(0.0f, 1.0f);   // 80% grass
    std::uniform_real_distribution<float> plantScaleDist(0.6f, 1.4f);
    std::uniform_real_distribution<float> plantTintDist(0.0f, 1.0f);

    for (int i = 0; i < numPlants; i++) {
        glm::vec3 pos;
        pos.x = worldStartX + posDist(m_rng);
        pos.z = worldStartZ + posDist(m_rng);
        if (!heights.empty() && heightmapSize > 0) {
            float hx = pos.x / heightmapSize;
            float hz = pos.z / heightmapSize;
            int hxInt = std::clamp((int)hx, 0, heightmapSize - 1);
            int hzInt = std::clamp((int)hz, 0, heightmapSize - 1);
            pos.y = heights[hzInt * heightmapSize + hxInt];
        } else {
            pos.y = 0.0f;
        }
        if (heights.empty() || isValidPosition(pos, heights, heightmapSize, 4.0f)) {
            Plant plant;
            plant.position = pos;
            plant.scale = plantScaleDist(m_rng);
            plant.rotation = rotDist(m_rng);
            const float r = plantTypeDist(m_rng);
            plant.type = (r < 0.80f) ? 0 : (r < 0.93f) ? 1 : 2;   // grass:flower:bush
            // Per-instance color variation: grass drifts between healthy green
            // and dry yellow; flowers/bushes get a milder green-range tint so
            // the whole field isn't one flat shade.
            const float t = plantTintDist(m_rng);
            if (plant.type == 0) {
                plant.tint = glm::mix(glm::vec3(0.55f, 0.95f, 0.45f),   // lush green
                                      glm::vec3(0.85f, 0.78f, 0.42f),   // dry yellow
                                      t);
            } else {
                plant.tint = glm::mix(glm::vec3(0.75f, 0.98f, 0.6f),
                                      glm::vec3(0.95f, 0.85f, 0.55f), t);
            }
            m_plants.push_back(plant);
        }
    }
}

bool VegetationSystem::isValidPosition(const glm::vec3& pos, const std::vector<float>& heights,
                                        int heightmapSize, float heightThreshold) const {
    if (heights.empty() || heightmapSize <= 0) return true;
    
    // Check slope (don't place on steep cliffs)
    float delta = 2.0f;
    float hL = 0, hR = 0, hD = 0, hU = 0;
    
    // Sample heights around position
    auto sampleHeight = [&](float x, float z) -> float {
        return terrain::sampleHeightBilinear(heights.data(), heightmapSize, x, z);
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
