#version 430 core
// ============================================================================
// Terrain Splat Fragment Shader — multi-layer height-based splatting + PBR
// ============================================================================
// Modern replacement for the flat, single-band terrain shaders.
//
// Highlights
//  - HEIGHT-BASED SPLAT ALGORITHM: each of 4 material layers (sand/grass/
//    rock/snow) is given a preferred elevation and a blend falloff width; the
//    per-layer weight is a smooth triangular function of |worldHeight - layer|
//    sharpened by uLayerSharpness. The weights are additionally modulated by
//    surface slope (flat -> ground covers, steep -> cliff rock) and, when
//    enabled, an artist-authored control/splat map (RGBA = layer weights).
//    Final weights are L1-normalised so the layers always sum to 1.0.
//  - MULTI-LAYER PBR: every layer carries albedo + normal + roughness + AO,
//    blended per-sample so steep faces keep crisp detail. Cook-Torrance
//    GGX (NDF + Smith-GGX geometry + Schlick Fresnel) runs on the result.
//  - TRIPLANAR CLIFF PROJECTION: detail textures blend between the XZ ground
//    plane and the Y cliff plane as slope -> vertical, removing the classic
//    "stretched cliffs" artefact with no manual UV seams.
//  - SMOOTH NORMALS, no dFdx/dFdy: the geometric normal arrives from the
//    vertex shader as an analytical heightfield gradient (see .vert). Detail
//    normals are composed into a world-space TBN built FROM that normal, so
//    the result stays correct on sloped terrain.
//  - ATMOSPHERE: hemispheric ambient (warm ground / cool sky) multiplied by
//    procedural cavity AO, plus volumetric height fog that settles into low
//    valleys (exp(-y * falloff)) and uses a dual-gradient horizon->zenith tint.
//  - Linear HDR output; tone mapping is deferred to the composite pass.
//
// Texture / uniform contract (host binds 4 layers; see TerrainChunk wiring
// notes below). Detail textures use GL_RGB(A)/GL_R internal formats; sample
// the normal map in GL convention (green = +Y) and unpack with *2 - 1.
// ============================================================================

layout(location=0) in vec3 vWorldPos;
layout(location=1) in vec3 vWorldNormal;   // smooth analytical geometric normal (world space)
layout(location=2) in vec2 vWorldUV;       // worldXZ / uTexScale
layout(location=3) in vec3 vViewDir;

out vec4 FragColor;

// ---- Heightmap (kept for optional re-tessellation; mostly informational) --
uniform sampler2D uHeightMap;

// ---- 4 terrain layers ------------------------------------------------------
// A layer whose texture is "missing" should be bound to a 1x1 white (albedo) /
// neutral (0.5,0.5,1.0 normal) / white (roughness/AO) texture so the tint and
// scalar fallbacks carry the layer cleanly.
uniform sampler2D uAlbedoLayers[4];   // albedo colour
uniform sampler2D uNormalLayers[4];   // world/GL normal map
uniform sampler2D uRoughLayers[4];   // roughness (R)
uniform sampler2D uAOLayers[4];       // ambient occlusion (R)

// ---- Layer blend parameters (the splat algorithm) --------------------------
uniform vec4  uLayerHeight;       // preferred world-Y elevation per layer  [sand,grass,rock,snow]
uniform vec4  uLayerBlendWidth;   // blend half-width (metres) around each layer height
uniform vec4  uLayerSlope;        // slope preference: 0 = flat-ground layer, 1 = steep-cliff layer
uniform float uLayerSharpness;    // power applied to the height falloff (>1 = crisper bands)
uniform vec3  uLayerTint[4];      // colour tint multiplied onto each layer's albedo
uniform float uLayerRough[4];     // fallback roughness (used when no roughness texture)
uniform float uLayerMetal[4];     // metallic per layer (terrain is dielectric, ~0)
uniform float uLayerAo[4];        // fallback AO (used when no AO texture)
uniform float uNormalStrength;    // detail normal-map strength (0 = geometric only)
uniform float uTexScale;          // world metres per texture tile

// ---- Optional control/splat map (RGBA = layer-0..3 painted weights) --------
uniform sampler2D uSplatMap;      // optional artist weights
uniform float     uUseSplatMap;   // >0.5 : multiply height weights by the splat map

// ---- Lighting ---------------------------------------------------------------
// uView is shared with the vertex shader (same program); it is needed here for
// cascade selection via view-space depth (see ShadowCalculationCSM).
uniform mat4 uView;
uniform vec3 uLightDir;           // normalized world-space sun direction
uniform vec3 uViewPos;            // camera world position
uniform float uWaterLevel;        // sea level, used for foam/colour hints

