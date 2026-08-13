#include "TerrainChunk.h"
#include <cmath>
#include <algorithm>
#include <iostream>

GLuint g_terrainShader = 0;
GLint g_terrainUniforms[TU_COUNT];

GLuint g_terrainBakeProgram = 0;
GLint g_terrainBakeView = -1, g_terrainBakeProj = -1, g_terrainBakeHeightMap = -1,
      g_terrainBakeHeightMapSize = -1, g_terrainBakeWaterLevel = -1;
GLint g_terrainBakeDesertTex = -1;

// Desert albedo texture path + world scale (loaded by Terrain once, shared
// by both the main and bake programs). The boulder albedo (Namaqualand dry
// desert rock) gives the terrain its warm desert look.
const char* kTerrainDesertTexPath =
    "assets/boulder/textures/namaqualand_boulder_02_diff_4k.jpg";
float g_terrainTexScale = 8.0f;   // world meters per texture tile

// Shared displacement vertex shader (used by both the main and bake programs).
static const char* kTerrainVS = R"(
    #version 330 core
    layout(location = 0) in vec3 aPos;   // world XZ grid position (y unused)

    out vec3 vWorldPos;
    out float vHeight;

    uniform mat4 uView;
    uniform mat4 uProjection;
    uniform sampler2D uHeightMap;
    uniform vec2 uHeightMapSize;
    uniform vec3 uViewPos;
    uniform float uLodDistance;

    void main() {
        // Static grid: the UV is derived from the world position, so the same
        // mesh works for any heightmap size (pure CDLOD - one grid, GPU displaces).
        // The +0.5 centers texel i on world coordinate i, matching the CPU
        // physics sampler exactly.
        vec2 uv = (aPos.xz + 0.5) / uHeightMapSize;

        // Fine height: direct bilinear (linear-filtered) sample of the
        // continuous heightfield - identical to CPU physics sampling.
        float hFine = texture(uHeightMap, uv).r;

        // Coarse height: sample the heightfield at the grid point of the
        // parent LOD ring. step doubles per LOD level.
        float dist = distance(aPos.xz, uViewPos.xz);
        float lodf = clamp(log2(max(1.0, dist / max(uLodDistance, 0.001))), 0.0, 3.0);
        int lod = int(floor(lodf));
        float t = fract(lodf);
        float morph = t * t * (3.0 - 2.0 * t);   // smoothstep

        float step = pow(2.0, float(lod + 1));
        vec2 coarseUv = (floor(uv * uHeightMapSize / step) * step + 0.5) / uHeightMapSize;
        float hCoarse = texture(uHeightMap, coarseUv).r;

        float h = mix(hFine, hCoarse, morph);

        vWorldPos = vec3(aPos.x, h, aPos.z);
        vHeight = h;
        gl_Position = uProjection * uView * vec4(vWorldPos, 1.0);
    }
)";

// Multi-layer auto-material (shared by bake + main passes).
//  - dry-desert palette driven by the tiling rock albedo texture, tinted by
//    height band: warm sand in the lowlands, sun-bleached rock on peaks,
//    raw rock on steep slopes (sand/soil peel off).
//  - Returns the unlit albedo color.
static const char* kTerrainMaterialGLSL = R"(
    uniform sampler2D uDesertTex;
    uniform float uTexScale;   // world meters per texture tile

    vec3 terrainMaterial(float height, vec3 normal, vec3 worldPos, float waterLevel) {
        float slope = 1.0 - clamp(normal.y, 0.0, 1.0);

        // Tile the rock albedo across the world in world space.
        vec2 uv = worldPos.xz / max(uTexScale, 0.001);
        vec3 rock = texture(uDesertTex, uv).rgb;
        // Slightly desaturate so lighting doesn't wash out the band tints.
        rock = mix(rock, vec3(dot(rock, vec3(0.299, 0.587, 0.114))), 0.35);

        // Dry-desert height bands: warm sand low, sparse dry grass, rock,
        // sun-bleached rock on the peaks.
        vec3 sand = rock * vec3(1.25, 1.12, 0.82);
        vec3 dryGrass = rock * vec3(0.85, 0.92, 0.65);
        vec3 rockTint = rock * vec3(1.02, 0.99, 0.94);
        vec3 bleach = rock * vec3(1.12, 1.10, 1.04);

        vec3 color = sand;
        float t = smoothstep(waterLevel - 1.0, waterLevel + 2.0, height);
        color = mix(color, dryGrass, t);
        t = smoothstep(waterLevel + 4.0, waterLevel + 14.0, height);
        color = mix(color, rockTint, t);
        t = smoothstep(waterLevel + 22.0, waterLevel + 30.0, height);
        color = mix(color, bleach, t);

        // Steep slopes expose raw rock (soil/sand peel off)
        color = mix(color, rock, smoothstep(0.55, 0.85, slope));
        return color;
    }
)";

