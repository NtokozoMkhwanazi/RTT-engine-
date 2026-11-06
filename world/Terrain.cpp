#include "Terrain.h"
#include "TerrainOptimizations.h"
#include "renderer/ShadowMapper.h"   // for the opt-in CSM shadow sampling
#include "lighting/LightingEnvironment.h" // canonical sun / fog / sky standard
#include "editor/config.h"               // forUI.txt Part 2/3: live splat params (Config::getTerrainSplatConfig)
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <vector>
#include <filesystem>
#include <unordered_map>   // TerrainTexCache (material texture reuse across rebuilds)

#include "shaderSystem/Shader.h"       // Shader::ID for the renderSplat runtime diagnostic
#include "shaderSystem/stb_image.h"
#include <chrono>                       // forUI.txt Bake HUD: steady_clock bake timing
#include "renderer/RVTProfiler.h"       // forUI.txt Bake HUD: RVT bake telemetry registry
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
static void LoadARMTexturesRaw(const char* path, GLuint& roughOut, GLuint& aoOut);

// ============================================================================
// Terrain material-texture cache
// ============================================================================
// A landscape rebuild destroys and re-creates the Terrain, which re-ran every
// material texture load: 4K stbi / OpenEXR decodes plus glGenerateTextureMipmap
// — measured at ~1 s per texture and 17-22 s per rebuild (the "editor freezes
// for seconds on Reload Complete Stage" stall), and the same again on boot.
// The authored splat/desert textures do NOT change between rebuilds, so the
// decoded + uploaded GL textures are kept here, keyed by (loader kind, path),
// and reused. This makes a rebuild cost only the heightmap decode.
//
// Ownership: the cache owns these GL textures, so Terrain's destructor must NOT
// delete them (see ~Terrain). Terrain::ClearTextureCache() releases them while
// the GL context is still alive.
namespace {

struct TerrainTexCache {
    std::unordered_map<std::string, GLuint> single;
    std::unordered_map<std::string, std::pair<GLuint, GLuint>> arm;  // rough, ao

    static std::string key(char kind, const char* path) {
        std::string k(1, kind);
        k += '|';
        k += path;
        return k;
    }

    GLuint getOrLoad(char kind, const char* path, GLuint (*load)(const char*)) {
        if (!path) return 0;
        const std::string k = key(kind, path);
        auto it = single.find(k);
        if (it != single.end()) return it->second;
        const GLuint id = load(path);
        if (id != 0) single.emplace(k, id);
        return id;
    }

    void getOrLoadArm(const char* path, GLuint& roughOut, GLuint& aoOut) {
        roughOut = 0;
        aoOut    = 0;
        if (!path) return;
        const std::string k = key('A', path);
        auto it = arm.find(k);
        if (it != arm.end()) {
            roughOut = it->second.first;
            aoOut    = it->second.second;
            return;
        }
        LoadARMTexturesRaw(path, roughOut, aoOut);
        if (roughOut != 0 || aoOut != 0) arm.emplace(k, std::make_pair(roughOut, aoOut));
    }

    void clear() {
        for (auto& kv : single) {
            if (kv.second != 0) glDeleteTextures(1, &kv.second);
        }
        single.clear();
        for (auto& kv : arm) {
            GLuint rough = kv.second.first;
            GLuint ao    = kv.second.second;
            if (rough != 0) glDeleteTextures(1, &rough);
            if (ao != 0)    glDeleteTextures(1, &ao);
        }
        arm.clear();
    }
};

TerrainTexCache g_terrainTexCache;

} // namespace

