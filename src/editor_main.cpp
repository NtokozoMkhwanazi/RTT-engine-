/**
 * Editor application entry point.
 *
 * Creates the graphics context via the RHI (Render Hardware Interface):
 *   --graphics opengl  -> OpenGL backend (default; full engine support)
 *   --graphics vulkan  -> Vulkan backend (swapchain scene + ImGui editor UI)
 *
 * BOTH backends drive the SAME Dear ImGui editor: the menu bar, toolbar, left
 * / right / bottom panels, viewport and status bar from editor/ui.h. They
 * differ only in HOW the 3D scene reaches the screen:
 *
 *   - OpenGL: the EditorApplication renders the real world (terrain, world
 *     objects, the play-mode character) into a viewport FBO; the viewport
 *     panel displays that texture. ImGui is owned by Editor::ImGuiContext
 *     (imgui_impl_opengl3).
 *   - Vulkan: the RHI renders an OffscreenScene INTO the swapchain
 *     (camera UBO, depth, instancing); the viewport panel is transparent
 *     (swapchain-backed) so the scene shows through below the header strip,
 *     and the same editor UI draws on top via imgui_impl_vulkan
 *     (rhi->renderFrameImGui). The scene content is the engine's REAL
 *     world-object assets (quiver_tree, boulder) loaded through the CPU-only
 *     assimp path (Model::LoadModelData - no GL) and scattered over a
 *     procedural terrain grid - the first renderers ported onto the RHI mesh
 *     path. Right-drag orbits the viewport camera, scroll zooms.
 *
 * Build with:  make editor
 */
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cfloat>
#include <random>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <cxxabi.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "rhi/RHI.h"
#include "rhi/RHIMath.h"
#include "world/TerrainHeight.h"
#include "editor/editor_application.h"
#include "editor/imgui_context.h"
#include "editor/editor_theme.h"
#include "editor/phosphor_imgui.h"
#include "editor/fa_imgui.h"
#include "editor/ui_config.h"
#include "editor/config.h"
#include "editor/ui.h"
#include "editor/editor_state.h"
#include "editor/PlayModeController.h"
#include "editor/AnimatedCharacter.h"
#include "editor/undo_redo.h"
#include "editor/scene_manager.h"
#include "editor/entity_manager.h"
#include "animationSystem/Animator.h"
#include "animationSystem/Skinning.h"
#include "modelSystem/Model.h"
#include "shaderSystem/stb_image.h"
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <Imath/ImathBox.h>
#include <half.h>
#include "cameraSystem/ThirdPersonCamera.h"
#include "geospatial/GeoAPI.h"
#include "geospatial/GPSTracker.h"
#include <imgui.h>
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

// ============================================================================
// Crash handler (ported from test.cpp): writes a demangled backtrace to
// crash.log and exits with the conventional signal code. Installed for
// SIGSEGV/SIGABRT/SIGFPE at startup so a rendering/loading fault is debuggable
// instead of a silent death.
// ============================================================================
static void crashHandler(int signal) {
    FILE* f = fopen("crash.log", "w");
    if (f) {
        fprintf(f, "=== CRASH LOG ===\nSignal: %d\n", signal);
        void* array[64];
        int size = backtrace(array, 64);
        fprintf(f, "\nBacktrace (%d frames):\n", size);
        backtrace_symbols_fd(array, size, fileno(f));

        char** symbols = backtrace_symbols(array, size);
        if (symbols) {
            fprintf(f, "\nDemangled frames:\n");
            for (int i = 0; i < size; ++i) {
                char* sym = symbols[i];
                if (!sym) continue;
                const char* addr_start = strrchr(sym, '[');
                unsigned long addr = addr_start ? strtoul(addr_start + 1, nullptr, 16) : 0;
                char* open_paren = strchr(sym, '(');
                char* plus = open_paren ? strchr(open_paren, '+') : nullptr;
                if (!open_paren || !plus) {
                    fprintf(f, "#%d 0x%lx %s\n", i, addr, sym);
                    continue;
                }
                *plus = '\0';
                size_t len = 0;
                int status = 0;
                char* demangled = abi::__cxa_demangle(open_paren + 1, nullptr, &len, &status);
                fprintf(f, "#%d 0x%lx %s\n", i, addr,
                        (status == 0 && demangled) ? demangled : open_paren + 1);
                free(demangled);
                *plus = '+';
            }
            free(symbols);
        }
        fclose(f);
    }
    std::cerr << "\n*** CRASH: Signal " << signal << " ***\n";
    _exit(128 + signal);
}

// ============================================================================
// Shared editor UI - the SAME panels on both backends (mirrors bin/engine's
// frame): menu bar (with View -> Graphics backend toggle), scene dialogs,
// toolbar, left/right/bottom panels, viewport and status bar.
//
// On GL the 3D scene is a viewport FBO texture drawn by RenderViewport
// (viewportTexture != 0, swapchainBacked = false). On Vulkan the RHI scene
// renders into the swapchain and the viewport panel is transparent
// (viewportTexture = 0, swapchainBacked = true) so the scene shows through.
// `play` is null on Vulkan (no GL character pipeline there yet); camModeName /
// cameraPos / cameraTarget feed the viewport overlay text.
// ============================================================================
static void RenderSharedEditorUI(Editor::Editor& editor,
                                 const Editor::PlayModeController* play,
                                 const char* camModeName,
                                 GLuint viewportTexture,
                                 int windowW, int windowH,
                                 GLFWwindow* window, ImGuiIO& io,
                                 const glm::mat4& projection,
                                 const glm::mat4& view,
                                 const glm::vec3* cameraPos,
                                 const glm::vec3* cameraTarget,
                                 bool swapchainBacked) {
    if (editor.entityCache().dirty) {
        UI::RebuildEntityCache(editor.entityCache(), editor.world());
    }
    UI::RenderMenuBar(editor, editor.uiState.sceneFile);
    if (editor.shouldClose()) glfwSetWindowShouldClose(window, true);
    UI::RenderSceneDialogs(editor);
    UI::RenderToolbar(editor);
    UI::RenderLeftPanel(editor);
    UI::RenderRightPanel(editor, play, camModeName);
    UI::RenderBottomPanel(editor, io.Framerate);
    UI::RenderViewport(editor, viewportTexture, windowW, windowH, window, &io,
                       projection, view, camModeName, cameraPos, cameraTarget,
                       swapchainBacked);
    UI::RenderStatusBar(editor.world().getEntityCount(), editor.selectedEntity(),
                        io.Framerate, editor.isPlaying(), editor.wasPlaying(),
                        windowW, windowH);
    UI::RenderAboutDialog(editor.showAboutRef());
}

// ============================================================================
// Vulkan viewport content - the engine's REAL world-object assets through the
// RHI mesh path (OffscreenMesh: interleaved pos/normal/uv + indices +
// per-instance transforms, lambert-lit). Model::LoadModelData is the CPU-only
// assimp path (no GL), so the actual quiver_tree / BOULDER assets load fine on
// the GLFW_NO_API window. They are normalized to target heights and scattered
// like the GL world; a procedural heightfield grid stands in for the
// (GL-backed) Terrain until that renderer ports onto the RHI mesh path.
// ============================================================================

// The engine's REAL terrain heightfield (world/TerrainHeight.h - the same
// function the GL Terrain renderer samples), heightScale 25 like the editor's
// Config::getTerrainConfig().
static float TerrainHeight(float x, float z) {
    return terrain::heightAtWorld(x, z, 25.0f);
}

