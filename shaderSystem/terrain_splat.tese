#version 430 core
// ============================================================================
// terrain_splat.tese — tessellation evaluation shader for the heightfield
// splat path. Pairs with terrain_splat.tesc (triangle patches) + .vert + .frag.
// ============================================================================
// Interpolates the patch control points (the VS-displaced triangle corners)
// across the subdivided domain and RE-DISPLACES by the master GL_R32F
// heightfield at the subdivided density, so the displacement is geometrically
// smooth between the coarse VS grid vertices instead of a faceted plane.
//
// The re-displacement uses the EXACT same heightfield convention as the vertex
// shader (world Y = texture(uHeightMap, (worldXZ+0.5)/uHeightMapSize).r) and
// the analytical-normal stencil from the VS comments), so the tessellated
// geometry stays in lock-step with the CPU heightfield used for physics /
// foot-planting. The varyings output here match the names the fragment shader
// expects (vWorldPos/vWorldNormal/vWorldUV/vViewDir), so the SAME .frag works
// for the tessellated and non-tessellated (VS+FS) paths.
// ============================================================================
layout(triangles, ccw, equal_spacing) in;

layout(location=0) in vec3 tcWorldPos[];     // from TCS (out location 0)
layout(location=1) in vec3 tcWorldNormal[];
layout(location=2) in vec2 tcWorldUV[];
layout(location=3) in vec3 tcViewDir[];

// Outputs to the fragment shader — locations MUST match terrain_splat.frag's `in`.
layout(location=0) out vec3 vWorldPos;
layout(location=1) out vec3 vWorldNormal;
layout(location=2) out vec2 vWorldUV;
layout(location=3) out vec3 vViewDir;

uniform mat4 uView;
uniform mat4 uProjection;
uniform vec3 uViewPos;
uniform sampler2D uHeightMap;
uniform vec2 uHeightMapSize;   // heightmap texel dims (== world extent in metres)
uniform float uTexScale;       // metres per detail tile (for vWorldUV)

// World XZ (metres) -> heightmap UV in [0,1]. Mirrors toHeightUV() in the VS.
vec2 toHeightUV(vec2 worldXZ) {
    return (worldXZ + 0.5) / uHeightMapSize;
}
// Height at a world XZ position (world Y, metres). Matches the VS sampler.
float sampleHeight(vec2 worldXZ) {
    return texture(uHeightMap, toHeightUV(worldXZ)).r;
}
// Analytical terrain normal from the height gradient (same as the VS).
vec3 terrainNormal(vec2 worldXZ) {
    const float k = 1.0;
    float hL = sampleHeight(worldXZ + vec2(-k, 0.0));
    float hR = sampleHeight(worldXZ + vec2(+k, 0.0));
    float hD = sampleHeight(worldXZ + vec2(0.0, -k));
    float hU = sampleHeight(worldXZ + vec2(0.0, +k));
    float dhdx = (hR - hL) / (2.0 * k);
    float dhdz = (hU - hD) / (2.0 * k);
    return normalize(vec3(-dhdx, 1.0, -dhdz));
}

void main() {
    // Interpolated world XZ across the patch domain (corners are VS-displaced).
    vec2 worldXZ = gl_TessCoord.x * tcWorldPos[0].xz
                 + gl_TessCoord.y * tcWorldPos[1].xz
                 + gl_TessCoord.z * tcWorldPos[2].xz;

    // Re-displace at subdivided density for smooth geometry.
    float h = sampleHeight(worldXZ);
    vec3 worldPos = vec3(worldXZ.x, h, worldXZ.y);

    vWorldPos    = worldPos;
    vWorldNormal = terrainNormal(worldXZ);
    vWorldUV     = worldXZ / max(uTexScale, 1e-4);
    vViewDir     = uViewPos - worldPos;

    gl_Position  = uProjection * uView * vec4(worldPos, 1.0);
}
