#include "Terrain.h"
#include "TerrainOptimizations.h"
#include "renderer/ShadowMapper.h"   // for the opt-in CSM shadow sampling
#include "lighting/LightingEnvironment.h" // canonical sun / fog / sky standard
#include "lighting/CVar.h"         // live-tunable POM/tessellation params (config/cvars.ini)
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <vector>

#include "shaderSystem/stb_image.h"
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <Imath/half.h>

// Shared with the terrain shaders (TerrainChunk.cpp).
extern const char* kTerrainDesertTexPath;
extern const char* kTerrainNormalTexPath;
extern const char* kTerrainRoughTexPath;
extern float g_terrainTexScale;

// True-PBR per-layer texture paths (TerrainChunk.cpp). Used by the splat path.
extern const char* kTerrainGrassAlbedoPath;
extern const char* kTerrainGrassNormalPath;
extern const char* kTerrainGrassRoughPath;
extern const char* kTerrainGrassAoPath;
extern const char* kTerrainRocksGroundAlbedoPath;
extern const char* kTerrainRocksGroundNormalPath;
extern const char* kTerrainRocksGroundRoughPath;
extern const char* kTerrainAerialAlbedoPath;
extern const char* kTerrainAerialNormalPath;
extern const char* kTerrainAerialRoughPath;

// Coastal rock material sets (coast_rocks / coast_land_rocks) — true-PBR
// textures used for the rocky splat layers. These ship a packed ARM JPG (R=AO,
// G=roughness, B=metallic) + a JPG normal map, so the loader splits the ARM
// into dedicated rough/AO slots and reads the normal JPG linear-RGB (see
// LoadLinearRGBTexture / LoadARMTextures below).
extern const char* kTerrainCoastRocksAlbedoPath;
extern const char* kTerrainCoastRocksNormalPath;
extern const char* kTerrainCoastRocksArmPath;
extern const char* kTerrainCoastLandRocksAlbedoPath;
extern const char* kTerrainCoastLandRocksNormalPath;
extern const char* kTerrainCoastLandRocksArmPath;

// Load a native linear-HDR OpenEXR map (normal/roughness) into a GL_RGBA16F
// texture with mipmaps. The EXR half-float data keeps full float precision;
// the shader's sampler2D reads the channels it needs (.xyz for normals, .r
// for roughness) from the packed RGBA. Returns 0 on any failure (the bake
// then falls back to geometric-only normals).
static GLuint LoadEXRTextureRGBA(const char* path) {
    try {
        Imf::InputFile file(path);
        const Imath::Box2i& dw = file.header().dataWindow();
        const int w = dw.max.x - dw.min.x + 1;
        const int h = dw.max.y - dw.min.y + 1;
        if (w <= 0 || h <= 0) return 0;

        // Zero-filled RGBA half buffer: channels missing from the file (e.g.
        // a single-channel roughness EXR) read as 0 / the alpha defaults to 1.
        std::vector<half> pixels(static_cast<size_t>(w) * h * 4, half(0.0f));
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = half(1.0f);

        const int xStride = static_cast<int>(sizeof(half) * 4);
        const int64_t yStride = static_cast<int64_t>(xStride) * w;
        const char* base = reinterpret_cast<const char*>(pixels.data()) -
                           (dw.min.x + static_cast<int64_t>(dw.min.y) * w) * xStride;
        Imf::FrameBuffer fb;
        const Imf::ChannelList& chans = file.header().channels();
        auto addChan = [&](const char* name, int comp) {
            if (chans.findChannel(name))
                fb.insert(name, Imf::Slice(Imf::HALF,
                                           const_cast<char*>(base) + comp * sizeof(half),
                                           xStride, yStride));
        };
        addChan("R", 0);
        addChan("G", 1);
        addChan("B", 2);
        addChan("A", 3);
        // Single-channel EXRs (e.g. the rough map stores its value in "Y",
        // not "R") must still fill the red component - the shader reads .r.
        if (!chans.findChannel("R")) addChan("Y", 0);
        file.setFrameBuffer(fb);
        file.readPixels(dw.min.y, dw.max.y);

        GLuint tex = 0;
        glCreateTextures(GL_TEXTURE_2D, 1, &tex);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        {
            GLsizei levels = 1;
            for (int d = (w > h ? w : h); d >>= 1; ++levels) {}
            glTextureStorage2D(tex, levels, GL_RGBA16F, w, h);
        }
        glTextureSubImage2D(tex, 0, 0, 0, w, h, GL_RGBA, GL_HALF_FLOAT, pixels.data());
        glGenerateTextureMipmap(tex);
        glBindTexture(GL_TEXTURE_2D, 0);  // preserve prior unbind
        std::cout << "[Terrain] PBR EXR loaded: " << path << " (" << w << "x" << h << ")\n";
        return tex;
    } catch (const std::exception& e) {
        std::cerr << "[Terrain] EXR load failed: " << path << ": " << e.what() << "\n";
        return 0;
    }
}