// Bounds of a merged model (used to normalize it to a target height and sit
// its base on the ground).
struct RawModelBounds {
    glm::vec3 min{FLT_MAX, FLT_MAX, FLT_MAX};
    glm::vec3 max{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    bool valid = false;
    float Height() const { return valid ? max.y - min.y : 0.0f; }
};

// Box-filter downsample of RGBA8 pixels (integer-ish ratio, row 0 = top).
// Keeps the editor's GPU footprint sane: 4K albedo maps on small props are
// overkill, and BOTH backends receive the SAME bytes so parity holds.
static void DownscaleRGBA(const uint8_t* src, int sw, int sh, int dw, int dh,
                          std::vector<uint8_t>& dst) {
    dst.resize(static_cast<size_t>(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
        const int y0 = y * sh / dh;
        const int y1 = std::max(y0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; ++x) {
            const int x0 = x * sw / dw;
            const int x1 = std::max(x0 + 1, (x + 1) * sw / dw);
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t* p = src + (static_cast<size_t>(yy) * sw + xx) * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; ++n;
                }
            uint8_t* d = dst.data() + (static_cast<size_t>(y) * dw + x) * 4;
            d[0] = static_cast<uint8_t>(r / n); d[1] = static_cast<uint8_t>(g / n);
            d[2] = static_cast<uint8_t>(b / n); d[3] = static_cast<uint8_t>(a / n);
        }
    }
}

// Attach the model's first diffuse/albedo texture (CPU pixels, from
// AsyncModelData - file I/O only, no GL) to the merged mesh. Picks a texture
// referenced by one of the raw meshes' texturePaths where possible, downscales
// to at most kMaxTexDim, and expands RGB -> RGBA. Meshes without a diffuse
// texture stay untextured (the RHI binds a 1x1 white texture for them).
static const int kMaxTexDim = 1024;
static void AttachDiffuseTexture(const AsyncModelData& data, RHI::OffscreenMesh& out) {
    // Paths the meshes actually reference as diffuse-ish textures.
    std::vector<std::string> wanted;
    for (const auto& rm : data.meshes) {
        for (const auto& tp : rm.texturePaths) {
            std::string ty = tp.second;
            std::transform(ty.begin(), ty.end(), ty.begin(), ::tolower);
            if (ty.find("diffuse") != std::string::npos ||
                ty.find("albedo") != std::string::npos ||
                ty.find("basecolor") != std::string::npos ||
                ty.find("base_color") != std::string::npos ||
                ty.find("color") != std::string::npos) {
                wanted.push_back(tp.first);
            }
        }
    }
    const TexturePixelData* tex = nullptr;
    for (const auto& t : data.textures) {
        if (t.isNormalMap || t.data.empty() || t.width <= 0 || t.height <= 0) continue;
        std::string ty = t.type;
        std::transform(ty.begin(), ty.end(), ty.begin(), ::tolower);
        const bool diffuseish = ty.find("diffuse") != std::string::npos ||
                                ty.find("albedo") != std::string::npos ||
                                ty.find("basecolor") != std::string::npos ||
                                ty.find("base_color") != std::string::npos ||
                                ty.find("color") != std::string::npos;
        if (!diffuseish) continue;
        // Prefer a texture referenced by the meshes (exact path match).
        if (std::find(wanted.begin(), wanted.end(), t.path) != wanted.end()) { tex = &t; break; }
        if (!tex) tex = &t;   // fallback: first diffuse in the file
    }
    if (!tex) return;

    const int srcW = tex->width, srcH = tex->height;
    int dw = srcW, dh = srcH;
    if (std::max(dw, dh) > kMaxTexDim) {
        const float s = static_cast<float>(kMaxTexDim) / static_cast<float>(std::max(dw, dh));
        dw = std::max(1, static_cast<int>(dw * s));
        dh = std::max(1, static_cast<int>(dh * s));
    }
    // Normalize to RGBA8 (RGB textures get an opaque alpha channel).
    std::vector<uint8_t> rgba;
    if (tex->channels >= 4 && dw == srcW && dh == srcH) {
        rgba = tex->data;
    } else if (tex->channels >= 4) {
        DownscaleRGBA(tex->data.data(), srcW, srcH, dw, dh, rgba);
    } else {
        // Expand RGB -> RGBA first, then downscale.
        std::vector<uint8_t> rgba4;
        rgba4.reserve(static_cast<size_t>(srcW) * srcH * 4);
        for (int i = 0; i < srcW * srcH; ++i) {
            rgba4.push_back(tex->data[static_cast<size_t>(i) * 3 + 0]);
            rgba4.push_back(tex->data[static_cast<size_t>(i) * 3 + 1]);
            rgba4.push_back(tex->data[static_cast<size_t>(i) * 3 + 2]);
            rgba4.push_back(255);
        }
        if (dw == srcW && dh == srcH) rgba = std::move(rgba4);
        else DownscaleRGBA(rgba4.data(), srcW, srcH, dw, dh, rgba);
    }
    out.texturePixels = std::move(rgba);
    out.textureWidth = dw;
    out.textureHeight = dh;
}

// Merge ALL raw sub-meshes of a CPU-loaded model into ONE interleaved
// OffscreenMesh (pos/normal/uv, 8 floats per vertex) so the whole model draws
// in a single indexed-instanced call. Also reports the merged bounds and
// attaches the model's diffuse texture (see AttachDiffuseTexture).
static bool MergeRawModelToOffscreenMesh(const AsyncModelData& data,
                                         RHI::OffscreenMesh& out,
                                         RawModelBounds& bounds) {
    size_t totalVerts = 0, totalIdx = 0;
    for (const auto& rm : data.meshes) {
        totalVerts += rm.vertices.size();
        totalIdx += rm.indices.size();
    }
    if (totalVerts == 0 || totalIdx == 0) return false;

    out.vertices.clear();
    out.indices.clear();
    out.instances.clear();
    out.texturePixels.clear();
    out.textureWidth = out.textureHeight = 0;
    out.vertices.reserve(totalVerts * 8);
    out.indices.reserve(totalIdx);
    uint32_t base = 0;
    for (const auto& rm : data.meshes) {
        for (const auto& v : rm.vertices) {
            out.vertices.push_back(v.Position.x);
            out.vertices.push_back(v.Position.y);
            out.vertices.push_back(v.Position.z);
            out.vertices.push_back(v.Normal.x);
            out.vertices.push_back(v.Normal.y);
            out.vertices.push_back(v.Normal.z);
            out.vertices.push_back(v.TexCoords.x);
            out.vertices.push_back(v.TexCoords.y);
            bounds.min = glm::min(bounds.min, v.Position);
            bounds.max = glm::max(bounds.max, v.Position);
        }
        for (uint32_t idx : rm.indices) out.indices.push_back(idx + base);
        base += static_cast<uint32_t>(rm.vertices.size());
    }
    bounds.valid = true;
    AttachDiffuseTexture(data, out);
    return true;
}

// Instance matrix: T(x,y,z) * Ry(rotDeg) * S(s) * C, where C re-centers the
// model in XZ and drops its base to local y=0 - so the object stands ON the
// placement point, `s * Height()` tall, and spins in place when rotated.
static RHI::Mat4 InstanceMatrix(const RawModelBounds& b, float x, float y, float z,
                                float rotDeg, float s) {
    const float a = rotDeg * 3.14159265f / 180.0f;
    const float c = std::cos(a), sn = std::sin(a);
    const float cx = (b.min.x + b.max.x) * 0.5f;
    const float cz = (b.min.z + b.max.z) * 0.5f;
    // Centering translation, scaled then rotated by Ry.
    const float vx = -cx * s, vy = -b.min.y * s, vz = -cz * s;
    RHI::Mat4 m{};
    m.m[0] = c * s;   m.m[1] = 0.0f;  m.m[2] = -sn * s; m.m[3] = 0.0f;
    m.m[4] = 0.0f;    m.m[5] = s;     m.m[6] = 0.0f;    m.m[7] = 0.0f;
    m.m[8] = sn * s;  m.m[9] = 0.0f;  m.m[10] = c * s;  m.m[11] = 0.0f;
    m.m[12] = x + (c * vx + sn * vz);
    m.m[13] = y + vy;
    m.m[14] = z + (-sn * vx + c * vz);
    m.m[15] = 1.0f;
    return m;
}

// Deterministic xorshift RNG so the scatter is stable across runs.
static unsigned g_rngState = 0x12345678u;
static float Rand01() {
    g_rngState ^= g_rngState << 13;
    g_rngState ^= g_rngState >> 17;
    g_rngState ^= g_rngState << 5;
    return (float)(g_rngState & 0xfffffu) / 1048575.0f;
}

// ============================================================================
// EXR Texture Loader (OpenEXR half-float -> RGBA8 pixels)
// ============================================================================
// Decodes a linear-HDR OpenEXR file into RGBA8 pixels (row 0 = TOP) so the
// Vulkan RHI can upload it as a standard combined image sampler.  The EXR
// half-float precision is tone-mapped to 8-bit; this is the same pattern used
// by Terrain.cpp::LoadEXRTextureRGBA for the GL path.
static bool LoadEXRToRGBA8(const char* path, std::vector<uint8_t>& outPixels,
                            int& outW, int& outH) {
    try {
        Imf::InputFile file(path);
        const Imath::Box2i& dw = file.header().dataWindow();
        outW = dw.max.x - dw.min.x + 1;
        outH = dw.max.y - dw.min.y + 1;

        // Read all available channels (R/G/B/A or single Y) as HALF
        std::vector<half> halfBuf(outW * outH * 4, 0.0f);
        Imf::FrameBuffer fb;
        const Imf::ChannelList& chans = file.header().channels();
        for (auto it = chans.begin(); it != chans.end(); ++it) {
            const std::string& name = it.name();
            int c = (name == "R" || name == "r") ? 0 :
                    (name == "G" || name == "g") ? 1 :
                    (name == "B" || name == "b") ? 2 :
                    (name == "A" || name == "a") ? 3 :
                    (name == "Y" || name == "y") ? 0 : -1;
            if (c >= 0) {
                fb.insert(name, Imf::Slice(Imf::HALF,
                    reinterpret_cast<char*>(halfBuf.data() + c),
                    sizeof(half) * 4, sizeof(half) * 4 * outW,
                    1, 1, 0.0));
            }
        }
        file.setFrameBuffer(fb);
        file.readPixels(dw.min.y, dw.max.y);

        // Convert half -> RGBA8 (tone-map linear HDR to 8-bit)
        outPixels.resize(outW * outH * 4);
        for (int y = 0; y < outH; ++y) {
            // OpenEXR origin is bottom-left: flip vertically
            int srcRow = (outH - 1 - y);
            for (int x = 0; x < outW; ++x) {
                int si = (srcRow * outW + x) * 4;
                int di = (y * outW + x) * 4;
                for (int ch = 0; ch < 4; ++ch) {
                    float v = halfBuf[si + ch];
                    // Simple linear HDR -> LDR clamp (no ACES here, that's
                    // done in the shader); most terrain EXRs are LDR range.
                    outPixels[di + ch] = (uint8_t)std::clamp(v * 255.0f, 0.0f, 255.0f);
                }
            }
        }
        std::cout << "[EXR] Loaded: " << path << " (" << outW << "x" << outH << ")\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[EXR] Load failed: " << path << ": " << e.what() << "\n";
        return false;
    }
}

// Load the real terrain albedo from disk. Tries EXR first (linear-HDR,
// Blender-authored), then falls back to JPG. Falls back to the procedural
// bake if nothing is found so the editor never renders a blank terrain.
static bool LoadTerrainTextureFile(RHI::OffscreenMesh& out, float worldSize) {
    // --- EXR first (Blender-authored linear-HDR terrain albedo) ----------
    const char* exrCandidates[] = {
        "assets/boulder/textures/rocky_terrain_diff_4k.exr",
        "assets/boulder/textures/rocky_terrain.exr",
    };
    for (const char* p : exrCandidates) {
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (LoadEXRToRGBA8(p, px, w, h) && w > 0 && h > 0) {
            const int kMaxDim = 2048;
            int dw = w, dh = h;
            if (std::max(dw, dh) > kMaxDim) {
                float s = (float)kMaxDim / (float)std::max(dw, dh);
                dw = std::max(1, (int)(w * s));
                dh = std::max(1, (int)(h * s));
            }
            if (dw != w || dh != h) {
                std::vector<uint8_t> resized(dw * dh * 4);
                for (int ry = 0; ry < dh; ++ry) {
                    for (int rx = 0; rx < dw; ++rx) {
                        int sx = std::clamp(rx * w / dw, 0, w - 1);
                        int sy = std::clamp(ry * h / dh, 0, h - 1);
                        std::memcpy(&resized[(ry * dw + rx) * 4],
                                    &px[(sy * w + sx) * 4], 4);
                    }
                }
                px = std::move(resized);
            }
            out.texturePixels = std::move(px);
            out.textureWidth = dw;
            out.textureHeight = dh;
            std::cout << "[Terrain] EXR albedo loaded: " << p
                      << " (" << dw << "x" << dh << ")\n";
            return true;
        }
    }

    // --- JPG fallback (legacy) ----------------------------------------------
    const char* candidates[] = {
        "assets/boulder/textures/rocky_terrain_diff_4k.jpg",
        "assets/boulder/textures/rocky_terrain.jpg",
    };
    stbi_set_flip_vertically_on_load(true);
    int w = 0, h = 0, c = 0;
    unsigned char* px = nullptr;
    for (const char* p : candidates) {
        if ((px = stbi_load(p, &w, &h, &c, 0)) != nullptr) {
            std::cout << "[Terrain] Real albedo loaded: " << p
                      << " (" << w << "x" << h << ", channels=" << c << ")\n";
            break;
        }
    }
    if (!px || w <= 0 || h <= 0) {
        std::cerr << "[Terrain] Real terrain texture not found; using procedural fallback\n";
        return false;
    }
    // Downscale to a terrain-appropriate resolution (max 2048 for a 300m grid).
    const int kTerrainMaxDim = 2048;
    int dw = w, dh = h;
    std::vector<uint8_t> pixels;
    if (std::max(dw, dh) > kTerrainMaxDim) {
        const float s = (float)kTerrainMaxDim / (float)std::max(dw, dh);
        dw = std::max(1, (int)(dw * s));
        dh = std::max(1, (int)(dh * s));
    }
    if (c >= 4 && dw == w && dh == h) {
        pixels.assign(px, px + (size_t)w * h * 4);
    } else if (c >= 3) {
        // Expand RGB->RGBA first, then downscale.
        std::vector<uint8_t> rgba4;
        rgba4.reserve((size_t)w * h * 4);
        for (int i = 0; i < w * h; ++i) {
            rgba4.push_back(px[(size_t)i*3+0]);
            rgba4.push_back(px[(size_t)i*3+1]);
            rgba4.push_back(px[(size_t)i*3+2]);
            rgba4.push_back(255);
        }
        if (dw == w && dh == h) pixels = std::move(rgba4);
        else DownscaleRGBA(rgba4.data(), w, h, dw, dh, pixels);
    } else {
        // Grayscale or unexpected: make a flat RGBA image.
        pixels.resize((size_t)w * h * 4, 255);
        for (int i = 0; i < w * h; ++i) {
            uint8_t v = c == 1 ? px[i] : 128;
            pixels[i*4+0] = pixels[i*4+1] = pixels[i*4+2] = v;
            pixels[i*4+3] = 255;
        }
        if (dw != w || dh != h) {
            std::vector<uint8_t> tmp;
            DownscaleRGBA(pixels.data(), w, h, dw, dh, tmp);
            pixels = std::move(tmp);
        }
    }
    stbi_image_free(px);
    out.texturePixels = std::move(pixels);
    out.textureWidth = dw;
    out.textureHeight = dh;
    return true;
}

// Procedural terrain albedo (512x512 RGBA, world-anchored to the grid's UVs):
// grass on gentle ground, rock on steep slopes, tinted by height, with a two-
// octave hash-noise for organic variation. Sampled through the RHI mesh
// texture path (both backends render the same bytes).
static void BuildTerrainTexture(RHI::OffscreenMesh& out, float worldSize) {
    const int TS = 512;
    out.textureWidth = TS;
    out.textureHeight = TS;
    out.texturePixels.resize(static_cast<size_t>(TS) * TS * 4);
    auto hash2 = [](float x, float y) -> float {
        const float n = std::sin(x * 127.1f + y * 311.7f) * 43758.5453f;
        return n - std::floor(n);
    };
    const float e = worldSize / TS;
    for (int y = 0; y < TS; ++y) {
        for (int x = 0; x < TS; ++x) {
            const float wx = (x + 0.5f) * e - worldSize * 0.5f;
            const float wz = (y + 0.5f) * e - worldSize * 0.5f;
            const float h = TerrainHeight(wx, wz);
            const float hxp = TerrainHeight(wx + e, wz), hxm = TerrainHeight(wx - e, wz);
            const float hzp = TerrainHeight(wx, wz + e), hzm = TerrainHeight(wx, wz - e);
            const float slope = std::sqrt((hxp - hxm) * (hxp - hxm) + (hzp - hzm) * (hzp - hzm)) / (2.0f * e);
            const float n1 = hash2(wx * 0.08f, wz * 0.08f);
            const float n2 = hash2(wx * 0.35f + 13.0f, wz * 0.35f + 7.0f);
            const float var = (n1 * 0.6f + n2 * 0.4f) * 2.0f - 1.0f;
            const float t = std::clamp(slope / 1.2f, 0.0f, 1.0f);
            glm::vec3 c = glm::mix(glm::vec3(0.30f + 0.05f * var, 0.42f + 0.06f * var, 0.20f + 0.04f * var),
                                   glm::vec3(0.42f + 0.06f * var, 0.40f + 0.06f * var, 0.36f + 0.05f * var), t);
            c *= 1.0f + std::clamp(h * 0.004f, -0.15f, 0.15f);   // lighten with altitude
            uint8_t* p = out.texturePixels.data() + (static_cast<size_t>(y) * TS + x) * 4;
            p[0] = static_cast<uint8_t>(std::clamp(c.r, 0.0f, 1.0f) * 255.0f);
            p[1] = static_cast<uint8_t>(std::clamp(c.g, 0.0f, 1.0f) * 255.0f);
            p[2] = static_cast<uint8_t>(std::clamp(c.b, 0.0f, 1.0f) * 255.0f);
            p[3] = 255;
        }
    }
}

// Procedural terrain grid (lambert-lit via the RHI mesh path). Stands in for
// the GL-backed Terrain renderer until it ports onto the RHI. The vertex grid
// carries the real heightfield; the albedo texture adds grass/rock coloring.
static void BuildGroundMesh(RHI::OffscreenMesh& out, int res, float worldSize) {
    out.vertices.clear(); out.indices.clear(); out.instances.clear();
    const float e = worldSize / (float)(res - 1);
    for (int y = 0; y < res; ++y) {
        for (int x = 0; x < res; ++x) {
            const float wx = ((float)x / (float)(res - 1) - 0.5f) * worldSize;
            const float wz = ((float)y / (float)(res - 1) - 0.5f) * worldSize;
            const float h = TerrainHeight(wx, wz);
            // Normal via central differences of the heightfield.
            const float gx = (TerrainHeight(wx + e, wz) - TerrainHeight(wx - e, wz)) / (2.0f * e);
            const float gz = (TerrainHeight(wx, wz + e) - TerrainHeight(wx, wz - e)) / (2.0f * e);
            const glm::vec3 n = glm::normalize(glm::vec3(-gx, 1.0f, -gz));
            out.vertices.push_back(wx);  out.vertices.push_back(h);  out.vertices.push_back(wz);
            out.vertices.push_back(n.x); out.vertices.push_back(n.y); out.vertices.push_back(n.z);
            out.vertices.push_back((float)x / (float)(res - 1));
            out.vertices.push_back((float)y / (float)(res - 1));
        }
    }
    for (int y = 0; y < res - 1; ++y) {
        for (int x = 0; x < res - 1; ++x) {
            const uint32_t i0 = (uint32_t)(y * res + x);
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + (uint32_t)res;
            const uint32_t i3 = i2 + 1;
            out.indices.push_back(i0); out.indices.push_back(i2); out.indices.push_back(i1);
            out.indices.push_back(i1); out.indices.push_back(i2); out.indices.push_back(i3);
        }
    }
    // Load the real terrain albedo texture (rocky_terrain_diff_4k.jpg). If the
    // file is missing, fall back to the procedural bake so the terrain is
    // never blank.
    if (!LoadTerrainTextureFile(out, worldSize)) {
        BuildTerrainTexture(out, worldSize);
    }
    RHI::OffscreenInstance inst{};
    const RHI::Mat4 id = RHI::mat4TranslateScale(0.0f, 0.0f, 0.0f, 1.0f);
    std::memcpy(inst.model, id.m, sizeof(id.m));
    // White: the terrain albedo texture carries the color.
    inst.color[0] = inst.color[1] = inst.color[2] = inst.color[3] = 1.0f;
    out.instances.push_back(inst);
}

// Load a model CPU-only (Model::LoadModelData - raw assimp data, no GL),
// CPU-decimate each sub-mesh with vertex clustering (GL-free, keeps the
// representative vertex's normals/uv/bones), then merge into ONE interleaved
// OffscreenMesh. The GL world decimates static props at load time too, so the
// Vulkan world gets the same treatment.
static bool LoadWorldModel(const std::string& path, RHI::OffscreenMesh& out,
                           RawModelBounds& bounds) {
    auto data = Model::LoadModelData(path);
    if (!data || !data->success) return false;
    for (auto& rm : data->meshes) {
        // Decimate every non-trivial sub-mesh: plants come out of assimp with
        // tens of thousands of triangles that are sub-pixel at instanced prop
        // scale - vertex clustering (GL-free, keeps normals/uv/bones) drops
        // them to a fraction. The threshold is low (300 verts) so dense plant
        // meshes (grass/periwinkle/othonna) are decimated too, not just the
        // world objects.
        if (rm.vertices.size() <= 300 || rm.indices.size() < 3) continue;
        glm::vec3 mn(FLT_MAX, FLT_MAX, FLT_MAX), mx(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const auto& v : rm.vertices) {
            mn = glm::min(mn, v.Position);
            mx = glm::max(mx, v.Position);
        }
        const float maxDim = std::max({mx.x - mn.x, mx.y - mn.y, mx.z - mn.z});
        if (maxDim <= 0.0f) continue;
        MeshUtils::VertexClustering(rm.vertices, rm.indices, maxDim / 48.0f);
    }
    return MergeRawModelToOffscreenMesh(*data, out, bounds);
}

// One placed world object, replicated from the GL VegetationSystem/WorldManager
// (see GenerateGLWorldPlacements).
struct PlacedProp {
    glm::vec3 pos;       // XZ chosen by the generator; Y snapped to the terrain
    float scale = 1.0f;  // GL placement scale (rendered height = refHeight * scale)
    float rotationDeg = 0.0f;
    int variant = 0;     // 0/1/2 per type
    glm::vec3 tint{1.0f};
};

// ============================================================================
// GL world placement parity: replicate VegetationSystem::generateForChunk +
// WorldManager's transfer-to-WorldObjectManager EXACTLY - same mt19937(42)
// seed, same distributions, same draw order, chunks x,z in [-viewDistance,
// viewDistance] row-major, empty-heightmap validity (no-op), then snapped to
// the real terrain. Both backends therefore place trees/boulders/plants in
// IDENTICAL world positions.
// ============================================================================
static void GenerateGLWorldPlacements(std::vector<PlacedProp>& trees,
                                      std::vector<PlacedProp>& rocks,
                                      std::vector<PlacedProp>& plants) {
    std::mt19937 rng(42);
    const float chunkSize = 80.0f;          // editor Config::getTerrainConfig
    const int viewDistance = 2;
    const float treeDensity = 0.002f, rockDensity = 0.0015f, grassDensity = 0.02f;
    const int maxTreesPerChunk = 16, maxRocksPerChunk = 10;
    const float minTreeHeight = 1.0f, maxTreeHeight = 2.0f;

    std::uniform_real_distribution<float> posDist(0.0f, chunkSize);
    std::uniform_real_distribution<float> heightDist(minTreeHeight, maxTreeHeight);
    std::uniform_int_distribution<int> typeDist(0, 2);
    std::uniform_real_distribution<float> scaleDist(0.5f, 2.0f);
    std::uniform_real_distribution<float> rotDist(0.0f, 360.0f);
    std::uniform_real_distribution<float> clusterJitter(-4.0f, 4.0f);
    std::uniform_real_distribution<float> plantScaleDist(0.6f, 1.4f);
    std::uniform_real_distribution<float> plantTypeDist(0.0f, 1.0f);
    std::uniform_real_distribution<float> plantTintDist(0.0f, 1.0f);

    for (int cx = -viewDistance; cx <= viewDistance; ++cx) {
        for (int cz = -viewDistance; cz <= viewDistance; ++cz) {
            const float wx0 = cx * chunkSize, wz0 = cz * chunkSize;

            // ---- Trees (VegetationSystem) ----
            const int numTrees = std::min((int)(chunkSize * chunkSize * treeDensity),
                                          maxTreesPerChunk);
            for (int i = 0; i < numTrees; ++i) {
                PlacedProp t;
                t.pos = glm::vec3(wx0 + posDist(rng), 0.0f, wz0 + posDist(rng));
                t.variant = typeDist(rng);
                t.scale = heightDist(rng) / 5.0f;   // WorldManager: height / 5
                trees.push_back(t);
            }

            // ---- Rocks (clustered outcrops) ----
            const int numRocks = std::min((int)(chunkSize * chunkSize * rockDensity),
                                          maxRocksPerChunk);
            const int rocksPerCluster = 3;
            const int numClusters = std::max(1, (numRocks + rocksPerCluster - 1) / rocksPerCluster);
            int rocksPlaced = 0;
            for (int c = 0; c < numClusters && rocksPlaced < numRocks; ++c) {
                const glm::vec2 center(wx0 + posDist(rng), wz0 + posDist(rng));
                const int here = std::min(rocksPerCluster, numRocks - rocksPlaced);
                for (int i = 0; i < here; ++i) {
                    PlacedProp r;
                    r.pos.x = std::clamp(center.x + clusterJitter(rng), wx0, wx0 + chunkSize - 0.01f);
                    r.pos.z = std::clamp(center.y + clusterJitter(rng), wz0, wz0 + chunkSize - 0.01f);
                    r.pos.y = 0.0f;
                    // GL builds vec3(scaleDist, scaleDist, scaleDist); only .x is used.
                    const float sx = scaleDist(rng);
                    (void)scaleDist(rng);
                    (void)scaleDist(rng);
                    r.scale = sx;
                    r.rotationDeg = rotDist(rng);
                    r.variant = typeDist(rng);
                    rocks.push_back(r);
                    ++rocksPlaced;
                }
            }

            // ---- Ground plants ----
            const int numPlants = std::clamp((int)(chunkSize * chunkSize * grassDensity), 20, 200);
            for (int i = 0; i < numPlants; ++i) {
                PlacedProp p;
                p.pos = glm::vec3(wx0 + posDist(rng), 0.0f, wz0 + posDist(rng));
                p.scale = plantScaleDist(rng);
                p.rotationDeg = rotDist(rng);
                const float r = plantTypeDist(rng);
                p.variant = (r < 0.80f) ? 0 : (r < 0.93f) ? 1 : 2;
                const float t = plantTintDist(rng);
                p.tint = (p.variant == 0)
                             ? glm::mix(glm::vec3(0.55f, 0.95f, 0.45f), glm::vec3(0.85f, 0.78f, 0.42f), t)
                             : glm::mix(glm::vec3(0.75f, 0.98f, 0.60f), glm::vec3(0.95f, 0.85f, 0.55f), t);
                plants.push_back(p);
            }
        }
    }

    // WorldManager::snapToTerrain - sit every placement on the real terrain.
    auto snap = [](PlacedProp& p) { p.pos.y = TerrainHeight(p.pos.x, p.pos.z); };
    for (auto& t : trees) snap(t);
    for (auto& r : rocks) snap(r);
    for (auto& p : plants) snap(p);
}

// Add `props` as instanced draws of `mesh`. GL scale semantics: a placement
// of `scale` renders refHeight*scale meters tall, so the instance scale is
// refHeight * scale / rawH (rawH = the merged model's actual height).
static void AddPropInstances(RHI::OffscreenMesh& mesh, const RawModelBounds& b,
                             float refHeight, const std::vector<PlacedProp>& props) {
    mesh.instances.clear();
    mesh.instances.reserve(props.size());
    // Authoring-local bounds half-extents: drive the debug AABB bounds boxes
    // (and would feed a future geometry-LOD selector) without re-deriving the
    // original model bounds per instance.
    mesh.boundsHalfExtents[0] = (b.max.x - b.min.x) * 0.5f;
    mesh.boundsHalfExtents[1] = (b.max.y - b.min.y) * 0.5f;
    mesh.boundsHalfExtents[2] = (b.max.z - b.min.z) * 0.5f;
    const float rawH = b.Height();
    if (rawH <= 0.001f) return;
    for (const auto& p : props) {
        RHI::OffscreenInstance inst{};
        const float s = refHeight * p.scale / rawH;
        const RHI::Mat4 m = InstanceMatrix(b, p.pos.x, p.pos.y, p.pos.z, p.rotationDeg, s);
        std::memcpy(inst.model, m.m, sizeof(m.m));
        inst.color[0] = p.tint.x; inst.color[1] = p.tint.y; inst.color[2] = p.tint.z;
        inst.color[3] = 1.0f;
        mesh.instances.push_back(inst);
    }
}

// Frustum + distance culling with a fade band for a static prop mesh. Each
// frame the camera moves, the visible subset is recomputed: instances outside
// the view frustum are dropped outright (the GL engine renderer gets the same
// win from its frustum culling), and instances in the outer fade band are
// uniformly scaled toward 0 so far props sink into the fog instead of popping
// (beyond the radius they are dropped entirely - sub-pixel / fully fogged).
// The mesh's instanceVersion bumps whenever the set changes, so the RHI
// re-uploads ONLY the small shared instance pool - the big static vertex/
// index buffers stay resident.
static int CullInstances(RHI::OffscreenMesh& mesh, const glm::mat4& viewProj,
                        const glm::vec3& camPos, float radius,
                        bool debugLOD = false, float lodBand1 = 0.40f,
                        float lodBand2 = 0.70f, float lodBand3 = 0.92f,
                        std::vector<RHI::OffscreenInstance>* debugBounds = nullptr) {
    // Frustum planes (Gribb-Hartmann) from the column-major view-projection.
    const float* m = &viewProj[0][0];
    const float planes[6][4] = {
        { m[3] + m[0],  m[7] + m[4],  m[11] + m[8],  m[15] + m[12] },  // left
        { m[3] - m[0],  m[7] - m[4],  m[11] - m[8],  m[15] - m[12] },  // right
        { m[3] + m[1],  m[7] + m[5],  m[11] + m[9],  m[15] + m[13] },  // bottom
        { m[3] - m[1],  m[7] - m[5],  m[11] - m[9],  m[15] - m[13] },  // top
        { m[3] + m[2],  m[7] + m[6],  m[11] + m[10], m[15] + m[14] },  // near
        { m[3] - m[2],  m[7] - m[6],  m[11] - m[10], m[15] - m[14] },  // far
    };
    auto inFrustum = [&](float x, float y, float z, float r) {
        for (const auto& p : planes) {
            const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (p[0] * x + p[1] * y + p[2] * z + p[3] < -len * r) return false;
        }
        return true;
    };

    // LOD bands (fractions of `radius`) drive tint + far thinning:
    //   (0, band1*r]    LOD0 full-detail   (green tint)
    //   (band1, band2*r] LOD1              (yellow tint)
    //   (band2, band3*r] LOD2              (red tint)
    //   beyond band3*r   dropped outright (saves draw calls / far LOD)
    const float band1 = radius * lodBand1, band2 = radius * lodBand2, band3 = radius * lodBand3;
    const float fadeStart = radius * 0.75f;
    const float fadeLen = std::max(radius - fadeStart, 1e-3f);
    const float cx = camPos.x, cz = camPos.z;    // 2D dist (XZ plane)
    const glm::vec3 halfExt(mesh.boundsHalfExtents[0], mesh.boundsHalfExtents[1],
                            mesh.boundsHalfExtents[2]);
    std::vector<RHI::OffscreenInstance> vis;
    vis.reserve(mesh.instances.size());
    if (debugBounds) debugBounds->clear();
    for (const auto& inst : mesh.instances) {
        const float px = inst.model[12], py = inst.model[13], pz = inst.model[14];
        const float dx = px - cx, dz = pz - cz;
        const float dist2 = dx * dx + dz * dz;
        if (dist2 > radius * radius) continue;              // beyond cull radius: gone
        const float dist = std::sqrt(dist2);
        if (dist > band3) continue;                        // LOD2 far thinning: dropped
        float fade = 1.0f;
        if (dist2 > fadeStart * fadeStart) {                // fade band into the fog
            const float t = (dist - fadeStart) / fadeLen;
            fade = 1.0f - t * t;
            if (fade <= 0.02f) continue;
        }
        if (!inFrustum(px, py, pz, radius * 0.5f)) continue;
        RHI::OffscreenInstance out = inst;
        if (fade < 1.0f) {                                  // uniform scale into the fog
            for (int i = 0; i < 9; ++i) out.model[i] *= fade;
        }
        // Debug LOD tint: near=green, mid=yellow, far=red.
        if (debugLOD) {
            const glm::vec3 tint = (dist > band2) ? glm::vec3(1.00f, 0.50f, 0.40f)
                               : (dist > band1) ? glm::vec3(1.00f, 0.90f, 0.50f)
                               :                 glm::vec3(0.60f, 1.00f, 0.70f);
            out.color[0] *= tint.x;
            out.color[1] *= tint.y;
            out.color[2] *= tint.z;
        }
        // Debug AABB bounds: a translucent box around the prop's world footprint.
        // The legacy cube buffer (vkScene.instances) is empty in the Vulkan scene
        // path, so the renderer owns it entirely when debugging is on.
        if (debugBounds) {
            RHI::OffscreenInstance cube{};
            // Uniform instance scale = |row 0| of the authored instance matrix.
            const float sc = std::sqrt(inst.model[0] * inst.model[0]
                                     + inst.model[1] * inst.model[1]
                                     + inst.model[2] * inst.model[2]);
            const float hx = halfExt.x * sc, hy = halfExt.y * sc, hz = halfExt.z * sc;
            // Unit cube is +/-0.5, so scale by 2*halfExt. RHI::Mat4.m is column
            // major (matches glm): diagonal = scale, m[12..14] = translation.
            RHI::Mat4 cm{};
            cm.m[0] = 2.0f * hx; cm.m[5] = 2.0f * hy; cm.m[10] = 2.0f * hz;
            cm.m[12] = px; cm.m[13] = py; cm.m[14] = pz; cm.m[15] = 1.0f;
            std::memcpy(cube.model, cm.m, sizeof(cube.model));
            cube.color[0] = 0.18f; cube.color[1] = 0.62f; cube.color[2] = 0.95f;
            cube.color[3] = 0.22f;
            debugBounds->push_back(cube);
        }
        vis.push_back(out);
    }
    // Always refresh: fade/LOD depend on the exact camera distance, so the
    // visible set changes every frame the camera moves. The pool is small (a
    // few hundred KB), and the RHI re-copies ONLY the instance slice - the
    // static vertex/index buffers stay resident.
    mesh.instances.swap(vis);
    mesh.instanceVersion++;
    return static_cast<int>(mesh.instances.size());
}

// CPU-skinned character for the Vulkan viewport: the bot's raw meshes merged
// into ONE OffscreenMesh with per-vertex bone ids/weights kept on the CPU, and
// a full AnimatedCharacter controller (the SAME pure-logic locomotion code the
// GL play mode uses - no GL) driving the pose. Per frame the controller takes
// keyboard input, the instance transform tracks its world matrix, and the
// shared Skinning::SkinVertices helper rewrites the interleaved vertex buffer
// from the pristine bind-pose copy; the version bump makes the RHI re-upload
// ONLY this mesh (incremental path).
//
// Built on the heap (unique_ptr) on purpose: AnimatedCharacter owns an Animator
// that holds a RAW pointer into its deep-copied skeleton, so the controller
// must never move after construction (a return-by-value move would leave the
// pointer dangling into the destroyed temporary).
struct SkinnedCharacter {
    RHI::OffscreenMesh mesh;
    // Pristine bind-pose copy of mesh.vertices (interleaved pos/normal/uv).
    // Skinning MUST read FROM this buffer INTO mesh.vertices: skinning in
    // place reads back the previous frame's output, so the bone transforms
    // compound frame over frame (p_n = T_n * ... * T_1 * p_bind) and the mesh
    // deforms into a mess within seconds.
    std::vector<float> bindVertices;
    std::vector<glm::ivec4> boneIds;     // per merged vertex
    std::vector<glm::vec4> boneWeights;  // per merged vertex
    RawModelBounds bounds;
    std::unique_ptr<AnimatedCharacter> chara;   // locomotion controller
    uint64_t versionCounter = 1;
    bool valid = false;
};

// Load bot.fbx CPU-only, merge all sub-meshes into one OffscreenMesh (keeping
// bone ids/weights on the CPU for skinning), then wire up the AnimatedCharacter
// (model + all locomotion clips + motion matching) exactly like the GL play
// mode. The per-frame instance transform comes from the controller's world
// matrix, so it starts at the origin facing forward on the real terrain.
static std::unique_ptr<SkinnedCharacter> BuildSkinnedCharacter() {
    auto c = std::make_unique<SkinnedCharacter>();
    auto data = Model::LoadModelData("assets/bot.fbx");
    if (!data || !data->success) return c;

    size_t totalVerts = 0, totalIdx = 0;
    for (const auto& rm : data->meshes) { totalVerts += rm.vertices.size(); totalIdx += rm.indices.size(); }
    if (totalVerts == 0 || totalIdx == 0) return c;

    c->mesh.vertices.reserve(totalVerts * 8);
    c->mesh.indices.reserve(totalIdx);
    c->boneIds.reserve(totalVerts);
    c->boneWeights.reserve(totalVerts);
    uint32_t base = 0;
    for (const auto& rm : data->meshes) {
        for (const auto& v : rm.vertices) {
            c->mesh.vertices.push_back(v.Position.x);
            c->mesh.vertices.push_back(v.Position.y);
            c->mesh.vertices.push_back(v.Position.z);
            c->mesh.vertices.push_back(v.Normal.x);
            c->mesh.vertices.push_back(v.Normal.y);
            c->mesh.vertices.push_back(v.Normal.z);
            c->mesh.vertices.push_back(v.TexCoords.x);
            c->mesh.vertices.push_back(v.TexCoords.y);
            c->boneIds.push_back(v.BoneIDs);
            c->boneWeights.push_back(v.Weights);
            c->bounds.min = glm::min(c->bounds.min, v.Position);
            c->bounds.max = glm::max(c->bounds.max, v.Position);
        }
        for (uint32_t idx : rm.indices) c->mesh.indices.push_back(idx + base);
        base += static_cast<uint32_t>(rm.vertices.size());
    }
    c->bounds.valid = true;
    // Snapshot the raw bind-pose vertices BEFORE any skinning mutates
    // mesh.vertices - the skin always re-reads this pristine copy.
    c->bindVertices = c->mesh.vertices;
    AttachDiffuseTexture(*data, c->mesh);

    // Placeholder instance (identity - overwritten every frame from the
    // character's world matrix). Keeps the mesh non-empty so the RHI draw
    // path is exercised even before the first update.
    RHI::OffscreenInstance inst{};
    const RHI::Mat4 id = RHI::mat4TranslateScale(0.0f, 0.0f, 0.0f, 1.0f);
    std::memcpy(inst.model, id.m, sizeof(id.m));
    inst.color[0] = inst.color[1] = inst.color[2] = inst.color[3] = 1.0f;
    c->mesh.instances.push_back(inst);

    // Full locomotion controller: model + all locomotion clips + motion
    // matching - the identical code path the GL editor play mode drives. The
    // skeleton indices assigned by Model::LoadModelData are deterministic, so
    // the boneIds/weights extracted above index this controller's animator
    // correctly.
    auto ac = std::make_unique<AnimatedCharacter>();
    if (!ac->load("assets/bot.fbx")) return c;
    if (ac->loadLocomotion("assets") < 1) return c;
    ac->position = glm::vec3(0.0f, TerrainHeight(0.0f, 0.0f), 0.0f);
    c->chara = std::move(ac);
    c->valid = c->chara->ready();
    std::cout << "[EditorMain] Character: " << totalVerts << " verts, "
              << data->skeleton.bones.size() << " bones, "
              << c->chara->clipCount() << " clips, motion matching "
              << (c->chara->isMotionMatchingActive() ? "on" : "off")
              << ", target height " << c->chara->targetHeight() << "m\n";
    return c;
}

// Advance the character with the given input and skin every vertex from the
// PRISTINE bind-pose buffer into the mesh's interleaved buffer (shared
// Skinning::SkinVertices - normalized weights, same math as the GL shader).
// The instance transform tracks the controller's world matrix (position +
// heading + scale), so walking/running/crouching moves the rendered bot. Both
// version bumps make the RHI re-upload only this mesh's slice of the shared
// buffers (geometry) and its instance.
static void UpdateSkinnedCharacter(SkinnedCharacter& c, float dt,
                                   const CharacterInput& input) {
    if (!c.valid) return;
    c.chara->update(dt, input, TerrainHeight);
    // The rendered instance follows the character exactly - the same world
    // matrix the GL renderer uses for the play-mode bot.
    RHI::OffscreenInstance inst = c.mesh.instances.front();
    const glm::mat4 mm = c.chara->modelMatrix();
    std::memcpy(inst.model, glm::value_ptr(mm), sizeof(inst.model));
    c.mesh.instances[0] = inst;
    c.mesh.instanceVersion++;
    Skinning::SkinVertices(c.bindVertices, c.boneIds, c.boneWeights,
                           c.chara->animator()->GetFinalBoneMatrices(),
                           c.mesh.vertices);
    c.mesh.version = ++c.versionCounter;
}

// Preloaded Vulkan viewport content (CPU-only model loads, no GL). The world
// objects mirror the GL world's placements; the character is the bot CPU-
// skinned with the Idle clip.
struct VulkanScene {
    RHI::OffscreenMesh ground;
    RHI::OffscreenMesh trees;       // all tree variants share quiver_tree
    RHI::OffscreenMesh boulders;    // rock variant 0 (stone/cliff assets missing -> skipped like GL)
    RHI::OffscreenMesh grass;
    RHI::OffscreenMesh flowers;
    RHI::OffscreenMesh bushes;
    // bot.fbx + full locomotion controller (AnimatedCharacter: Idle/Walk/Run/
    // Jump/Fall/Crouch clips + motion matching), CPU-skinned per frame.
    // unique_ptr so the SkinnedCharacter (and its controller's skeleton
    // pointer) never moves.
    std::unique_ptr<SkinnedCharacter> character;
    std::string error;              // non-empty when an asset failed to load
};

static VulkanScene BuildVulkanScene() {
    VulkanScene s;
    BuildGroundMesh(s.ground, 192, 300.0f);
    s.ground.version = 1;

    std::vector<PlacedProp> trees, rocks, plants;
    GenerateGLWorldPlacements(trees, rocks, plants);

    auto fail = [&s](const char* what, const std::unique_ptr<AsyncModelData>& d) {
        s.error += std::string("[") + what + "] " +
                   (d && !d->error.empty() ? d->error : std::string("load failed")) + "; ";
    };

    RawModelBounds tb;
    if (LoadWorldModel("assets/quiver_tree/q.gltf", s.trees, tb)) {
        for (auto& t : trees) t.tint = glm::vec3(1.0f);  // authored albedo now arrives via the RHI mesh path
        AddPropInstances(s.trees, tb, 75.0f, trees);
    } else {
        fail("tree", Model::LoadModelData("assets/quiver_tree/q.gltf"));
    }

    RawModelBounds bb;
    if (LoadWorldModel("assets/boulder/BOULDER.gltf", s.boulders, bb)) {
        // Only variant 0 (boulder) - stone.fbx / Rock1.fbx are missing, so the
        // GL world skips variants 1/2 too (placeObject returns on unloaded models).
        std::vector<PlacedProp> boulderProps;
        for (auto& r : rocks) if (r.variant == 0) boulderProps.push_back(r);
        AddPropInstances(s.boulders, bb, 2.0f, boulderProps);
    } else {
        fail("boulder", Model::LoadModelData("assets/boulder/BOULDER.gltf"));
    }

    RawModelBounds gb;
    if (LoadWorldModel("assets/grass/grass.gltf", s.grass, gb)) {
        std::vector<PlacedProp> g; for (auto& p : plants) if (p.variant == 0) g.push_back(p);
        AddPropInstances(s.grass, gb, 1.0f, g);
    } else {
        fail("grass", Model::LoadModelData("assets/grass/grass.gltf"));
    }
    RawModelBounds fb;
    if (LoadWorldModel("assets/periwinkle/periwinkle_plant_4k.gltf", s.flowers, fb)) {
        std::vector<PlacedProp> f; for (auto& p : plants) if (p.variant == 1) f.push_back(p);
        AddPropInstances(s.flowers, fb, 1.0f, f);
    } else {
        fail("flower", Model::LoadModelData("assets/periwinkle/periwinkle_plant_4k.gltf"));
    }
    RawModelBounds ob;
    if (LoadWorldModel("assets/othonna/othonna.gltf", s.bushes, ob)) {
        std::vector<PlacedProp> o; for (auto& p : plants) if (p.variant == 2) o.push_back(p);
        AddPropInstances(s.bushes, ob, 1.0f, o);
    } else {
        fail("bush", Model::LoadModelData("assets/othonna/othonna.gltf"));
    }

    // Play-mode character: the bot driven by the full locomotion controller
    // (motion matching over Idle/Walk/Run/Jump/Fall/Crouch clips), standing on
    // the terrain at the world origin and controlled with WASD + Shift/C/Space.
    s.character = BuildSkinnedCharacter();
    if (!s.character || !s.character->valid) {
        fail("character", Model::LoadModelData("assets/bot.fbx"));
    }
    return s;
}

// Orbit camera for the Vulkan viewport (right-drag orbit, scroll zoom). The
// orbit target follows the animated character, so walking around the scene
// keeps the bot in view; it stays glued above the real terrain so the camera
// never drops underground.
struct OrbitCamera {
    // Close, cinematic framing on the character (the whole point of the
    // viewport is watching the animation). dist 13 at pitch 15 puts the
    // ~1.8m bot front-and-center with the world receding behind it.
    float yaw = 40.0f;     // degrees around Y
    float pitch = 15.0f;   // degrees above the horizon
    float dist = 13.0f;
    glm::vec3 target{0.0f, TerrainHeight(0.0f, 0.0f) + 1.6f, 0.0f};
    float autoOrbitT = 0.0f;   // seconds since the user last dragged - after
                               // a pause the camera slowly orbits to showcase
                               // the scene from all sides

