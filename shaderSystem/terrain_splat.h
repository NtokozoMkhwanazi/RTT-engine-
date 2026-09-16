#pragma once
// ============================================================================
// terrain_splat.h — Height-based multi-layer splat-map terrain shader module
// ============================================================================
// Drop-in, opt-in replacement for the embedded terrain shaders
// (world/TerrainChunk.cpp g_terrainShader). Adds:
//   - multi-layer (4) splatting via a HEIGHT-BASED blend algorithm
//   - full PBR (Cook-Torrance GGX) lighting
//   - smooth analytical normals (no dFdx/dFdy) computed in the vertex shader
//   - triplanar cliff projection, hemispheric ambient, volumetric height fog
//
// Files: shaderSystem/terrain_splat.vert + shaderSystem/terrain_splat.frag
//        (+ shaderSystem/terrain_splat.tesc/.tese for optional GPU
//         tessellation — loaded automatically by terrainSplatLoad() and
//         enabled only if the 4-stage program links; otherwise it falls back
//         to VS+FS with vertex displacement + POM still active).
//        (loaded through the existing Shader / Config file loader, so nothing
//         new needs to be added to the asset build pipeline).
//
// Usage:
//   #include "shaderSystem/terrain_splat.h"
//   ...
//   Shader* splat = terrainSplatLoad();          // lazy: loads once
//   if (splat) {
//       terrainSplatBind(splat, params);         // set all uniforms + sampler units
//       // bind 16 textures on units 1..16 (4 layers x 4 maps) + heightmap@0 +
//       // optional control map@17  ... see terrain_splat.cpp "Texture unit map"
//       chunk->draw();                           // existing position-only VAO works as-is
//   }
// ============================================================================
#include <glm/glm.hpp>
#include <array>

// Forward declaration (Shader.h declares `class Shader`). Must match so the
// pointer types reconcile in the .cpp where Shader.h is included.
class Shader;

// Texture unit map (set once by terrainSplatLoad and reused by bind):
//   0  : uHeightMap          (GL_R32F master heightfield)
//   1..4  : uAlbedoLayers[0..3]
//   5..8  : uNormalLayers[0..3]
//   9..12 : uRoughLayers[0..3]
//   13..16: uAOLayers[0..3]
//   17    : uSplatMap (optional control map; bind a white 1x1 texture if unused)
//   20    : uShadowMapArray (CSM cascade 2D-array; bound by the host when
//          params.shadowEnabled is true — kept high to avoid colliding with
//          the layer units above)
static constexpr int kSplatFirstAlbedoUnit = 1;
static constexpr int kSplatFirstNormalUnit = 5;
static constexpr int kSplatFirstRoughUnit  = 9;
static constexpr int kSplatFirstAoUnit       = 13;
static constexpr int kSplatControlUnit      = 17;
static constexpr int kSplatShadowUnit       = 20;

struct TerrainSplatParams {
    // camera / lighting
    glm::mat4  view       = glm::mat4(1.0f);
    glm::mat4  projection = glm::mat4(1.0f);
    glm::vec3  viewPos    = glm::vec3(0.0f);
    glm::vec3  lightDir   = glm::normalize(glm::vec3(0.5f, 1.0f, 0.3f));
    float      waterLevel = 5.0f;

    // heightfield geometry
    // NOTE: the heightmap already stores world-Y metres on the CPU side
    // (see Terrain::generateHeightmap), and terrain_splat.vert samples it
    // without any extra scale — so no heightScale/worldSize uniforms are sent.
    int        heightmapSize = 2040;     // heightmap texel grid (square; also world extent in metres)
    float      texScale    = 8.0f;       // metres per detail tile

    // ---- Parallax Occlusion Mapping (POM) over the heightfield ---------------
    // Ray-marches the master GL_R32F heightmap in the fragment shader so the
    // surface texels shift with the view angle (ported from the RHI
    // rhi/shaders/pbr_mesh.frag kernel). 0 = off (vertex displacement alone).
    // The height value is normalized to [0,1] by heightScale before the
    // parallax math, so a single parallaxScale controls the visible effect.
    float      heightScale   = 100.0f;   // world-Y range of the heightmap (normalize [0,1])
    float      parallaxScale = 0.04f;    // POM strength in detail-UV units; 0 disables

