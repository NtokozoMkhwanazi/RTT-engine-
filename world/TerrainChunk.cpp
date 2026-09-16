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
GLint g_terrainBakeNormalTex = -1;
GLint g_terrainBakeRoughTex = -1;
GLint g_terrainBakeGrassTex = -1;

// G-Buffer terrain shader (deferred path — same VS, outputs MRT)
// Declared as file-scope so Terrain::render can use it in the deferred pass.
GLuint g_terrainGBufferShader = 0;
GLint g_terrainGBufferUniforms[TU_COUNT];

// Terrain albedo texture path + world scale (loaded by Terrain once, shared
// by both the main and bake programs). Now using the rocky-trail PBR set
// (assets/rocky_trail) - the trail's 4K albedo/normal/rough were downscaled
// to 1K (textures_1k/) at import: 1K is plenty for an 8m tile and cuts
// startup by avoiding the 4K EXR/HDR decode path, while the rocky-trail
// colour/normal/roughness drive the height-based rock layers. The lowland
// grass is still provided by planted grass MODELS on top, not a terrain
// texture.
const char* kTerrainDesertTexPath =
    "assets/rocky_trail/textures_1k/rocky_trail_02_diff_4k.jpg";
// PBR rock micro-detail maps. These load as native JPGs (linear, GL normal
// convention: green = +Y) via the JPG loader path in Terrain.cpp so normal
// and roughness keep their channel layout (rough.r in the ARM pack's green,
// normal.rgb) while avoiding the 4K EXR/HDR startup cost.
const char* kTerrainNormalTexPath =
    "assets/rocky_trail/textures_1k/rocky_trail_02_nor_gl_4k.jpg";
const char* kTerrainRoughTexPath =
    "assets/rocky_trail/textures_1k/rocky_trail_02_arm_4k.jpg";
float g_terrainTexScale = 8.0f;   // world meters per texture tile

// ---- True-PBR per-layer texture sets for the height-based splat shader.
//   Layer 0 = lush grass (moist lowlands)
//   Layer 0 = grass (low slopes, wet)
//   Layer 1 = coastal rocks (mid slopes)   -> coast_rocks PBR set
//   Layer 2 = coastal land rocks (high slopes) -> coast_land_rocks PBR set
//   Layer 3 = snow (peaks — tint-only, no asset)
// coast_rocks/coast_land_rocks ship a packed ARM JPG (R=AO, G=roughness,
// B=metallic) + a JPG normal map, so they load through the linear-RGB JPG
// path and the ARM splitter in Terrain.cpp (not the EXR/rough-JPG path above).
const char* kTerrainGrassAlbedoPath  = "assets/grass/textures/grass_medium_01_diff_4k.jpg";
const char* kTerrainGrassNormalPath  = "assets/grass/textures/grass_medium_01_nor_gl_4k.exr";
const char* kTerrainGrassRoughPath   = "assets/grass/textures/grass_medium_01_rough_4k.exr";
const char* kTerrainGrassAoPath      = "assets/grass/textures/grass_medium_01_ao_4k.jpg";

// Authored displacement heightmap (grayscale height in [0,1]). When wired via
// Terrain::setHeightmapSource(), this feeds the GL_R32F master heightmap that
// drives the terrain_splat vertex displacement (see Terrain::loadHeightmapFromFile).
// 8-bit PNG is the shipped asset; a 16/32-bit EXR height would give higher
// displacement precision for "true terrain" — but for now the PNG is resampled
// (bilinear) down to the heightmap resolution and pre-scaled to [0, heightScale].
const char* kTerrainHeightmapPath    = "assets/boulder/textures/rocky_terrain_disp_4k.png";

//   Layer 1 = rocky ground (mid slopes) — the rocks_ground PBR set (the
//   "rocky terrain we used before" looked flat; this higher-quality set is
//   the base rocky material for the AAA look). Albedo is sRGB; normal is EXR;
//   roughness ships as an 8-bit JPG (no EXR), so it loads through the LDR
//   single-channel path in Terrain.cpp.
const char* kTerrainRocksGroundAlbedoPath = "assets/rocks_ground/textures/rocks_ground_02_col_4k.jpg";
const char* kTerrainRocksGroundNormalPath = "assets/rocks_ground/textures/rocks_ground_02_nor_gl_4k.exr";
const char* kTerrainRocksGroundRoughPath  = "assets/rocks_ground/textures/rocks_ground_02_rough_4k.jpg";
// (rocks_ground_02_height_4k.png exists for a future detail-displacement pass;
//  the vertex shader currently displaces from the master heightmap only.)