// Load an 8-bit sRGB JPG/PNG colour map (albedo / AO) into GL_SRGB8 with
// mipmaps + tiling. sRGB → linear decode happens automatically on sample, so
// the shader receives linear radiometric data (true PBR).
static GLuint LoadSRGBTexture(const char* path) {
    stbi_set_flip_vertically_on_load(true);
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path, &w, &h, &n, 3);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        std::cerr << "[Terrain] SRGB texture load failed: " << path << "\n";
        return 0;
    }
    GLuint tex = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    {
        GLsizei levels = 1;
        for (int d = (w > h ? w : h); d >>= 1; ++levels) {}
        glTextureStorage2D(tex, levels, GL_SRGB8, w, h);
    }
    glTextureSubImage2D(tex, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    glGenerateTextureMipmap(tex);
    glBindTexture(GL_TEXTURE_2D, 0);  // preserve prior unbind
    stbi_image_free(px);
    std::cout << "[Terrain] SRGB texture loaded: " << path << " (" << w << "x" << h << ")\n";
    return tex;
}

// Load a single-channel 8-bit map (roughness / AO JPG) into GL_R8 linear with
// mipmaps + tiling. The shader's sampleR() reads .r, so a grayscale JPG maps
// straight onto the roughness / AO channel. (AO is sRGB by convention, but as
// a scalar occlusion factor the linear decode error is negligible.)
static GLuint LoadRTexture(const char* path) {
    stbi_set_flip_vertically_on_load(true);
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path, &w, &h, &n, 1);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        std::cerr << "[Terrain] R texture load failed: " << path << "\n";
        return 0;
    }
    GLuint tex = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    {
        GLsizei levels = 1;
        for (int d = (w > h ? w : h); d >>= 1; ++levels) {}
        glTextureStorage2D(tex, levels, GL_R8, w, h);
    }
    glTextureSubImage2D(tex, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, px);
    glGenerateTextureMipmap(tex);
    glBindTexture(GL_TEXTURE_2D, 0);  // preserve prior unbind
    stbi_image_free(px);
    std::cout << "[Terrain] R texture loaded: " << path << " (" << w << "x" << h << ")\n";
    return tex;
}

// Load a linear-RGB JPG (no sRGB decode) into GL_RGB8 with mipmaps + tiling.
// Used for JPG normal maps (e.g. coast_rocks nor_gl) — normals must NOT be
// sRGB-decoded, so they can't reuse LoadSRGBTexture (which flags GL_SRGB8).
static GLuint LoadLinearRGBTexture(const char* path) {
    stbi_set_flip_vertically_on_load(true);
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path, &w, &h, &n, 3);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        std::cerr << "[Terrain] RGB(normal) texture load failed: " << path << "\n";
        return 0;
    }
    GLuint tex = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    {
        GLsizei levels = 1;
        for (int d = (w > h ? w : h); d >>= 1; ++levels) {}
        glTextureStorage2D(tex, levels, GL_RGB8, w, h);
    }
    glTextureSubImage2D(tex, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    glGenerateTextureMipmap(tex);
    glBindTexture(GL_TEXTURE_2D, 0);  // preserve prior unbind
    stbi_image_free(px);
    std::cout << "[Terrain] RGB(normal) texture loaded: " << path << " ("
              << w << "x" << h << ")\n";
    return tex;
}

// glTF ARM pack (roughness/metallic PBR asset): R = ambient occlusion,
// G = roughness, B = metallic. Splits one ARM JPG into two GL_R8 textures
// (linear, tiled, mipmapped) so the splat layout keeps its dedicated
// uRoughLayers[i].r and uAOLayers[i].r samplers. The terrain never reads
// metallic from a texture (rock is dielectric, layerMetal=0.0), so B is dropped.
static void LoadARMTextures(const char* path, GLuint& roughOut, GLuint& aoOut) {
    roughOut = aoOut = 0;
    stbi_set_flip_vertically_on_load(true);
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path, &w, &h, &n, 3);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        std::cerr << "[Terrain] ARM load failed: " << path << "\n";
        return;
    }
    auto makeR = [&](int channel) -> GLuint {
        GLuint id = 0;
        glCreateTextures(GL_TEXTURE_2D, 1, &id);
        glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        GLsizei levels = 1;
        for (int d = (w > h ? w : h); d >>= 1; ++levels) {}
        glTextureStorage2D(id, levels, GL_R8, w, h);
        std::vector<unsigned char> chan(w * h);
        for (int i = 0; i < w * h; ++i) chan[i] = px[i * 3 + channel];
        glTextureSubImage2D(id, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, chan.data());
        glGenerateTextureMipmap(id);
        return id;
    };
    roughOut = makeR(1);   // G = roughness
    aoOut    = makeR(0);   // R = ambient occlusion
    stbi_image_free(px);
    std::cout << "[Terrain] ARM split: " << path << " rough=" << roughOut
              << " ao=" << aoOut << " (" << w << "x" << h << ")\n";
}

// 1x1 white RGBA texture used as a neutral fallback for tint-only splat layers
// (e.g. snow peaks) or any layer whose texture failed to load. albedo reads
// as white (tint-controlled), normal as flat-up, roughness/AO as 1.0.
static GLuint CreateWhiteTexture() {
    GLuint tex = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    unsigned char white[4] = {255, 255, 255, 255};
    glTextureStorage2D(tex, 1, GL_RGBA8, 1, 1);
    glTextureSubImage2D(tex, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, white);
    return tex;
}