// ---- Parallax Occlusion Mapping (ported from the dual-backend RHI shader
// rhi/shaders/pbr_mesh.frag, adapted onto this terrain's GL_R32F heightfield).
// Ray-marches the master heightmap along the view direction projected onto the
// terrain plane, in DETAIL-texel space, so the surface texels shift with the
// viewing angle instead of reading flat. uParallaxScale = 0 disables POM. The
// height value is the SAME heightmap that drives vertex displacement, so the
// fragment parallax stays geographically in lock-step with the geometry the CPU
// sees for physics.
uniform float uParallaxScale;     // POM strength in detail-UV units (0 = off)
uniform float uHeightScale;       // world-Y range of the heightmap (to normalize to [0,1])
uniform vec2  uHeightMapSize;     // heightmap texel dims (reused from the vertex shader's sampler)

// ---- Cascaded Shadow Maps (CSM, 3-cascade 2D-array) ------------------------
// Optional: uShadowEnabled = 0 (default) disables sampling entirely so the
// shader stays safely lit when no shadow map is bound. Bind the cascade array
// (units match RenderPipeline::ShadowMapper) and populate the matrices below.
uniform sampler2DArray uShadowMapArray;
uniform float uCascadeSplits[3];
uniform mat4  uLightSpaceMatrix[3];
uniform float uShadowEnabled;     // >0.5 : sample the shadow array

// ---- Atmosphere / fog -------------------------------------------------------
uniform vec3 uFogColorHorizon;    // warm horizon tint
uniform vec3 uFogColorZenith;     // cool skyzenith tint
uniform float uFogNear;           // linear fog start (metres)
uniform float uFogFar;            // linear fog end (metres)
uniform float uFogHeight;         // base height for volumetric height fog
uniform float uFogHeightFalloff; // exponential decay rate (larger = thicker valley fog)
uniform float uFogMaxOpacity;     // cap on the fog mix factor (keeps distant terrain readable)
uniform float uFogEnabled;       // master switch (0 = fog fully disabled)

// --- ambient / sky-light (canonical LightingEnvironment values) ---------------
uniform vec3  uGroundBounce;       // warm earth hemispheric tint
uniform vec3  uSkyTint;            // muted sky hemispheric tint (de-saturated)
uniform float uAmbientStrength;    // ambient floor (cavity/ao lifts toward full)
uniform float uDetailNormalStrength; // detail-normal octave strength
uniform float uSkyLightStrength;     // warm sky-light fill on shaded sides (tunable)

// ---- Physical constants -----------------------------------------------------
const float PI     = 3.14159265359;
const float FLT_EPS = 1e-4;

// ----------------------------------------------------------------------------
// Cook-Torrance BRDF terms (mirrors shaderSystem/pbrFS.glsl for consistency)
// ----------------------------------------------------------------------------
float DistributionGGX(vec3 N, vec3 H, float rough) {
    float a  = rough * rough;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    d = max(d, FLT_EPS);
    return max(a2, FLT_EPS) / (PI * d * d);
}