// Compile a program from the shared terrain VS and a fragment source.
// Returns 0 on failure (logs the error).
static GLuint CompileTerrainProgram(const char* fsSource) {
    GLuint vert = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vert, 1, &kTerrainVS, nullptr);
    glCompileShader(vert);

    GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(frag, 1, &fsSource, nullptr);
    glCompileShader(frag);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);

    int ok = 0;
    glGetShaderiv(vert, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(vert, sizeof(log), nullptr, log);
        std::cerr << "[TerrainShader] VS error: " << log << "\n";
    }
    glGetShaderiv(frag, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(frag, sizeof(log), nullptr, log);
        std::cerr << "[TerrainShader] FS error: " << log << "\n";
    }
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::cerr << "[TerrainShader] Link error: " << log << "\n";
    }

    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}

void initTerrainShader() {
    if (g_terrainShader != 0) return;

    // ------------------------------------------------------------------
    // GPU terrain: a static flat grid is displaced by sampling the master
    // heightmap texture. Vertices in the LOD transition band are smoothly
    // morphed (CDLOD-style geomorphing) so LOD switches never pop.
    // ------------------------------------------------------------------

    // Main fragment shader: samples the RVT-baked material atlas when a page
    // is available (single texture fetch, Unreal-style), else falls back to
    // computing the layers live. Lighting + fog stay per-pixel.
    // (kTerrainMaterialGLSL is prepended after the #version line below, so the
    // raw body starts after it.)
    const char* fs = R"(
        in vec3 vWorldPos;
        in float vHeight;

        out vec4 fragColor;

        uniform vec3 uLightDir;
        uniform vec3 uViewPos;
        uniform vec3 uFogColor;
        uniform float uFogNear;
        uniform float uFogFar;
        uniform float uWaterLevel;

        uniform sampler2D uMaterialAtlas;
        uniform vec2 uAtlasPage;     // page origin in atlas UV space
        uniform vec2 uChunkOrigin;   // chunk min corner (XZ)
        uniform float uChunkSize;
        uniform float uUseAtlas;

        void main() {
            vec3 normal = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));

            vec3 color;
            if (uUseAtlas > 0.5) {
                // Sample the pre-baked material from the RVT page (one fetch).
                vec2 local = (vWorldPos.xz - uChunkOrigin) / uChunkSize;
                vec2 uv = uAtlasPage + local * (128.0 / 4096.0);
                color = texture(uMaterialAtlas, uv).rgb;
            } else {
                color = terrainMaterial(vHeight, normal, vWorldPos, uWaterLevel);
            }

            vec3 L = normalize(uLightDir);
            float diff = max(dot(normal, L), 0.0);
            float ambient = 0.35;
            float lighting = ambient + diff * 0.65;
            vec3 lit = color * lighting;

            float dist = distance(vWorldPos, uViewPos);
            float fog = clamp((dist - uFogNear) / (uFogFar - uFogNear), 0.0, 1.0);
            lit = mix(lit, uFogColor, fog * 0.7);

            fragColor = vec4(lit, 1.0);
        }
    )";

    // The shared multi-layer material (with the desert albedo sampler) must
    // come before the main body, which calls terrainMaterial(). #version must
    // stay on the first line.
    std::string fsFull = std::string("#version 330 core\n") +
                         kTerrainMaterialGLSL + fs;
    g_terrainShader = CompileTerrainProgram(fsFull.c_str());
    if (g_terrainShader == 0) return;

    g_terrainUniforms[TU_VIEW]          = glGetUniformLocation(g_terrainShader, "uView");
    g_terrainUniforms[TU_PROJECTION]    = glGetUniformLocation(g_terrainShader, "uProjection");
    g_terrainUniforms[TU_LIGHT_DIR]     = glGetUniformLocation(g_terrainShader, "uLightDir");
    g_terrainUniforms[TU_VIEW_POS]      = glGetUniformLocation(g_terrainShader, "uViewPos");
    g_terrainUniforms[TU_FOG_COLOR]     = glGetUniformLocation(g_terrainShader, "uFogColor");
    g_terrainUniforms[TU_FOG_NEAR]      = glGetUniformLocation(g_terrainShader, "uFogNear");
    g_terrainUniforms[TU_FOG_FAR]       = glGetUniformLocation(g_terrainShader, "uFogFar");
    g_terrainUniforms[TU_HEIGHT_MAP]    = glGetUniformLocation(g_terrainShader, "uHeightMap");
    g_terrainUniforms[TU_HEIGHT_MAP_SIZE] = glGetUniformLocation(g_terrainShader, "uHeightMapSize");
    g_terrainUniforms[TU_LOD_DISTANCE]  = glGetUniformLocation(g_terrainShader, "uLodDistance");
    g_terrainUniforms[TU_WATER_LEVEL]   = glGetUniformLocation(g_terrainShader, "uWaterLevel");
    g_terrainUniforms[TU_MATERIAL_ATLAS]= glGetUniformLocation(g_terrainShader, "uMaterialAtlas");
    g_terrainUniforms[TU_ATLAS_PAGE]    = glGetUniformLocation(g_terrainShader, "uAtlasPage");
    g_terrainUniforms[TU_CHUNK_ORIGIN]  = glGetUniformLocation(g_terrainShader, "uChunkOrigin");
    g_terrainUniforms[TU_CHUNK_SIZE]    = glGetUniformLocation(g_terrainShader, "uChunkSize");
    g_terrainUniforms[TU_USE_ATLAS]     = glGetUniformLocation(g_terrainShader, "uUseAtlas");
    g_terrainUniforms[TU_DESERT_TEX]    = glGetUniformLocation(g_terrainShader, "uDesertTex");
    g_terrainUniforms[TU_TEX_SCALE]     = glGetUniformLocation(g_terrainShader, "uTexScale");

    // Bind samplers once (program is shared): heightmap=0, atlas=1, desert=2.
    glUseProgram(g_terrainShader);
    glUniform1i(g_terrainUniforms[TU_HEIGHT_MAP], 0);
    glUniform1i(g_terrainUniforms[TU_MATERIAL_ATLAS], 1);
    glUniform1i(g_terrainUniforms[TU_DESERT_TEX], 2);
    glUniform1f(g_terrainUniforms[TU_TEX_SCALE], g_terrainTexScale);
    glUseProgram(0);

    // ------------------------------------------------------------------
    // Bake program: renders the multi-layer auto-material into an atlas page.
    // Same displacement VS; the FS writes the unlit albedo.
    // ------------------------------------------------------------------
    const char* bakeFS = R"(
        in vec3 vWorldPos;
        in float vHeight;

        out vec4 fragColor;

        uniform float uWaterLevel;

        void main() {
            vec3 normal = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));
            vec3 color = terrainMaterial(vHeight, normal, vWorldPos, uWaterLevel);
            fragColor = vec4(color, 1.0);
        }
    )";

    std::string bakeFull = std::string("#version 330 core\n") +
                           kTerrainMaterialGLSL + bakeFS;
    g_terrainBakeProgram = CompileTerrainProgram(bakeFull.c_str());
    if (g_terrainBakeProgram != 0) {
        g_terrainBakeView = glGetUniformLocation(g_terrainBakeProgram, "uView");
        g_terrainBakeProj = glGetUniformLocation(g_terrainBakeProgram, "uProjection");
        g_terrainBakeHeightMap = glGetUniformLocation(g_terrainBakeProgram, "uHeightMap");
        g_terrainBakeHeightMapSize = glGetUniformLocation(g_terrainBakeProgram, "uHeightMapSize");
        g_terrainBakeWaterLevel = glGetUniformLocation(g_terrainBakeProgram, "uWaterLevel");
        g_terrainBakeDesertTex = glGetUniformLocation(g_terrainBakeProgram, "uDesertTex");
        glUseProgram(g_terrainBakeProgram);
        glUniform1i(g_terrainBakeHeightMap, 0);
        glUniform1i(g_terrainBakeDesertTex, 2);
        glUniform1f(glGetUniformLocation(g_terrainBakeProgram, "uTexScale"), g_terrainTexScale);
        glUseProgram(0);
    }
}