static GLuint LoadEXRTextureRGBA_uncached(const char* path) {
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
static GLuint LoadSRGBTexture_uncached(const char* path) {
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
static GLuint LoadRTexture_uncached(const char* path) {
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
static GLuint LoadLinearRGBTexture_uncached(const char* path) {
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
static void LoadARMTexturesRaw(const char* path, GLuint& roughOut, GLuint& aoOut) {
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

// Cached front-ends for the material loaders (see TerrainTexCache). These are
// the entry points initialize() uses; the *_uncached/_Raw functions above do the
// real decode + GL upload and only run once per unique asset path.
static GLuint LoadEXRTextureRGBA(const char* path) {
    return g_terrainTexCache.getOrLoad('E', path, &LoadEXRTextureRGBA_uncached);
}
static GLuint LoadSRGBTexture(const char* path) {
    return g_terrainTexCache.getOrLoad('S', path, &LoadSRGBTexture_uncached);
}
static GLuint LoadRTexture(const char* path) {
    return g_terrainTexCache.getOrLoad('R', path, &LoadRTexture_uncached);
}
static GLuint LoadLinearRGBTexture(const char* path) {
    return g_terrainTexCache.getOrLoad('L', path, &LoadLinearRGBTexture_uncached);
}
static void LoadARMTextures(const char* path, GLuint& roughOut, GLuint& aoOut) {
    g_terrainTexCache.getOrLoadArm(path, roughOut, aoOut);
}

void Terrain::ClearTextureCache() {
    g_terrainTexCache.clear();
}

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
    // NOTE: m_desertTex / m_desertNormalTex / m_desertRoughTex are NOT deleted
    // here — they come from the shared material-texture cache (TerrainTexCache),
    // which owns them so a landscape rebuild doesn't re-decode and re-upload
    // 4K assets. They are released by Terrain::ClearTextureCache() at teardown.
    m_desertTex = 0;
    m_desertNormalTex = 0;
    m_desertRoughTex = 0;
}

// forUI.txt: load an authored heightmap (.exr float or .jpg/.png L8) into the
// CPU m_heightmap, replacing the procedural Perlin arena. The sampled range is
// remapped to [0, heightScale] (matching generateHeightmap's `height *
// heightScale`). Reads bottom-up (EXR native / stbi flip) so the GPU texture
// upload is a straight glTextureSubImage2D with no extra rowswap. Returns
// false (leaving any existing heightmap intact) on a missing/unreadable file.
// GL-free heightmap decode: EXR / JPG / PNG / TGA -> square float grid remapped
// to [0, heightScale]. Split out of loadHeightmap() so the editor can run this
// on a worker thread (it is a multi-second 4K decode) and hand the result to
// initialize(), keeping the landscape rebuild off the main thread.
bool Terrain::DecodeHeightmapFile(const std::string& path, float heightScale,
                                  std::vector<float>& out, int& outSize) {
    out.clear();
    outSize = 0;
    namespace fs = std::filesystem;
    if (path.empty() || !fs::exists(path) || !fs::is_regular_file(path)) {                std::cerr << "[Terrain] Heightmap not found: " << path << "\n";
        return false;
    }
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    std::vector<float> data;
    int w = 0, h = 0;

    if (ext == ".exr") {
        try {
            Imf::InputFile file(path.c_str());
            const Imath::Box2i& dw = file.header().dataWindow();
            w = dw.max.x - dw.min.x + 1;
            h = dw.max.y - dw.min.y + 1;
            if (w <= 0 || h <= 0) return false;
            std::vector<half> px(static_cast<size_t>(w) * h, half(0.0f));
            Imf::FrameBuffer fb;
            const Imf::ChannelList& chans = file.header().channels();
            const char* chanName = chans.findChannel("R") ? "R"
                          : chans.findChannel("Y") ? "Y" : nullptr;
            if (!chanName && chans.begin() != chans.end())
                chanName = chans.begin().name();
            if (!chanName) {
                std::cerr << "[Terrain] EXR heightmap has no readable channel: "
                          << path << "\n";
                return false;
            }
            const int xStride = sizeof(half);
            const int64_t yStride = static_cast<int64_t>(xStride) * w;
            const char* base = reinterpret_cast<const char*>(px.data());
            fb.insert(chanName, Imf::Slice(Imf::HALF, const_cast<char*>(base),
                                           xStride, yStride));
            file.setFrameBuffer(fb);
            file.readPixels(dw.min.y, dw.max.y);
            data.resize(static_cast<size_t>(w) * h);
            for (size_t i = 0; i < data.size(); ++i)
                data[i] = float(px[i]);
        } catch (const std::exception& e) {
            std::cerr << "[Terrain] EXR heightmap load failed: " << path << ": "
                      << e.what() << "\n";
            return false;
        }
    } else {
        // JPG / PNG / TGA -> 8-bit grayscale (L8) -> [0,1].
        stbi_set_flip_vertically_on_load(true);  // bottom-up (GL convention)
        int n = 0;
        unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 1);
        if (!px || w <= 0 || h <= 0) {
            if (px) stbi_image_free(px);
            std::cerr << "[Terrain] Heightmap image load failed: " << path << "\n";
            return false;
        }
        data.resize(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<float>(px[i]) / 255.0f;
        stbi_image_free(px);
    }

    if (data.empty()) return false;

    // Remap [min,max] -> [0, heightScale].
    float mn = *std::min_element(data.begin(), data.end());
    float mx = *std::max_element(data.begin(), data.end());
    float span = (mx - mn);
    if (span <= 0.0f) span = 1.0f;
    for (auto& v : data)
        v = ((v - mn) / span) * heightScale;

    // The terrain grid assumes a square heightfield — crop non-square inputs
    // to the smaller axis (no crash, no OOB texture subimage).
    if (w != h) {
        const int size = (w < h ? w : h);
        std::vector<float> sq(static_cast<size_t>(size) * size, mn);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                sq[static_cast<size_t>(y) * size + x] =
                    data[static_cast<size_t>(y) * w + x];
        data = std::move(sq);
        w = h = size;
        std::cerr << "[Terrain] heightmap not square; cropping to " << size
                  << "x" << size << "\n";
    }
    out     = std::move(data);
    outSize = w;
    std::cout << "[Terrain] Heightmap loaded: " << path << " " << w << "x"
              << h << " (range " << mn << ".." << mx << ", scale "
              << heightScale << "m)\n";
    return true;
}

bool Terrain::loadHeightmap(const std::string& path) {
    std::vector<float> data;
    int size = 0;
    if (!DecodeHeightmapFile(path, m_config.heightScale, data, size)) return false;
    m_heightmap = std::move(data);
    m_config.heightmapSize = size;
    return true;
}

// Re-upload the current CPU m_heightmap to the GL_R32F height texture. Used
// for live sculpt (@:ModifyHeightmapRegion already patches sub-regions) and
// for a live heightmap reload that doesn't require regenerating chunk VBOs.
void Terrain::uploadHeightTexture() {
    if (m_heightTexture == 0 || m_heightmap.empty() || m_config.heightmapSize <= 0)
        return;
    glTextureSubImage2D(m_heightTexture, 0, 0, 0, m_config.heightmapSize,
                        m_config.heightmapSize, GL_RED, GL_FLOAT, m_heightmap.data());
}

void Terrain::initialize(const std::vector<float>* preloadedHeightmap, int preloadedSize) {
    // LOD band must scale with the chunk: TerrainChunk::updateLOD switches at
    // lodDistance * {1,2,4} measured from the chunk CENTRE, and every render
    // path drops chunks at LOD >= 3. A 500 m chunk (engine_config.json sets
    // chunkSize=500) keeps its centre 0..354 m away, so the stale 60 m default
    // put EVERY chunk past 4*60=240 m -> LOD 3 -> the terrain drew nothing at
    // all (viewport showed empty sky with a floating bot). Deriving the band
    // from the chunk size keeps the intended CDLOD behaviour (LOD 0 at the
    // chunk you stand on, coarsening ~1 chunk-size per level) for any chunk
    // size, while an explicitly larger lodDistance is still honoured.
    if (m_config.lodDistance < m_config.chunkSize) {
        m_config.lodDistance = m_config.chunkSize;
    }

    std::cout << "[Terrain] Initializing (async chunk loading)...\n";
    std::cout << "  Chunk size: " << m_config.chunkSize << "m\n";
    std::cout << "  Chunk resolution: " << m_config.chunkResolution << "x" << m_config.chunkResolution << "\n";
    std::cout << "  View distance: " << m_config.viewDistance << " chunks\n";
    std::cout << "  LOD distance: " << m_config.lodDistance << "m\n";

    // Pre-decoded heightmap (worker thread, editor stage reload): adopt it
    // directly instead of re-reading and re-decoding the 4K source here.
    if (preloadedHeightmap != nullptr && preloadedSize > 0 &&
        preloadedHeightmap->size() == static_cast<size_t>(preloadedSize) * preloadedSize) {
        m_heightmap = *preloadedHeightmap;
        m_config.heightmapSize = preloadedSize;
        std::cout << "[Terrain] Using pre-decoded heightmap (" << preloadedSize
                  << "x" << preloadedSize << ")\n";
    } else if (!m_config.heightmapPath.empty()) {
        std::cout << "[Terrain] Loading authored heightmap: "
                  << m_config.heightmapPath << "\n";
        if (!loadHeightmap(m_config.heightmapPath)) {
            std::cerr << "[Terrain] Falling back to procedural Perlin terrain\n";
            generateHeightmap();
        }
    } else {
        generateHeightmap();
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

    // ---- Verify.txt: artist-authored terrain material textures ----
    // The artist's config (terrain_material block) specifies the albedo /
    // normal / roughness / AO paths for the 4 splat layers + the desert base
    // textures. When the config path is empty or the file is missing, fall
    // back to the compiled-in default so the terrain still renders.
    // NO procedural material generation — the artist must provide textures.

    // Helper: pick the config path if non-empty, else the hardcoded default.
    auto terrainPath = [](const std::string& configured, const char* fallback) -> const char* {
        return (configured.empty() || !std::filesystem::exists(configured)) ? fallback : configured.c_str();
    };

    // Verify.txt: load desert/base textures from config or fallback defaults.
    {
        stbi_set_flip_vertically_on_load(true);
        int w = 0, h = 0, n = 0;
        const char* desertPath = terrainPath(m_config.desertAlbedoPath, kTerrainDesertTexPath);
        unsigned char* px = stbi_load(desertPath, &w, &h, &n, 3);
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
            glBindTexture(GL_TEXTURE_2D, 0);
            std::cout << "[Terrain] Desert albedo loaded: " << desertPath
                      << " (" << w << "x" << h << ")\n";
        } else {
            if (px) stbi_image_free(px);
            std::cerr << "[Terrain] Desert albedo NOT found: " << desertPath << "\n";
        }
        if (px) stbi_image_free(px);
    }

    {
        const char* normalPath = terrainPath(m_config.desertNormalPath, kTerrainNormalTexPath);
        if (m_desertNormalTex == 0) {
            // rocky_ground ships its normal map as EXR; JPG/PNG normals use the
            // stbi-based loader. Dispatch by extension to match the splat-layer
            // path so no desert texture silently fails to load.
            std::string np(normalPath);
            std::transform(np.begin(), np.end(), np.begin(), ::tolower);
            const bool npExr = np.size() >= 4 &&
                               np.compare(np.size() - 4, 4, ".exr") == 0;
            m_desertNormalTex = npExr ? LoadEXRTextureRGBA(normalPath)
                                      : LoadLinearRGBTexture(normalPath);
        }
        if (m_desertNormalTex == 0)
            std::cerr << "[Terrain] Desert normal NOT found: " << normalPath << "\n";
    }

    {
        const char* roughPath = terrainPath(m_config.desertRoughPath, kTerrainRoughTexPath);
        if (m_desertRoughTex == 0) {
            GLuint desertAo = 0;
            LoadARMTextures(roughPath, m_desertRoughTex, desertAo);
            if (m_desertRoughTex == 0)
                std::cerr << "[Terrain] Desert rough/ARM NOT found: " << roughPath << "\n";
        }
    }

    // ---- True-PBR per-layer texture sets for the splat shader ----------------
    // Each layer pulls paths from the artist's config; falls back to the
    // compiled-in defaults for any path that's empty or missing on disk.
    // verify.txt: artists may point these at EXR (marble_cliff) or JPG
    // (coast_rocks / coast_land_rocks) — the loader detects the file type
    // by extension and dispatches to LoadEXRTextureRGBA (.exr) or the
    // stbi-based JPG/PNG readers, so no texture silently fails.

    // Helper: is this path an EXR?
    auto isExr = [](const char* p) -> bool {
        if (!p) return false;
        std::string s(p);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s.size() >= 4 && s.compare(s.size()-4, 4, ".exr") == 0;
    };

    // Layer 0: grass
    m_splatLayers[0].albedo = LoadSRGBTexture(m_config.materialLayers[0].albedo.empty()
                                              ? kTerrainGrassAlbedoPath : m_config.materialLayers[0].albedo.c_str());
    m_splatLayers[0].normal = LoadEXRTextureRGBA(m_config.materialLayers[0].normal.empty()
                                                 ? kTerrainGrassNormalPath : m_config.materialLayers[0].normal.c_str());
    m_splatLayers[0].rough  = LoadEXRTextureRGBA(m_config.materialLayers[0].rough.empty()
                                                 ? kTerrainGrassRoughPath : m_config.materialLayers[0].rough.c_str());
    m_splatLayers[0].ao     = LoadRTexture(m_config.materialLayers[0].ao.empty()
                                           ? kTerrainGrassAoPath : m_config.materialLayers[0].ao.c_str());

    // Layer 1: marble_cliff / coastal rocks (supports EXR or JPG textures)
    {
        const char* armPath  = m_config.materialLayers[1].rough.empty()
                               ? kTerrainCoastRocksArmPath : m_config.materialLayers[1].rough.c_str();
        const char* albPath  = m_config.materialLayers[1].albedo.empty()
                               ? kTerrainCoastRocksAlbedoPath : m_config.materialLayers[1].albedo.c_str();
        const char* norPath  = m_config.materialLayers[1].normal.empty()
                               ? kTerrainCoastRocksNormalPath : m_config.materialLayers[1].normal.c_str();
        const char* aoPath   = m_config.materialLayers[1].ao.empty()
                               ? kTerrainCoastRocksArmPath : m_config.materialLayers[1].ao.c_str();

        if (isExr(armPath)) {
            m_splatLayers[1].rough = LoadEXRTextureRGBA(armPath);
        } else {
            GLuint rough1 = 0, ao1 = 0;
            LoadARMTextures(armPath, rough1, ao1);
            m_splatLayers[1].rough = rough1;
            // If AO is not separately specified, use the AO from the ARM split
            if (!m_config.materialLayers[1].ao.empty()) {
                m_splatLayers[1].ao = LoadRTexture(aoPath);
            } else {
                m_splatLayers[1].ao = ao1;
            }
        }
        m_splatLayers[1].albedo = LoadSRGBTexture(albPath);
        m_splatLayers[1].normal = isExr(norPath)
                                  ? LoadEXRTextureRGBA(norPath)
                                  : LoadLinearRGBTexture(norPath);
        // Load AO if it's a separate JPG/PNG file (not from ARM split)
        if (!m_splatLayers[1].ao && !isExr(aoPath)) {
            m_splatLayers[1].ao = LoadRTexture(aoPath);
        }
    }

    // Layer 2: coastal land rocks (supports EXR or JPG textures)
    {
        const char* armPath  = m_config.materialLayers[2].rough.empty()
                               ? kTerrainCoastLandRocksArmPath : m_config.materialLayers[2].rough.c_str();
        const char* albPath  = m_config.materialLayers[2].albedo.empty()
                               ? kTerrainCoastLandRocksAlbedoPath : m_config.materialLayers[2].albedo.c_str();
        const char* norPath  = m_config.materialLayers[2].normal.empty()
                               ? kTerrainCoastLandRocksNormalPath : m_config.materialLayers[2].normal.c_str();
        const char* aoPath   = m_config.materialLayers[2].ao.empty()
                               ? kTerrainCoastLandRocksArmPath : m_config.materialLayers[2].ao.c_str();

        if (isExr(armPath)) {
            m_splatLayers[2].rough = LoadEXRTextureRGBA(armPath);
        } else {
            GLuint rough2 = 0, ao2 = 0;
            LoadARMTextures(armPath, rough2, ao2);
            m_splatLayers[2].rough = rough2;
            if (!m_config.materialLayers[2].ao.empty()) {
                m_splatLayers[2].ao = LoadRTexture(aoPath);
            } else {
                m_splatLayers[2].ao = ao2;
            }
        }
        m_splatLayers[2].albedo = LoadSRGBTexture(albPath);
        m_splatLayers[2].normal = isExr(norPath)
                                  ? LoadEXRTextureRGBA(norPath)
                                  : LoadLinearRGBTexture(norPath);
        if (!m_splatLayers[2].ao && !isExr(aoPath)) {
            m_splatLayers[2].ao = LoadRTexture(aoPath);
        }
    }

    // Layer 3 (snow peaks): tint-only — no texture asset, snow tint carries colour.

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

    // forUI.txt Bake HUD: capture high-resolution bake timing for the viewport diagnostic HUD.
    auto startCompileTick = std::chrono::steady_clock::now();

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

    // forUI.txt Bake HUD: record elapsed bake duration for the viewport diagnostic HUD.
    auto endCompileTick = std::chrono::steady_clock::now();
    float elapsedMs = std::chrono::duration<float, std::milli>(endCompileTick - startCompileTick).count();
    RVTProfiler::Instance().recordBake(elapsedMs);

    return page;
}

// forUI.txt Pillar 3: CPU-side heightmap sculpt. Mutates the master m_heightmap
// float cache with the exact four falloff curves as the fragment shader, then
// does a fast in-place glTextureSubImage2D sub-region sync (no full re-alloc).
void Terrain::ModifyHeightmapRegion(float worldX, float worldZ, float radius,
                                     float strength, int falloffType, float softWidth) {
    if (m_heightmap.empty() || m_config.heightmapSize <= 0 || radius <= 0.001f) return;

    int centerTexelX = static_cast<int>(std::round(worldX));
    int centerTexelZ = static_cast<int>(std::round(worldZ));
    int radiusTexels = static_cast<int>(std::ceil(radius));

    int minX = std::clamp(centerTexelX - radiusTexels, 0, m_config.heightmapSize - 1);
    int maxX = std::clamp(centerTexelX + radiusTexels, 0, m_config.heightmapSize - 1);
    int minZ = std::clamp(centerTexelZ - radiusTexels, 0, m_config.heightmapSize - 1);
    int maxZ = std::clamp(centerTexelZ + radiusTexels, 0, m_config.heightmapSize - 1);

    bool heightmapChanged = false;
    float outerRadius = radius;
    float innerRadius = radius * (1.0f - glm::clamp(softWidth, 0.0f, 1.0f));

    for (int z = minZ; z <= maxZ; ++z) {
        for (int x = minX; x <= maxX; ++x) {
            float texelWorldX = static_cast<float>(x);
            float texelWorldZ = static_cast<float>(z);

            float distanceToBrush = glm::distance(glm::vec2(texelWorldX, texelWorldZ), glm::vec2(worldX, worldZ));

            if (distanceToBrush <= outerRadius) {
                float factor = 1.0f;

                if (distanceToBrush > innerRadius && std::abs(outerRadius - innerRadius) > 0.001f) {
                    float normalizedT = (distanceToBrush - innerRadius) / (outerRadius - innerRadius);

                    if (falloffType == 0) {
                        factor = 1.0f - (3.0f * normalizedT * normalizedT - 2.0f * normalizedT * normalizedT * normalizedT);
                    } else if (falloffType == 1) {
                        factor = 1.0f - normalizedT;
                    } else if (falloffType == 2) {
                        factor = std::sqrt(1.0f - normalizedT * normalizedT);
                    } else {
                        factor = 0.0f;
                    }
                } else if (distanceToBrush > innerRadius) {
                    factor = 0.0f;
                }

                size_t index = static_cast<size_t>(z) * m_config.heightmapSize + x;
                m_heightmap[index] += strength * factor;
                m_heightmap[index] = glm::clamp(m_heightmap[index], 0.0f, m_config.heightScale);
                heightmapChanged = true;
            }
        }
    }

    if (heightmapChanged && m_heightTexture != 0) {
        int subWidth  = (maxX - minX) + 1;
        int subHeight = (maxZ - minZ) + 1;

        std::vector<float> subRegionBuffer;
        subRegionBuffer.reserve(static_cast<size_t>(subWidth) * subHeight);

        for (int z = minZ; z <= maxZ; ++z) {
            for (int x = minX; x <= maxX; ++x) {
                subRegionBuffer.push_back(m_heightmap[static_cast<size_t>(z) * m_config.heightmapSize + x]);
            }
        }

        glTextureSubImage2D(m_heightTexture, 0, minX, minZ, subWidth, subHeight,
                            GL_RED, GL_FLOAT, subRegionBuffer.data());
    }
}

// forUI.txt Pillar 3: sculpt brush loop access to a chunk instance by coord.
TerrainChunk* Terrain::GetChunkInstance(int chunkX, int chunkY) {
    auto it = m_chunks.find(ChunkCoord{chunkX, chunkY});
    return (it != m_chunks.end()) ? it->second.get() : nullptr;
}

const TerrainChunk* Terrain::GetChunkInstance(int chunkX, int chunkY) const {
    auto it = m_chunks.find(ChunkCoord{chunkX, chunkY});
    return (it != m_chunks.end()) ? it->second.get() : nullptr;
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
        // verify.txt: debug — log first-fill chunk generation
        std::cout << "[Terrain] update: first-fill chunk generation around ("
                  << camChunkX << "," << camChunkY << ") viewDist=" << m_config.viewDistance << "\n";
        for (int dy = -m_config.viewDistance; dy <= m_config.viewDistance; dy++) {
            for (int dx = -m_config.viewDistance; dx <= m_config.viewDistance; dx++) {
                float dist = sqrt(dx * dx + dy * dy);
                if (dist > m_config.viewDistance) continue;

                ChunkCoord coord{camChunkX + dx, camChunkY + dy};
                if (m_chunks.count(coord)) continue;

                PendingChunk data = generateChunkData(coord.x, coord.y);
                size_t heightCount = data.heights.size();
                auto chunk = std::make_unique<TerrainChunk>(coord.x, coord.y,
                                                            data.chunkSize, data.resolution);
                chunk->generateHeightmapFromData(std::move(data.heights));
                bakeChunkMaterial(*chunk);   // RVT bake
                m_chunks[coord] = std::move(chunk);
                std::cout << "[Terrain]   chunk (" << coord.x << "," << coord.y
                          << ") created, heights=" << heightCount << "\n";
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
            static int debugFrame = 0;
            if (++debugFrame % 300 == 0) {
                std::cout << "[Terrain] renderSplat: " << m_chunks.size()
                          << " chunks total\n";
            }
            renderSplat(view, projection, cameraPos, shadow, lighting);
            return;
        }
        // Shader unavailable → silently fall back to the legacy path.
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

    // Batch: set shader state once, then draw all chunks. Guard the legacy
    // path: initTerrainShader() returns 0 when CompileTerrainProgram fails
    // (missing GL features / bad source). glUseProgram(0) + glUniform* on
    // program 0 is a no-op -> silently draws nothing ("loaded but
    // invisible"). Skip the batch instead and let the one-shot null-shader
    // diagnostic in renderSplat (or this guard) surface the cause.
    initTerrainShader();
    if (g_terrainShader == 0) {
        static int legacyNoShaderFrame = 0;
        if (++legacyNoShaderFrame % 300 == 0) {
            std::cerr << "[Terrain] legacy shader not compiled (g_terrainShader==0) -> "
                      << "legacy Terrain::render SKIPPED. Enable 'Multi-Layer Splat "
                      << "Shading Path' or fix the [TerrainShader] compile/link error.\n";
        }
        return;
    }
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
    // uHeightMapSize is a vec2 in the shader; the old glUniform1f only wrote
    // .x (leaving .y == 0), so the VS height UV = (worldXZ+0.5)/uHeightMapSize
    // divided by zero on Y -> NaN/Inf displacement -> collapsed/invisible
    // geometry in the deferred path. Write both components.
    glUniform2f(g_terrainGBufferUniforms[TU_HEIGHT_MAP_SIZE],
                (float)m_config.heightmapSize, (float)m_config.heightmapSize);
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
    if (!splat) {
        // Silent-failure clamp for forCheckme.txt §Solution 3: a broken splat
        // program used to make renderSplat() early-return with ZERO output, so
        // the terrain vanished with no diagnostic. Surface it (throttled) so a
        // shader compile/link regression is diagnosable at runtime.
        static int nullShaderFrame = 0;
        if (++nullShaderFrame % 300 == 0) {
            std::cerr << "[Terrain] renderSplat: splat shader NOT loaded (terrainSplatLoad()==null) -> "
                      << "falling through to legacy path. Check shaderSystem/terrain_splat.* compile logs.\n";
        }
        return;
    }

    TerrainOptimizations::FrustumCuller culler;
    if (m_frustumCulling) culler.update(projection * view);

    std::vector<const TerrainChunk*> visibleChunks;
    visibleChunks.reserve(m_chunks.size());

    int skippedFrustum = 0, skippedLOD = 0;
    for (const auto& [coord, chunk] : m_chunks) {
        if (m_frustumCulling && !culler.isBoxInFrustum(
                (chunk->getBoundsMin() + chunk->getBoundsMax()) * 0.5f,
                (chunk->getBoundsMax() - chunk->getBoundsMin()) * 0.5f)) {
            ++skippedFrustum;
            continue;
        }
        if (chunk->getLOD() >= 3) { ++skippedLOD; continue; }
        visibleChunks.push_back(chunk.get());
    }

    if (visibleChunks.empty()) {
        static int emptyWarnFrame = 0;
        if (std::getenv("TDBG") && m_chunks.size() > 0) {
            static bool dumped = false;
            if (!dumped && emptyWarnFrame > 150) {
                dumped = true;
                int shown = 0;
                int lodHist[8] = {0,0,0,0,0,0,0,0};
                const TerrainChunk* nearest = nullptr; float nd = 1e30f;
                for (const auto& [coord, chunk] : m_chunks) {
                    ++lodHist[std::min(7, std::max(0, chunk->getLOD()))];
                    const float d = chunk->getDistanceToCamera(cameraPos);
                    if (d < nd) { nd = d; nearest = chunk.get(); }
                }
                std::cerr << "[TDBG] lodHist 0..7:";
                for (int i = 0; i < 8; ++i) std::cerr << " " << lodHist[i];
                std::cerr << "\n";
                if (nearest) {
                    const glm::vec3 mn = nearest->getBoundsMin(), mx = nearest->getBoundsMax();
                    const glm::vec3 c = (mn + mx) * 0.5f, he = (mx - mn) * 0.5f;
                    std::cerr << "[TDBG] nearest dist=" << nd << " lod=" << nearest->getLOD()
                              << " boundsMin=(" << mn.x << "," << mn.y << "," << mn.z << ")"
                              << " boundsMax=(" << mx.x << "," << mx.y << "," << mx.z << ")"
                              << " inFrustum=" << culler.isBoxInFrustum(c, he) << "\n";
                }
                for (const auto& [coord, chunk] : m_chunks) {
                    if (shown++ >= 4) break;
                    const glm::vec3 mn = chunk->getBoundsMin(), mx = chunk->getBoundsMax();
                    const glm::vec3 c = (mn + mx) * 0.5f, he = (mx - mn) * 0.5f;
                    int rejectPlane = -1;
                    for (int i = 0; i < 6; ++i) {
                        const TerrainOptimizations::FrustumCuller::Plane& pl = culler.plane(i);
                        const float r = he.x * std::abs(pl.normal.x) +
                                        he.y * std::abs(pl.normal.y) +
                                        he.z * std::abs(pl.normal.z);
                        if (glm::dot(pl.normal, c) + pl.distance + r < 0.0f) { rejectPlane = i; break; }
                    }
                    std::cerr << "[TDBG] chunk " << coord.x << "," << coord.y
                              << " boundsMin=(" << mn.x << "," << mn.y << "," << mn.z << ")"
                              << " boundsMax=(" << mx.x << "," << mx.y << "," << mx.z << ")"
                              << " lod=" << chunk->getLOD()
                              << " rejectPlane=" << rejectPlane << "\n";
                }
                std::cerr << "[TDBG] cam=(" << cameraPos.x << "," << cameraPos.y << "," << cameraPos.z
                          << ") chunks=" << m_chunks.size() << " frustumSkip=" << skippedFrustum
                          << " lodSkip=" << skippedLOD << "\n";
                for (int c = 0; c < 4; ++c) {
                    std::cerr << "[TDBG] proj col" << c << " = "
                              << projection[c][0] << " " << projection[c][1] << " "
                              << projection[c][2] << " " << projection[c][3] << "\n";
                }
                for (int c = 0; c < 4; ++c) {
                    std::cerr << "[TDBG] view col" << c << " = "
                              << view[c][0] << " " << view[c][1] << " "
                              << view[c][2] << " " << view[c][3] << "\n";
                }
            }
        }
        if (++emptyWarnFrame % 180 == 0) {
            std::cout << "[Terrain] renderSplat: 0 visible chunks (total="
                      << m_chunks.size() << ", culling=" << m_frustumCulling
                      << ", frustumSkip=" << skippedFrustum
                      << ", lodSkip=" << skippedLOD
                      << ", cam=" << cameraPos.x << "," << cameraPos.y << "," << cameraPos.z << ")\n";
        }
        return;
    }

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
    params.texScale    = Config::getTerrainSplatConfig().globalTextureScale; // forUI.txt: live from UI
    // Fog + sky from the canonical environment (edit config/cvars.ini to retune).
    params.fogHorizon  = env.fogHorizon;
    params.fogZenith   = env.fogZenith;
    params.fogNear     = env.fogNear;
    params.fogFar      = env.fogFar;
    params.fogHeight   = env.fogHeight;
    params.fogHeightFalloff = env.fogHeightFalloff;
    params.fogMaxOpacity     = env.fogMaxOpacity;
    params.fogEnabled        = env.fogEnabled;
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
    // forUI.txt Part 2/3: layer bands + tints come live from the World Settings
    // "PBR Landscape Splat Layers" panel (Config::getTerrainSplatConfig(),
    // persisted to engine_config.json "terrain_splat_pbr"). Defaults match the
    // historical hardcoded bands, so there is zero visual regression until the
    // artist edits a slider.
    const Config::TerrainSplatUiState& ts = Config::getTerrainSplatConfig();
    params.layerHeight     = ts.layerHeights;
    params.layerBlendWidth = ts.blendWidths;
    params.layerSlope      = ts.slopeBands;
    params.layerTint[0] = ts.grassTint;   // grass (texture-backed)
    params.layerTint[1] = ts.rockTint;    // rocky ground (texture-backed)
    params.layerTint[2] = ts.stoneTint;   // aerial stone (texture-backed)
    params.layerTint[3] = ts.snowTint;    // snow (tint-only peak)

    // forUI.txt: live POM / normal-strength / tessellation sliders, plus the
    // heightfield world-Y range so the shader normalizes parallax to [0,1]
    // (params.heightScale -> uHeightScale, read by terrain_splat.frag).
    params.parallaxScale   = ts.parallaxScale;
    params.normalStrength  = ts.normalStrength;
    params.tessFactorOuter = ts.tessFactorOuter;
    params.tessFactorInner = ts.tessFactorInner;
    params.heightScale    = m_config.heightScale;

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

    // When the 4-stage (VS+TCS+TES+FS) program is active the TES expects
    // GL_PATCHES as the input primitive — drawing with GL_TRIANGLES means the
    // tessellation evaluation shader never fires, so the chunk's displaced
    // vertices stay at their flat grid positions (y=0) and the terrain reads
    // as a featureless grey quad (or nothing at all vs. the skybox). Use
    // GL_PATCHES when tessellation linked; fall back to GL_TRIANGLES for the
    // VS+FS-only path.
    const GLenum prim = terrainSplatTessEnabled() ? GL_PATCHES : GL_TRIANGLES;

    // Tell the driver each tessellation patch is a single triangle (3 control
    // vertices), matching the triangle-laid-out chunk index buffer. Without
    // this the GL_PATCH_VERTICES default (4) makes the TES assemble 4-vertex
    // patches over triangle indices, so the draw calls are dropped / rasterize
    // as a featureless quad — terrain renders invisible even though the CPU
    // heightmap (and the viewport FBO, once rebound) are both correct.
    // (forCheckme.txt §Solution 2)
    if (prim == GL_PATCHES) {
        glPatchParameteri(GL_PATCH_VERTICES, 3);
    }

    // Throttled runtime diagnostic (forCheckme.txt §"confirm whether the engine
    // actually reaches renderSplat"). Surfaces the per-frame render state so an
    // "invisible terrain" report can be correlated with a concrete value instead
    // of a silent no-op. Fires once every ~3s at 60Hz.
    static int diagFrame = 0;
    if (++diagFrame % 180 == 0) {
        std::cerr << "[Terrain] renderSplat: init=" << m_initialized
                  << " chunks=" << m_chunks.size()
                  << " visible=" << visibleChunks.size()
                  << " frustumSkip=" << skippedFrustum
                  << " lodSkip=" << skippedLOD
                  << " shader=" << (splat ? splat->ID : 0)
                  << " tess=" << (terrainSplatTessEnabled() ? 1 : 0)
                  << " prim=" << (prim == GL_PATCHES ? "PATCHES" : "TRIANGLES")
                  << " heightTx=" << m_heightTexture
                  << " splatLayers0.albedo=" << (m_splatLayers[0].albedo ? m_splatLayers[0].albedo : 0)
                  << " cull=" << m_frustumCulling << "\n";
    }

    for (const auto* chunk : visibleChunks) {
        glBindVertexArray(chunk->getVAO());
        glDrawElements(prim, chunk->getIndexCount(), GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);

    // DEFECT-5: the splat path previously never checked glGetError(), so a
    // failed-link/broken program (GL_INVALID_OPERATION from glUseProgram) or a
    // bad draw silently drew zero pixels every frame — exactly the
    // "loaded but invisible" symptom. Surface it (throttled) so it's diagnosable.
    static int glErrFrame = 0;
    if (++glErrFrame % 180 == 0) {
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            std::cerr << "[Terrain] renderSplat GL error: 0x" << std::hex << err
                      << " (prim=" << (prim == GL_PATCHES ? "PATCHES" : "TRIANGLES")
                      << ", tessEnabled=" << (terrainSplatTessEnabled() ? 1 : 0)
                      << ", chunks=" << visibleChunks.size() << ")\n" << std::dec;
        }
    }
}