float GeometrySchlick(float NdotV, float rough) {
    float r  = rough + 1.0;
    float k  = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, FLT_EPS);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float rough) {
    return GeometrySchlick(max(dot(N, V), 0.0), rough) *
           GeometrySchlick(max(dot(N, L), 0.0), rough);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ----------------------------------------------------------------------------
// CSM shadow factor [0 = lit, 1 = fully shadowed], 3-cascade 2D-array.
// Mirrors shaderSystem/pbrFS.glsl's ShadowCalculationCSM, adapted to this
// shader's uniform names. Safe no-op when uShadowEnabled == 0.
// ----------------------------------------------------------------------------
float ShadowCalculationCSM(vec3 fragPosWorld, vec3 N, vec3 L) {
    // View-space depth selects the cascade (negative Z = forward in GL).
    float fragDepth = -(uView * vec4(fragPosWorld, 1.0)).z;

    int cascadeIndex = 2;            // default to farthest cascade
    for (int i = 0; i < 3; ++i) {
        if (fragDepth < uCascadeSplits[i]) {
            cascadeIndex = i;
            break;
        }
    }

    vec4 fragPosLightSpace = uLightSpaceMatrix[cascadeIndex] * vec4(fragPosWorld, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    if (projCoords.z > 1.0) return 0.0;            // past far plane -> unshadowed

    float currentDepth = projCoords.z;
    float bias = max(0.005 * (1.0 - dot(N, L)), 0.001);

    // PCF 5x5 for soft shadow penumbrae. With GL_COMPARE_REF_TO_TEXTURE the
    // sampler returns 1.0 when the fragment is lit and 0.0 when shadowed.
    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(uShadowMapArray, 0).xy);
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            float pcfDepth = texture(uShadowMapArray,
                vec3(projCoords.xy + vec2(x, y) * texelSize, float(cascadeIndex))).r;
            shadow += (currentDepth - bias) > pcfDepth ? 1.0 : 0.0;
        }
    }
    shadow /= 25.0;

    // Seamless blend across the cascade boundary.
    if (cascadeIndex > 0) {
        float span = uCascadeSplits[cascadeIndex] - uCascadeSplits[cascadeIndex - 1];
        float bf = clamp((uCascadeSplits[cascadeIndex] - fragDepth) / span, 0.0, 1.0);
        vec4 prevLS = uLightSpaceMatrix[cascadeIndex - 1] * vec4(fragPosWorld, 1.0);
        vec3 prevPC = prevLS.xyz / prevLS.w * 0.5 + 0.5;
        float prevShadow = 0.0;
        for (int x = -2; x <= 2; ++x) {
            for (int y = -2; y <= 2; ++y) {
                float d = texture(uShadowMapArray,
                    vec3(prevPC.xy + vec2(x, y) * texelSize, float(cascadeIndex - 1))).r;
                prevShadow += (currentDepth - bias) > d ? 1.0 : 0.0;
            }
        }
        prevShadow /= 25.0;
        shadow = mix(prevShadow, shadow, bf);
    }
    return shadow;
}

// ----------------------------------------------------------------------------
// Height-based splat weighting
// ----------------------------------------------------------------------------
// For each layer the weight grows as the world height approaches the layer's
// preferred elevation, falling off linearly to zero at +/- blendWidth. The
// falloff is then sharpened with a power so band boundaries stay crisp while
// the interior stays flat. Slope and (optionally) a painted control map bias
// the result. Weights are normalised to 1.
vec4 computeSplatWeights(float height, float slope, vec2 worldUV) {
    // 1) Triangular height falloff, clamped to [0,1]
    vec4 hw = 1.0 - abs(height - uLayerHeight) / max(uLayerBlendWidth, 0.01);
    hw = max(hw, 0.0);
    // sharpen transitions (component-wise pow; vec4 exponent is fine in 430)
    hw = pow(hw, vec4(uLayerSharpness));

    // 2) Slope modulation: uLayerSlope==0 favours flat ground (1-slope),
    //    uLayerSlope==1 favours vertical faces (slope).
    vec4 sw = mix(vec4(1.0 - slope), vec4(slope), uLayerSlope);
    vec4 w  = hw * sw;

    // 3) Optional artist control map
    if (uUseSplatMap > 0.5) {
        w *= texture(uSplatMap, worldUV).rgba;
    }

    // 4) L1-normalise (guard against the all-zero case)
    float s = w.x + w.y + w.z + w.w;
    if (s < FLT_EPS) {
        return vec4(0.25);
    }
    return w / s;
}

// ----------------------------------------------------------------------------
// Triplanar texture fetch for a 2D layer sampler.
// Blends the XZ (ground) projection with a Y (cliff) projection based on slope
// so vertical faces read the texture without the stretching a single projection
// would produce.
// ----------------------------------------------------------------------------
vec3 sampleAlbedo(sampler2D tex, vec2 uvXZ, vec2 uvY, float cliffT) {
    vec3 ground = texture(tex, uvXZ).rgb;
    vec3 wall   = texture(tex, uvY).rgb;
    return mix(ground, wall, cliffT);
}

vec3 sampleNormal(sampler2D tex, vec2 uvXZ, vec2 uvY, float cliffT) {
    vec3 ground = texture(tex, uvXZ).rgb * 2.0 - 1.0;
    vec3 wall   = texture(tex, uvY).rgb * 2.0 - 1.0;
    return normalize(mix(ground, wall, cliffT));
}

float sampleR(sampler2D tex, vec2 uvXZ, vec2 uvY, float cliffT) {
    return mix(texture(tex, uvXZ).r, texture(tex, uvY).r, cliffT);
}

