// terrain_splat.cpp — implementation of the height-based splat-map terrain
// shader module. See terrain_splat.h / terrain_splat.vert / .frag for the
// shading algorithm itself.
#include "terrain_splat.h"

#include <glad/glad.h>
#include <memory>
#include <iostream>

#include "Shader.h"          // shaderSystem/Shader.h  (file-based GLSL loader)
#include "editor/config.h"   // Config::getShaderPath

// Lazy, load-once splat shader. The Shader class reads the .vert/.frag files
// via Config::getShaderPath("terrain_splat.vert") etc. It mirrors how
// render_pipeline.cpp loads its shaders; a missing file is logged by Shader's
// own error handling and leaves ID == 0, which we detect here.
static std::unique_ptr<Shader> g_splatShader;
static bool g_splatTried = false;
static bool g_tessLoaded = false;   // true when the 4-stage (VS+TCS+TES+FS) program linked

Shader* terrainSplatLoad() {
    if (g_splatTried) {
        // g_splatTried is sticky, but only hand back a linked program; a
        // previously-loaded-but-broken program must not be re-reported as good.
        return (g_splatShader && g_splatShader->isLinked()) ? g_splatShader.get() : nullptr;
    }
    g_splatTried = true;

    // --- Attempt the full 4-stage pipeline (vertex displacement + POM +
    //     GPU tessellation). If the tessellation shaders fail to compile or
    //     the program fails to link (e.g. driver without GL 4.3 tessellation,
    //     or a shader error), fall back to the plain VS+FS path — vertex
    //     displacement + POM still apply, tessellation is simply skipped.
    //
    // NOTE: we test isLinked(), NOT `ID != 0`. glCreateProgram() always
    // returns a non-zero handle even on link failure, and checkCompileErrors
    // only logs — so the old `tess->ID != 0` check was true even for a
    // broken program, which made the VS+FS fallback (and thus the terrain)
    // unreachable and the terrain rendered invisible under GL_PATCHES against
    // a non-linked program (glUseProgram -> GL_INVALID_OPERATION, no pixels).
    // Keep the resolved path strings alive for the lifetime of the Shader
    // constructions below. Config::getShaderPath() returns by value (a
    // temporary std::string); binding its .c_str() to a const char* would dangle
    // once the temporary is destroyed at the end of the statement, causing the
    // Shader file-loader to read freed memory (seen as a garbled path /
    // FILE_NOT_SUCCESSFULLY_READ) and silently disabling the splat path.
    const std::string vsPath  = Config::getShaderPath("terrain_splat.vert");
    const std::string tcsPath = Config::getShaderPath("terrain_splat.tesc");
    const std::string tesPath = Config::getShaderPath("terrain_splat.tese");
    const std::string fsPath  = Config::getShaderPath("terrain_splat.frag");
    try {
        auto tess = std::make_unique<Shader>(vsPath.c_str(), tcsPath.c_str(),
                                             tesPath.c_str(), fsPath.c_str());

        if (tess && tess->isLinked()) {
            std::cerr << "[TerrainSplat] loaded 4-stage pipeline (VS+TCS+TES+FS)\n";
            g_splatShader = std::move(tess);
            g_tessLoaded = true;
        } else {
            std::cerr << "[TerrainSplat] 4-stage program failed to link (or a "
                         "stage file was unreadable), falling back to VS+FS\n";
            auto vsfs = std::make_unique<Shader>(vsPath.c_str(), fsPath.c_str());
            if (vsfs && vsfs->isLinked()) {
                g_splatShader = std::move(vsfs);
                g_tessLoaded = false;
            }
            // else: leave g_splatShader null -> render() drops to the legacy path
        }
    } catch (const std::exception& e) {
        std::cerr << "[TerrainSplat] failed to load shader: " << e.what() << "\n";
        return nullptr;
    }

    Shader* sh = g_splatShader.get();
    if (!sh || !sh->isLinked()) {
        std::cerr << "[TerrainSplat] shader did not link — splat path disabled\n";
        g_splatShader.reset();
        g_tessLoaded = false;
        return nullptr;
    }

    // Bind the sampler uniforms to the texture units documented in the header.
    sh->use();
    sh->setInt("uHeightMap",        0);
    sh->setInt("uSplatMap",         kSplatControlUnit);
    for (int i = 0; i < 4; ++i) {
        sh->setInt("uAlbedoLayers[" + std::to_string(i) + "]", kSplatFirstAlbedoUnit + i);
        sh->setInt("uNormalLayers[" + std::to_string(i) + "]", kSplatFirstNormalUnit + i);
        sh->setInt("uRoughLayers[" + std::to_string(i) + "]", kSplatFirstRoughUnit + i);
        sh->setInt("uAOLayers[" + std::to_string(i) + "]",    kSplatFirstAoUnit + i);
    }
    glUseProgram(0);
    return sh;
}

