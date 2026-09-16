#version 430 core
// ============================================================================
// Terrain Vertex Shader — GPU heightmap displacement + analytical smooth normal
// ============================================================================
// Pairs with terrain_splat.frag for the modern height-based splat-map path.
//
// Design notes
//  * The static grid is a unit XZ plane; vertex x/z are WORLD coordinates
//    (meters). The fragment shader reads world-aligned UVs, so the mesh needs
//    only position (it reuses the existing TerrainVertex layout: location 0).
//  * Displacement: samples the master heightmap (GL_R32F). This MUST match
//    world/Terrain.cpp's CPU heightfield sampling exactly
//      uv = (worldXZ + 0.5) / heightmapSize;
//      h  = texture(uHeightMap, uv).r;          // heightmap already in world Y
//    so rendered geometry stays in lock-step with physics foot-planting.
//    (The heightmap is pre-scaled to [0, heightScale] on the CPU, so no extra
//     height-scale multiplier is applied here — same as the legacy kTerrainVS.)
//  * The geometric normal is computed ANALYTICALLY on the GPU by sampling the
//    height field at a 1-metre cross (1 texel == 1 metre by the heightfield
//    convention) and differencing. This yields a smooth normal with NO
//    dFdx()/dFdy() (per the shading suggestions) and stays correct under
//    multisampling / helper-invocation / tessellation. It is interpolated
//    smoothly across the patch and later combined with the detail normal map.
//
// Uniforms expected from the host (see TerrainSplatParams + terrain_splat.cpp):
//   uView           mat4  view matrix
//   uProjection     mat4  projection matrix
//   uViewPos        vec3  camera world position
//   uHeightMap      sampler2D  red-channel heightfield (already world-Y)
//   uHeightMapSize  vec2  heightmap texel dims (world extent = size)
//   uTexScale       float world metres per texture tile (detail UVs)
// ============================================================================

layout(location = 0) in vec3 aPos;      // world XZ grid point (y unused)

layout(location=0) out vec3  vWorldPos;     // displaced world position
layout(location=1) out vec3  vWorldNormal;  // analytical, smooth geometric normal (world space)
layout(location=2) out vec2  vWorldUV;      // worldXZ / uTexScale  (detail texture address)
layout(location=3) out vec3  vViewDir;      // uViewPos - vWorldPos

uniform mat4 uView;
uniform mat4 uProjection;
uniform vec3 uViewPos;

uniform sampler2D uHeightMap;
uniform vec2 uHeightMapSize;
uniform float uTexScale;

// World XZ (meters) -> heightmap UV in [0,1]. Mirrors the CPU bilinear sampler
// in TerrainChunk.h (terrain::worldToTexel + sampleHeightBilinear).
vec2 toHeightUV(vec2 worldXZ) {
    return (worldXZ + 0.5) / uHeightMapSize;
}

// Height at a world XZ position (world Y, metres). Matches kTerrainVS exactly.
float sampleHeight(vec2 worldXZ) {
    return texture(uHeightMap, toHeightUV(worldXZ)).r;
}

// Analytical terrain normal from the height gradient (world units).
// Surface S(x,z) = (x, h(x,z), z).
//   dS/dx = (1, dh/dx, 0)   dS/dz = (0, dh/dz, 1)
//   n = normalize(cross(dS/dz, dS/dx)) = normalize(-dh/dx, 1, -dh/dz)
// Probe spacing = 1 metre (== 1 heightmap texel) per the terrain convention.
vec3 terrainNormal(vec2 worldXZ) {
    const float kStep = 1.0;
    float hL = sampleHeight(worldXZ + vec2(-kStep, 0.0));
    float hR = sampleHeight(worldXZ + vec2(+kStep, 0.0));
    float hD = sampleHeight(worldXZ + vec2(0.0, -kStep));
    float hU = sampleHeight(worldXZ + vec2(0.0, +kStep));
    float dhdx = (hR - hL) / (2.0 * kStep);
    float dhdz = (hU - hD) / (2.0 * kStep);
    return normalize(vec3(-dhdx, 1.0, -dhdz));
}

void main() {
    vec2 worldXZ = aPos.xz;
    float h = sampleHeight(worldXZ);
    vec3 worldPos = vec3(worldXZ.x, h, worldXZ.y);  // worldXZ.y == world z

    vWorldPos    = worldPos;
    vWorldNormal = terrainNormal(worldXZ);
    vWorldUV     = worldXZ / max(uTexScale, 1e-4);
    vViewDir     = uViewPos - worldPos;

    gl_Position  = uProjection * uView * vec4(worldPos, 1.0);
}