// ----------------------------------------------------------------------------
// Compose a detail normal into world space using a TBN built from the smooth
// geometric normal. Terrain T/B follow the dominant world axes (X right,
// Z forward) re-orthogonalised against N, which is exact for axis-aligned
// heightfields and a good approximation in general.
// ----------------------------------------------------------------------------
vec3 composeNormal(vec3 detailTangent, vec3 geomNormal, float strength) {
    if (strength < FLT_EPS) return normalize(geomNormal);
    vec3 N = normalize(geomNormal);
    // pick the tangent axis most orthogonal to N to stay stable near vertical
    vec3 T = abs(N.y) < 0.99 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 B = normalize(cross(N, T));
    T = normalize(cross(B, N));
    mat3 TBN = mat3(T, B, N);
    return normalize(N + TBN * detailTangent * strength);
}

// ----------------------------------------------------------------------------
// Volumetric height fog: exponential decay below uFogHeight (mist pools in
// valleys) blended with a view-distance component, tinted by a dual-gradient
// horizon->zenith colour ramp.
// ----------------------------------------------------------------------------
vec3 heightFog(vec3 color, vec3 worldPos, vec3 viewPos, vec3 viewDir) {
    float dist = distance(worldPos, viewPos);
    // Volumetric height fog: full strength at/below the base height, decaying
    // exponentially aloft so mountain tops stay crisp while valleys fill with
    // mist. The previous exp(+...) form was always >= 1 (never thinning above
    // the base height), which washed every altitude band in the fog tint.
    float heightFactor = exp(-max(0.0, worldPos.y - uFogHeight) * uFogHeightFalloff);
    // View-distance roll-off using the configured near/far fog planes (the
    // old form hard-coded 0.02 and never read uFogNear/uFogFar at all).
    float distFactor = smoothstep(uFogNear, uFogFar, dist);
    float fogFactor = 1.0 - exp(-heightFactor * distFactor);
    // Cap opacity (configurable via uFogMaxOpacity) so distant terrain stays
    // readable instead of turning into an opaque blue wall.
    fogFactor = clamp(fogFactor, 0.0, uFogMaxOpacity);

    // Dual-gradient tint: warm horizon mist -> deep sky-blue overhead, keyed on
    // the view ray's vertical angle.
    float up = clamp(viewDir.y, 0.0, 1.0);
    vec3 fogTint = mix(uFogColorHorizon, uFogColorZenith, up);
    return mix(color, fogTint, fogFactor);
}

// ----------------------------------------------------------------------------
// Heightmap helpers (mirrors terrain_splat.vert so the POM sampling matches the
// GPU vertex displacement exactly — same bilinear convention, same world-Y).
// ----------------------------------------------------------------------------
vec2 toHeightUV_terrain(vec2 worldXZ) {
    return (worldXZ + 0.5) / uHeightMapSize;
}