const char* kTerrainAerialAlbedoPath = "assets/boulder/textures/aerial_rocks_02_diff_4k.jpg";
const char* kTerrainAerialNormalPath = "assets/boulder/textures/aerial_rocks_02_nor_gl_4k.exr";
// NOTE: aerial_rocks_02 ships roughness as an 8-bit JPG (no EXR), so it loads
// through the LDR single-channel path in Terrain.cpp.
const char* kTerrainAerialRoughPath  = "assets/boulder/textures/aerial_rocks_02_rough_4k.jpg";

// Coastal rock material sets (coast_rocks / coast_land_rocks) — true-PBR
// textures used for the rocky splat layers (Layer 1 & 2 above). Retained
// alongside the legacy rocks_ground/aerial sets (which are no longer the
// default splat layers but stay defined for fallback/experimentation).
const char* kTerrainCoastRocksAlbedoPath     = "assets/coast_rocks/textures/coast_rocks_03_diff_4k.jpg";
const char* kTerrainCoastRocksNormalPath     = "assets/coast_rocks/textures/coast_rocks_03_nor_gl_4k.jpg";
const char* kTerrainCoastRocksArmPath        = "assets/coast_rocks/textures/coast_rocks_03_arm_4k.jpg";
const char* kTerrainCoastLandRocksAlbedoPath = "assets/coast_land_rocks/textures/coast_land_rocks_04_diff_4k.jpg";
const char* kTerrainCoastLandRocksNormalPath = "assets/coast_land_rocks/textures/coast_land_rocks_04_nor_gl_4k.jpg";
const char* kTerrainCoastLandRocksArmPath    = "assets/coast_land_rocks/textures/coast_land_rocks_04_arm_4k.jpg";

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

