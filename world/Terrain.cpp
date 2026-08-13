#include "Terrain.h"
#include "TerrainOptimizations.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <algorithm>
#include <iostream>

#include "shaderSystem/stb_image.h"

// Shared with the terrain shaders (TerrainChunk.cpp).
extern const char* kTerrainDesertTexPath;
extern float g_terrainTexScale;

// ... (PerlinNoise class remains the same)

class PerlinNoise {
public:
    PerlinNoise(unsigned int seed = 12345) {
        for (int i = 0; i < 256; i++) p[i] = i;
        for (int i = 255; i > 0; i--) {
            int j = seed % (i + 1);
            std::swap(p[i], p[j]);
            seed = seed * 1103515245 + 12345;
        }
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

Terrain::Terrain() : m_config() {}
Terrain::Terrain(const TerrainConfig& config) : m_config(config) {}

Terrain::~Terrain() {
    if (m_running) {
        m_running = false;
        m_queueCV.notify_all();
        if (m_workerThread.joinable()) m_workerThread.join();
    }
    // Texture cleanup must happen while the GL context is alive. The editor
    // tears terrain down before glfwTerminate (see Editor::shutdown).
    if (m_bakeFBO != 0) {
        glDeleteFramebuffers(1, &m_bakeFBO);
        m_bakeFBO = 0;
    }
    if (m_materialAtlas != 0) {
        glDeleteTextures(1, &m_materialAtlas);
        m_materialAtlas = 0;
    }
    if (m_heightTexture != 0) {
        glDeleteTextures(1, &m_heightTexture);
        m_heightTexture = 0;
    }
    if (m_desertTex != 0) {
        glDeleteTextures(1, &m_desertTex);
        m_desertTex = 0;
    }
}

void Terrain::initialize() {
    std::cout << "[Terrain] Initializing (async chunk loading)...\n";
    std::cout << "  Chunk size: " << m_config.chunkSize << "m\n";
    std::cout << "  Chunk resolution: " << m_config.chunkResolution << "x" << m_config.chunkResolution << "\n";
    std::cout << "  View distance: " << m_config.viewDistance << " chunks\n";

    generateHeightmap();
    m_initialized = true;

    // Upload the master heightmap as a GPU texture. The vertex shader displaces
    // every chunk's static grid by sampling this (GL_LINEAR = the same continuous
    // bilinear surface the CPU physics queries, so foot-planting always matches
    // the rendered terrain).
    glGenTextures(1, &m_heightTexture);
    glBindTexture(GL_TEXTURE_2D, m_heightTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, m_config.heightmapSize, m_config.heightmapSize,
                 0, GL_RED, GL_FLOAT, m_heightmap.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    std::cout << "[Terrain] Heightmap texture uploaded (" << m_config.heightmapSize
              << "x" << m_config.heightmapSize << " R32F)\n";

    // Dry-desert albedo: tiling rock texture that drives the terrain material
    // (Rock1_Diffuse.png - warm desert rock). Tiles every g_terrainTexScale m.
    {
        stbi_set_flip_vertically_on_load(true);
        int w = 0, h = 0, n = 0;
        unsigned char* px = stbi_load(kTerrainDesertTexPath, &w, &h, &n, 3);
        if (px && w > 0 && h > 0) {
            glGenTextures(1, &m_desertTex);
            glBindTexture(GL_TEXTURE_2D, m_desertTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
            glGenerateMipmap(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, 0);
            std::cout << "[Terrain] Desert albedo loaded: " << kTerrainDesertTexPath
                      << " (" << w << "x" << h << ", tile=" << g_terrainTexScale << "m)\n";
        } else {
            std::cerr << "[Terrain] Failed to load desert albedo: "
                      << kTerrainDesertTexPath << "\n";
        }
        if (px) stbi_image_free(px);
    }

    // RVT material atlas: multi-layer auto-material baked per chunk page.
    glGenTextures(1, &m_materialAtlas);
    glBindTexture(GL_TEXTURE_2D, m_materialAtlas);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kMaterialAtlasSize, kMaterialAtlasSize,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &m_bakeFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_bakeFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           m_materialAtlas, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[Terrain] Material bake FBO incomplete - RVT disabled\n";
        glDeleteFramebuffers(1, &m_bakeFBO);
        m_bakeFBO = 0;
        glDeleteTextures(1, &m_materialAtlas);
        m_materialAtlas = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (m_materialAtlas != 0) {
        std::cout << "[Terrain] RVT material atlas ready (" << kMaterialAtlasSize
                  << "x" << kMaterialAtlasSize << ", " << getMaterialPageCount()
                  << " pages)\n";
    }

    m_running = true;
    m_workerThread = std::thread(&Terrain::workerThread, this);

    std::cout << "[Terrain] Async loading thread started\n";
    std::cout << "[Terrain] Initialized successfully!\n";
}

void Terrain::generateHeightmap() {
    m_heightmap.resize(m_config.heightmapSize * m_config.heightmapSize);
    PerlinNoise perlin(42);

    float scale1 = 0.003f;
    float scale2 = 0.01f;
    float scale3 = 0.03f;

    for (int y = 0; y < m_config.heightmapSize; y++) {
        for (int x = 0; x < m_config.heightmapSize; x++) {
            float height = perlin.octaveNoise(x * scale1, y * scale1, 4, 0.5f) * 0.6f;
            height += perlin.octaveNoise(x * scale2, y * scale2, 3, 0.5f) * 0.3f;
            height += perlin.octaveNoise(x * scale3, y * scale3, 2, 0.5f) * 0.1f;
            height = (height + 1.0f) * 0.5f;
            m_heightmap[y * m_config.heightmapSize + x] = height * m_config.heightScale;
        }
    }

    std::cout << "[Terrain] Generated heightmap: " << m_config.heightmapSize << "x"
              << m_config.heightmapSize << "\n";
}

void Terrain::workerThread() {
    while (m_running) {
        ChunkCoord coord;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCV.wait(lock, [this]{ return !m_pendingRequests.empty() || !m_running; });
            if (!m_running && m_pendingRequests.empty()) return;
            coord = m_pendingRequests.front();
            m_pendingRequests.pop();
        }

        PendingChunk data = generateChunkData(coord.x, coord.y);

        {
            std::lock_guard<std::mutex> lock(m_completedMutex);
            m_completedChunks.push(std::move(data));
        }
    }
}

PendingChunk Terrain::generateChunkData(int chunkX, int chunkY) {
    PendingChunk result;
    result.chunkX = chunkX;
    result.chunkY = chunkY;
    result.resolution = m_config.chunkResolution;
    result.chunkSize = m_config.chunkSize;

    int vertsPerSide = m_config.chunkResolution + 1;
    result.heights.resize(vertsPerSide * vertsPerSide);

    float step = m_config.chunkSize / m_config.chunkResolution;
    float worldStartX = chunkX * m_config.chunkSize;
    float worldStartZ = chunkY * m_config.chunkSize;

    for (int z = 0; z < vertsPerSide; z++) {
        for (int x = 0; x < vertsPerSide; x++) {
            float worldX = worldStartX + x * step;
            float worldZ = worldStartZ + z * step;
            // Heightmap texel space maps 1:1 to world meters (texel i sits at
            // world x=i). The old worldX/heightmapSize mapping collapsed the
            // entire world onto texel 0, rendering flat terrain.
            const int hxInt = terrain::worldToTexel(worldX, m_config.heightmapSize);
            const int hzInt = terrain::worldToTexel(worldZ, m_config.heightmapSize);
            result.heights[z * vertsPerSide + x] = m_heightmap[hzInt * m_config.heightmapSize + hxInt];
        }
    }

    return result;
}

int Terrain::bakeChunkMaterial(const TerrainChunk& chunk) {
    // RVT-style bake: render the chunk's multi-layer auto-material into its
    // atlas page once. Requires the bake program + atlas (GL alive, main thread).
    if (m_materialAtlas == 0 || g_terrainBakeProgram == 0 || !m_initialized) return -1;
    if (m_heightTexture == 0) return -1;

    ChunkCoord coord{chunk.getChunkX(), chunk.getChunkY()};
    int page = -1;
    auto it = m_chunkPage.find(coord);
    if (it != m_chunkPage.end()) {
        page = it->second;
    } else {
        page = m_nextFreePage;
        m_nextFreePage = (m_nextFreePage + 1) % getMaterialPageCount();
        // Evict any previous owner of this recycled page so two chunks never
        // sample the same baked material.
        for (auto pit = m_chunkPage.begin(); pit != m_chunkPage.end(); ) {
            if (pit->second == page) pit = m_chunkPage.erase(pit);
            else ++pit;
        }
        m_chunkPage[coord] = page;
    }

    const int px = page % kMaterialPagesPerSide;
    const int py = page / kMaterialPagesPerSide;

    // Ortho camera spanning the chunk's XZ footprint, mapped onto the page.
    const glm::vec3 origin = chunk.getWorldPosition();
    glm::mat4 proj = glm::ortho(origin.x, origin.x + m_config.chunkSize,
                                origin.z + m_config.chunkSize, origin.z,
                                -10.0f, 100.0f);

    // Save/restore the viewport: the bake targets a 128x128 page, and leaking
    // that viewport into the main pass would shrink subsequent renders.
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLboolean wasBlend = GL_FALSE;
    glGetBooleanv(GL_BLEND, &wasBlend);

    glBindFramebuffer(GL_FRAMEBUFFER, m_bakeFBO);
    glViewport(px * kMaterialPageSize, py * kMaterialPageSize,
               kMaterialPageSize, kMaterialPageSize);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);   // never blend the bake into a page

    glUseProgram(g_terrainBakeProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_heightTexture);
    if (m_desertTex != 0 && g_terrainBakeDesertTex >= 0) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_desertTex);
        glActiveTexture(GL_TEXTURE0);
    }
    glUniformMatrix4fv(g_terrainBakeView, 1, GL_FALSE, &glm::mat4(1.0f)[0][0]);
    glUniformMatrix4fv(g_terrainBakeProj, 1, GL_FALSE, &proj[0][0]);
    glUniform2f(g_terrainBakeHeightMapSize,
                (float)m_config.heightmapSize, (float)m_config.heightmapSize);
    glUniform1f(g_terrainBakeWaterLevel, 5.0f);

