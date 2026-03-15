#include "Terrain.h"
#include <cmath>
#include <algorithm>
#include <iostream>

// Simple Perlin noise implementation for terrain generation
class PerlinNoise {
public:
    PerlinNoise(unsigned int seed = 12345) {
        // Initialize permutation table
        for (int i = 0; i < 256; i++) p[i] = i;
        // Shuffle
        for (int i = 255; i > 0; i--) {
            int j = seed % (i + 1);
            std::swap(p[i], p[j]);
            seed = seed * 1103515245 + 12345;
        }
        // Duplicate for overflow
        for (int i = 0; i < 256; i++) p[256 + i] = p[i];
    }
    
    float noise(float x, float y) const {
        int X = (int)floor(x) & 255;
        int Y = (int)floor(y) & 255;
        
        x -= floor(x);
        y -= floor(y);
        
        float u = fade(x);
        float v = fade(y);
        
        int A = p[X] + Y, B = p[X + 1] + Y;
        
        return lerp(v, lerp(u, grad(p[A], x, y), grad(p[B], x-1, y)),
                       lerp(u, grad(p[A+1], x, y-1), grad(p[B+1], x-1, y-1)));
    }
    
    // Octave noise for more natural terrain
    float octaveNoise(float x, float y, int octaves, float persistence) const {
        float total = 0;
        float frequency = 1;
        float amplitude = 1;
        float maxValue = 0;
        
        for (int i = 0; i < octaves; i++) {
            total += noise(x * frequency, y * frequency) * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= 2;
        }
        
        return total / maxValue;
    }
    
private:
    float fade(float t) const { return t * t * t * (t * (t * 6 - 15) + 10); }
    float lerp(float t, float a, float b) const { return a + t * (b - a); }
    float grad(int hash, float x, float y) const {
        int h = hash & 3;
        float u = h < 2 ? x : y;
        float v = h < 2 ? y : x;
        return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
    }
    
    int p[512];
};

Terrain::Terrain()
    : m_config()
{
}

Terrain::Terrain(const TerrainConfig& config)
    : m_config(config)
{
}

Terrain::~Terrain() {
}

void Terrain::initialize() {
    std::cout << "[Terrain] Initializing...\n";
    std::cout << "  Chunk size: " << m_config.chunkSize << "m\n";
    std::cout << "  Chunk resolution: " << m_config.chunkResolution << "x" << m_config.chunkResolution << "\n";
    std::cout << "  View distance: " << m_config.viewDistance << " chunks\n";
    std::cout << "  Total area: " << (m_config.chunkSize * m_config.viewDistance * 2) << "m x " 
              << (m_config.chunkSize * m_config.viewDistance * 2) << "m\n";
    
    generateHeightmap();
    m_initialized = true;
    
    std::cout << "[Terrain] Initialized successfully!\n";
}

void Terrain::generateHeightmap() {
    m_heightmap.resize(m_config.heightmapSize * m_config.heightmapSize);
    
    PerlinNoise perlin(42);  // Fixed seed for consistent terrain
    
    float scale1 = 0.003f;   // Large features
    float scale2 = 0.01f;    // Medium features
    float scale3 = 0.03f;    // Small features
    
    for (int y = 0; y < m_config.heightmapSize; y++) {
        for (int x = 0; x < m_config.heightmapSize; x++) {
            // Combine multiple octaves for natural terrain
            float height = perlin.octaveNoise(x * scale1, y * scale1, 4, 0.5f) * 0.6f;
            height += perlin.octaveNoise(x * scale2, y * scale2, 3, 0.5f) * 0.3f;
            height += perlin.octaveNoise(x * scale3, y * scale3, 2, 0.5f) * 0.1f;
            
            // Normalize to 0..1 range
            height = (height + 1.0f) * 0.5f;
            
            // Apply height scale
            m_heightmap[y * m_config.heightmapSize + x] = height * m_config.heightScale;
        }
    }
    
    std::cout << "[Terrain] Generated heightmap: " << m_config.heightmapSize << "x" 
              << m_config.heightmapSize << " (" << (m_config.heightmapSize * m_config.heightmapSize * sizeof(float) / 1024) << "KB)\n";
}

void Terrain::update(const glm::vec3& cameraPos, float dt) {
    if (!m_initialized) return;
    
    // Get current chunk coordinates
    int camChunkX, camChunkY;
    getChunkCoords(cameraPos.x, cameraPos.z, camChunkX, camChunkY);
    
    // Only update if we moved to a new chunk
    if (camChunkX == m_lastChunkX && camChunkY == m_lastChunkY) {
        // Still update LOD on existing chunks
        for (auto& [key, chunk] : m_chunks) {
            chunk->updateLOD(cameraPos, m_config.lodDistance);
        }
        return;
    }
    
    m_lastChunkX = camChunkX;
    m_lastChunkY = camChunkY;
    
    // Load new chunks in view distance
    for (int dy = -m_config.viewDistance; dy <= m_config.viewDistance; dy++) {
        for (int dx = -m_config.viewDistance; dx <= m_config.viewDistance; dx++) {
            int chunkX = camChunkX + dx;
            int chunkY = camChunkY + dy;
            
            // Check if chunk should be loaded (circular view distance)
            float dist = sqrt(dx * dx + dy * dy);
            if (dist <= m_config.viewDistance) {
                getOrCreateChunk(chunkX, chunkY);
            }
        }
    }
    
    // Unload far chunks
    std::vector<int> chunksToRemove;
    for (auto& [key, chunk] : m_chunks) {
        float dist = chunk->getDistanceToCamera(cameraPos);
        if (dist > m_config.chunkSize * (m_config.viewDistance + 2)) {
            chunksToRemove.push_back(key);
        }
    }
    
    for (int key : chunksToRemove) {
        // Remove chunk (unique_ptr handles cleanup)
        m_chunks.erase(key);
    }
    
    // Update LOD on all chunks
    for (auto& [key, chunk] : m_chunks) {
        chunk->updateLOD(cameraPos, m_config.lodDistance);
    }
}