// Reports whether the active splat program is the 4-stage tessellation path.
bool terrainSplatTessEnabled() {
    return g_tessLoaded;
}

void terrainSplatBind(Shader* sh, const TerrainSplatParams& p) {
    if (!sh || sh->ID == 0) return;
    sh->use();

    // --- matrices / camera / lighting ---
    sh->setMat4("uView",       p.view);
    sh->setMat4("uProjection", p.projection);
    sh->setVec3("uViewPos",    p.viewPos);
    sh->setVec3("uLightDir",   p.lightDir);
    sh->setFloat("uWaterLevel", p.waterLevel);

    // --- heightfield geometry ---
    // heightmap dims are vec2 in GLSL -> raw glUniform (Shader API has no setVec2)
    GLint locHMSize = sh->getUniformLocation("uHeightMapSize");
    glUniform2f(locHMSize, (float)p.heightmapSize, (float)p.heightmapSize);
    sh->setFloat("uTexScale",    p.texScale);

    // --- tessellation factors (no-op if the bound program is the VS+FS fallback) ---
    sh->setFloat("uTessFactorOuter", p.tessFactorOuter);
    sh->setFloat("uTessFactorInner", p.tessFactorInner);
    sh->setFloat("uTessFadeDist",    p.tessFadeDist);

    // --- Parallax Occlusion Mapping over the heightfield ---
    sh->setFloat("uParallaxScale", p.parallaxScale);
    sh->setFloat("uHeightScale",   p.heightScale);

    // --- splat layer blend parameters ---
    auto set4 = [&](const char* name, const glm::vec4& v) {
        GLint loc = sh->getUniformLocation(name);
        glUniform4fv(loc, 1, &v[0]);
    };
    set4("uLayerHeight",      p.layerHeight);
    set4("uLayerBlendWidth",   p.layerBlendWidth);
    set4("uLayerSlope",       p.layerSlope);
    sh->setFloat("uLayerSharpness", p.layerSharpness);
    sh->setFloat("uNormalStrength", p.normalStrength);
    sh->setFloat("uUseSplatMap",   p.useSplatMap);

    // per-layer tints + scalars (arrays)
    for (int i = 0; i < 4; ++i) {
        sh->setVec3("uLayerTint["  + std::to_string(i) + "]", p.layerTint[i]);
        sh->setFloat("uLayerRough[" + std::to_string(i) + "]", p.layerRough[i]);
        sh->setFloat("uLayerMetal[" + std::to_string(i) + "]", p.layerMetal[i]);
        sh->setFloat("uLayerAo["    + std::to_string(i) + "]", p.layerAo[i]);
    }

    // --- atmosphere / fog ---
    sh->setVec3("uFogColorHorizon", p.fogHorizon);
    sh->setVec3("uFogColorZenith",  p.fogZenith);
    sh->setFloat("uFogNear",        p.fogNear);
    sh->setFloat("uFogFar",         p.fogFar);
    sh->setFloat("uFogHeight",      p.fogHeight);
    sh->setFloat("uFogHeightFalloff", p.fogHeightFalloff);
    sh->setFloat("uFogMaxOpacity",  p.fogMaxOpacity);
    sh->setFloat("uFogEnabled",    p.fogEnabled ? 1.0f : 0.0f);

    // --- ambient / sky-light / detail (canonical LightingEnvironment values) ---
    sh->setVec3("uGroundBounce",        p.groundBounce);
    sh->setVec3("uSkyTint",             p.skyTint);
    sh->setFloat("uAmbientStrength",    p.ambientStrength);
    sh->setFloat("uDetailNormalStrength", p.detailNormalStrength);
    sh->setFloat("uSkyLightStrength",   p.skyLightStrength);

    // --- Cascaded Shadow Maps (optional; no-op when disabled) ---------------
    sh->setFloat("uShadowEnabled", p.shadowEnabled ? 1.0f : 0.0f);
    if (p.shadowEnabled) {
        sh->setInt("uShadowMapArray", kSplatShadowUnit);
        for (int i = 0; i < 3; ++i) {
            sh->setMat4("uLightSpaceMatrix[" + std::to_string(i) + "]", p.shadowMatrices[i]);
            sh->setFloat("uCascadeSplits[" + std::to_string(i) + "]", p.shadowSplits[i]);
        }
    }
}