    chunk.draw();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    if (wasBlend) glEnable(GL_BLEND);
    return page;
}

void Terrain::commitChunks() {
    // Drain the completed queue under the lock, then do the GL work (chunk
    // creation + RVT bake) outside it so the worker never blocks on the bake.
    std::vector<PendingChunk> batch;
    {
        std::lock_guard<std::mutex> lock(m_completedMutex);
        while (!m_completedChunks.empty()) {
            batch.push_back(std::move(m_completedChunks.front()));
            m_completedChunks.pop();
        }
    }

    for (PendingChunk& data : batch) {
        ChunkCoord coord{data.chunkX, data.chunkY};
        // The synchronous first fill may already have created this chunk (the
        // coord was queued for the worker before the fill ran) - don't
        // regenerate + re-bake a duplicate.
        if (m_chunks.count(coord)) continue;

        auto chunk = std::make_unique<TerrainChunk>(data.chunkX, data.chunkY, data.chunkSize, data.resolution);
        chunk->generateHeightmapFromData(std::move(data.heights));
        bakeChunkMaterial(*chunk);   // RVT bake (main thread, GL alive)
        m_chunks[coord] = std::move(chunk);
    }
}

void Terrain::update(const glm::vec3& cameraPos, float dt) {
    if (!m_initialized) return;

    // Commit finished chunks (GPU upload - main thread only)
    commitChunks();

    int camChunkX, camChunkY;
    getChunkCoords(cameraPos.x, cameraPos.z, camChunkX, camChunkY);
    ChunkCoord camChunk{camChunkX, camChunkY};

    if (camChunk == m_lastChunk) {
        for (auto& [coord, chunk] : m_chunks) {
            chunk->updateLOD(cameraPos, m_config.lodDistance);
        }
        return;
    }

    m_lastChunk = camChunk;

    // Determine desired chunks and request missing ones
    {
        std::lock_guard<std::mutex> lock(m_knownMutex);

        for (int dy = -m_config.viewDistance; dy <= m_config.viewDistance; dy++) {
            for (int dx = -m_config.viewDistance; dx <= m_config.viewDistance; dx++) {
                float dist = sqrt(dx * dx + dy * dy);
                if (dist > m_config.viewDistance) continue;

                ChunkCoord coord{camChunkX + dx, camChunkY + dy};
                if (m_knownChunks.count(coord)) continue;

                m_knownChunks.insert(coord);

                std::lock_guard<std::mutex> qlock(m_queueMutex);
                m_pendingRequests.push(coord);
                m_queueCV.notify_one();
            }
        }

        // Remove far chunks from tracking
        std::vector<ChunkCoord> toRemove;
        for (const auto& coord : m_knownChunks) {
            int dx = coord.x - camChunkX;
            int dy = coord.y - camChunkY;
            float dist = sqrt(dx * dx + dy * dy);
            if (dist > m_config.viewDistance + 2) {
                toRemove.push_back(coord);
            }
        }
        for (const auto& coord : toRemove) {
            m_knownChunks.erase(coord);
            m_chunks.erase(coord);
        }
    }

    // Synchronous first fill: the async worker takes a moment to generate the
    // initial ring, which left the terrain missing (pure sky) for the first
    // ~second of a cinematic intro. Generate the ring on the main thread the
    // first time so the terrain is present from frame one.
    if (m_chunks.empty()) {
        for (int dy = -m_config.viewDistance; dy <= m_config.viewDistance; dy++) {
            for (int dx = -m_config.viewDistance; dx <= m_config.viewDistance; dx++) {
                float dist = sqrt(dx * dx + dy * dy);
                if (dist > m_config.viewDistance) continue;

                ChunkCoord coord{camChunkX + dx, camChunkY + dy};
                if (m_chunks.count(coord)) continue;

                PendingChunk data = generateChunkData(coord.x, coord.y);
                auto chunk = std::make_unique<TerrainChunk>(coord.x, coord.y,
                                                            data.chunkSize, data.resolution);
                chunk->generateHeightmapFromData(std::move(data.heights));
                bakeChunkMaterial(*chunk);   // RVT bake
                m_chunks[coord] = std::move(chunk);
            }
        }
    }

    // Update LOD
    for (auto& [coord, chunk] : m_chunks) {
        chunk->updateLOD(cameraPos, m_config.lodDistance);
    }
}