// PBR auto-material (shared by bake + main passes). Upgrades the terrain from
// a flat albedo * Lambertian to a real material with micro-detail and
// roughness response, per the terrain-improvement suggestions:
//   - normal map (converted from the 4K EXR source) applied with a
//     screen-space derivative TBN so rock faces get micro-bump;
//   - triplanar sampling: the Y projection replaces the XZ one on steep
//     slopes, so cliff faces stop stretching the world-space UV;
//   - roughness map driving a roughness-based specular in the main pass;
//   - procedural cavity AO (darkens crevices where the normal tilts away
//     from up) so rock detail reads as geometry, not paint;
//   - the height-band tints (sand / dry grass / rock / bleached rock) and
//     slope rock exposure from before are kept on the albedo.
// macroMix (0..1) blends the fine tile toward a 4x-larger one for distant
// viewing (bake passes 0 - the bake must stay distance-independent).
static const char* kTerrainMaterialGLSL = R"(
    in vec3 vWorldPos;   // fragment world position (derivatives for the TBN)

    uniform sampler2D uDesertTex;
    uniform sampler2D uDesertNormal;
    uniform sampler2D uDesertRough;
    uniform float uTexScale;   // world meters per texture tile

    // Unpack a GL-convention normal map texel (green channel = +Y).
    vec3 unpackNormal(vec3 texel) {
        return texel * 2.0 - 1.0;
    }

    // Derivative-based TBN (per the PBR terrain suggestion): rebuild a tangent
    // basis from the interpolated world position + UV derivatives so the
    // tangent-space normal map bumps the surface in world space regardless of
    // the projection used (XZ ground plane or Y cliff plane).
    vec3 getNormalFromMap(sampler2D normalMap, vec2 uv, vec3 geomNormal) {
        vec3 tangentNormal = unpackNormal(texture(normalMap, uv).xyz);
        vec3 q1 = dFdx(vWorldPos);
        vec3 q2 = dFdy(vWorldPos);
        vec2 st1 = dFdx(uv);
        vec2 st2 = dFdy(uv);
        vec3 N = normalize(geomNormal);
        vec3 T = normalize(q1 * st2.t - q2 * st1.t);
        vec3 B = -normalize(cross(N, T));
        mat3 TBN = mat3(T, B, N);
        return normalize(TBN * tangentNormal);
    }

    // Full PBR material lookup. Fills albedo (height-band tinted, triplanar),
    // world-space normal (geometric + normal-map micro-detail), roughness and
    // ambient occlusion. Used live AND baked (into the packed PBR atlas).
    void getTerrainPBR(float height, vec3 geomNormal, vec3 worldPos,
                       float waterLevel, float macroMix,
                       out vec3 outAlbedo, out vec3 outNormal,
                       out float outRoughness, out float outAO) {
        vec3 N = normalize(geomNormal);
        float slope = 1.0 - clamp(N.y, 0.0, 1.0);

        // World-space UVs: ground (XZ) plane + Y projection for cliffs.
        vec2 uvXZ = worldPos.xz / max(uTexScale, 0.001);
        vec2 uvY  = worldPos.xy / max(uTexScale, 0.001);
        float cliff = smoothstep(0.55, 0.85, slope);

        // Triplanar albedo: ground plane, Y plane blended in on steep faces
        // (fixes the stretched-texture cliffs) + the usual slight desaturate.
        vec3 rock = mix(texture(uDesertTex, uvXZ).rgb,
                        texture(uDesertTex, uvY).rgb, cliff);
        rock = mix(rock, vec3(dot(rock, vec3(0.299, 0.587, 0.114))), 0.35);
        // Distance macro-blend: far away, blend toward a 4x-larger tile scale
        // so the repeating 8m grid dissolves into large-scale rock variation.
        rock = mix(rock, texture(uDesertTex, uvXZ * 0.25).rgb, macroMix);

        // Rocky height bands: warm sand low, sparse dry grass, rock,
        // sun-bleached rock on the peaks; steep slopes expose raw rock.
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
        color = mix(color, rock, smoothstep(0.55, 0.85, slope));
        outAlbedo = color;

        // Normal: geometric base + normal-map micro-detail (triplanar so
        // cliffs get detail too).
        vec3 n = getNormalFromMap(uDesertNormal, uvXZ, N);
        n = mix(n, getNormalFromMap(uDesertNormal, uvY, N), cliff);
        outNormal = n;

        // Roughness: tiling roughness map (triplanar), clamped sane.
        float rough = mix(texture(uDesertRough, uvXZ).r,
                          texture(uDesertRough, uvY).r, cliff);
        outRoughness = clamp(rough, 0.1, 1.0);

        // Procedural cavity AO: crevices (normals tilted away from up) occlude
        // a little, so cracks read as shadowed geometry instead of flat paint.
        outAO = clamp(0.7 + 0.3 * outNormal.y, 0.35, 1.0);
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
        in float vHeight;

        out vec4 fragColor;

        uniform vec3 uLightDir;
        uniform vec3 uViewPos;
        uniform vec3 uFogColor;
        uniform float uFogNear;
        uniform float uFogFar;
        uniform float uWaterLevel;

        uniform sampler2D uMaterialAtlas;    // baked albedo pages (RVT)
        uniform sampler2D uMaterialPbrAtlas; // baked world-normal(rgb)/roughness(a)
        uniform vec2 uAtlasPage;     // page origin in atlas UV space
        uniform vec2 uChunkOrigin;   // chunk min corner (XZ)
        uniform float uChunkSize;
        uniform float uUseAtlas;

        // Roughness-driven specular (Blinn-Phong breakdown, per the PBR
        // terrain suggestion): rock is a dielectric, so a small fixed F0 and a
        // specular lobe that widens as roughness rises.
        float specularStrength(vec3 N, vec3 H, float roughness) {
            float NDotH = max(dot(N, H), 0.0);
            float specPower = max(1.0, pow(2.0, (1.0 - roughness) * 10.0));
            return pow(NDotH, specPower) * (1.0 - roughness);
        }

        void main() {
            vec3 geomNormal = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));

            vec3 albedo;
            vec3 normal;
            float roughness;
            float ao;

            if (uUseAtlas > 0.5) {
                // RVT path: fetch the baked albedo + packed normal/roughness
                // (one extra fetch, still no per-frame layer blending).
                vec2 local = (vWorldPos.xz - uChunkOrigin) / uChunkSize;
                vec2 uv = uAtlasPage + local * (128.0 / 4096.0);
                albedo = texture(uMaterialAtlas, uv).rgb;
                vec4 pbr = texture(uMaterialPbrAtlas, uv);
                normal = normalize(pbr.rgb * 2.0 - 1.0);
                roughness = clamp(pbr.a, 0.1, 1.0);
                ao = clamp(0.7 + 0.3 * normal.y, 0.35, 1.0);
            } else {
                // Live fallback: full PBR material, with the distance macro-
                // blend so far tiles don't repeat visibly.
                float dist = distance(vWorldPos, uViewPos);
                float macroMix = smoothstep(40.0, 160.0, dist);
                getTerrainPBR(vHeight, geomNormal, vWorldPos, uWaterLevel,
                              macroMix, albedo, normal, roughness, ao);
            }

            // PBR-ish lighting: AO-scaled ambient + rough Lambert diffuse +
            // roughness-driven specular.
            vec3 L = normalize(uLightDir);
            vec3 V = normalize(uViewPos - vWorldPos);
            vec3 H = normalize(L + V);
            float NDotL = max(dot(normal, L), 0.0);

            vec3 ambient = vec3(0.30) * albedo * ao;
            vec3 diffuse = albedo * NDotL * (0.85 + 0.15 * ao);
            vec3 specular = vec3(0.25) * specularStrength(normal, H, roughness) * NDotL;
            vec3 lit = ambient + diffuse + specular;

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
    g_terrainUniforms[TU_DESERT_NORMAL] = glGetUniformLocation(g_terrainShader, "uDesertNormal");
    g_terrainUniforms[TU_DESERT_ROUGH]  = glGetUniformLocation(g_terrainShader, "uDesertRough");
    g_terrainUniforms[TU_PBR_ATLAS]     = glGetUniformLocation(g_terrainShader, "uMaterialPbrAtlas");

    // Bind samplers once (program is shared): heightmap=0, atlas=1, desert=2,
    // PBR atlas=3, desert normal=4, desert rough=5.
    glUseProgram(g_terrainShader);
    glUniform1i(g_terrainUniforms[TU_HEIGHT_MAP], 0);
    glUniform1i(g_terrainUniforms[TU_MATERIAL_ATLAS], 1);
    glUniform1i(g_terrainUniforms[TU_DESERT_TEX], 2);
    glUniform1i(g_terrainUniforms[TU_PBR_ATLAS], 3);
    glUniform1i(g_terrainUniforms[TU_DESERT_NORMAL], 4);
    glUniform1i(g_terrainUniforms[TU_DESERT_ROUGH], 5);
    glUniform1f(g_terrainUniforms[TU_TEX_SCALE], g_terrainTexScale);
    glUseProgram(0);

    // ------------------------------------------------------------------
    // Bake program: renders the multi-layer auto-material into an atlas page.
    // Same displacement VS; the FS writes the unlit albedo.
    // ------------------------------------------------------------------
    const char* bakeFS = R"(
        in float vHeight;

        layout(location = 0) out vec4 fragColor;  // albedo page
        layout(location = 1) out vec4 fragPbr;    // packed normal(rgb)/roughness(a)

        uniform float uWaterLevel;

        void main() {
            vec3 normal = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));
            vec3 albedo, n; float rough, ao;
            // macroMix = 0: the bake is distance-independent (baked pages are
            // sampled at any distance by the main pass).
            getTerrainPBR(vHeight, normal, vWorldPos, uWaterLevel, 0.0,
                          albedo, n, rough, ao);
            fragColor = vec4(albedo, 1.0);
            fragPbr = vec4(n * 0.5 + 0.5, rough);   // world normal rgb + roughness
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
        g_terrainBakeNormalTex = glGetUniformLocation(g_terrainBakeProgram, "uDesertNormal");
        g_terrainBakeRoughTex = glGetUniformLocation(g_terrainBakeProgram, "uDesertRough");
        glUseProgram(g_terrainBakeProgram);
        glUniform1i(g_terrainBakeHeightMap, 0);
        glUniform1i(g_terrainBakeDesertTex, 2);
        glUniform1i(g_terrainBakeNormalTex, 4);
        glUniform1i(g_terrainBakeRoughTex, 5);
        glUniform1f(glGetUniformLocation(g_terrainBakeProgram, "uTexScale"), g_terrainTexScale);
        glUseProgram(0);
    }

}