TerrainChunk::TerrainChunk(int chunkX, int chunkY, float chunkSize, int resolution)
    : m_chunkX(chunkX), m_chunkY(chunkY), m_chunkSize(chunkSize), m_resolution(resolution)
{
    m_boundsMin = glm::vec3(m_chunkX * m_chunkSize, -10.0f, m_chunkY * m_chunkSize);
    m_boundsMax = glm::vec3((m_chunkX + 1) * m_chunkSize, 100.0f, (m_chunkY + 1) * m_chunkSize);
}

TerrainChunk::~TerrainChunk() {
    if (m_VAO != 0) glDeleteVertexArrays(1, &m_VAO);
    if (m_VBO != 0) glDeleteBuffers(1, &m_VBO);
    if (m_EBO != 0) glDeleteBuffers(1, &m_EBO);
}

glm::vec3 TerrainChunk::getWorldPosition() const {
    return glm::vec3(m_chunkX * m_chunkSize, 0.0f, m_chunkY * m_chunkSize);
}

float TerrainChunk::getDistanceToCamera(const glm::vec3& cameraPos) const {
    glm::vec3 center = getWorldPosition() + glm::vec3(m_chunkSize / 2.0f, 0.0f, m_chunkSize / 2.0f);
    center.y = cameraPos.y;
    return glm::distance(cameraPos, center);
}