// ----------------------------------------------------------------------------
// Parallax Occlusion Mapping over the heightfield (ported from
// rhi/shaders/pbr_mesh.frag:39-74). Ray-march the master GL_R32F heightfield
// along the view direction projected onto the terrain plane, in detail-UV space
// (detailUV = worldXZ / uTexScale — see vWorldUV in the vertex shader). The
// surface height (world Y, metres) is normalized to [0,1] by uHeightScale so
// one uParallaxScale controls the visible parallax, exactly like pbr_mesh's
// normalized texture-alpha height channel. The kernel structure (adaptive
// layer count, ray-march, sub-layer linear interpolation) is the same; only
// the height source (heightmap vs. alpha) and the tangent frame
// (heightfield plane .xz vs. mesh TBN) differ.
// ----------------------------------------------------------------------------
vec2 CalculateTerrainPOM(vec2 detailUV, vec3 viewDirWorld, float heightScale, float parallaxScale)
{
    if (parallaxScale <= 0.0 || heightScale <= 0.0) return detailUV;

    vec3 viewDir = normalize(viewDirWorld);
    // Grazing views need more steps; near-bird's-eye views need fewer.
    float numLayers  = max(mix(48.0, 16.0, smoothstep(0.0, 0.2, abs(viewDir.y))), 8.0);
    float layerDepth = 1.0 / numLayers;

    // detail-UV step along the view ray, projected onto the terrain plane.
    float viewY      = max(abs(viewDir.y), 1e-3);  // guard near-flat (level) views
    vec2  deltaUV    = (viewDir.xz / viewY) * parallaxScale / numLayers;

    vec2  currentUV  = detailUV;
    vec2  worldXZ    = currentUV * uTexScale;      // detailUV = worldXZ / uTexScale
    float surfaceH   = texture(uHeightMap, toHeightUV_terrain(worldXZ)).r / heightScale;
    float depth       = 0.0;                        // ray "elevation" from the ground up

    // Ray-march until the ray rises above the surface height.
    while (depth < surfaceH) {
        currentUV -= deltaUV;
        worldXZ    = currentUV * uTexScale;
        surfaceH   = texture(uHeightMap, toHeightUV_terrain(worldXZ)).r / heightScale;
        depth     += layerDepth;
    }

    // Sub-layer interpolation between the last below-/above-surface samples.
    float prevH = texture(uHeightMap, toHeightUV_terrain((currentUV + deltaUV) * uTexScale)).r / heightScale;
    float nextDepth   = surfaceH - depth;
    float prevDepth   = prevH - (depth - layerDepth);
    float weight      = (abs(nextDepth - prevDepth) > 1e-4)
                        ? nextDepth / (nextDepth - prevDepth) : 0.0;
    return mix(currentUV, currentUV + deltaUV, weight);
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------
void main() {
    float height = vWorldPos.y;
    vec3  Ngeom  = normalize(vWorldNormal);
    float slope  = 1.0 - Ngeom.y;                 // 0 flat, 1 vertical
    float cliffT = smoothstep(0.45, 0.85, slope); // triplanar blend to cliff

    vec2 uvXZ = vWorldUV;
    vec2 uvY  = vec2(vWorldPos.z, vWorldPos.y) / max(uTexScale, 1e-4);

    // --- Parallax Occlusion Mapping ------------------------------------------
    // Shift the detail/albedo/normal fetches with the view angle over the
    // heightfield (see CalculateTerrainPOM). Same master heightmap that drives
    // the GPU vertex displacement, so the parallax is geographically locked to
    // the terrain the CPU sees. uParallaxScale <= 0 disables it (no cost beyond
    // the disabled-branch; the heightmap is already bound to unit 0).
    if (uParallaxScale > 0.0) {
        uvXZ = CalculateTerrainPOM(uvXZ, vViewDir, uHeightScale, uParallaxScale);
    }

    // --- splat weights (the height-based algorithm) -------------------------
    vec4 w = computeSplatWeights(height, slope, uvXZ);

    // --- accumulate the 4 layers (unrolled for clarity / no dyn-index) ------
    vec3  albedo   = vec3(0.0);
    vec3  detailN  = vec3(0.0);
    float rough    = 0.0;
    float ao       = 0.0;
    float metal    = 0.0;

    // Layer 0
    albedo   += sampleAlbedo(uAlbedoLayers[0], uvXZ, uvY, cliffT) * uLayerTint[0] * w.x;
    detailN  += sampleNormal(uNormalLayers[0], uvXZ, uvY, cliffT) * w.x;
    rough    += sampleR(uRoughLayers[0], uvXZ, uvY, cliffT) * w.x;
    ao       += sampleR(uAOLayers[0], uvXZ, uvY, cliffT) * w.x;
    metal    += uLayerMetal[0] * w.x;

    // Layer 1
    albedo   += sampleAlbedo(uAlbedoLayers[1], uvXZ, uvY, cliffT) * uLayerTint[1] * w.y;
    detailN  += sampleNormal(uNormalLayers[1], uvXZ, uvY, cliffT) * w.y;
    rough    += sampleR(uRoughLayers[1], uvXZ, uvY, cliffT) * w.y;
    ao       += sampleR(uAOLayers[1], uvXZ, uvY, cliffT) * w.y;
    metal    += uLayerMetal[1] * w.y;

    // Layer 2
    albedo   += sampleAlbedo(uAlbedoLayers[2], uvXZ, uvY, cliffT) * uLayerTint[2] * w.z;
    detailN  += sampleNormal(uNormalLayers[2], uvXZ, uvY, cliffT) * w.z;
    rough    += sampleR(uRoughLayers[2], uvXZ, uvY, cliffT) * w.z;
    ao       += sampleR(uAOLayers[2], uvXZ, uvY, cliffT) * w.z;
    metal    += uLayerMetal[2] * w.z;

    // Layer 3
    albedo   += sampleAlbedo(uAlbedoLayers[3], uvXZ, uvY, cliffT) * uLayerTint[3] * w.w;
    detailN  += sampleNormal(uNormalLayers[3], uvXZ, uvY, cliffT) * w.w;
    rough    += sampleR(uRoughLayers[3], uvXZ, uvY, cliffT) * w.w;
    ao       += sampleR(uAOLayers[3], uvXZ, uvY, cliffT) * w.w;
    metal    += uLayerMetal[3] * w.w;

    // --- detail normal octave (crisp rocky surface up close) ----------------
    // Re-sample the DOMINANT layer's normal map at 2x frequency (with a small
    // offset to break the regular repeat) and fold it into the material
    // normal. The 4K rock/normal maps carry real surface chatter that blurs out
    // at the 8 m tile scale; this octave recovers it so slopes read as rough
    // stone instead of a smooth plane when the camera is close. Cost: 1 extra
    // normal fetch per fragment (dominant layer only). kDetailStrength is the
    // normal fetch per fragment (dominant layer only). The strength is the
    // environment-driven uDetailNormalStrength (tune in config/cvars.ini).
    vec2 duv = uvXZ * 2.0 + vec2(0.13, 0.71);
    float domW = max(w.x, max(w.y, max(w.z, w.w)));
    vec3 nDetail = vec3(0.0, 0.0, 1.0);
    if      (w.x == domW) nDetail = texture(uNormalLayers[0], duv).rgb * 2.0 - 1.0;
    else if (w.y == domW) nDetail = texture(uNormalLayers[1], duv).rgb * 2.0 - 1.0;
    else if (w.z == domW) nDetail = texture(uNormalLayers[2], duv).rgb * 2.0 - 1.0;
    else                nDetail = texture(uNormalLayers[3], duv).rgb * 2.0 - 1.0;
    detailN += nDetail * uDetailNormalStrength;

    // --- surface normal -----------------------------------------------------
    vec3 N = composeNormal(detailN, Ngeom, uNormalStrength);

    // --- lighting (single main directional sun + hemispheric ambient) -------
    vec3 L = normalize(-uLightDir);  // light direction -> to-light
    vec3 V = normalize(vViewDir);
    vec3 H = normalize(L + V);

    float NdotL = max(dot(N, L), 0.0);

    // --- Cook-Torrance BRDF --------------------------------------------------
    float D = DistributionGGX(N, H, rough);
    float G = GeometrySmith(N, V, L, rough);
    vec3  F = fresnelSchlick(max(dot(H, V), 0.0), mix(vec3(0.04), albedo, metal));

    vec3 specular = (D * G * F) / (4.0 * max(dot(N, V), 0.0) * NdotL + 1e-4);
    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metal);

    // procedural cavity: darken where the geometric normal tilts away from up
    float cavity = clamp(0.7 + 0.3 * N.y, 0.4, 1.0);

    // --- Cascaded Shadow Mapping (gated; no-op when shadows are disabled) ---
    float shadow = (uShadowEnabled > 0.5) ? ShadowCalculationCSM(vWorldPos, N, L) : 0.0;
    float lit = max(NdotL, 0.0) * (1.0 - shadow);

    vec3 Lo = (kD * albedo / PI + specular) * (lit * ao * cavity + 0.15) * vec3(1.0);

    // --- hemispheric ambient (warm ground / cool sky) + sky-light fill -------
    float upFactor = N.y * 0.5 + 0.5;            // 0 down, 1 up
    vec3 ambient   = mix(uGroundBounce, uSkyTint, upFactor) *
                     (uAmbientStrength + (1.0 - uAmbientStrength) * cavity * ao);

    // Warm sky-light fill: lifts the BACK sides of slopes / the shaded sides of
    // rocky ridges (proportional to how much direct sun they miss) so they read
    // as sun-bathed ground rather than flat blue ambient. Unshadowed (a real
    // sky dome reaches into shadow), kept small (uSkyLightStrength) so the key
    // sun + CSM still carry the contrast.
    float shade = 1.0 - NdotL;
    vec3 skyLight = vec3(0.60, 0.52, 0.42) * shade * uSkyLightStrength;
    ambient += skyLight;

    // Shaded-and-shadowed points still get the warm fill above a tiny floor so
    // nothing collapses to pure black.
    ambient = max(ambient, vec3(0.06, 0.06, 0.05));

    vec3 color = ambient + Lo;

    // --- height fog ---------------------------------------------------------
    // Gated by uFogEnabled so the whole effect can be switched off (e.g. while
    // tuning the sun) without touching the distance/opacity parameters.
    if (uFogEnabled > 0.5) {
        color = heightFog(color, vWorldPos, uViewPos, V);
    }

    FragColor = vec4(color, 1.0);
}
