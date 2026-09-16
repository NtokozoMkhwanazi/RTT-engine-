/**
 * RHI - Render Hardware Interface
 *
 * A thin graphics-API abstraction that lets the engine run on OpenGL (low /
 * budget machines - the default, and currently the full renderer) or Vulkan
 * (high graphics quality - backend initialized incrementally; today it
 * creates the device + swapchain and presents a clear frame, proving the
 * toggle is real).
 *
 * The OpenGL backend drives the existing GL renderer with zero changes to
 * draw code: it owns GLFW window + GL context creation and presentation. The
 * Vulkan backend initializes a Vulkan instance/device/swapchain and presents
 * a cleared frame; the scene renderers get ported to it progressively.
 *
 * Selection: Config::getRenderConfig().graphicsApi, overridable on the
 * command line with --graphics opengl|vulkan.
 */
#pragma once

#include <string>
#include <memory>
#include <vector>

struct GLFWwindow;
struct ImDrawData;

namespace RHI {

// Which graphics API the engine should run on.
enum class GraphicsAPI {
    OpenGL,   // low/optimized mode - full engine support (default)
    Vulkan,   // high-quality mode - device/swapchain/clear-frame today
};

// Human-readable API name (for logs / UI).
const char* graphicsApiName(GraphicsAPI api);

// Parse "--graphics opengl|vulkan" style tokens. Returns false for unknown.
bool parseGraphicsApi(const std::string& token, GraphicsAPI& out);

// Camera description for the offscreen scene render.
struct OffscreenCamera {
    float eye[3] = {0, 0, 0};        // camera position
    float target[3] = {0, 0, -1};     // look-at point
    float up[3] = {0, 1, 0};          // up hint
    float fovDeg = 60.0f;
    float farPlane = 100.0f;          // projection far clip (world must fit)
};

// Phase 3 (FSR3 motion vectors): prevModel is the previous-frame model matrix,
// the source of the per-pixel velocity (currXY - prevXY) the temporal
// upscaler reprojects from. On GL it is a raw byte range in the instance VBO
// (the vertex attrib for it is added in a later step); on Vulkan it is the
// third member of the scalar-layout `Instance` SSBO struct. It MUST sit at
// offset 80 (after model@0[64] / color@64[16]) and NOT shift those - 144 bytes
// total, 16-byte aligned for both std140/scalar SSBO and the GL raw-memcpy view.
struct OffscreenInstance {
    float model[16];
    float color[4];
    float prevModel[16];
};

// A generic triangle mesh for the offscreen scene render. Interleaved vertex
// data - 8 floats per vertex (position xyz, normal xyz, uv) - plus 32-bit
// indices and its own per-instance transforms. This is the building block the
// engine's real renderers (world objects, terrain, character) port onto: each
// draws its meshes through this path instead of calling GL directly.
struct OffscreenMesh {
    // Interleaved: [pos.xyz][normal.xyz][uv] per vertex (8 floats).
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    std::vector<OffscreenInstance> instances;   // drawn instanced

    // Optional albedo texture (RGBA8, row 0 = TOP, tightly packed). When
    // textureWidth * textureHeight > 0 the mesh fragment shader samples it
    // and multiplies by the per-instance color; otherwise a shared 1x1 white
    // texture is sampled so the shader math (and therefore GL/Vulkan pixel
    // parity) is IDENTICAL for every mesh. Both backends upload these bytes
    // as-is - the same UV convention - so they render the same pixels.
    std::vector<uint8_t> texturePixels;
    int textureWidth = 0;
    int textureHeight = 0;

    // Upload-cache version: bump it when vertices/indices change (NOT when
    // only instances change - use instanceVersion for that). The Vulkan
    // backend skips re-uploading meshes whose version matches the last
    // upload, so STATIC world geometry uploads to the GPU once (an editor
    // scene can hold hundreds of thousands of vertices) while per-frame
    // dynamic meshes (e.g. a CPU-skinned character) only re-upload their own
    // slice of the shared buffers. Mesh ORDER and per-mesh VERTEX/INDEX
    // counts must stay stable for the incremental path; changing them forces
    // a full re-upload (handled automatically).
    uint64_t version = 0;

    // Separate upload-cache version for the INSTANCE list: bump it when the
    // per-instance transforms/count change but the geometry does not (e.g.
    // per-frame visibility culling). Instance counts are NOT part of the
    // layout, so culling may add/remove instances every frame without a full
    // re-upload - the backend re-copies the (small) instance pool instead.
    uint64_t instanceVersion = 0;

    // Authoring-local half-extents (X, Y, Z) of the mesh's merged bounds,
    // captured at authoring time so the editor can draw debug AABB bounds
    // around each instance without re-deriving the original model bounds.
    float boundsHalfExtents[3] = { 0.5f, 1.0f, 0.5f };