void Terrain::render(const glm::mat4& view, const glm::mat4& projection,
                     const glm::vec3& cameraPos, float fovDegrees, float aspectRatio,
                     float nearPlane, float farPlane) const {
    if (!m_initialized) return;

    // Bind the master heightmap for GPU displacement (unit 0, matches the
    // sampler bound once in initTerrainShader).
    if (m_heightTexture != 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_heightTexture);
    }

    // Use robust frustum culler from optimizations
    TerrainOptimizations::FrustumCuller culler;
    culler.update(projection * view);

    // Collect visible chunks
    std::vector<const TerrainChunk*> visibleChunks;
    visibleChunks.reserve(m_chunks.size());

    for (const auto& [coord, chunk] : m_chunks) {
        // Use AABB culling for better accuracy
        if (!culler.isBoxInFrustum(
                (chunk->getBoundsMin() + chunk->getBoundsMax()) * 0.5f,
                (chunk->getBoundsMax() - chunk->getBoundsMin()) * 0.5f)) {
            continue;
        }
        
        if (chunk->getLOD() >= 3) continue;
        visibleChunks.push_back(chunk.get());
    }

    if (visibleChunks.empty()) return;

    // Sort by distance
    std::sort(visibleChunks.begin(), visibleChunks.end(),
        [&cameraPos](const TerrainChunk* a, const TerrainChunk* b) {
            return a->getDistanceToCamera(cameraPos) < b->getDistanceToCamera(cameraPos);
        });

    // Batch: set shader state once, then draw all chunks
    initTerrainShader();
    glUseProgram(g_terrainShader);

    glUniformMatrix4fv(g_terrainUniforms[TU_VIEW], 1, GL_FALSE, &view[0][0]);
    glUniformMatrix4fv(g_terrainUniforms[TU_PROJECTION], 1, GL_FALSE, &projection[0][0]);
    glUniform3f(g_terrainUniforms[TU_LIGHT_DIR], 0.5f, 0.8f, 0.3f);
    glUniform3f(g_terrainUniforms[TU_VIEW_POS], cameraPos.x, cameraPos.y, cameraPos.z);
    glUniform3f(g_terrainUniforms[TU_FOG_COLOR], 0.5f, 0.6f, 0.8f);
    glUniform1f(g_terrainUniforms[TU_FOG_NEAR], 150.0f);
    glUniform1f(g_terrainUniforms[TU_FOG_FAR], 900.0f);
    glUniform2f(g_terrainUniforms[TU_HEIGHT_MAP_SIZE],
                (float)m_config.heightmapSize, (float)m_config.heightmapSize);
    glUniform1f(g_terrainUniforms[TU_LOD_DISTANCE], m_config.lodDistance);
    glUniform1f(g_terrainUniforms[TU_WATER_LEVEL], 5.0f);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Bind the material atlas on unit 1 (matches the sampler set once in
    // initTerrainShader). Chunks with a baked page sample it; others fall back
    // to live layer blending.
    if (m_materialAtlas != 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_materialAtlas);
    }

    // Desert albedo on unit 2 (sampled by the live layer path + bake).
    if (m_desertTex != 0) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_desertTex);
    }

    const float pageScale = (float)kMaterialPageSize / (float)kMaterialAtlasSize;
    for (const auto* chunk : visibleChunks) {
        // Per-chunk RVT uniforms.
        auto pageIt = m_chunkPage.find(ChunkCoord{chunk->getChunkX(), chunk->getChunkY()});
        if (m_materialAtlas != 0 && pageIt != m_chunkPage.end()) {
            const int px = pageIt->second % kMaterialPagesPerSide;
            const int py = pageIt->second / kMaterialPagesPerSide;
            glUniform2f(g_terrainUniforms[TU_ATLAS_PAGE],
                        (float)px * pageScale, (float)py * pageScale);
            const glm::vec3 origin = chunk->getWorldPosition();
            glUniform2f(g_terrainUniforms[TU_CHUNK_ORIGIN], origin.x, origin.z);
            glUniform1f(g_terrainUniforms[TU_CHUNK_SIZE], m_config.chunkSize);
            glUniform1f(g_terrainUniforms[TU_USE_ATLAS], 1.0f);
        } else {
            glUniform1f(g_terrainUniforms[TU_USE_ATLAS], 0.0f);
        }
        chunk->draw();
    }
}