float TerrainChunk::getHeightAt(float localX, float localZ) const {
    // Bilinear interpolation over the 2x2 height cell surrounding (localX,
    // localZ): the heightfield is a continuous mathematical surface, so
    // sampling it linearly gives smooth foot-planting instead of blocky
    // nearest-neighbor steps. Shared with the master-heightmap fallback and
    // the GPU displacement shader via terrain:: (see TerrainChunk.h).
    if (m_heights.empty()) return 0.0f;
    return terrain::sampleChunkBilinear(m_heights.data(), m_resolution,
                                        localX, localZ, m_chunkSize);
}

void TerrainChunk::generateHeightmap(const std::vector<float>& heightmap, int heightmapSize) {
    m_heights.clear();
    m_heights.resize((m_resolution + 1) * (m_resolution + 1));

    int vertsPerSide = m_resolution + 1;
    float step = m_chunkSize / m_resolution;
    float worldStartX = m_chunkX * m_chunkSize;
    float worldStartZ = m_chunkY * m_chunkSize;

    // Heightmap texel space maps 1:1 to world meters (texel i sits at world
    // x=i) - same mapping as Terrain::generateChunkData and the GPU shader.
    for (int z = 0; z < vertsPerSide; z++) {
        for (int x = 0; x < vertsPerSide; x++) {
            const float worldX = worldStartX + x * step;
            const float worldZ = worldStartZ + z * step;
            const int hxInt = terrain::worldToTexel(worldX, heightmapSize);
            const int hzInt = terrain::worldToTexel(worldZ, heightmapSize);
            m_heights[z * vertsPerSide + x] = heightmap[hzInt * heightmapSize + hxInt];
        }
    }

    m_loaded = true;
    createMesh();
}