    // --- Virtualized Geometry (Phase 1+) ---
    // Optional cluster list: when non-empty, the renderer uses GPU-driven
    // cluster culling (vkCmdDrawIndexedIndirect) instead of the legacy
    // per-instance vkCmdDraw. Each entry describes one 128-triangle cluster
    // with its bounding sphere and draw offsets into the global vertex/index
    // pools managed by the backend.
    struct ClusterRange {
        uint32_t firstVertex;  // base vertex in the global VB
        uint32_t firstIndex;   // base index in the global IB
        uint32_t indexCount;   // usually 384 (128 tris * 3)
        uint32_t instanceId;   // maps to the instance's model matrix
        float    center[3];    // bounding sphere center (world space)
        float    radius;       // bounding sphere radius
    };
    std::vector<ClusterRange> clusters;  // empty = legacy draw path
};

// A full offscreen scene: camera + instanced cubes AND/OR generic meshes.
// `instances` (cubes) is the simple/legacy path; `meshes` is the generic path
// the engine renderers use. Both may be present in one scene.
struct OffscreenScene {
    OffscreenCamera camera;
    std::vector<OffscreenInstance> instances;   // instanced cubes (legacy path)
    std::vector<OffscreenMesh> meshes;          // generic meshes
    float clearColor[4] = {0.04f, 0.04f, 0.04f, 1.0f};   // dark gray

    // Distance fog (exponential-squared). fogDensity = 0 (default) disables
    // it: the fog factor is exactly 0 and the fragment output is unchanged,
    // so scenes rendered by the parity tests stay pixel-identical. The mesh
    // shaders compute the fog on BOTH backends with the identical math and
    // mix toward fogColor (which should match the horizon / clear color).
    float fogDensity = 0.0f;
    float fogColor[3] = {0.55f, 0.62f, 0.70f};   // hazy sky blue

    // When true, generic `meshes` render with the Cook-Torrance PBR path
    // (pbr_mesh.vert/frag on Vulkan, the GL PBR port on OpenGL) — proper PBR
    // textures via the same CameraUBO + per-mesh albedo. Cubes keep the solid
    // color path (they have no normal/uv). Both backends run IDENTICAL PBR
    // math (same light dir/color, GGX, Smith, Fresnel-Schlick, ACESFilm,
    // gamma 1/2.2, exponential-squared fog) so GL/Vulkan pixel parity holds.
    bool pbrEnabled = false;
};

// ---------------------------------------------------------------------------
// RHI instance - owns the window, the API context, and the presentation
// loop. The engine's rendering code runs between beginFrame() and endFrame().
// ---------------------------------------------------------------------------
class IRHI {
public:
    virtual ~IRHI() = default;

    // API this backend implements.
    virtual GraphicsAPI api() const = 0;

    // True when the backend can create a window + render on THIS machine.
    // Pure capability probe - no resources are created. Lets callers pick a
    // backend at startup and tests skip cleanly when the API is unavailable
    // (e.g. no Vulkan driver, or headless without a display for WSI).
    virtual bool available() const = 0;

    // Create the window + initialize the graphics context/device. Must be
    // called once, before any frame work. The GLFW window handle is shared so
    // ImGui / input keep working unchanged.
    virtual bool initialize(int width, int height, const char* title) = 0;
    virtual void shutdown() = 0;

    // GLFW window (valid after initialize()).
    virtual GLFWwindow* window() const = 0;

    // Per-frame lifecycle. beginFrame() acquires the swapchain image / clears
    // state; endFrame() presents it and swaps buffers.
    virtual bool beginFrame() = 0;
    virtual void endFrame() = 0;

    // Window state query (drives input scaling / viewport sizing).
    virtual int width() const = 0;
    virtual int height() const = 0;

    // ---------------------------------------------------------------------
    // Offscreen render (headless verification of the render stack)
    // ---------------------------------------------------------------------
    // Render a fixed test triangle into an OFFSCREEN image and copy the raw
    // RGBA8 pixels into outRGBA (w*h*4 bytes). This exercises the backend's
    // real render path (shaders, pipeline, buffers, render pass, command
    // submission) WITHOUT needing a window/surface - the Vulkan backend can
    // do this on headless machines, which is how the tests validate that the
    // ported render stack actually produces pixels.
    virtual bool renderOffscreenTriangle(int w, int h, unsigned char* outRGBA) = 0;

