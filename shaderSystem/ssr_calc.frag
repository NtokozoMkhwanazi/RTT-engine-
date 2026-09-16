#version 430 core
// ============================================================================
// SSR Ray Marching Fragment Shader
// ============================================================================
// Traces screen-space reflections by marching a reflected ray in view space,
// reprojecting to screen space at each step, and comparing depths.
//
// Input:  G-Position (view-space), G-Normal (view-space), scene color, depth
// Output: RGBA — RGB = reflection color, A = reflection intensity (0-1)
// ============================================================================

out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D gPosition;      // view-space position (RGBA16F)
uniform sampler2D gNormal;        // view-space normal (RGBA16F)
uniform sampler2D uSceneColor;    // scene color (HDR, from deferred lighting)
uniform sampler2D uDepth;         // depth buffer (for hit detection)

uniform mat4 uProjection;
uniform mat4 uView;
uniform mat4 uInvProjection;
uniform mat4 uInvView;
uniform vec2 uScreenSize;
uniform int  uMaxSteps;
uniform float uThickness;

// ---- Reproject view-space position to screen-space UV ----------------------
vec2 projectToScreen(vec3 viewPos) {
    vec4 clipPos = uProjection * vec4(viewPos, 1.0);
    vec2 ndc = clipPos.xy / clipPos.w;
    return ndc * 0.5 + 0.5;
}

// ---- Get view-space position from depth buffer -----------------------------
vec3 viewPosFromDepth(vec2 uv, float depth) {
    // Reconstruct view-space position from depth
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = uInvProjection * clipPos;
    return viewPos.xyz / viewPos.w;
}

void main() {
    vec2 uv = TexCoords;

    // Sample G-Buffer
    vec3 viewPos = texture(gPosition, uv).xyz;
    vec3 viewNormal = normalize(texture(gNormal, uv).xyz);

    // Background pixel (no geometry)
    if (length(viewNormal) < 0.1 || length(viewPos) < 0.01) {
        FragColor = vec4(0.0);
        return;
    }

    // Compute reflected ray direction in view space
    vec3 viewDir = normalize(-viewPos);
    vec3 reflected = reflect(-viewDir, viewNormal);

    // ---- Screen-space ray march --------------------------------------------
    vec3 startPos = viewPos + reflected * 0.05;  // small offset to avoid self-intersection
    vec2 startUV = projectToScreen(startPos);

    // March in view space, sample in screen space
    vec3 currentPos = startPos;
    vec2 currentUV = startUV;

    float stepSize = 0.1;  // view-space step length
    vec3 marchStep = reflected * stepSize;

    float reflectionIntensity = 0.0;
    vec3 reflectionColor = vec3(0.0);

    for (int i = 0; i < uMaxSteps; ++i) {
        currentPos += marchStep;
        currentUV = projectToScreen(currentPos);

        // Out of screen bounds — fade out
        if (currentUV.x < 0.0 || currentUV.x > 1.0 ||
            currentUV.y < 0.0 || currentUV.y > 1.0) {
            break;
        }

        // Sample depth at current screen position
        float sampledDepth = texture(uDepth, currentUV).r;
        vec3 sampledViewPos = viewPosFromDepth(currentUV, sampledDepth);

        // Compare depths in view space (Z is negative looking into the screen)
        float currentViewDepth = currentPos.z;
        float sampledViewDepth = sampledViewPos.z;

        // Check if the ray is close to geometry (hit detection)
        float depthDiff = sampledViewDepth - currentViewDepth;
        if (depthDiff > 0.0 && depthDiff < uThickness) {
            // Hit! Sample scene color at the hit point
            reflectionColor = texture(uSceneColor, currentUV).rgb;

            // Fade based on distance from start (further = more faded)
            float travelDist = length(currentPos - startPos);
            float fade = 1.0 - smoothstep(0.0, 50.0, travelDist);

            // Edge fade (screen edges)
            float edgeFade = smoothstep(0.0, 0.1, currentUV.x) *
                             smoothstep(0.0, 0.1, currentUV.y) *
                             smoothstep(0.0, 0.1, 1.0 - currentUV.x) *
                             smoothstep(0.0, 0.1, 1.0 - currentUV.y);

            // Fresnel-based intensity (more reflective at grazing angles)
            float fresnel = pow(1.0 - max(dot(viewNormal, viewDir), 0.0), 3.0);
            fresnel = mix(0.02, 1.0, fresnel);  // min 2% reflectivity

            reflectionIntensity = fade * edgeFade * fresnel;
            break;
        }

        // Adaptive step size — larger steps when far from geometry
        float adaptiveStep = stepSize * (1.0 + i * 0.02);
        marchStep = reflected * adaptiveStep;
    }

    FragColor = vec4(reflectionColor, reflectionIntensity);
}