    // ---- height-based splat layers (sand, grass, rock, snow) ------------------
    // Preferred world-Y elevation + blend half-width + slope preference
    // (0 = flat-ground layer, 1 = steep-cliff layer).
    glm::vec4  layerHeight     = glm::vec4(0.0f, 6.0f, 30.0f, 70.0f);
    glm::vec4  layerBlendWidth = glm::vec4(4.0f, 6.0f, 8.0f, 6.0f);
    glm::vec4  layerSlope      = glm::vec4(0.0f, 0.0f, 0.4f, 0.8f);
    float      layerSharpness  = 2.0f;   // >1 sharpens band boundaries

    // per-layer colour tint (multiply onto the albedo texture)
    std::array<glm::vec3, 4> layerTint  = {
        glm::vec3(1.25f, 1.12f, 0.82f),  // sand  (warm)
        glm::vec3(0.20f, 0.50f, 0.20f),  // grass (green)
        glm::vec3(0.40f, 0.38f, 0.33f),  // rock
        glm::vec3(1.00f, 1.00f, 1.02f)   // snow
    };
    // per-layer PBR scalar fallbacks (used when a layer's texture is a 1x1
    // default; bind neutral textures if you want the real texel data instead)
    std::array<float, 4> layerRough = {0.85f, 0.90f, 0.70f, 0.20f};
    std::array<float, 4> layerMetal = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> layerAo    = {0.5f, 0.7f, 0.6f, 1.0f};

    float      normalStrength = 1.0f;     // detail normal-map strength
    float      useSplatMap    = 0.0f;    // >0.5 : multiply by uSplatMap

    // ---- atmosphere / fog ----
    glm::vec3  fogHorizon   = glm::vec3(0.45f, 0.35f, 0.30f); // warm horizon
    glm::vec3  fogZenith    = glm::vec3(0.12f, 0.22f, 0.40f); // cool sky
    float      fogNear      = 150.0f;
    float      fogFar       = 900.0f;
    float      fogHeight    = 0.0f;        // base height for valley fog
    float      fogHeightFalloff = 0.15f;   // larger = mist pools deeper
    float      fogMaxOpacity = 0.65f;      // cap so distant terrain stays readable
    bool       fogEnabled     = true;      // master fog switch (temp off = disabled)

    // ---- ambient / sky-light (fed by LightingEnvironment) ----
    glm::vec3  skyTint            = glm::vec3(0.22f, 0.32f, 0.48f); // muted sky
    glm::vec3  groundBounce       = glm::vec3(0.42f, 0.30f, 0.20f); // warm earth
    float      ambientStrength    = 0.30f;                          // ambient scale
    float      detailNormalStrength = 0.45f;   // detail-normal octave strength
    float      skyLightStrength    = 0.18f;   // warm fill on shaded sides

    // ---- Cascaded Shadow Maps (optional) ----
    // When shadowEnabled is true the host must also bind the cascade 2D-array
    // texture (RenderPipeline's ShadowMapper) to uShadowMapArray's unit and set
    // the light-space matrices + split distances below. Set to false to keep
    // the terrain fully lit (no shadow sampling — safe when no maps exist).
    bool       shadowEnabled      = false;
    glm::mat4  shadowMatrices[3]   = {};
    float      shadowSplits[3]     = {};

    // ---- GPU tessellation (requires the 4-stage program to link) ------------
    // Distance-adaptive triangle subdivision. The TES re-displaces by the
    // heightmap at the subdivided density for smooth geometry between the coarse
    // VS grid vertices. outer/inner are the MAX levels; 1 = no subdivision.
    // tessFadeDist is the camera distance (metres) over which levels fade to 1.
    // These are no-ops if the program fell back to VS+FS.
    float      tessFactorOuter    = 4.0f;
    float      tessFactorInner    = 4.0f;
    float      tessFadeDist       = 80.0f;
};

// Lazily load (file-backed) the splat shader. Returns nullptr on failure.
// Caches the result; safe to call every frame.
Shader* terrainSplatLoad();

// True if the 4-stage (VS+TCS+TES+FS) tessellation program loaded and linked;
// false if the 2-stage (VS+FS) fallback is active. Lets the host switch the
// draw primitive to GL_PATCHES only when tessellation is actually wired up.
bool terrainSplatTessEnabled();

// Set every non-texture uniform + sampler-unit binding on `sh`.
// The host is responsible for binding the heightmap + 16 layer textures +
// optional control map to the units listed above before drawing.
void terrainSplatBind(Shader* sh, const TerrainSplatParams& p);