    // Free-fly mode: WASD moves the camera directly (editor navigation)
    // instead of moving the character. Toggled with 'F' while the viewport
    // is hovered. Right-drag still orbits pitch/yaw; the camera sits at
    // flyEye and looks along the same direction the orbit camera would.
    bool freeFly = false;
    glm::vec3 flyEye{0.0f, 25.0f, 50.0f};
};

// Point the (static-content) scene's camera from the orbit state. Only the
// camera floats change per frame; the meshes stay cached.
static void SetSceneCamera(RHI::OffscreenScene& scene, const OrbitCamera& cam) {
    const float a = cam.pitch * 3.14159265f / 180.0f;
    const float b = cam.yaw * 3.14159265f / 180.0f;
    const float cp = std::cos(a);
    const float sp = std::sin(a);
    const float cy = std::cos(b), sy = std::sin(b);
    scene.camera.fovDeg = 60.0f;
    scene.camera.up[0] = 0; scene.camera.up[1] = 1; scene.camera.up[2] = 0;
    if (cam.freeFly) {
        // Free-fly: position is flyEye, look direction uses the same
        // yaw/pitch convention as the orbit camera (points "into" the scene
        // the way the orbit camera looks at its target).
        scene.camera.eye[0] = cam.flyEye.x;
        scene.camera.eye[1] = cam.flyEye.y;
        scene.camera.eye[2] = cam.flyEye.z;
        const float lookDist = 0.01f;
        scene.camera.target[0] = cam.flyEye.x - cp * cy * lookDist;
        scene.camera.target[1] = cam.flyEye.y - sp * lookDist;
        scene.camera.target[2] = cam.flyEye.z - cp * sy * lookDist;
    } else {
        scene.camera.eye[0] = cam.target.x + cam.dist * cp * cy;
        scene.camera.eye[1] = cam.target.y + cam.dist * sp;
        scene.camera.eye[2] = cam.target.z + cam.dist * cp * sy;
        scene.camera.target[0] = cam.target.x;
        scene.camera.target[1] = cam.target.y;
        scene.camera.target[2] = cam.target.z;
    }
}

// ============================================================================
// Vulkan viewport camera modes - ported from the GL engine loop (test.cpp):
// 0 Free / 1 Follow / 2 Orbit / 3 Top-Down / 4 First-Person. Mode ids match
// editor.uiState.playCameraMode, which the shared Camera menu + toolbar icons
// + World Settings combo already write (keys 0-4 switch too). 0 = fly camera;
// 1-4 orbit the character with the state-aware ThirdPersonCamera.
// ============================================================================
static const char* kVkCamModeNames[] = {"Free", "Follow", "Orbit", "Top-Down", "First-Person", "Cinematic"};
static constexpr int kVkCamModeCount = 6;

// Map the character's animation state to the follow camera's state so the
// camera's smoothing adapts per movement (idle/walk/run/jump/fall/crouch).
static CameraState VkAnimStateToCamState(AnimationState s) {
    switch (s) {
        case AnimationState::IDLE:        return CameraState::IDLE;
        case AnimationState::WALK:        return CameraState::WALK;
        case AnimationState::RUN:         return CameraState::RUN;
        case AnimationState::JUMP:        return CameraState::JUMP;
        case AnimationState::FALL:        return CameraState::FALL;
        case AnimationState::CROUCH:
        case AnimationState::CROUCH_WALK: return CameraState::CROUCH;
        default:                          return CameraState::IDLE;
    }
}

// Ported from test.cpp's UpdatePlayCamera - same math, with the mouse input
// supplied by the caller from ImGui instead of Input::InputState. Handles the
// three character-orbiting modes: Follow (0) auto-faces the bot's heading,
// Orbit (1) is a free mouse orbit with scroll zoom, Top-down (2) is the aerial
// overview, First-person (3) sits at head height looking along the heading.
static void UpdateVulkanPlayCamera(int mode, ThirdPersonCamera& cam,
                                   const AnimatedCharacter& cc, float dt, float aspect,
                                   bool drag, float mouseDx, float mouseDy,
                                   float scrollDy, bool& firstInit,
                                   int prevMode, float& fpPitch) {
    const glm::vec3 charPos = cc.position;
    const float heading = cc.heading;

    CameraInput camIn;
    camIn.characterPosition = charPos;
    camIn.characterVelocity = cc.velocity;
    camIn.moveMagnitude = cc.currentSpeed();
    camIn.isGrounded = cc.grounded;
    camIn.characterForward = glm::vec3(-std::sin(heading), 0.0f, -std::cos(heading));
    camIn.animState = VkAnimStateToCamState(cc.state());
    // Cinematic mode (mode == 4): drive the camera via the CameraController
    // cinematic clock (U7). The controller places the camera on its perpetual
    // orbit each frame and advances its internal timeline, bypassing the
    // yaw/pitch-driven ThirdPersonCamera smoothing below. setMode (which
    // resets the timeline) is called ONLY on entry; calling it per-frame
    // would reset cinematicTime to 0 and freeze the orbit.
    if (mode == 4) {
        static CameraController cinematicController(&cam);
        if (cinematicController.currentMode != CameraController::CameraMode::CINEMATIC) {
            cinematicController.setMode(CameraController::CameraMode::CINEMATIC);
        }
        cinematicController.update(dt, camIn);
        return;
    }

    // #7 Mouse-delta stutter: cap per-frame mouse movement so a single slow frame (an
    // fps spike) can't inject a huge yaw/pitch jump. ImGui io.MouseDelta already scales
    // with frame time (pixels moved since last frame), so naive `* dt` would AMPLIFY
    // spikes; a magnitude cap gives frame-rate-stable orbiting across modes 0/1/2/3.
    constexpr float kMaxMouseDelta = 40.0f;  // px/frame cap; tune to taste
    mouseDx = std::clamp(mouseDx, -kMaxMouseDelta, kMaxMouseDelta);
    mouseDy = std::clamp(mouseDy, -kMaxMouseDelta, kMaxMouseDelta);


    // ---- First-person: camera at head height, look follows heading + pitch --
    if (mode == 3) {
        if (prevMode != 3) fpPitch = 0.0f;
        if (drag) fpPitch = std::clamp(fpPitch + mouseDy * 0.1f, -75.0f, 75.0f);
        const float p = glm::radians(fpPitch);
        const glm::vec3 fwd = glm::normalize(
            glm::vec3(-std::sin(heading) * std::cos(p),
                       std::sin(p),
                       -std::cos(heading) * std::cos(p)));
        cam.position = charPos + glm::vec3(0.0f, 1.55f, 1.5f);  // eye height
        cam.target = cam.position + fwd * 10.0f;
        return;
    }

    // ---- Follow / Orbit / Top-down share the state-aware follow camera ------
    if (firstInit) {
        // Start slightly above and behind the character, chest-level target.
        cam.position = charPos + glm::vec3(0.0f, 2.2f, -3.5f);  // U6: seed behind the -Z-forward character
        cam.target = charPos + glm::vec3(0.0f, 1.3f, 0.0f);
        cam.currentState = camIn.animState;
        cam.yaw = 180.0f;   // U6 Z-forward: behind a -Z-forward character
        cam.pitch = 10.0f;
        cam.config.distance = 4.0f;
        cam.config.height = 1.6f;
        firstInit = false;
    }

    if (mode == 1) {  // Orbit: free mouse orbit + scroll zoom
        if (drag) {
            cam.yaw -= mouseDx;
            // Drag up = camera higher (Unreal orbit convention)
            cam.pitch = std::clamp(cam.pitch - mouseDy * 0.3f,
                                   cam.config.minPitch, cam.config.maxPitch);
        }
        cam.config.distance = std::clamp(cam.config.distance - scrollDy * 1.5f, 2.0f, 10.0f);
    } else if (mode == 2) {  // Top-down: aerial overview, rotate around the character
        if (drag) cam.yaw -= mouseDx;
        cam.pitch = 80.0f;
        cam.config.distance = 9.0f;
    } else {  // Follow: auto-face the bot's heading, right-drag tilts, scroll zooms
        const float targetYaw = glm::degrees(std::atan2(-std::sin(heading), -std::cos(heading)));  // U6 Z-forward atan2(fwd.x,fwd.z)
        float dy = targetYaw - cam.yaw;
        while (dy > 180.0f) dy -= 360.0f;
        while (dy < -180.0f) dy += 360.0f;
        cam.yaw += dy * std::min(1.0f, 1.0f - std::exp(-6.0f * dt));
        if (drag) {
            // Drag up = look from higher (Unreal convention)
            cam.pitch = std::clamp(cam.pitch - mouseDy * 0.3f, -20.0f, 55.0f);  // #7 unify 0.3 (Orbit pitch factor)
        } else {
            cam.pitch = glm::mix(cam.pitch, 10.0f, std::min(1.0f, 1.0f - std::exp(-3.0f * dt)));
        }
        cam.config.distance = std::clamp(cam.config.distance - scrollDy * 1.5f, 2.0f, 9.0f);
    }

    cam.update(dt, camIn, aspect);
}

// Load the persisted backend choice (graphics_api.cfg, written by the editor's
// View -> Graphics menu). The --graphics flag overrides it.
static RHI::GraphicsAPI LoadGraphicsApi() {
    std::ifstream f(RHI::graphicsApiConfigPath());
    std::string token;
    RHI::GraphicsAPI api = RHI::GraphicsAPI::OpenGL;
    if (f >> token && RHI::parseGraphicsApi(token, api)) {
        std::cout << "[EditorMain] graphics_api.cfg -> " << token << std::endl;
    }
    return api;
}

int main(int argc, char** argv) {
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
    signal(SIGFPE, crashHandler);

    RHI::GraphicsAPI api = LoadGraphicsApi();
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--graphics") == 0 && i + 1 < argc) {
            if (!RHI::parseGraphicsApi(argv[i + 1], api)) {
                std::cerr << "[EditorMain] Unknown graphics API '" << argv[i + 1]
                          << "' (expected opengl|vulkan); using opengl" << std::endl;
                api = RHI::GraphicsAPI::OpenGL;
            }
        }
    }

    auto rhi = RHI::createRHI(api);
    if (!rhi) {
        std::cerr << "[EditorMain] Failed to create " << RHI::graphicsApiName(api)
                  << " backend" << std::endl;
        return 1;
    }
    std::cout << "[EditorMain] Graphics backend: " << RHI::graphicsApiName(api) << std::endl;

    // Vulkan uses a smaller swapchain to keep the software rasterizer (lavapipe)
    // happy and to match the OpenGL editor's viewport scale. The GL backend
    // keeps the full 1920x1080 window; Vulkan renders into a 1280x720 swapchain.
    int initW = 1920, initH = 1080;
    if (api == RHI::GraphicsAPI::Vulkan) {
        initW = 1280; initH = 720;
    }
    if (!rhi->initialize(initW, initH, "3D Game Engine - Editor")) {
        std::cerr << "[EditorMain] Failed to initialize " << RHI::graphicsApiName(api)
                  << " backend" << std::endl;
        return 1;
    }

    if (api == RHI::GraphicsAPI::Vulkan) {
        // The engine's scene renderers are still GL-backed; the Vulkan path
        // renders the RHI scene (camera UBO + depth + instancing) into the
        // swapchain and draws the SAME Dear ImGui editor UI on top - menu
        // bar, toolbar, panels and a transparent viewport through which the
        // live scene shows. The scene content is real engine assets (trees /
        // boulders) loaded CPU-only and scattered over a terrain grid - the
        // world-object renderer's first port onto the RHI mesh path.
        std::cout << "[EditorMain] Vulkan mode: swapchain scene (real world "
                     "objects) + full editor UI.\n";

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        // Same look & feel as the GL editor: UI font + Phosphor/FontAwesome6
        // icons + persisted theme mode (ui_panel.cfg). The imgui_impl_vulkan
        // backend uploads the font atlas lazily on the first NewFrame().
        EditorTheme::LoadEditorFonts(io);
        PhosphorImGui::Load(io, 14.0f);
        FaImGui::Load(io);
        UIConfig::LoadConfig(&g_editor.uiState.showOutliner, &g_editor.uiState.showDetails);
        EditorTheme::SetThemeMode(UIConfig::gThemeMode == 1 ? EditorTheme::ThemeMode::Light
                                                            : EditorTheme::ThemeMode::Dark);
        EditorTheme::ApplyTheme();

        ImGui_ImplGlfw_InitForVulkan(rhi->window(), true);
        if (!rhi->initializeImGui()) {
            std::cerr << "[EditorMain] Failed to initialize ImGui on Vulkan"
                      << std::endl;
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            rhi->shutdown();
            return 1;
        }

        // Preload the viewport content ONCE: the engine's real world-object
        // assets (quiver_tree trees, boulders, plants) via the CPU-only
        // loader, plus the real terrain grid and the skinned character. Only
        // the camera + character pose change per frame.
        VulkanScene vkContent = BuildVulkanScene();
        if (!vkContent.error.empty()) {
            std::cerr << "[EditorMain] Vulkan scene warnings: " << vkContent.error << std::endl;
        }
        RHI::OffscreenScene vkScene;
        vkScene.meshes.push_back(vkContent.ground);
        if (!vkContent.trees.instances.empty()) vkScene.meshes.push_back(vkContent.trees);
        if (!vkContent.boulders.instances.empty()) vkScene.meshes.push_back(vkContent.boulders);
        if (!vkContent.grass.instances.empty()) vkScene.meshes.push_back(vkContent.grass);
        if (!vkContent.flowers.instances.empty()) vkScene.meshes.push_back(vkContent.flowers);
        if (!vkContent.bushes.instances.empty()) vkScene.meshes.push_back(vkContent.bushes);
        // Character mesh appended LAST (stable index - skinning rewrites it
        // every frame; the RHI re-uploads just this slice incrementally).
        const size_t charMeshIdx = vkScene.meshes.size();
        if (vkContent.character && vkContent.character->valid)
            vkScene.meshes.push_back(vkContent.character->mesh);
        // Distant terrain must never clip: far plane out to the world edge,
        // atmospheric haze (exponential-squared fog) blending to the same sky
        // blue as the clear color so the horizon melts into the sky.
        vkScene.camera.farPlane = 600.0f;
        vkScene.fogDensity = 0.005f;
        vkScene.fogColor[0] = 0.45f; vkScene.fogColor[1] = 0.62f; vkScene.fogColor[2] = 0.78f;
        vkScene.clearColor[0] = 0.45f; vkScene.clearColor[1] = 0.62f;
        vkScene.clearColor[2] = 0.78f; vkScene.clearColor[3] = 1.0f;
        // Vulkan viewport camera: default to Orbit (2) around the character -
        // the same view the editor has always opened on. Modes 0-4 match
        // test.cpp's engine loop (Free / Follow / Orbit / Top-Down /
        // First-Person) and the shared Camera menu + toolbar icons.
        g_editor.uiState.playCameraMode = 2;
        // Play mode defaults ON (F5 / the toolbar Stop button toggles it off
        // to the idle showcase): WASD drives the bot immediately, matching
        // the behavior the Vulkan editor has always had.
        g_editor.setPlaying(true);

        // NOTE: WorldManager is NOT initialized on the Vulkan path because
        // its Terrain::initialize() calls GL functions (glGenTextures, etc.)
        // which crash when no GL context exists. The Vulkan scene has its own
        // terrain via BuildVulkanScene(). Surface height uses TerrainHeight()
        // directly; physics collision is skipped (no PhysicsWorld).

        OrbitCamera cam;                    // modes 0 (free-fly) and 2 (orbit)
        ThirdPersonCamera followCam;        // modes 1, 3, 4 around the character
        followCam.config.orientToCharacterForward = true;  // match GL editor
        bool followInit = false;
        float fpPitch = 0.0f;
        int lastFollowMode = -1;
        SetSceneCamera(vkScene, cam);

        // Bring the GeoAPI facade (used by the Geo tracking panel + status
        // bar) in sync with the ECS geospatial pipeline's origin, then seed
        // the panel state - ported from test.cpp so the shared Geo UI shows
        // live data on the Vulkan backend too.
        {
            auto& geoApi = g_editor.geospatialSystem().getGeoAPI();
            geoApi.initialize(-33.8568, 151.2153, 50.0);   // Sydney origin (matches ECS)
            geoApi.setGPSMode(GPSTracker::Mode::SIMULATED_WALK);
            const geo::GeoConfig geoCfg = geoApi.getConfig();
            g_editor.geoPanelState.originLat = geoCfg.originLat;
            g_editor.geoPanelState.originLon = geoCfg.originLon;
            g_editor.geoPanelState.originAlt = geoCfg.originAlt;
            g_editor.geoPanelState.gpsModeIndex = (int)geoCfg.gpsMode;
        }

        while (!glfwWindowShouldClose(rhi->window())) {
            glfwPollEvents();
            ImGui_ImplGlfw_NewFrame();
            ImGui_ImplVulkan_NewFrame();
            ImGui::NewFrame();

            // Frame time (clamped so a hitch never teleports the animation /
            // camera).
            const float dt = std::max(io.DeltaTime, 1.0f / 120.0f);

            // ImGui captures the mouse/keyboard over ANY window - including
            // the (transparent, swapchain-backed) viewport panel itself - so
            // io.WantCaptureMouse/Keyboard read true while the cursor sits in
            // the viewport, which would block orbit, scroll and WASD entirely
            // ("mouse is blocked by io window capture"). Release that capture
            // while the cursor is over the viewport, mirroring the GL editor's
            // processInput routing: menus / other panels / active widgets and
            // text fields keep ImGui's capture, the viewport canvas gets it.
            const bool overViewport = UI::IsViewport3DHovered() && !io.WantTextInput;
            bool captureMouse = io.WantCaptureMouse;
            bool captureKb = io.WantCaptureKeyboard;
            if (overViewport) captureMouse = false;
            if (overViewport && !ImGui::IsAnyItemActive() && !io.WantTextInput) {
                captureKb = false;
            }

            // ---- Camera mode + play mode keys (ported from test.cpp) --------
            // Keys 0-4 switch camera modes (matching the Camera menu / toolbar
            // labels); F is a quick jump to the Free camera (the old free-fly
            // toggle). F5 toggles play mode (also the toolbar Play/Stop
            // button, which drives the same g_editor state). F8 toggles the
            // Geo tracking panel.
            const bool playing = g_editor.isPlaying();
            if (!captureKb) {
                if (ImGui::IsKeyPressed(ImGuiKey_0)) g_editor.uiState.playCameraMode = 0;
                else if (ImGui::IsKeyPressed(ImGuiKey_1)) g_editor.uiState.playCameraMode = 1;
                else if (ImGui::IsKeyPressed(ImGuiKey_2)) g_editor.uiState.playCameraMode = 2;
                else if (ImGui::IsKeyPressed(ImGuiKey_3)) g_editor.uiState.playCameraMode = 3;
                else if (ImGui::IsKeyPressed(ImGuiKey_4)) g_editor.uiState.playCameraMode = 4;
            else if (ImGui::IsKeyPressed(ImGuiKey_5)) g_editor.uiState.playCameraMode = 5; // Cinematic
                else if (ImGui::IsKeyPressed(ImGuiKey_F)) g_editor.uiState.playCameraMode = 0;
                if (ImGui::IsKeyPressed(ImGuiKey_F5)) g_editor.setPlaying(!playing);
                if (ImGui::IsKeyPressed(ImGuiKey_F8)) {
                    g_editor.uiState.showGameMode = !g_editor.uiState.showGameMode;
                    // Geo tab is index 3 in the fixed tab bar {Outliner, Layers, World, Geo}.
                    if (g_editor.uiState.showGameMode) g_editor.scenePanelConfig.activeTabIndex = 3;
                }
            }
            const int camMode = std::clamp(g_editor.uiState.playCameraMode, 0, kVkCamModeCount - 1);

            // Viewport keyboard focus latch (port of test.cpp): clicking the
            // viewport latches keyboard ownership so WASD keeps driving the
            // camera / character even if the cursor drifts to a panel edge.
            // Clicking anywhere else releases the latch.
            static bool s_viewportFocused = false;
            if (io.MouseClicked[0]) {
                s_viewportFocused = overViewport;
            }
            const bool cameraOwnsKeyboard =
                (overViewport || s_viewportFocused || playing) &&
                !ImGui::IsAnyItemActive() && !io.WantTextInput;
            if (cameraOwnsKeyboard) captureKb = false;

            // ---- Editor keyboard shortcuts (Ctrl combos + Delete) ----------
            // Ported from test.cpp: undo/redo, save, duplicate, new scene,
            // open scene, delete entity.
            if (!captureKb) {
                const bool ctrl = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) ||
                                  ImGui::IsKeyDown(ImGuiKey_RightCtrl);
                if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
                    UndoRedo::Undo();
                    g_editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
                }
                if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
                    UndoRedo::Redo();
                    g_editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
                }
                if (ctrl && ImGui::IsKeyDown(ImGuiKey_S)) {
                    SceneManager::SaveScene(g_editor.uiState.sceneFile, g_editor.world());
                }
                if (ctrl && ImGui::IsKeyDown(ImGuiKey_D)) {
                    ecs::EntityID dup = EntityManager::DuplicateEntity(g_editor.selectedEntity());
                    if (dup != ecs::INVALID_ENTITY_ID) g_editor.setSelectedEntity(dup);
                }
                if (ctrl && ImGui::IsKeyDown(ImGuiKey_N)) {
                    g_editor.world().shutdown();
                    g_editor.world().init();
                    UndoRedo::Clear();
                    g_editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
                }
                if (ctrl && ImGui::IsKeyDown(ImGuiKey_O)) {
                    g_editor.uiState.showOpenScene = true;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
                    ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                    EntityManager::DeleteEntity(g_editor.selectedEntity());
                    g_editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
                }
            }

            // ---- Play mode enter/exit logging (port of test.cpp) ----------
            static bool s_playWasActive = false;
            if (playing && !s_playWasActive) {
                followInit = false;  // re-init follow camera on play enter
                std::cout << "[Editor] Entering play mode\n";
            } else if (!playing && s_playWasActive) {
                std::cout << "[Editor] Exiting play mode\n";
            }
            s_playWasActive = playing;

            // FPS timer
            static double s_fpsTimer = 0.0;
            static int s_fpsFrames = 0;
            static float s_fps = 0.0f;
            s_fpsTimer += dt;
            s_fpsFrames++;
            if (s_fpsTimer >= 1.0) {
                s_fps = (float)s_fpsFrames / (float)s_fpsTimer;
                s_fpsFrames = 0;
                s_fpsTimer = 0.0;
            }

            // When entering Free mode, snap the fly position to the current
            // camera eye (the old F-toggle did this on each toggle); any mode
            // change also resets the idle-orbit timer.
            static int s_lastCamMode = -1;
            if (camMode != s_lastCamMode) {
                if (camMode == 0) {
                    cam.flyEye = glm::vec3(vkScene.camera.eye[0],
                                           vkScene.camera.eye[1],
                                           vkScene.camera.eye[2]);
                }
                s_lastCamMode = camMode;
                cam.autoOrbitT = 0.0f;
            }

            // Viewport camera mouse control: right-drag + scroll. Only while
            // the cursor is over the viewport panel and ImGui is not actively
            // using the mouse elsewhere (menus / other panels win). Modes 0/2
            // drive the orbit/fly camera directly; modes 1/3/4 hand the same
            // deltas to the follow camera below.
            const bool viewportHovered = overViewport && !captureMouse;
            const bool viewportFocused = viewportHovered; // hovering implies focus for viewport input
            const bool drag = viewportHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right);
            if (drag && (camMode == 0 || camMode == 2)) {
                cam.autoOrbitT = 0.0f;
                cam.yaw += io.MouseDelta.x * 0.01f;
                cam.pitch = std::clamp(cam.pitch + io.MouseDelta.y * 0.01f, -85.0f, 85.0f);
            }
            if (viewportHovered && io.MouseWheel != 0.0f && camMode == 2) {
                cam.autoOrbitT = 0.0f;
                cam.dist = std::clamp(cam.dist * std::exp(-io.MouseWheel * 0.12f), 8.0f, 220.0f);
            }
            // Gentle showcase orbit (Orbit mode only): when the user hasn't
            // touched the viewport camera for a few seconds, swing around the
            // scene slowly so the terrain, props and the animated character
            // are seen from every angle (drag or scroll to take back control).
            cam.autoOrbitT += dt;
            if (camMode == 2 && cam.autoOrbitT > 3.0f) cam.yaw += dt * 4.0f;
            // Free-fly camera movement (Free mode): WASD moves flyEye in the
            // camera's local frame (W = forward into the scene, A/D = strafe,
            // S = back, Shift = run, Q/E = down/up). The direction math
            // matches the orbit camera's yaw/pitch convention so movement
            // feels consistent between modes.
            if (camMode == 0 && viewportFocused && !captureKb) {
                const float yawB = cam.yaw * 3.14159265f / 180.0f;
                const float pitchB = cam.pitch * 3.14159265f / 180.0f;
                const float cp = std::cos(pitchB);
                // Forward: same direction the orbit camera looks (into screen).
                glm::vec3 fwd(-cp * std::cos(yawB), -std::sin(pitchB),
                              -cp * std::sin(yawB));
                // Strafe right = cross(world-up, fwd) -> (fwd.z, 0, -fwd.x) normalized.
                glm::vec3 right(fwd.z, 0.0f, -fwd.x);
                if (glm::length(right) > 0.001f) right = glm::normalize(right);
                float flySpeed = 12.0f;  // world units / second
                if (ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
                    ImGui::IsKeyDown(ImGuiKey_RightShift)) flySpeed = 30.0f;
                if (ImGui::IsKeyDown(ImGuiKey_W)) cam.flyEye += fwd * flySpeed * dt;
                if (ImGui::IsKeyDown(ImGuiKey_S)) cam.flyEye -= fwd * flySpeed * dt;
                if (ImGui::IsKeyDown(ImGuiKey_D)) cam.flyEye += right * flySpeed * dt;
                if (ImGui::IsKeyDown(ImGuiKey_A)) cam.flyEye -= right * flySpeed * dt;
                if (ImGui::IsKeyDown(ImGuiKey_Q)) cam.flyEye.y += flySpeed * dt;
                if (ImGui::IsKeyDown(ImGuiKey_E)) cam.flyEye.y -= flySpeed * dt;
            }

            // ---- Character update (ported play mode) ------------------------
            // In play mode (F5 / toolbar Play) WASD drives the bot; outside
            // play mode it gets zero input and idles in place (test.cpp
            // parity). In Free camera mode WASD drives the camera instead of
            // the character. Movement is CAMERA-relative so W always walks
            // into the screen no matter where the active camera sits.
            if (vkContent.character) {
                CharacterInput input;
                glm::vec2 move(0.0f);
                if (playing && camMode != 0) {
                    if (!captureKb) {
                        // World-space WASD (matches test.cpp KeyboardInput):
                        // W = -Z, S = +Z, A = -X, D = +X. The follow camera
                        // auto-orients behind the heading, so the character
                        // always walks "forward" regardless of camera angle.
                        // Camera-relative moves create a feedback loop where
                        // the camera orbits endlessly when strafing.
                        if (ImGui::IsKeyDown(ImGuiKey_W)) move.y -= 1.0f;
                        if (ImGui::IsKeyDown(ImGuiKey_S)) move.y += 1.0f;
                        if (ImGui::IsKeyDown(ImGuiKey_A)) move.x -= 1.0f;
                        if (ImGui::IsKeyDown(ImGuiKey_D)) move.x += 1.0f;
                        input.jump = ImGui::IsKeyDown(ImGuiKey_Space);
                        input.crouch = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) ||
                                       ImGui::IsKeyDown(ImGuiKey_C);
                        input.sprint = ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
                                       ImGui::IsKeyDown(ImGuiKey_RightShift);
                        // Motion-matching context switching (contextual
                        // databases): Ctrl+1..5 for Locomotion/Crouch/Combat/
                        // Capoeira/Dance - plain 1-4 are camera modes now
                        // (test.cpp parity). Edge-triggered.
                        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) ||
                            ImGui::IsKeyDown(ImGuiKey_RightCtrl)) {
                            if (ImGui::IsKeyPressed(ImGuiKey_1)) input.motionContext = (int)AnimatedCharacter::MotionContext::LOCOMOTION;
                            else if (ImGui::IsKeyPressed(ImGuiKey_2)) input.motionContext = (int)AnimatedCharacter::MotionContext::CROUCH;
                            else if (ImGui::IsKeyPressed(ImGuiKey_3)) input.motionContext = (int)AnimatedCharacter::MotionContext::COMBAT;
                            else if (ImGui::IsKeyPressed(ImGuiKey_4)) input.motionContext = (int)AnimatedCharacter::MotionContext::CAPOEIRA;
                            else if (ImGui::IsKeyPressed(ImGuiKey_5)) input.motionContext = (int)AnimatedCharacter::MotionContext::DANCE;
                        }
                    }
                    const float len = glm::length(move);
                    if (len > 0.001f) move /= len;
                    input.moveDirection = move;
                    input.moveMagnitude = std::min(1.0f, len);
                    input.grounded = true;
                }
                // else: zero input -> the character idles in place (play mode
                // off), and Free mode leaves WASD to the camera.

                // Drive the controller, skin the bot from its bind pose, and
                // copy the updated mesh (geometry + instance) into the scene;
                // the version bumps make the RHI re-upload ONLY this mesh's
                // slice of the shared buffers (static world stays resident).
                UpdateSkinnedCharacter(*vkContent.character, dt, input);

                // NOTE: Physics collision vs world objects skipped on Vulkan path
                // (no PhysicsWorld — WorldManager not initialized to avoid GL calls).
                // Character is grounded by TerrainHeight() below.

                if (vkContent.character->valid)
                    vkScene.meshes[charMeshIdx] = vkContent.character->mesh;

                // Orbit mode: the camera target follows the character (with a
                // little smoothing) so walking around the scene keeps the bot
                // framed; it stays glued above the terrain.
                if (camMode == 2) {
                    const float sx = vkContent.character->chara->position.x;
                    const float sz = vkContent.character->chara->position.z;
                    const glm::vec3 wanted(sx,
                                           TerrainHeight(sx, sz) + 1.6f,
                                           sz);
                    cam.target += (wanted - cam.target) * std::min(1.0f, 5.0f * dt);
                }

                // One-shot: frame the bot on the first frame after character
                // is valid. The camera starts at the world origin; the bot
                // stands at terrain height (y ~12+), so it was clipped off
                // frame. Point the free camera at the bot's chest from a
                // comfortable distance.
                static bool s_cameraFramed = false;
                if (!s_cameraFramed && camMode == 0) {
                    s_cameraFramed = true;
                    cam.flyEye = vkContent.character->chara->position
                               + glm::vec3(0.0f, 2.0f, 5.0f);
                }
            }

            // ---- Active camera per mode (ported from test.cpp) --------------
            int w = 0, h = 0;
            glfwGetWindowSize(rhi->window(), &w, &h);
            if (h == 0) h = 1;
            if (camMode == 0 || camMode == 2) {
                SetSceneCamera(vkScene, cam);
            } else if (vkContent.character) {
                // Follow(1) / Top-down(3) / First-person(4): state-aware
                // ThirdPersonCamera around the character.
                const float aspect = (float)w / (float)h;
                const int followMode = camMode - 1;
                // Ground clamp: the camera never sinks below terrain
                // (matches test.cpp followCam.groundHeightFn = terrain).
                followCam.groundHeightFn = [](float x, float z) -> float {
                    return TerrainHeight(x, z);
                };
                UpdateVulkanPlayCamera(followMode, followCam, *vkContent.character->chara,
                                       dt, aspect, drag, io.MouseDelta.x, io.MouseDelta.y,
                                       viewportHovered ? io.MouseWheel : 0.0f,
                                       followInit, lastFollowMode, fpPitch);
                lastFollowMode = followMode;
                vkScene.camera.eye[0] = followCam.position.x;
                vkScene.camera.eye[1] = followCam.position.y;
                vkScene.camera.eye[2] = followCam.position.z;
                vkScene.camera.target[0] = followCam.target.x;
                vkScene.camera.target[1] = followCam.target.y;
                vkScene.camera.target[2] = followCam.target.z;
                vkScene.camera.up[0] = 0; vkScene.camera.up[1] = 1; vkScene.camera.up[2] = 0;
            }

            // View/projection for the viewport overlay + hover rect (the RHI
            // computes the identical matrices internally for the swapchain
            // render, including the GL->Vulkan clip correction).
            const RHI::Mat4 projM = RHI::mat4Perspective(vkScene.camera.fovDeg,
                                                         (float)w / (float)h, 0.1f,
                                                         vkScene.camera.farPlane);
            const RHI::Mat4 viewM = RHI::mat4LookAt(vkScene.camera.eye, vkScene.camera.target,
                                                    vkScene.camera.up);
            const glm::mat4 proj = glm::make_mat4(projM.m);
            const glm::mat4 view = glm::make_mat4(viewM.m);
            const glm::vec3 camPos(vkScene.camera.eye[0], vkScene.camera.eye[1], vkScene.camera.eye[2]);
            const glm::vec3 camTgt(vkScene.camera.target[0], vkScene.camera.target[1], vkScene.camera.target[2]);
            const glm::mat4 viewProj = proj * view;

            // Per-frame visibility culling of the static props: frustum +
            // distance (with a fade band into the fog). Only the small
            // instance pool re-uploads when the visible set changes.
            // Ground (0) and the skinned character (last) are always drawn.
            // The prop meshes are appended conditionally (skipped if they
            // have zero instances), so walk the array by index rather than
            // hardcoding [1..5] (which would crash if a prop failed to load).
            {
                const float radii[] = { 200.0f, 150.0f, 70.0f, 80.0f, 100.0f };
                const auto& ws = g_editor.uiState.worldSettings;
                int visibleTotal = 0;
                std::vector<RHI::OffscreenInstance> debugBounds;
                size_t i = 1;
                for (size_t r = 0; r < sizeof(radii)/sizeof(radii[0]) && i < vkScene.meshes.size(); ++r, ++i) {
                    if (!vkScene.meshes[i].instances.empty()) {
                        visibleTotal += CullInstances(vkScene.meshes[i], viewProj, camPos, radii[r],
                                                      ws.cullingDebug, ws.lodBand1, ws.lodBand2,
                                                      ws.lodBand3, ws.cullingDebug ? &debugBounds : nullptr);
                    }
                }
                // Global instance budget (default 500, tunable in World Settings):
                // never draw more than maxVisibleInstances props this frame. Trim
                // the tail of the per-mesh visible lists (debugBounds is debug viz).
                if (visibleTotal > ws.maxVisibleInstances) {
                    int keep = ws.maxVisibleInstances;
                    for (size_t j = 1; j < vkScene.meshes.size() && keep > 0; ++j) {
                        auto& insts = vkScene.meshes[j].instances;
                        if ((int)insts.size() > keep) { insts.resize(keep); keep = 0; }
                        else { keep -= (int)insts.size(); }
                    }
                    visibleTotal = ws.maxVisibleInstances;
                }
                // Debug: AABB bounds boxes around every visible prop (solid, the
                // cube pipeline has no wireframe mode). The cube buffer is empty in
                // the normal Vulkan scene path, so we own it entirely when on.
                if (ws.cullingDebug) {
                    vkScene.instances.clear();
                    vkScene.instances = std::move(debugBounds);
                    float vx = 0, vy = 0, vw = 0, vh = 0;
                    if (UI::GetViewportContentRect(vx, vy, vw, vh)) {
                        char badge[176];
                        std::snprintf(badge, sizeof(badge),
                                      "Culling: %d visible / cap %d  |  LOD: green=near  yellow=mid  red=far",
                                      visibleTotal, ws.maxVisibleInstances);
                        ImDrawList* dl = ImGui::GetBackgroundDrawList();
                        const ImVec2 ts = ImGui::CalcTextSize(badge);
                        const float bx = vx + 8.0f, by = vy + 8.0f;
                        dl->AddRectFilled(ImVec2(bx, by),
                                          ImVec2(bx + ts.x + 10.0f, by + ts.y + 8.0f),
                                          IM_COL32(0, 0, 0, 170), 4.0f);
                        dl->AddText(ImVec2(bx + 5.0f, by + 4.0f),
                                    IM_COL32(255, 255, 255, 235), badge);
                    }
                } else {
                    vkScene.instances.clear();
                }
            }

            // ---- Geo pipeline <-> UI bridge (ported from test.cpp) ----------
            // Push the live ECS geospatial pipeline state into the GeoAPI
            // facade so the Geo tracking panel + status bar show real GPS data,
            // and push back any panel-driven GPS config changes (mode/speed/
            // noise).
            {
                auto& geoSystem = g_editor.geospatialSystem();
                auto& geoApi = geoSystem.getGeoAPI();
                geoApi.syncTo(geoSystem.getIngestionSystem());
                geo::GeoStats stats{};
                stats.trackedEntityCount = geoSystem.getGeospatialEntityCount();
                stats.totalPointsStored = geoSystem.getTimeSeriesDB().size();
                stats.ingestionRate = 0.0;
                stats.lastUpdateTime = glfwGetTime();
                geoApi.syncFrom(geoSystem.getCurrentGPSFix(),
                                geoSystem.getEntitySnapshots(), stats);
            }

            RenderSharedEditorUI(g_editor, nullptr, kVkCamModeNames[camMode],
                                 0, w, h, rhi->window(), io,
                                 proj, view, &camPos, &camTgt, /*swapchainBacked=*/true);

            // Key-binding hint overlay pinned to the bottom-right of the
            // viewport canvas (draw-list only - no window, no input capture).
            // Text adapts to the active camera mode + play state (port of the
            // engine's play-mode hint, updated for the Vulkan mode set).
            float vx = 0, vy = 0, vw = 0, vh = 0;
            if (UI::GetViewportContentRect(vx, vy, vw, vh)) {
                const char* hints = playing
                    ? (camMode == 0
                          ? "0-5: camera   WASD: fly   Shift: fast   Q/E: down/up   Right-drag: look   F5: play"
                          : "WASD: move   Shift: run   C: crouch   Space: jump   0-5: camera   Right-drag: orbit   Scroll: zoom")
                    : "F5: play   F8: geo   0-5: camera   Right-drag: orbit   Scroll: zoom";
                const ImVec2 ts = ImGui::CalcTextSize(hints);
                const float px = vx + vw - ts.x - 12.0f;
                const float py = vy + vh - ts.y - 10.0f;
                ImDrawList* dl = ImGui::GetBackgroundDrawList();
                dl->AddRectFilled(ImVec2(px - 6.0f, py - 4.0f),
                                  ImVec2(px + ts.x + 6.0f, py + ts.y + 4.0f),
                                  IM_COL32(0, 0, 0, 150), 4.0f);
                dl->AddText(ImVec2(px, py), IM_COL32(255, 255, 255, 220), hints);
            }

            ImGui::Render();
            if (rhi->beginFrame()) {
                rhi->renderFrameScene(vkScene);
                rhi->renderFrameImGui(ImGui::GetDrawData());
                rhi->endFrame();
            }
        }

        // rhi->shutdown() shuts down imgui_impl_vulkan itself (it owns the
        // backend since initializeImGui), so it must run while the ImGui
        // context is still alive - then the GLFW binding and the context go.
        ImGui_ImplGlfw_Shutdown();
        rhi->shutdown();
        ImGui::DestroyContext();
        return 0;
    }

    // ---- OpenGL path: full editor (window + GL context already live) ----
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "[EditorMain] Failed to initialize GLAD" << std::endl;
        rhi->shutdown();
        return 1;
    }

    // Enable framebuffer sRGB encoding so the composited output is gamma-
    // corrected (linear -> sRGB8) on present. The post-composite pass
    // (shaderSystem/post_composite.frag) assumes this is active. Safe to enable
    // here because the engine's intermediate FBOs use unorm/linear attachments,
    // not GL_SRGB8*, so no double-gamma is introduced on offscreen targets.
    glEnable(GL_FRAMEBUFFER_SRGB);

    // ImGui integration (owns the ImGui context + backend bindings).
    Editor::ImGuiContext imgui;
    if (!imgui.initialize(rhi->window())) {
        std::cerr << "[EditorMain] Failed to initialize ImGui" << std::endl;
        rhi->shutdown();
        return 1;
    }

    // Editor look & feel: UI font + Phosphor/FontAwesome6 icons + theme.
    {
        ImGuiIO& io = ImGui::GetIO();
        EditorTheme::LoadEditorFonts(io);
        PhosphorImGui::Load(io, 14.0f);
        FaImGui::Load(io);

        // Restore the persisted theme mode (ui_panel.cfg) and paint it.
        UIConfig::LoadConfig(&g_editor.uiState.showOutliner, &g_editor.uiState.showDetails);
        EditorTheme::SetThemeMode(UIConfig::gThemeMode == 1 ? EditorTheme::ThemeMode::Light
                                                            : EditorTheme::ThemeMode::Dark);
        EditorTheme::ApplyTheme();
    }

    Editor::EditorApplication& app = Editor::EditorApplication::getInstance();
    if (!app.initialize(1920, 1080)) {
        std::cerr << "[EditorMain] Failed to initialize editor application" << std::endl;
        imgui.shutdown();
        rhi->shutdown();
        return 1;
    }

    // Full editor state (grid, gizmo, ECS world) for the shared panels.
    InitEditor();

    // Drive the SAME editor UI the engine uses: the menu bar + panels render
    // over the viewport FBO texture each frame, between the scene render and
    // the buffer swap. This is what makes the GL editor_app show the identical
    // editor as bin/engine (and as --graphics vulkan).
    app.setUiFrameHook([&](float /*dt*/) {
        int w = 0, h = 0;
        glfwGetWindowSize(rhi->window(), &w, &h);
        imgui.beginFrame();
        const glm::vec3* camPos = app.isPlayCharacterLoaded() ? &app.lastCameraPosition() : nullptr;
        const glm::vec3* camTgt = app.isPlayCharacterLoaded() ? &app.lastCameraTarget() : nullptr;
        RenderSharedEditorUI(g_editor, &app.getPlayController(), "Follow",
                             app.getRenderPipeline().getFramebufferTexture(),
                             w, h, rhi->window(), ImGui::GetIO(),
                             app.lastProjectionMatrix(), app.lastViewMatrix(),
                             camPos, camTgt, /*swapchainBacked=*/false);
        imgui.endFrame();
        glfwSwapBuffers(rhi->window());
    });

    const int result = app.run();

    app.shutdown();
    CleanupEditor();          // editor GL resources while the context is alive
    imgui.shutdown();
    rhi->shutdown();
    return result;
}