void TerrainChunk::generateHeightmapFromData(std::vector<float> heights) {
    m_heights = std::move(heights);
    m_loaded = true;
    createMesh();
}

void TerrainChunk::createMesh() {
    if (!m_loaded) return;

    m_vertices.clear();
    m_indices.clear();

    int vertsPerSide = m_resolution + 1;
    float step = m_chunkSize / m_resolution;
    float worldStartX = m_chunkX * m_chunkSize;
    float worldStartZ = m_chunkY * m_chunkSize;

    m_vertices.resize(vertsPerSide * vertsPerSide);

    for (int z = 0; z < vertsPerSide; z++) {
        for (int x = 0; x < vertsPerSide; x++) {
            TerrainVertex& v = m_vertices[z * vertsPerSide + x];
            v.position.x = worldStartX + x * step;
            v.position.z = worldStartZ + z * step;
            v.position.y = 0.0f;  // displaced in the vertex shader (UV derived in-shader)
        }
    }

    for (int z = 0; z < m_resolution; z++) {
        for (int x = 0; x < m_resolution; x++) {
            int topLeft = z * vertsPerSide + x;
            int topRight = topLeft + 1;
            int bottomLeft = (z + 1) * vertsPerSide + x;
            int bottomRight = bottomLeft + 1;

            m_indices.push_back(topLeft);
            m_indices.push_back(bottomLeft);
            m_indices.push_back(topRight);

            m_indices.push_back(topRight);
            m_indices.push_back(bottomLeft);
            m_indices.push_back(bottomRight);
        }
    }

    updateBounds();

    if (m_VAO == 0) {
        glGenVertexArrays(1, &m_VAO);
        glGenBuffers(1, &m_VBO);
        glGenBuffers(1, &m_EBO);
    }

    glBindVertexArray(m_VAO);

    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, m_vertices.size() * sizeof(TerrainVertex),
                 m_vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indices.size() * sizeof(unsigned int),
                 m_indices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex), (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

void TerrainChunk::updateBounds() {
    if (m_heights.empty()) return;

    float minHeight = m_heights[0];
    float maxHeight = m_heights[0];
    for (float h : m_heights) {
        minHeight = std::min(minHeight, h);
        maxHeight = std::max(maxHeight, h);
    }

    m_boundsMin.y = minHeight;
    m_boundsMax.y = maxHeight;
}

bool TerrainChunk::isPotentiallyVisible(const glm::vec3& cameraPos, const TerrainChunk* otherChunk) const {
    if (!otherChunk || otherChunk == this) return true;

    glm::vec3 thisCenter = getWorldPosition() + glm::vec3(m_chunkSize / 2.0f, 0.0f, m_chunkSize / 2.0f);
    glm::vec3 otherCenter = otherChunk->getWorldPosition() + glm::vec3(otherChunk->m_chunkSize / 2.0f, 0.0f, otherChunk->m_chunkSize / 2.0f);

    float distToThis = glm::distance(cameraPos, thisCenter);
    float distToOther = glm::distance(cameraPos, otherCenter);

    if (distToOther >= distToThis) return true;

    glm::vec3 dirToThis = glm::normalize(thisCenter - cameraPos);
    glm::vec3 dirToOther = glm::normalize(otherCenter - cameraPos);
    float dot = glm::dot(dirToThis, dirToOther);

    if (dot < 0.9f) return true;

    float otherMaxHeight = otherChunk->getBoundsMax().y;
    float thisMinHeight = m_boundsMin.y;
    float angleToThis = atan2(thisMinHeight - cameraPos.y, distToThis);
    float angleToOtherMax = atan2(otherMaxHeight - cameraPos.y, distToOther);

    return angleToThis > angleToOtherMax;
}

bool TerrainChunk::isVisibleInFrustum(const glm::vec3& cameraPos, float fovDegrees,
                                       float aspectRatio, float nearPlane, float farPlane) const {
    float dist = getDistanceToCamera(cameraPos);
    if (dist > farPlane) return false;

    float fovRad = glm::radians(fovDegrees);
    float tanHalfFov = tan(fovRad / 2.0f);
    glm::vec3 center = getWorldPosition() + glm::vec3(m_chunkSize / 2.0f, 0.0f, m_chunkSize / 2.0f);
    glm::vec3 toChunk = center - cameraPos;
    float distToChunk = glm::length(toChunk);

    if (distToChunk < 0.001f) return true;

    glm::vec3 dirToChunk = glm::normalize(toChunk);
    glm::vec3 cameraForward = glm::normalize(glm::vec3(dirToChunk.x, 0.0f, dirToChunk.z));
    float horizontalAngle = acos(glm::clamp(glm::dot(cameraForward, glm::vec3(0.0f, 0.0f, 1.0f)), -1.0f, 1.0f));
    float maxHorizontalAngle = atan(tanHalfFov * aspectRatio);
    float verticalAngle = acos(glm::clamp(glm::dot(dirToChunk, glm::vec3(0.0f, 1.0f, 0.0f)), -1.0f, 1.0f));
    float maxVerticalAngle = atan(tanHalfFov);

    float chunkRadius = m_chunkSize * 0.707f;
    float angularSize = atan(chunkRadius / distToChunk);

    return (horizontalAngle < maxHorizontalAngle + angularSize) &&
           (verticalAngle < maxVerticalAngle + angularSize);
}

void TerrainChunk::updateLOD(const glm::vec3& cameraPos, float lodDistance) {
    m_distanceToCamera = getDistanceToCamera(cameraPos);

    int newLOD = 0;
    if (m_distanceToCamera > lodDistance * 4.0f) newLOD = 3;
    else if (m_distanceToCamera > lodDistance * 2.0f) newLOD = 2;
    else if (m_distanceToCamera > lodDistance) newLOD = 1;

    if (newLOD != m_lod) {
        m_lod = newLOD;
        if (m_loaded) generateIndices(m_lod);
    }
}

void TerrainChunk::generateIndices(int lod) {
    m_indices.clear();

    int step = 1 << lod;
    int vertsPerSide = m_resolution + 1;

    for (int z = 0; z < m_resolution; z += step) {
        for (int x = 0; x < m_resolution; x += step) {
            int topLeft = z * vertsPerSide + x;
            int topRight = topLeft + step;
            int bottomLeft = (z + step) * vertsPerSide + x;
            int bottomRight = bottomLeft + step;

            if (topRight >= vertsPerSide * vertsPerSide ||
                bottomLeft >= vertsPerSide * vertsPerSide ||
                bottomRight >= vertsPerSide * vertsPerSide) continue;

            m_indices.push_back(topLeft);
            m_indices.push_back(bottomLeft);
            m_indices.push_back(topRight);

            m_indices.push_back(topRight);
            m_indices.push_back(bottomLeft);
            m_indices.push_back(bottomRight);
        }
    }

    if (m_EBO != 0) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indices.size() * sizeof(unsigned int),
                     m_indices.data(), GL_STATIC_DRAW);
    }
}

void TerrainChunk::draw() const {
    if (!m_loaded || m_VAO == 0 || m_lod >= 3) return;

    glBindVertexArray(m_VAO);
    glDrawElements(GL_TRIANGLES, (GLsizei)m_indices.size(), GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}