// 1x1 flat-up tangent-space normal (0.5, 0.5, 1.0, 1.0). The shader unpacks
// normals with *2-1, so this decodes to (0,0,1) = "pointing straight up" and
// yields the geometric normal (no spurious 45-degree detail). Used as the
// normal-map fallback for tint-only splat layers (e.g. snow peaks).
static GLuint CreateFlatNormalTexture() {
    GLuint tex = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    unsigned char flat[4] = {128, 128, 255, 255};  // (0.5, 0.5, 1.0, 1.0)
    glTextureStorage2D(tex, 1, GL_RGBA8, 1, 1);
    glTextureSubImage2D(tex, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, flat);
    return tex;
}

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
    if (m_materialPbrAtlas != 0) {
        glDeleteTextures(1, &m_materialPbrAtlas);
        m_materialPbrAtlas = 0;
    }
    if (m_heightTexture != 0) {
        glDeleteTextures(1, &m_heightTexture);
        m_heightTexture = 0;
    }
    if (m_desertTex != 0) {
        glDeleteTextures(1, &m_desertTex);
        m_desertTex = 0;
    }
    if (m_desertNormalTex != 0) {
        glDeleteTextures(1, &m_desertNormalTex);
        m_desertNormalTex = 0;
    }
    if (m_desertRoughTex != 0) {
        glDeleteTextures(1, &m_desertRoughTex);
        m_desertRoughTex = 0;
    }
}