float Terrain::getHeightAt(float worldX, float worldZ) const {
    if (!m_initialized) return 0.0f;

    int chunkX, chunkY;
    getChunkCoords(worldX, worldZ, chunkX, chunkY);
    ChunkCoord coord{chunkX, chunkY};

    auto it = m_chunks.find(coord);
    if (it != m_chunks.end()) {
        float localX = fmod(worldX, m_config.chunkSize);
        if (localX < 0) localX += m_config.chunkSize;
        float localZ = fmod(worldZ, m_config.chunkSize);
        if (localZ < 0) localZ += m_config.chunkSize;
        return it->second->getHeightAt(localX, localZ);
    }

    // Fallback (chunk not loaded yet): bilinear sample of the master heightmap
    // with the same 1-texel-per-meter mapping the loaded-chunk path and the GPU
    // displacement shader use, so loaded and unloaded regions agree exactly.
    return terrain::sampleHeightBilinear(m_heightmap.data(),
                                         m_config.heightmapSize, worldX, worldZ);
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
    ChunkCoord coord{chunkX, chunkY};
    return m_chunks.find(coord) != m_chunks.end();
}

void Terrain::getChunkCoords(float worldX, float worldZ, int& chunkX, int& chunkY) const {
    chunkX = (int)floor(worldX / m_config.chunkSize);
    chunkY = (int)floor(worldZ / m_config.chunkSize);
}