void Terrain::render(const glm::vec3& cameraPos, float fovDegrees, float aspectRatio, 
                     float nearPlane, float farPlane) const {
    if (!m_initialized) return;

    int renderedChunks = 0;
    int frustumCulled = 0;
    int occlusionCulled = 0;

    // First pass: collect visible chunks (frustum culling)
    std::vector<const TerrainChunk*> visibleChunks;
    visibleChunks.reserve(m_chunks.size());

    for (const auto& [key, chunk] : m_chunks) {
        // Frustum culling (Priority 1 optimization)
        if (!chunk->isVisibleInFrustum(cameraPos, fovDegrees, aspectRatio, nearPlane, farPlane)) {
            frustumCulled++;
            continue;
        }

        // LOD-based culling (don't render lowest LOD)
        if (chunk->getLOD() >= 3) {
            frustumCulled++;
            continue;
        }

        visibleChunks.push_back(chunk.get());
    }

    // Second pass: occlusion culling (Priority 4 optimization)
    // Sort by distance (render closest first for better occlusion)
    std::sort(visibleChunks.begin(), visibleChunks.end(),
        [&cameraPos](const TerrainChunk* a, const TerrainChunk* b) {
            return a->getDistanceToCamera(cameraPos) < b->getDistanceToCamera(cameraPos);
        });

    // Render with occlusion culling
    for (size_t i = 0; i < visibleChunks.size(); i++) {
        const TerrainChunk* chunk = visibleChunks[i];
        bool occluded = false;

        // Check against closer chunks (simple occlusion test)
        for (size_t j = 0; j < i && !occluded; j++) {
            if (!chunk->isPotentiallyVisible(cameraPos, visibleChunks[j])) {
                occluded = true;
            }
        }

        if (occluded) {
            occlusionCulled++;
            continue;
        }

        chunk->render();
        renderedChunks++;
    }

    // Debug output (can be removed in release)
    // std::cout << "[Terrain] Rendered: " << renderedChunks 
    //           << ", Frustum culled: " << frustumCulled
    //           << ", Occlusion culled: " << occlusionCulled << "\n";
}

float Terrain::getHeightAt(float worldX, float worldZ) const {
    if (!m_initialized) return 0.0f;
    
    int chunkX, chunkY;
    getChunkCoords(worldX, worldZ, chunkX, chunkY);
    
    int key = getChunkKey(chunkX, chunkY);
    auto it = m_chunks.find(key);
    if (it != m_chunks.end()) {
        float localX = fmod(worldX, m_config.chunkSize);
        if (localX < 0) localX += m_config.chunkSize;
        float localZ = fmod(worldZ, m_config.chunkSize);
        if (localZ < 0) localZ += m_config.chunkSize;
        return it->second->getHeightAt(localX, localZ);
    }
    
    // Fallback: sample from heightmap directly
    float hx = worldX / m_config.chunkSize / m_config.heightmapSize * m_config.heightmapSize;
    float hz = worldZ / m_config.chunkSize / m_config.heightmapSize * m_config.heightmapSize;
    
    int hxInt = std::clamp((int)hx, 0, m_config.heightmapSize - 1);
    int hzInt = std::clamp((int)hz, 0, m_config.heightmapSize - 1);
    
    return m_heightmap[hzInt * m_config.heightmapSize + hxInt];
}

glm::vec3 Terrain::getNormalAt(float worldX, float worldZ) const {
    float delta = 1.0f;
    
    float hL = getHeightAt(worldX - delta, worldZ);
    float hR = getHeightAt(worldX + delta, worldZ);
    float hD = getHeightAt(worldX, worldZ - delta);
    float hU = getHeightAt(worldX, worldZ + delta);
    
    glm::vec3 normal(hL - hR, 2.0f * delta, hD - hU);
    return glm::normalize(normal);
}

bool Terrain::isLoadedAt(float worldX, float worldZ) const {
    int chunkX, chunkY;
    getChunkCoords(worldX, worldZ, chunkX, chunkY);
    
    int key = getChunkKey(chunkX, chunkY);
    return m_chunks.find(key) != m_chunks.end();
}

int Terrain::getChunkKey(int chunkX, int chunkY) const {
    // Combine two ints into one key for map lookup
    return (chunkX << 16) | (chunkY & 0xFFFF);
}

TerrainChunk* Terrain::getOrCreateChunk(int chunkX, int chunkY) {
    int key = getChunkKey(chunkX, chunkY);
    
    auto it = m_chunks.find(key);
    if (it != m_chunks.end()) {
        return it->second.get();
    }
    
    // Create new chunk
    auto chunk = std::make_unique<TerrainChunk>(chunkX, chunkY, m_config.chunkSize, m_config.chunkResolution);
    chunk->generateHeightmap(m_heightmap, m_config.heightmapSize);
    
    TerrainChunk* ptr = chunk.get();
    m_chunks[key] = std::move(chunk);
    
    return ptr;
}

void Terrain::removeChunk(int chunkX, int chunkY) {
    int key = getChunkKey(chunkX, chunkY);
    m_chunks.erase(key);
}

void Terrain::getChunkCoords(float worldX, float worldZ, int& chunkX, int& chunkY) const {
    chunkX = (int)floor(worldX / m_config.chunkSize);
    chunkY = (int)floor(worldZ / m_config.chunkSize);
}
