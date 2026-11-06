#version 430 core
// ============================================================================
// terrain_splat.tesc — tessellation control shader for the heightfield splat
// path. Paired with terrain_splat.tese + terrain_splat.vert + .frag.
// ============================================================================
// Operates on triangle patches (3 vertices = 1 terrain triangle). The control
// points are the VS-displaced triangle corners; this shader only sets the
// tessellation levels (distance-adaptive, camera-facing) and passes the corner
// data through to the TES, which does the real subdivision + re-displacement.
//
// If the 4-stage program fails to link at runtime, terrain_splat.cpp falls
// back to the plain VS+FS path (no tessellation, vertex displacement + POM
// still apply), so this shader is strictly opt-in / safe to add.
// ============================================================================
layout(vertices = 3) out;

// Per-vertex inputs from the vertex shader (locations match the VS outputs
// exactly so the linker can pair them).
layout(location=0) in vec3 vWorldPos[];
layout(location=1) in vec3 vWorldNormal[];
layout(location=2) in vec2 vWorldUV[];
layout(location=3) in vec3 vViewDir[];

// Per-vertex outputs to the tessellation evaluation shader. Only the corner
// world positions are needed (the TES re-derives normal/UV/viewDir at the
// subdivided density), but we forward everything for completeness/debugging.
layout(location=0) out vec3 tcWorldPos[];
layout(location=1) out vec3 tcWorldNormal[];
layout(location=2) out vec2 tcWorldUV[];
layout(location=3) out vec3 tcViewDir[];

// Host uniforms (set by terrainSplatBind).
uniform vec3 uViewPos;          // camera world position (for distance fade)
uniform mat4 uView;
uniform mat4 uProjection;
uniform float uTessFactorOuter; // max outer tessellation level (e.g. 4)
uniform float uTessFactorInner; // max inner tessellation level
uniform float uTessFadeDist;    // distance (metres) over which tess fades to 1
uniform float uWaterLevel;          // sea level (forUI.txt: underwater patch clip)

void main() {
    // Pass through the control-point data unchanged.
    tcWorldPos[gl_InvocationID]    = vWorldPos[gl_InvocationID];
    tcWorldNormal[gl_InvocationID] = vWorldNormal[gl_InvocationID];
    tcWorldUV[gl_InvocationID]     = vWorldUV[gl_InvocationID];
    tcViewDir[gl_InvocationID]     = vViewDir[gl_InvocationID];

    // Set tessellation levels once (invocation 0). Distance-adaptive: closer
    // triangles subdivide more, far ones collapse to ~1 (no subdivision).
    if (gl_InvocationID == 0) {
        // Early patch clip: if all 3 corner vertices are deep below the water line
        // (uWaterLevel - 2.0), collapse tessellation to 0.0 so the TES/fragment
        // pipe never runs for the submerged patch — saving vertex processing.
        float h0 = vWorldPos[0].y;
        float h1 = vWorldPos[1].y;
        float h2 = vWorldPos[2].y;
        if (h0 < (uWaterLevel - 2.0) && h1 < (uWaterLevel - 2.0) && h2 < (uWaterLevel - 2.0)) {
            // Underwater patch: collapse tessellation to the minimum (1.0).
            // Per the OpenGL spec, tessellation levels are clamped to [1, max];
            // setting 0.0 is undefined behavior (some drivers drop the patch,
            // others treat it as 1) and previously let an invalid level leak
            // into the TES. Clamp explicitly so the spec-violation is gone.
            // Full patch culling (skipping TES/FS for submerged terrain) is
            // better done via gl_ClipDistance — deferred to the deferred pass.
            gl_TessLevelOuter[0] = 1.0;
            gl_TessLevelOuter[1] = 1.0;
            gl_TessLevelOuter[2] = 1.0;
            gl_TessLevelInner[0] = 1.0;
        } else {
            // Distance-adaptive: closer triangles subdivide more, far ones collapse to ~1.
            vec3 center = (vWorldPos[0] + vWorldPos[1] + vWorldPos[2]) / 3.0;
            float dist  = distance(uViewPos, center);
            float f     = 1.0 - smoothstep(0.0, uTessFadeDist, dist);
            float tess  = max(1.0, mix(2.0, uTessFactorOuter, f));

            gl_TessLevelOuter[0] = tess;
            gl_TessLevelOuter[1] = tess;
            gl_TessLevelOuter[2] = tess;
            gl_TessLevelInner[0] = max(1.0, mix(1.0, uTessFactorInner, f));
        }
    }
}