void Terrain::initialize() {
    std::cout << "[Terrain] Initializing (async chunk loading)...\n";
    std::cout << "  Chunk size: " << m_config.chunkSize << "m\n";
    std::cout << "  Chunk resolution: " << m_config.chunkResolution << "x" << m_config.chunkResolution << "\n";
    std::cout << "  View distance: " << m_config.viewDistance << " chunks\n";

    generateHeightmap();
    if (!m_heightmapAssetPath.empty()) {
        // Overwrite the procedural Perlin heightfield with the authored asset
        // (falls back to procedural on load failure inside the loader).
        loadHeightmapFromFile(m_heightmapAssetPath);
    }
    m_initialized = true;

    // Upload the master heightmap as a GPU texture. The vertex shader displaces
    // every chunk's static grid by sampling this (GL_LINEAR = the same continuous
    // bilinear surface the CPU physics queries, so foot-planting always matches
    // the rendered terrain).
    glCreateTextures(GL_TEXTURE_2D, 1, &m_heightTexture);
    glTextureStorage2D(m_heightTexture, 1, GL_R32F, m_config.heightmapSize, m_config.heightmapSize);
    glTextureSubImage2D(m_heightTexture, 0, 0, 0, m_config.heightmapSize, m_config.heightmapSize,
                        GL_RED, GL_FLOAT, m_heightmap.data());
    glTextureParameteri(m_heightTexture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_heightTexture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_heightTexture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_heightTexture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);  // preserve prior unbind
    std::cout << "[Terrain] Heightmap texture uploaded (" << m_config.heightmapSize
              << "x" << m_config.heightmapSize << " R32F)\n";

    // Dry-desert albedo: tiling rock texture that drives the terrain material
    // (Rock1_Diffuse.png - warm desert rock). Tiles every g_terrainTexScale m.
    {
        stbi_set_flip_vertically_on_load(true);
        int w = 0, h = 0, n = 0;
        unsigned char* px = stbi_load(kTerrainDesertTexPath, &w, &h, &n, 3);
        if (px && w > 0 && h > 0) {
            glCreateTextures(GL_TEXTURE_2D, 1, &m_desertTex);
            glTextureParameteri(m_desertTex, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTextureParameteri(m_desertTex, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTextureParameteri(m_desertTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTextureParameteri(m_desertTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            {
                GLsizei levels = 1;
                for (int d = (w > h ? w : h); d >>= 1; ++levels) {}
                glTextureStorage2D(m_desertTex, levels, GL_RGB8, w, h);
            }
            glTextureSubImage2D(m_desertTex, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
            glGenerateTextureMipmap(m_desertTex);
            glBindTexture(GL_TEXTURE_2D, 0);  // preserve prior unbind
            std::cout << "[Terrain] Desert albedo loaded: " << kTerrainDesertTexPath
                      << " (" << w << "x" << h << ", tile=" << g_terrainTexScale << "m)\n";
        } else {
            std::cerr << "[Terrain] Failed to load desert albedo: "
                      << kTerrainDesertTexPath << "\n";
        }
        if (px) stbi_image_free(px);
    }

    // PBR rock micro-detail maps (linear JPG normal + ARM-packed rough). The
    // bake AND main shaders sample these for the rock material - the shader
    // reads normal.rgb / rough.r (see getTerrainPBR in TerrainChunk.cpp),
    // which matches the JPG normal map and the ARM rough channel (G -> .r).
    // AO is procedural (0.7 + 0.3*n.y in getTerrainPBR), so only the roughness
    // half of the ARM split is needed here.
    if (m_desertNormalTex == 0) m_desertNormalTex = LoadLinearRGBTexture(kTerrainNormalTexPath);
    if (m_desertRoughTex == 0) {
        GLuint desertAo = 0;
        LoadARMTextures(kTerrainRoughTexPath, m_desertRoughTex, desertAo);
    }

    // ---- True-PBR per-layer texture sets for the splat shader ----------------
    // Each elevation band gets a full PBR set (albedo / normal / roughness / ao).
    // Albedo is sRGB (decoded to linear on sample); normals + EXR roughes are
    // GL_RGBA16F; JPG roughness/AO load as GL_R8. Layers without an asset (the
    // snow peak band, layer 3) leave their slots at 0 and renderSplat substitutes
    // m_whiteTexture so the per-layer tint carries the colour. ROUGHNESS is the
    // only channel the shader reads for those fallback slots, so sand/snow render
    // matte (snow) or tint-only rather than a repeated rock texture.
    m_splatLayers[0].albedo = LoadSRGBTexture(kTerrainGrassAlbedoPath);
    m_splatLayers[0].normal = LoadEXRTextureRGBA(kTerrainGrassNormalPath);
    m_splatLayers[0].rough  = LoadEXRTextureRGBA(kTerrainGrassRoughPath);
    m_splatLayers[0].ao     = LoadRTexture(kTerrainGrassAoPath);

    // Layer 1: coastal rocks (true-PBR). sRGB albedo, linear-RGB JPG normal,
    // ARM-split roughness (G) + AO (R). Coast rock is a dielectric (metal=0), so
    // the arm B-metal channel is discarded. This replaces the rocks_ground
    // layer so coastal terrain reads its true colour (no fog/blue wash — fog is
    // driven by LightingEnvironment and is off by default while tuning).
    GLuint rough1 = 0, ao1 = 0;
    LoadARMTextures(kTerrainCoastRocksArmPath, rough1, ao1);
    m_splatLayers[1].albedo = LoadSRGBTexture(kTerrainCoastRocksAlbedoPath);
    m_splatLayers[1].normal = LoadLinearRGBTexture(kTerrainCoastRocksNormalPath);
    m_splatLayers[1].rough  = rough1;
    m_splatLayers[1].ao     = ao1;

    // Layer 2: coastal land rocks (true-PBR) — same ARM-split layout. Replaces
    // the aerial rocks so the higher rocky band is also a true coastal stone.
    GLuint rough2 = 0, ao2 = 0;
    LoadARMTextures(kTerrainCoastLandRocksArmPath, rough2, ao2);
    m_splatLayers[2].albedo = LoadSRGBTexture(kTerrainCoastLandRocksAlbedoPath);
    m_splatLayers[2].normal = LoadLinearRGBTexture(kTerrainCoastLandRocksNormalPath);
    m_splatLayers[2].rough  = rough2;
    m_splatLayers[2].ao     = ao2;

    // Layer 3 (snow peaks): tint-only — no texture asset exists, so all slots
    // stay at 0 and renderSplat binds m_whiteTexture + the cool snow tint.

    if (m_whiteTexture == 0) m_whiteTexture = CreateWhiteTexture();
    if (m_flatNormalTexture == 0) m_flatNormalTexture = CreateFlatNormalTexture();

    // RVT material atlas: multi-layer auto-material baked per chunk page,
    // plus the packed PBR atlas (world normal rgb + roughness a) baked in the
    // same pass as COLOR_ATTACHMENT1.
    glCreateTextures(GL_TEXTURE_2D, 1, &m_materialAtlas);
    glTextureStorage2D(m_materialAtlas, 1, GL_RGBA8, kMaterialAtlasSize, kMaterialAtlasSize);
    glTextureParameteri(m_materialAtlas, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_materialAtlas, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_materialAtlas, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_materialAtlas, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);  // preserve prior unbind

    glCreateTextures(GL_TEXTURE_2D, 1, &m_materialPbrAtlas);
    glTextureStorage2D(m_materialPbrAtlas, 1, GL_RGBA8, kMaterialAtlasSize, kMaterialAtlasSize);
    glTextureParameteri(m_materialPbrAtlas, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_materialPbrAtlas, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_materialPbrAtlas, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_materialPbrAtlas, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);  // preserve prior unbind

    glGenFramebuffers(1, &m_bakeFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_bakeFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           m_materialAtlas, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                           m_materialPbrAtlas, 0);
    const GLenum drawBufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, drawBufs);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[Terrain] Material bake FBO incomplete - RVT disabled\n";
        glDeleteFramebuffers(1, &m_bakeFBO);
        m_bakeFBO = 0;
        glDeleteTextures(1, &m_materialAtlas);
        m_materialAtlas = 0;
        glDeleteTextures(1, &m_materialPbrAtlas);
        m_materialPbrAtlas = 0;
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

void Terrain::loadHeightmapFromFile(const std::string& path) {
    // Load a grayscale height asset (PNG/EXR/etc via stbi 1-channel) and
    // bilinearly resample it down to the GL_R32F master heightmap (heightmapSize)
    // so vertex displacement + CPU physics queries read one continuous surface.
    // Output values are pre-scaled to [0, heightScale] (world Y) to match the
    // procedural path and the terrain_splat vertex convention. On any failure
    // the caller (initialize) keeps the already-generated procedural heightmap.
    int iw = 0, ih = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &iw, &ih, &n, 1);
    if (px == nullptr) {
        std::cerr << "[Terrain] Heightmap asset load FAILED (keeping procedural): "
                  << path << "\n";
        return;
    }
    const int hs = m_config.heightmapSize;
    m_heightmap.resize((size_t)hs * hs);
    auto sample = [&](float u, float v) -> float {
        u = std::min(1.0f, std::max(0.0f, u));
        v = std::min(1.0f, std::max(0.0f, v));
        float fx = u * (iw - 1);
        float fy = v * (ih - 1);
        int x0 = (int)std::floor(fx), x1 = std::min(x0 + 1, iw - 1);
        int y0 = (int)std::floor(fy), y1 = std::min(y0 + 1, ih - 1);
        float tx = fx - x0, ty = fy - y0;
        float p00 = px[(size_t)y0 * iw + x0] / 255.0f;
        float p10 = px[(size_t)y0 * iw + x1] / 255.0f;
        float p01 = px[(size_t)y1 * iw + x0] / 255.0f;
        float p11 = px[(size_t)y1 * iw + x1] / 255.0f;
        float v0 = p00 * (1 - tx) + p10 * tx;
        float v1 = p01 * (1 - tx) + p11 * tx;
        return v0 * (1 - ty) + v1 * ty;
    };
    for (int y = 0; y < hs; y++) {
        float v = (hs <= 1) ? 0.0f : (float)y / (hs - 1);
        for (int x = 0; x < hs; x++) {
            float u = (hs <= 1) ? 0.0f : (float)x / (hs - 1);
            m_heightmap[(size_t)y * hs + x] = sample(u, v) * m_config.heightScale;
        }
    }
    stbi_image_free(px);
    std::cout << "[Terrain] Heightmap loaded from authored asset: " << path
              << " (" << iw << "x" << ih << " -> " << hs << "x" << hs
              << " R32F, scaled to [0," << m_config.heightScale << "])\n";
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

    // Top-down ortho camera spanning the chunk's XZ footprint, mapped onto
    // the page (world X -> right, world Z -> up, height -> depth). The camera
    // looks down from high above (+Y) with a WIDE depth range: the bake's VS
    // displaces vertices by the master heightmap (0..heightScale, e.g. 100),
    // and a near=-10 far=100 ortho would push every displaced vertex behind
    // the near plane (NDC z < -1) and clip the whole page. Looking from y=500
    // with near=10 far=1000 keeps the full height band inside the frustum.
    const glm::vec3 origin = chunk.getWorldPosition();
    const float half = m_config.chunkSize * 0.5f;
    const glm::vec3 center = origin + glm::vec3(half, 0.0f, half);
    glm::mat4 proj = glm::ortho(-half, half, -half, half, 10.0f, 1000.0f) *
                     glm::lookAt(center + glm::vec3(0.0f, 500.0f, 0.0f), center,
                                 glm::vec3(0.0f, 0.0f, 1.0f));

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
    }
    if (m_desertNormalTex != 0 && g_terrainBakeNormalTex >= 0) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, m_desertNormalTex);
    }
    if (m_desertRoughTex != 0 && g_terrainBakeRoughTex >= 0) {
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, m_desertRoughTex);
    }
    glActiveTexture(GL_TEXTURE0);
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

    // LOD evaluation is throttled: recomputing every chunk's distance (and
    // re-uploading its index buffer on a level change) each frame while the
    // camera is stationary is wasted work. Refresh when the camera has moved
    // >1m or 0.5s has elapsed since the last evaluation.
    m_lodEvalTimer -= dt;
    const bool lodDue = m_lodEvalTimer <= 0.0f ||
                        glm::length(cameraPos - m_lodEvalPos) > 1.0f;
    if (lodDue) {
        m_lodEvalTimer = 0.5f;
        m_lodEvalPos = cameraPos;
    }

    int camChunkX, camChunkY;
    getChunkCoords(cameraPos.x, cameraPos.z, camChunkX, camChunkY);
    ChunkCoord camChunk{camChunkX, camChunkY};

    if (camChunk == m_lastChunk) {
        if (lodDue) {
            for (auto& [coord, chunk] : m_chunks) {
                chunk->updateLOD(cameraPos, m_config.lodDistance);
            }
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

    // Update LOD (throttled - see above)
    if (lodDue) {
        for (auto& [coord, chunk] : m_chunks) {
            chunk->updateLOD(cameraPos, m_config.lodDistance);
        }
    }
}

void Terrain::render(const glm::mat4& view, const glm::mat4& projection,
                     const glm::vec3& cameraPos, float fovDegrees, float aspectRatio,
                     float nearPlane, float farPlane, ShadowMapper* shadow,
                     const LightingEnvironment& lighting) const {
    if (!m_initialized) return;

    // ---- Opt-in: height-based splat-map terrain shader ---------------------
    // When enabled, the whole chunk batch is drawn through the modern
    // multi-layer splat shader (shaderSystem/terrain_splat.*) instead of the
    // legacy RVT/embedded terrain shader. Off by default; the legacy path
    // below is untouched when this is disabled or the shader fails to load.
    if (m_useSplatShader) {
        Shader* splat = terrainSplatLoad();
        if (splat) {
            renderSplat(view, projection, cameraPos, shadow, lighting);
            return;
        }
        // Shader unavailable → fall back to the legacy path so the terrain is
        // still visible even if the file-backed splat program fails to link.
        static bool logged = false;
        if (!logged) {
            std::cerr << "[Terrain] terrainSplatLoad() returned null — "
                         "falling back to legacy terrain shader\n";
            logged = true;
        }
    }

    // Bind the master heightmap for GPU displacement (unit 0, matches the
    // sampler bound once in initTerrainShader).
    if (m_heightTexture != 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_heightTexture);
    }

    // Use robust frustum culler from optimizations (skip when the editor
    // has disabled frustum culling for debugging / fully-visible terrain).
    TerrainOptimizations::FrustumCuller culler;
    if (m_frustumCulling) culler.update(projection * view);

    // Collect visible chunks
    std::vector<const TerrainChunk*> visibleChunks;
    visibleChunks.reserve(m_chunks.size());

    int skippedFrustum = 0, skippedLOD = 0;
    for (const auto& [coord, chunk] : m_chunks) {
        if (m_frustumCulling && !culler.isBoxInFrustum(
                (chunk->getBoundsMin() + chunk->getBoundsMax()) * 0.5f,
                (chunk->getBoundsMax() - chunk->getBoundsMin()) * 0.5f)) {
            skippedFrustum++;
            continue;
        }
        if (chunk->getLOD() >= 3) { skippedLOD++; continue; }
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

void Terrain::renderGBuffer(const glm::mat4& view, const glm::mat4& projection,
                            const glm::vec3& cameraPos, float fovDegrees, float aspectRatio,
                            float nearPlane, float farPlane) const {
    if (!m_initialized) return;
    if (g_terrainGBufferShader == 0) {
        // Fallback: G-Buffer shader not compiled, use forward path
        render(view, projection, cameraPos, fovDegrees, aspectRatio, nearPlane, farPlane);
        return;
    }

    if (m_heightTexture != 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_heightTexture);
    }

    TerrainOptimizations::FrustumCuller culler;
    if (m_frustumCulling) culler.update(projection * view);

    std::vector<const TerrainChunk*> visibleChunks;
    visibleChunks.reserve(m_chunks.size());

    for (const auto& [coord, chunk] : m_chunks) {
        if (m_frustumCulling && !culler.isBoxInFrustum(
                (chunk->getBoundsMin() + chunk->getBoundsMax()) * 0.5f,
                (chunk->getBoundsMax() - chunk->getBoundsMin()) * 0.5f)) {
            continue;
        }
        if (chunk->getLOD() >= 3) continue;
        visibleChunks.push_back(chunk.get());
    }

    if (visibleChunks.empty()) return;

    std::sort(visibleChunks.begin(), visibleChunks.end(),
        [&cameraPos](const TerrainChunk* a, const TerrainChunk* b) {
            return a->getDistanceToCamera(cameraPos) < b->getDistanceToCamera(cameraPos);
        });

    glUseProgram(g_terrainGBufferShader);

    glUniformMatrix4fv(g_terrainGBufferUniforms[TU_VIEW], 1, GL_FALSE, &view[0][0]);
    glUniformMatrix4fv(g_terrainGBufferUniforms[TU_PROJECTION], 1, GL_FALSE, &projection[0][0]);
    glUniform3f(g_terrainGBufferUniforms[TU_VIEW_POS], cameraPos.x, cameraPos.y, cameraPos.z);
    glUniform1f(g_terrainGBufferUniforms[TU_WATER_LEVEL], 5.0f);
    glUniform1f(g_terrainGBufferUniforms[TU_HEIGHT_MAP_SIZE], (float)m_config.heightmapSize);
    glUniform1f(g_terrainGBufferUniforms[TU_LOD_DISTANCE], m_config.lodDistance);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    if (m_materialAtlas != 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_materialAtlas);
    }
    if (m_desertTex != 0) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_desertTex);
    }
    if (m_materialPbrAtlas != 0) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, m_materialPbrAtlas);
    }
    if (m_desertNormalTex != 0) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, m_desertNormalTex);
    }
    if (m_desertRoughTex != 0) {
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, m_desertRoughTex);
    }

    const float pageScale = (float)kMaterialPageSize / (float)kMaterialAtlasSize;
    for (const auto* chunk : visibleChunks) {
        auto pageIt = m_chunkPage.find(ChunkCoord{chunk->getChunkX(), chunk->getChunkY()});
        if (m_materialAtlas != 0 && pageIt != m_chunkPage.end()) {
            const int px = pageIt->second % kMaterialPagesPerSide;
            const int py = pageIt->second / kMaterialPagesPerSide;
            glUniform2f(g_terrainGBufferUniforms[TU_ATLAS_PAGE],
                        (float)px * pageScale, (float)py * pageScale);
            const glm::vec3 origin = chunk->getWorldPosition();
            glUniform2f(g_terrainGBufferUniforms[TU_CHUNK_ORIGIN], origin.x, origin.z);
            glUniform1f(g_terrainGBufferUniforms[TU_CHUNK_SIZE], m_config.chunkSize);
            glUniform1f(g_terrainGBufferUniforms[TU_USE_ATLAS], 1.0f);
        } else {
            glUniform1f(g_terrainGBufferUniforms[TU_USE_ATLAS], 0.0f);
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

// ---------------------------------------------------------------------------
// Opt-in height-based splat-map render path
// ---------------------------------------------------------------------------
// Draws the visible chunk batch through shaderSystem/terrain_splat.* (the
// modern multi-layer PBR shader). Re-uses the existing frustum culler + LOD
// filter + position-only TerrainVertex layout, so it stays in lock-step with
// the physics heightfield. Binds real per-layer PBR texture sets (grass /
// rocky terrain / aerial rocks on units 1..16 per terrain_splat.h); the snow
// peak band is tint-only (no asset) and falls back to m_whiteTexture. AO is
// proxied from each layer's roughness map where no AO asset exists.
// ---------------------------------------------------------------------------
void Terrain::renderSplat(const glm::mat4& view, const glm::mat4& projection,
                          const glm::vec3& cameraPos, ShadowMapper* shadow,
                          const LightingEnvironment& lighting) const {
    Shader* splat = terrainSplatLoad();
    if (!splat) return;

    TerrainOptimizations::FrustumCuller culler;
    if (m_frustumCulling) culler.update(projection * view);

    std::vector<const TerrainChunk*> visibleChunks;
    visibleChunks.reserve(m_chunks.size());

    for (const auto& [coord, chunk] : m_chunks) {
        if (m_frustumCulling && !culler.isBoxInFrustum(
                (chunk->getBoundsMin() + chunk->getBoundsMax()) * 0.5f,
                (chunk->getBoundsMax() - chunk->getBoundsMin()) * 0.5f)) {
            continue;
        }
        if (chunk->getLOD() >= 3) continue;
        visibleChunks.push_back(chunk.get());
    }

    if (visibleChunks.empty()) return;

    std::sort(visibleChunks.begin(), visibleChunks.end(),
        [&cameraPos](const TerrainChunk* a, const TerrainChunk* b) {
            return a->getDistanceToCamera(cameraPos) < b->getDistanceToCamera(cameraPos);
        });

    // Uniforms shared by every chunk (view/proj/etc. set once per batch).
    // All lighting/fog/sky values are read from the canonical LightingEnvironment
    // (loaded from config/cvars.ini by RenderPipeline) so the terrain, the mesh
    // lighting and the vegetation all share ONE sun / fog / ambient definition.
    // #3: env threaded in by the caller -- no Instance() here.
    const LightingEnvironment& env = lighting;

    TerrainSplatParams params;
    params.view       = view;
    params.projection = projection;
    params.viewPos    = cameraPos;
    // uLightDir is the SUNSHINE direction (terrain_splat.frag computes
    // L = normalize(-uLightDir)). The environment stores surface->sun, so negate.
    params.lightDir   = -env.sunDirection;
    params.waterLevel = 5.0f;
    params.heightmapSize = m_config.heightmapSize;
    params.texScale    = g_terrainTexScale;
    // POM normalizes the heightmap to [0,1] by heightScale, then ray-marches it
    // for view-dependent texture parallax (ported from the RHI pbr_mesh kernel).
    params.heightScale     = m_config.heightScale;
    // POM + tessellation are live-tunable via cvars (config/cvars.ini or the in-game
    // console) — the big win for on-the-fly visual iteration. The legacy
    // CVar::getFloat returns the fallback if the var isn't registered, so these
    // are safe even before cvars.ini is loaded. terrain_pom_scale = 0 disables
    // parallax (vertex displacement carries the terrain on its own).
    params.parallaxScale   = CVar::Instance().getFloat("terrain_pom_scale",   0.04f);
    params.tessFactorOuter = CVar::Instance().getFloat("terrain_tess_outer",  4.0f);
    params.tessFactorInner = CVar::Instance().getFloat("terrain_tess_inner",  4.0f);
    params.tessFadeDist    = CVar::Instance().getFloat("terrain_tess_fade",  80.0f);
    // Fog + sky from the canonical environment (edit config/cvars.ini to retune).
    params.fogHorizon  = env.fogHorizon;
    params.fogZenith   = env.fogZenith;
    params.fogNear     = env.fogNear;
    params.fogFar      = env.fogFar;
    params.fogHeight   = env.fogHeight;
    params.fogHeightFalloff = env.fogHeightFalloff;
    params.fogMaxOpacity     = env.fogMaxOpacity;
    params.fogEnabled        = env.fogEnabled &&
                                CVar::Instance().getFloat("terrain_fog_enable", 1.0f) > 0.5f;
    params.skyTint          = env.skyLightColor;
    params.groundBounce     = env.groundBounce;
    params.ambientStrength  = env.ambientStrength;
    params.detailNormalStrength = env.detailNormalStrength;
    params.skyLightStrength     = env.skyLightStrength;

    // Height-based layer bands (world metres) matched to the loaded true-PBR
    // texture sets: lush grass lowlands -> rocky ground -> aerial stone high
    // -> snow peaks. Texture-backed layers use a neutral white tint so the
    // albedo texel data shows through; the tint-only snow peak keeps a cool
    // white tint. Tints + heights are tunable here without recompiling shaders.
    params.layerHeight     = glm::vec4( 2.0f, 16.0f, 38.0f, 62.0f);
    params.layerBlendWidth = glm::vec4( 6.0f,  9.0f,  9.0f,  8.0f);
    params.layerSlope      = glm::vec4( 0.0f,  0.15f, 0.60f, 0.85f);
    params.layerTint[0] = glm::vec3(1.0f);                  // grass (texture-backed)
    params.layerTint[1] = glm::vec3(1.0f);                  // rocky ground (texture-backed)
    params.layerTint[2] = glm::vec3(1.0f);                  // aerial stone (texture-backed)
    params.layerTint[3] = glm::vec3(0.88f, 0.92f, 1.0f);    // snow (tint-only peak)

    // --- optional CSM: bind the cascade array + light-space matrices ----------
    // terrainSplatBind wires the uniform side; we bind the GL object here so
    // the shadow sampler resolves (unit 20, per terrain_splat.h). When no
    // shadow mapper is supplied the shader leaves uShadowEnabled == 0 and the
    // CSM function is a safe no-op.
    if (shadow && shadow->IsInitialized() && shadow->GetCascadeArrayTex() != 0) {
        shadow->Bind(kSplatShadowUnit);
        params.shadowEnabled = true;
        for (int i = 0; i < 3; ++i) {
            params.shadowMatrices[i] = shadow->GetLightSpaceMatrix(i);
            params.shadowSplits[i]   = shadow->GetSplitDistance(i);
        }
    }
    terrainSplatBind(splat, params);

    // True-PBR per-layer textures on units 1..16 (see terrain_splat.h). Each
    // layer binds its real albedo/normal/roughness/AO set; any slot that is
    // missing (e.g. the tint-only snow peak, or a texture that failed to load)
    // falls back to m_whiteTexture (neutral: white albedo, flat-up normal,
    // rough/AO = 1.0). AO without a dedicated map is proxied from the layer's
    // roughness channel, matching the legacy desert bake.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_heightTexture);
    const GLuint whiteFallback   = m_whiteTexture;
    const GLuint flatNormal      = m_flatNormalTexture;
    for (int i = 0; i < 4; ++i) {
        const SplatLayerTextures& layer = m_splatLayers[i];
        GLuint albedo = layer.albedo ? layer.albedo : whiteFallback;
        GLuint normal = layer.normal ? layer.normal : flatNormal;
        GLuint rough  = layer.rough  ? layer.rough  : whiteFallback;
        // AO: prefer the baked AO map; if absent, proxy from the roughness
        // channel (crevices darken); otherwise neutral white.
        GLuint ao     = layer.ao ? layer.ao
                                 : (layer.rough ? layer.rough : whiteFallback);
        glActiveTexture(GL_TEXTURE0 + kSplatFirstAlbedoUnit + i);
        glBindTexture(GL_TEXTURE_2D, albedo);
        glActiveTexture(GL_TEXTURE0 + kSplatFirstNormalUnit + i);
        glBindTexture(GL_TEXTURE_2D, normal);
        glActiveTexture(GL_TEXTURE0 + kSplatFirstRoughUnit  + i);
        glBindTexture(GL_TEXTURE_2D, rough);
        glActiveTexture(GL_TEXTURE0 + kSplatFirstAoUnit     + i);
        glBindTexture(GL_TEXTURE_2D, ao);
    }
    glActiveTexture(GL_TEXTURE0);  // default back to unit 0

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Tessellation: if the 4-stage program linked, emit triangle patches (the
    // chunk's index buffer is already a triangle list, so patch size 3 maps
    // 1:1) and let the TCS/TES subdivide + re-displace the heightfield. If the
    // program fell back to VS+FS, draw plain GL_TRIANGLES (vertex displacement
    // + POM still active — tessellation is the only thing skipped).
    // terrain_tess_enable (default 1 = on): lets an on-device build force the
    // 2-stage VS+FS path to isolate TES link/runtime issues (glslc validates
    // syntax only, not tessellation behaviour). No behaviour change when set=1.
    bool tess = terrainSplatTessEnabled() &&
                CVar::Instance().getFloat("terrain_tess_enable", 1.0f) > 0.5f;
    if (tess) {
        glPatchParameteri(GL_PATCH_VERTICES, 3);
    }

    // Diagnostic: chunk visibility + tessellation state (every ~3s at 60fps).
    // Told "terrain invisible but boulders draw" -> if this reads visibleChunks=0
    // the frustum culler is eating the near chunk (camera angle / cam-behind-ground);
    // if it reads >0 the terrain draws but outputs dark (sampler/lighting at runtime).
    static int s_splatDiag = 0;
    if (++s_splatDiag % 180 == 1) {
        std::cout << "[Terrain] renderSplat: visibleChunks=" << visibleChunks.size()
                  << " tess=" << (tess ? 1 : 0)
                  << " heightmapSize=" << params.heightmapSize
                  << " cam=(" << (int)params.viewPos.x << "," << (int)params.viewPos.y
                  << "," << (int)params.viewPos.z << ")\n";
    }

    for (const auto* chunk : visibleChunks) {
        chunk->draw(tess);
    }
}