    // ---------------------------------------------------------------------
    // Offscreen 3D scene render (camera UBO + depth + instancing)
    // ---------------------------------------------------------------------
    // Render a 3D scene through a perspective camera into an offscreen image
    // and copy the raw RGBA8 pixels into outRGBA (w*h*4 bytes, row 0 = TOP of
    // the image on both backends - the GL backend flips its bottom-up readback
    // to match). Depth testing is on, so nearer geometry occludes farther ones
    // exactly like the engine's batched renderer. The scene holds INSTANCED
    // CUBES (OffscreenScene::instances) and/or GENERIC MESHES
    // (OffscreenScene::meshes - interleaved pos/normal/uv + indices, drawn
    // instanced). This is the ported render path: uniform buffer (camera),
    // depth attachment, per-instance model matrices, arbitrary geometry. Both
    // backends must render the SAME scene to IDENTICAL pixels (verified by
    // the tests).
    //
    // outVelocityRG (Phase 3b, motion vectors): when non-null, the backend
    // also renders a SECOND color attachment (RG32f velocity, 2 floats/pixel)
    // holding the per-pixel screen-space motion vector
    // (currNDC - prevNDC, where prevNDC = viewProj * prevModel * pos).
    // Cubes do not write velocity (their velocity attachment is cleared to 0);
    // only generic meshes write it. outRGBA (color) may be nullptr when only
    // velocity is needed. The velocity buffer is headless-verifiable (no
    // swapchain/surface required) and is GL<->Vulkan parity-safe: the color
    // path (loc0) is byte-identical to the color-only render, and the
    // velocity math is identical on both backends.
    virtual bool renderOffscreenScene(int w, int h, const OffscreenScene& scene,
                                      unsigned char* outRGBA,
                                      float* outVelocityRG = nullptr) = 0;

    // ---------------------------------------------------------------------
    // Offscreen 3D scene render + FSR3 EASU spatial upscale (Phase 4)
    // ---------------------------------------------------------------------
    // Renders the scene at HALF the requested output resolution (a low-res
    // G-buffer) and upscales it with AMD FSR3's Enhanced Subpixel-scale Upscale
    // (EASU) — FSR's edge-directed 4-tap spatial upscaler — into an output
    // image of exactly (outW x outH) pixels, copied into outRGBA.
    //
    // This is the Phase-4 building block for FSR3: the EASU compute pass runs
    // headlessly on Vulkan (no swapchain/surface required) and is verified by
    // the tests without presenting. The 1:1 (no-op) case (render==output size)
    // falls back to a plain scene render so callers can probe EASU with the
    // canonical spatial upscale always available.
    //
    // On the OpenGL backend (OpenGL 3.3 has no compute shaders) the upscale is
    // done with a GLSL 3.30 fullscreen-fragment pass that ports AMD's canonical
    // ffxFsrEasuFloat 12-tap edge-directed filter (Phase 5b), instead of a 1:1
    // passthrough — so GL and Vulkan run the same spatial upscale in this call.
    // outRGBA is w*h*4 bytes, row 0 = TOP of the image (matched on both
    // backends). Returns false when the backend/scene cannot produce pixels.
    virtual bool renderOffscreenSceneEasu(int outW, int outH,
                                          const OffscreenScene& scene,
                                          unsigned char* outRGBA) = 0;

    // ---------------------------------------------------------------------
    // Swapchain scene render (visible window content on Vulkan)
    // ---------------------------------------------------------------------
    // Render the scene INTO the CURRENT swapchain image (must be called
    // between beginFrame() and endFrame(), with a surface/swapchain). The
    // image is then presented by endFrame(), so the window shows real 3D
    // content instead of a clear color. This is the step that moves the
    // Vulkan backend from "clear-frame foundation" to "actual rendered
    // viewport". Returns false (no-op) when there is no surface/swapchain
    // (headless device-only mode) or the scene is empty.
    virtual bool renderFrameScene(const OffscreenScene& scene) = 0;

    // ---------------------------------------------------------------------
    // Dear ImGui UI overlay (the editor UI on the swapchain)
    // ---------------------------------------------------------------------
    // initializeImGui() must be called once after initialize() (it needs a
    // live swapchain AND a Dear ImGui context: ImGui::CreateContext() must
    // have been called). It sets up the backend's ImGui integration. Each
    // frame the caller then drives the normal ImGui flow
    // (ImGui::NewFrame() -> build UI -> ImGui::Render()) and passes the
    // resulting ImDrawData to renderFrameImGui(), which draws the UI ON TOP
    // of the scene rendered by renderFrameScene() in the same swapchain
    // image before endFrame() presents. The GL backend is a no-op here (the
    // editor's own imgui_context owns ImGui on GL); the Vulkan backend owns
    // imgui_impl_vulkan.
    virtual bool initializeImGui() = 0;
    virtual bool renderFrameImGui(ImDrawData* drawData) = 0;
};

// Factory: create the backend for the requested API. Returns nullptr when the
// API is unavailable on this machine (Vulkan without a driver, etc.).
std::unique_ptr<IRHI> createRHI(GraphicsAPI api);

// ---------------------------------------------------------------------------
// Runtime backend state (editor UI toggle)
// ---------------------------------------------------------------------------
// The active backend the engine is running on right now. The editor's
// Graphics menu shows this and lets the user switch for the NEXT launch
// (persisted via the config file; a live hot-swap of two graphics contexts
// mid-frame is deliberately not attempted).
GraphicsAPI activeGraphicsApi();
// Set the backend for the next launch + persist it. Returns false if the
// requested API is not available on this machine.
bool setGraphicsApi(GraphicsAPI api);
// Path of the persisted graphics-api config file (for UI tooltips / logs).
const char* graphicsApiConfigPath();

} // namespace RHI