void initTerrainGBufferShader() {
    if (g_terrainGBufferShader != 0) return;

    // G-Buffer fragment shader: same material logic, but outputs to MRT
    const char* fsGB = R"(
        in float vHeight;
        in vec3 vWorldPos;
        in vec2 vTexCoord;

        layout(location = 0) out vec4 gPosition;
        layout(location = 1) out vec4 gNormal;
        layout(location = 2) out vec4 gAlbedoMetal;
        layout(location = 3) out vec4 gRoughAOEmissive;

        uniform mat4 uView;
        uniform mat4 uProjection;
        uniform vec3 uViewPos;
        uniform float uWaterLevel;

        uniform sampler2D uMaterialAtlas;
        uniform sampler2D uMaterialPbrAtlas;
        uniform vec2 uAtlasPage;
        uniform vec2 uChunkOrigin;
        uniform float uChunkSize;
        uniform float uUseAtlas;

        void main() {
            vec3 geomNormal = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));

            vec3 albedo;
            vec3 normal;
            float roughness;
            float ao;

            if (uUseAtlas > 0.5) {
                vec2 local = (vWorldPos.xz - uChunkOrigin) / uChunkSize;
                vec2 uv = uAtlasPage + local * (128.0 / 4096.0);
                albedo = texture(uMaterialAtlas, uv).rgb;
                vec4 pbr = texture(uMaterialPbrAtlas, uv);
                normal = normalize(pbr.rgb * 2.0 - 1.0);
                roughness = clamp(pbr.a, 0.1, 1.0);
                ao = clamp(0.7 + 0.3 * normal.y, 0.35, 1.0);
            } else {
                float dist = distance(vWorldPos, uViewPos);
                float macroMix = smoothstep(40.0, 160.0, dist);
                getTerrainPBR(vHeight, geomNormal, vWorldPos, uWaterLevel,
                              macroMix, albedo, normal, roughness, ao);
            }

            vec3 viewPos = (uView * vec4(vWorldPos, 1.0)).xyz;
            vec3 viewNormal = normalize(mat3(uView) * normal);

            gPosition = vec4(viewPos, 1.0);
            gNormal = vec4(viewNormal, 0.0);
            gAlbedoMetal = vec4(albedo, 0.0);
            gRoughAOEmissive = vec4(roughness, ao, 0.0, 0.0);
        }
    )";

    std::string fsGBFull = std::string("#version 330 core\n") +
                           kTerrainMaterialGLSL + fsGB;
    g_terrainGBufferShader = CompileTerrainProgram(fsGBFull.c_str());
    if (g_terrainGBufferShader == 0) return;

    g_terrainGBufferUniforms[TU_VIEW]          = glGetUniformLocation(g_terrainGBufferShader, "uView");
    g_terrainGBufferUniforms[TU_PROJECTION]    = glGetUniformLocation(g_terrainGBufferShader, "uProjection");
    g_terrainGBufferUniforms[TU_VIEW_POS]      = glGetUniformLocation(g_terrainGBufferShader, "uViewPos");
    g_terrainGBufferUniforms[TU_WATER_LEVEL]   = glGetUniformLocation(g_terrainGBufferShader, "uWaterLevel");
    g_terrainGBufferUniforms[TU_HEIGHT_MAP]    = glGetUniformLocation(g_terrainGBufferShader, "uHeightMap");
    g_terrainGBufferUniforms[TU_HEIGHT_MAP_SIZE] = glGetUniformLocation(g_terrainGBufferShader, "uHeightMapSize");
    g_terrainGBufferUniforms[TU_MATERIAL_ATLAS]= glGetUniformLocation(g_terrainGBufferShader, "uMaterialAtlas");
    g_terrainGBufferUniforms[TU_ATLAS_PAGE]    = glGetUniformLocation(g_terrainGBufferShader, "uAtlasPage");
    g_terrainGBufferUniforms[TU_CHUNK_ORIGIN]  = glGetUniformLocation(g_terrainGBufferShader, "uChunkOrigin");
    g_terrainGBufferUniforms[TU_CHUNK_SIZE]    = glGetUniformLocation(g_terrainGBufferShader, "uChunkSize");
    g_terrainGBufferUniforms[TU_USE_ATLAS]     = glGetUniformLocation(g_terrainGBufferShader, "uUseAtlas");
    g_terrainGBufferUniforms[TU_DESERT_TEX]    = glGetUniformLocation(g_terrainGBufferShader, "uDesertTex");
    g_terrainGBufferUniforms[TU_TEX_SCALE]     = glGetUniformLocation(g_terrainGBufferShader, "uTexScale");
    g_terrainGBufferUniforms[TU_DESERT_NORMAL] = glGetUniformLocation(g_terrainGBufferShader, "uDesertNormal");
    g_terrainGBufferUniforms[TU_DESERT_ROUGH]  = glGetUniformLocation(g_terrainGBufferShader, "uDesertRough");
    g_terrainGBufferUniforms[TU_PBR_ATLAS]     = glGetUniformLocation(g_terrainGBufferShader, "uMaterialPbrAtlas");

    glUseProgram(g_terrainGBufferShader);
    glUniform1i(g_terrainGBufferUniforms[TU_HEIGHT_MAP], 0);
    glUniform1i(g_terrainGBufferUniforms[TU_MATERIAL_ATLAS], 1);
    glUniform1i(g_terrainGBufferUniforms[TU_DESERT_TEX], 2);
    glUniform1i(g_terrainGBufferUniforms[TU_PBR_ATLAS], 3);
    glUniform1i(g_terrainGBufferUniforms[TU_DESERT_NORMAL], 4);
    glUniform1i(g_terrainGBufferUniforms[TU_DESERT_ROUGH], 5);
    glUniform1f(g_terrainGBufferUniforms[TU_TEX_SCALE], g_terrainTexScale);
    glUseProgram(0);
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

    // Hysteresis on the LOD boundaries: coarsen only beyond threshold*COARSEN,
    // refine only inside threshold*REFINE. Without the dead band a chunk whose
    // distance hovers on a threshold (camera moving along a chunk edge) flips
    // LOD every frame, re-uploading its index buffer each time - visible
    // shimmer and a steady GL churn. The dead band also hides the switch from
    // the CDLOD geomorphing in the vertex shader (it morphs across the band).
    constexpr float kCoarsen = 1.1f;   // must be BEYOND threshold*1.1 to coarsen
    constexpr float kRefine = 0.9f;    // must be INSIDE threshold*0.9 to refine

    int coarser = 0;
    if (m_distanceToCamera > lodDistance * 4.0f * kCoarsen) coarser = 3;
    else if (m_distanceToCamera > lodDistance * 2.0f * kCoarsen) coarser = 2;
    else if (m_distanceToCamera > lodDistance * kCoarsen) coarser = 1;

    int finer = 0;
    if (m_distanceToCamera > lodDistance * 4.0f * kRefine) finer = 3;
    else if (m_distanceToCamera > lodDistance * 2.0f * kRefine) finer = 2;
    else if (m_distanceToCamera > lodDistance * kRefine) finer = 1;

    int newLOD = m_lod;
    if (coarser > m_lod) newLOD = coarser;   // moving away: coarsen at the far margin
    else if (finer < m_lod) newLOD = finer;  // moving in: refine only at the near margin

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

void TerrainChunk::draw(bool asPatches) const {
    if (!m_loaded || m_VAO == 0 || m_lod >= 3) return;

    glBindVertexArray(m_VAO);
    if (asPatches) {
        // Tessellation path: each triangle in the index list becomes a patch.
        glDrawElements(GL_PATCHES, (GLsizei)m_indices.size(), GL_UNSIGNED_INT, 0);
    } else {
        glDrawElements(GL_TRIANGLES, (GLsizei)m_indices.size(), GL_UNSIGNED_INT, 0);
    }
    glBindVertexArray(0);
}
