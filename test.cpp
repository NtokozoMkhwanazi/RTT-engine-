// ============================================================================
//  test.cpp - RTT Engine + Editor Application
// ============================================================================
//  The single engine entry point. Two phases:
//
//    1. SELF-CHECK: runs the complete Google Test suite (494 tests across 53
//       suites) to validate every engine system headlessly.
//    2. ENGINE + EDITOR APP: boots the full application - GLFW window, OpenGL
//       context, ImGui editor (menu bar, toolbar, outliner/details/bottom
//       panels, live viewport, status bar), terrain world, physics floor,
//       cinematic camera intro, and a play mode with the motion-matching
//       character (bot.fbx), third-person follow camera and play-mode HUD.
//
//  Headless mode (--headless): hidden window, bounded frame budget with a fixed
//  60 Hz timestep, auto-enters play mode with the scripted cinematic demo and
//  prints an engine summary before exiting 0. No OpenGL context at all falls
//  back to a GL-free logic simulation of the character + matcher core.
//
//  Build & run:   make run           (tests, then the editor app)
//                 make run-headless  (bounded automated run)
// ============================================================================

#include <gtest/gtest.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <execinfo.h>
#include <cxxabi.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "ecs/systems/Systems.h"
#include "renderer/Renderer.h"
#include "cameraSystem/flyCamera.h"
#include "cameraSystem/ThirdPersonCamera.h"
#include "shaderSystem/Shader.h"
#include "modelSystem/Model.h"
#include "editor/render_pipeline.h"
#include "editor/world_manager.h"
#include "editor/editor_state.h"
#include "editor/config.h"
#include "editor/input_manager.h"
#include "editor/ui_config.h"
#include "editor/editor_application.h"
#include "editor/ui.h"
#include "editor/phosphor_imgui.h"
#include "editor/editor_theme.h"
#include "editor/fa_imgui.h"
#include "editor/gl_context_lifecycle.h"
#include "editor/undo_redo.h"
#include "editor/scene_manager.h"
#include "editor/entity_manager.h"
#include "editor/PlayModeController.h"
#include "animationSystem/AnimationStateMachine.h"
#include "cameraSystem/CinematicDemo.h"
#include "demo/DemoRecorder.h"
#include "geospatial/GeoAPI.h"
#include "geospatial/GPSTracker.h"
#include "renderer/GPUProfilerAdvanced.h"

// ============================================================================
// Options
// ============================================================================
struct AppOptions {
    bool skipTests = false;   // --skip-tests   / RTT_SKIP_TESTS=1
    bool headless  = false;   // --headless     / RTT_HEADLESS=1
    bool help      = false;
    int  frames    = 900;     // --frames N     / RTT_FRAMES=N  (headless bound)
};

static const char* kUsage =
    "Usage: engine [options]\n"
    "  --skip-tests      Skip the Google Test self-check phase\n"
    "  --headless        Bounded run with a hidden window (auto-exit; no display\n"
    "                    falls back to a GL-free logic sim)\n"
    "  --frames N        Headless frame budget (default 900)\n"
    "  --help            Show this help\n"
    "\n"
    "Editor controls: mouse look (right-drag), WASD fly, F5 play/pause mode,\n"
    "WASD/Space/Ctrl/Shift move the character in play mode, F9 cinematic demo\n"
    "script, 1-4 switch play camera (follow/orbit/top-down/first-person),\n"
    "Esc / window close quits.\n";

static AppOptions parseOptions(int argc, char** argv) {
    AppOptions o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--skip-tests") o.skipTests = true;
        else if (a == "--headless") o.headless = true;
        else if (a == "--help") o.help = true;
        else if (a == "--frames" && i + 1 < argc) { o.frames = std::atoi(argv[++i]); if (o.frames < 1) o.frames = 1; }
    }
    if (const char* e = std::getenv("RTT_SKIP_TESTS")) o.skipTests = (std::atoi(e) != 0);
    if (const char* e = std::getenv("RTT_HEADLESS"))   o.headless  = (std::atoi(e) != 0);
    if (const char* e = std::getenv("RTT_FRAMES"))     o.frames    = std::max(1, std::atoi(e));
    return o;
}

// ============================================================================
// Crash handler (demangled backtrace -> crash.log)
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
// System inventory (module -> test suite -> description)
// ============================================================================
static const struct { const char* name; const char* suite; const char* desc; } kInventory[] = {
    {"ECS / Archetypes",   "ECS*Archetype*Blueprint*Relationship*", "Archetypes, blueprints, relationships, events, jobs, serialization"},
    {"Physics",            "PhysicsTest*",                          "GJK/EPA collision, constraints, rigid bodies, gravity, floor"},
    {"Camera System",      "Camera*",                               "Third-person follow, modes, collision, smoothing, NaN regression"},
    {"Animation & FSM",    "Animation*Animator*FSM*",               "Blending, layers, events, crossfade, locomotion FSM"},
    {"Motion Matching",    "MotionMatching*MMGaitPhase*",           "Pose search, gait phase, trajectory prediction, foot planting"},
    {"Character",          "Character*PlayModeController*",         "PlayModeController + AnimatedCharacter input->movement->snap"},
    {"FBX / Models",       "FBX*Model*",                            "Assimp FBX loading, skeletons, skinning, bone buffers"},
    {"Terrain / World",    "TerrainTest*WorldTest*SceneManager*",   "Heightmaps, chunks, LOD, normals, scene save/load"},
    {"Memory",             "Memory*",                               "Arenas, pools, stack allocators, asset handles"},
    {"Geo / GPS",          "Geo*",                                  "GeoAPI, HTTP server, NMEA feeds, GPS modes"},
    {"Editor & UI",        "EditorClass*UI*ImGui*UndoRedo*EntityManager*Viewport*", "Panels, undo/redo, entity ops, viewport FBO, ImGui safety"},
    {"Demo / Playback",    "Playback*CinematicDemo*",               "Demo recorder/player, cinematic script"},
    {"Integration",        "IntegrationTest*",                      "Input -> FSM -> motion matching -> pose sync"},
};

static void printSystemInventory() {
    std::cout << "\n  System inventory wired into this run:\n"
              << "  ------------------------------------------------------------------\n";
    for (const auto& m : kInventory) {
        std::printf("   %-24s %-32s %s\n", m.name, m.suite, m.desc);
    }
    std::cout << "  ------------------------------------------------------------------\n\n";
}

// ============================================================================
// Phase 1 - Google Test self-check
// ============================================================================
static int runTestPhase(int argc, char** argv) {
    std::cout << "\n============================================================\n"
              << "  PHASE 1/2 - SELF-CHECK: running the full test suite\n"
              << "  (494 tests across 53 suites)\n"
              << "============================================================\n";
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    std::cout << "\n============================================================\n"
              << "  SELF-CHECK " << (result == 0 ? "PASSED" : "FAILED")
              << " (" << result << " failing test groups)\n"
              << "============================================================\n";
    return result;
}

// ============================================================================
// GL-free logic simulation (fallback when no OpenGL context is available)
// ============================================================================
static int runLogicSim(const AppOptions& opts) {
    std::cout << "\n[Engine] No OpenGL context available - running GL-free logic\n"
              << "         simulation of the character + motion-matching core.\n";

    Editor::PlayModeController ctrl;
    if (!ctrl.load("assets/bot.fbx", "assets")) {
        std::cerr << "[Engine] FAILED to load assets/bot.fbx - is the working dir the repo root?\n";
        return 1;
    }

    auto terrain = [](float, float) -> float { return 0.0f; };
    const float dt = 1.0f / 60.0f;
    double t = 0.0;
    int lastPhase = -1;
    float maxSpeed = 0.0f;
    float distance = 0.0f;
    glm::vec3 start = ctrl.character().position;

    for (int frame = 0; frame < opts.frames; ++frame) {
        const int phase = CinematicDemo::PhaseAt(t);
        const bool edge = (phase != lastPhase);
        lastPhase = phase;
        const CinematicDemo::Input cd = CinematicDemo::At(t, edge);
        CharacterInput in;
        in.moveDirection = cd.moveDirection;
        in.moveMagnitude = 1.0f;
        in.sprint = cd.sprint;
        in.jump = cd.jump;
        in.crouch = false;
        in.grounded = true;
        in.verticalVelocity = 0.0f;
        ctrl.update(dt, in, terrain);
        const glm::vec3 pos = ctrl.character().position;
        distance += glm::length(pos - start);
        start = pos;
        maxSpeed = std::max(maxSpeed, ctrl.character().currentSpeed());
        t += dt;
    }

    const auto& cc = ctrl.character();
    const auto diag = ctrl.debugPoseDiag();
    std::cout << "\n[Engine] Logic sim complete (" << opts.frames << " frames, "
              << (opts.frames * dt) << "s simulated)\n"
              << "  final position : " << cc.position.x << ", " << cc.position.y << ", " << cc.position.z << "\n"
              << "  distance moved : " << distance << " units\n"
              << "  max speed      : " << maxSpeed << " m/s\n"
              << "  final state    : " << AnimationStateToString(cc.state()) << "\n"
              << "  matcher clip   : " << diag.matcherClip << "\n"
              << "  motion match   : " << (cc.isMotionMatchingActive() ? "ACTIVE" : "off") << "\n";
    std::cout << "[Engine] Logic sim OK.\n";
    return 0;
}

// ============================================================================
// Helpers shared by the play-mode loop
// ============================================================================
static CameraState AnimStateToCamState(AnimationState s) {
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

// ============================================================================
// Viewport camera modes - Free / Follow / Orbit / Top-down / First-person
// ============================================================================
// Mode ids match editor.uiState.playCameraMode, the Camera menu and the
// World Settings combo. 0 = Free (fly camera), 1-4 orbit the bot. Keys 0-4
// switch modes, right-drag orbits/tilts, scroll wheel zooms.
static const char* kPlayCameraModeNames[] = {"Free", "Follow", "Orbit", "Top-Down", "First-Person"};
static constexpr int kPlayCameraModeCount = 5;

static void UpdatePlayCamera(int mode, ThirdPersonCamera& cam,
                             const AnimatedCharacter& cc, float dt, float aspect,
                             const Input::InputState& in, bool& firstInit,
                             int prevMode, float& fpPitch) {
    const glm::vec3 charPos = cc.position;
    const float heading = cc.heading;
    const bool drag = in.isMouseButtonDown(Input::MouseButton::RIGHT);

    CameraInput camIn;
    camIn.characterPosition = charPos;
    camIn.characterVelocity = cc.velocity;
    camIn.moveMagnitude = cc.currentSpeed();
    camIn.isGrounded = cc.grounded;
    camIn.characterForward = glm::vec3(-std::sin(heading), 0.0f, -std::cos(heading));
    camIn.animState = AnimStateToCamState(cc.state());

    // ---- First-person: camera at head height, look follows heading + pitch --
    if (mode == 3) {
        if (prevMode != 3) fpPitch = 0.0f;
        if (drag) fpPitch = std::clamp(fpPitch + in.mouseDelta.y * 0.1f, -75.0f, 75.0f);
        const float p = glm::radians(fpPitch);
        const glm::vec3 fwd = glm::normalize(
            glm::vec3(-std::sin(heading) * std::cos(p),
                       std::sin(p),
                       -std::cos(heading) * std::cos(p)));
        cam.position = charPos + glm::vec3(0.0f, 1.55f, 0.0f);  // eye height
        cam.target = cam.position + fwd * 10.0f;
        return;
    }

    // ---- Follow / Orbit / Top-down share the state-aware follow camera ------
    if (firstInit) {
        // Start slightly above and behind the character, chest-level target.
        cam.position = charPos + glm::vec3(0.0f, 2.2f, 3.5f);
        cam.target = charPos + glm::vec3(0.0f, 1.3f, 0.0f);
        cam.currentState = camIn.animState;
        cam.yaw = -90.0f;
        cam.pitch = 10.0f;
        cam.config.distance = 4.0f;
        cam.config.height = 1.6f;
        firstInit = false;
    }

    if (mode == 1) {  // Orbit: free mouse orbit + scroll zoom
        if (drag) {
            cam.yaw -= in.mouseDelta.x ;
            // Drag up = camera higher (Unreal orbit convention)
            cam.pitch = std::clamp(cam.pitch - in.mouseDelta.y * 0.3f,
                                   cam.config.minPitch, cam.config.maxPitch);
        }
        cam.config.distance = std::clamp(cam.config.distance - in.scrollDelta.y * 1.5f, 2.0f, 10.0f);
    } else if (mode == 2) {  // Top-down: aerial overview, rotate around the character
        if (drag) cam.yaw -= in.mouseDelta.x;
        cam.pitch = 80.0f;
        cam.config.distance = 9.0f;
    } else {  // Follow: auto-face the bot's heading, right-drag tilts, scroll zooms
        const float targetYaw = glm::degrees(std::atan2(-std::cos(heading), -std::sin(heading)));
        float dy = targetYaw - cam.yaw;
        while (dy > 180.0f) dy -= 360.0f;
        while (dy < -180.0f) dy += 360.0f;
        cam.yaw += dy * std::min(1.0f, 1.0f - std::exp(-6.0f * dt));
        if (drag) {
            // Drag up = look from higher (Unreal convention)
            cam.pitch = std::clamp(cam.pitch - in.mouseDelta.y ,
                                   -20.0f, 55.0f);
        } else {
            cam.pitch = glm::mix(cam.pitch, 10.0f, std::min(1.0f, 1.0f - std::exp(-3.0f * dt)));
        }
        cam.config.distance = std::clamp(cam.config.distance - in.scrollDelta.y * 1.5f, 2.0f, 9.0f);
    }

    cam.update(dt, camIn, aspect);
}

static CharacterInput KeyboardInput(const Input::InputState& ks) {
    CharacterInput input;
    glm::vec2 move(0.0f);
    if (ks.isKeyDown(GLFW_KEY_W)) move.y -= 1.0f;
    if (ks.isKeyDown(GLFW_KEY_S)) move.y += 1.0f;
    if (ks.isKeyDown(GLFW_KEY_A)) move.x += 1.0f;
    if (ks.isKeyDown(GLFW_KEY_D)) move.x -= 1.0f;
    const float len = glm::length(move);
    if (len > 0.001f) move /= len;
    input.moveDirection = move;
    input.moveMagnitude = std::min(1.0f, len);
    input.jump = ks.isKeyDown(GLFW_KEY_SPACE);
    input.crouch = ks.isKeyDown(GLFW_KEY_LEFT_CONTROL) || ks.isKeyDown(GLFW_KEY_C);
    input.sprint = ks.isKeyDown(GLFW_KEY_LEFT_SHIFT) || ks.isKeyDown(GLFW_KEY_RIGHT_SHIFT);
    input.grounded = true;
    input.verticalVelocity = 0.0f;
    return input;
}

static void RenderPlayCharacter(Editor::PlayModeController& play, Model* playModel,
                                Render::RenderPipeline& pipeline,
                                const glm::mat4& view, const glm::mat4& proj,
                                const glm::vec3& cameraPos) {
    if (!playModel || !play.isLoaded()) return;
    Shader* shader = pipeline.getModelShader();
    if (!shader) return;

    AnimatedCharacter& cc = play.character();
    glm::mat4 model(1.0f);
    model = glm::translate(model, cc.position);
    model = glm::rotate(model, cc.heading, glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::scale(model, glm::vec3(cc.scale));

    shader->use();
    shader->setMat4("projection", proj);
    shader->setMat4("view", view);
    shader->setMat4("model", model);
    shader->setVec3("lightPos", glm::vec3(10.0f, 15.0f, 10.0f));
    shader->setVec3("viewPos", cameraPos);
    shader->setInt("uDisableInstancing", 1);
    shader->setInt("uDisableSkinning", 0);
    shader->setInt("uPaletteSize", 256);
    shader->setInt("uShowDebug", 0);

    Animator* animator = cc.animator();
    if (animator) playModel->Draw(*shader, *animator);
    else playModel->DrawStatic(*shader);
}

// ============================================================================
// Phase 2 - full engine + editor application
// ============================================================================
static int runEditorApp(const AppOptions& opts) {
    std::cout << "\n============================================================\n"
              << "  PHASE 2/2 - ENGINE + EDITOR APP\n"
              << "============================================================\n";

    // ---- GLFW --------------------------------------------------------------
    if (!glfwInit()) {
        std::cerr << "[Engine] glfwInit failed" << std::endl;
        return runLogicSim(opts);
    }

    const bool hidden = opts.headless || (std::getenv("DISPLAY") == nullptr);
    glfwWindowHint(GLFW_VISIBLE, hidden ? GLFW_FALSE : GLFW_TRUE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    auto& renderConfig = Config::getRenderConfig();
    GLFWwindow* window = glfwCreateWindow(renderConfig.defaultWindowWidth,
                                          renderConfig.defaultWindowHeight,
                                          "RTT Engine - Editor", nullptr, nullptr);
    if (!window) {
        std::cerr << "[Engine] Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return runLogicSim(opts);
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(hidden ? 0 : 1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "[Engine] Failed to initialize GLAD" << std::endl;
        glfwDestroyWindow(window);
        glfwTerminate();
        return runLogicSim(opts);
    }
    std::cout << "[Engine] OpenGL: " << glGetString(GL_VERSION)
              << " | " << glGetString(GL_RENDERER) << "\n";
    glctx::setAlive(true);

    // ---- Editor state (shaders, meshes, grid, ECS world, camera) -----------
    std::cout << "[Engine] Initializing editor..." << std::endl;
    InitEditor();
    std::cout << "[Engine] Editor initialized." << std::endl;

    // Bring the GeoAPI facade (used by the Geo tracking panel + status bar) in
    // sync with the ECS geospatial pipeline's origin, then seed the panel state.
    {
        auto& geoApi = g_editor.geospatialSystem().getGeoAPI();
        geoApi.initialize(-33.8568, 151.2153, 50.0);   // Sydney origin (matches ECS)
        geoApi.setGPSMode(GPSTracker::Mode::SIMULATED_WALK);
        const geo::GeoConfig geoCfg = geoApi.getConfig();
        g_editor.geoPanelState.originLat = geoCfg.originLat;
        g_editor.geoPanelState.originLon = geoCfg.originLon;
        g_editor.geoPanelState.originAlt = geoCfg.originAlt;
        // Keep the panel's mode selector in sync with the running pipeline
        // (mode enum: 0=DISABLED, 1=STATIC, 2=WALK, 3=VEHICLE, 4=AIRCRAFT).
        g_editor.geoPanelState.gpsModeIndex = (int)geoCfg.gpsMode;
    }

    // ---- ImGui + Phosphor icons ---------------------------------------------
    std::remove("engine_ui.ini");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "engine_ui.ini";
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigWindowsResizeFromEdges = true;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 450");

    // Editor look & feel: JetBrains Mono UI font (default), then the Phosphor
    // + FontAwesome6 icon fonts, then the modern theme painted onto ImGui.
    EditorTheme::LoadEditorFonts(io);
    if (!PhosphorImGui::Load(io, 14.0f)) {
        std::cerr << "[WARN] Failed to load Phosphor icons\n";
    }
    FaImGui::Load(io);

    // Restore persisted UI preferences (panel toggles + theme mode) from
    // ui_panel.cfg, then paint the chosen theme.
    UIConfig::LoadConfig(&g_editor.uiState.showOutliner, &g_editor.uiState.showDetails);
    EditorTheme::SetThemeMode(UIConfig::gThemeMode == 1 ? EditorTheme::ThemeMode::Light
                                                        : EditorTheme::ThemeMode::Dark);
    EditorTheme::ApplyTheme();

    flyCamera* cam = g_editor.camera();
    if (cam) {
        cam->Position = glm::vec3(0.0f, 10.0f, 20.0f);
        cam->Target = glm::vec3(0.0f, 0.0f, 0.0f);
    }

    // ---- Input manager -------------------------------------------------------
    Input::InputManager& inputManager = Input::InputManager::getInstance();
    inputManager.initialize(window);

    // ---- Render pipeline (viewport FBO + skybox + batched renderer) ----------
    Render::RenderPipeline& renderPipeline = Render::RenderPipeline::getInstance();
    if (!renderPipeline.initialize()) {
        std::cerr << "[Engine] Failed to initialize render pipeline\n";
        glctx::setAlive(false);
        CleanupEditor();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // ---- World manager (terrain + physics floor + world objects) -------------
    World::WorldManager& worldManager = World::WorldManager::getInstance();
    Config::TerrainConfig terrainConfig = Config::getTerrainConfig();
    Config::VegetationConfig vegConfig = Config::getVegetationConfig();
    terrainConfig.viewDistance = 2;
    terrainConfig.lodDistance = renderConfig.terrainLODDistance;
    vegConfig.vegetationDrawDistance = renderConfig.vegetationLODDistance;
    if (!worldManager.initialize(terrainConfig, vegConfig)) {
        std::cerr << "[Engine] World manager initialization had issues\n";
    }

    ecs::World& world = g_editor.world();

    // ---- Play mode state ------------------------------------------------------
    Editor::PlayModeController play;
    std::unique_ptr<Model> playModel;
    ThirdPersonCamera followCam;
    bool followCamInit = false;
    int lastCamMode = -1;
    float fpPitch = 0.0f;
    bool playWasActive = false;
    double demoTime = 0.0;
    int lastPhase = -1;

    // ---- The bot lives in the viewport permanently -----------------------------
    // Load the character + its locomotion clips once at startup so it stands
    // (idle animation) in the editor viewport. Play mode hands the player
    // control; camera modes 1-4 orbit it.
    if (!play.load("assets/bot.fbx", "assets")) {
        std::cerr << "[Engine] Failed to load character\n";
    }
    playModel = std::make_unique<Model>("assets/bot.fbx");
    if (playModel->GetMeshCount() == 0) playModel.reset();

    // ---- Timing ----------------------------------------------------------------
    float lastTime = static_cast<float>(glfwGetTime());
    float fpsTimer = 0.0f;
    int frames = 0;
    float fps = 0.0f;
    int frameCount = 0;
    float maxSpeed = 0.0f;
    glm::vec3 startPos(0.0f);
    bool haveStart = false;

    // ---- Cinematic camera intro (skipped in headless) --------------------------
    double cinematicStart = -1.0;
    bool cinematicActive = !opts.headless;
    constexpr double CINEMATIC_DURATION = 6.0;
    constexpr float CINEMATIC_RADIUS = 32.0f;
    constexpr float CINEMATIC_HEIGHT = 14.0f;
    if (cinematicActive && cam) {
        cinematicStart = glfwGetTime();
        cam->Position = glm::vec3(-CINEMATIC_RADIUS, CINEMATIC_HEIGHT * 1.5f, 0.0f);
        cam->Target = glm::vec3(0.0f, 1.5f, 0.0f);
    }

    EntityCache entityCache;
    entityCache.dirty = true;

    // ---- Main loop --------------------------------------------------------------
    while (!glfwWindowShouldClose(window)) {
        // Headless: fixed 60 Hz timestep for a deterministic bounded run.
        float now = static_cast<float>(glfwGetTime());
        float dt = opts.headless ? (1.0f / 60.0f) : (now - lastTime);
        lastTime = now;
        if (dt > 0.1f) dt = 0.1f;
        if (dt < 0.0f) dt = 0.0f;

        ++frameCount;
        ++frames;
        fpsTimer += dt;
        if (fpsTimer >= 1.0f) {
            fps = static_cast<float>(frames) / fpsTimer;
            frames = 0;
            fpsTimer = 0.0f;
        }

        inputManager.update();
        const Input::InputState& inputState = inputManager.getInputState();
        bool wantCaptureKeyboard = io.WantCaptureKeyboard;
        bool wantCaptureMouse = io.WantCaptureMouse;
        // When the cursor is over the viewport panel (and nothing - menu,
        // modal, other panel - is stacked on top of it), hand the mouse +
        // keyboard to the camera (orbit / zoom / fly / bot control) instead
        // of ImGui swallowing it. Without this, io.WantCaptureKeyboard/Mouse
        // are true whenever the cursor is over ANY ImGui window - including
        // the viewport - which silently kills WASD + trackpad/mouse control.
        // Clicking the viewport also latches KEYBOARD ownership, so WASD keeps
        // flying even if the cursor drifts over a panel edge (the mouse itself
        // stays with the panel it is over; text fields / active widgets still
        // keep ImGui's keyboard capture).
        static bool s_viewportFocused = false;
        const bool overViewport = UI::IsViewport3DHovered() && !io.WantTextInput;
        if (inputState.isMouseButtonPressed(Input::MouseButton::LEFT) || io.MouseClicked[0]) {
            s_viewportFocused = overViewport;
        }
        if (overViewport) wantCaptureMouse = false;
        // Keyboard goes to the game (camera / bot) when the viewport is hovered
        // or focused, and ALWAYS during play mode - so WASD drives the bot even
        // when the cursor sits over a panel. Active widgets (sliders, combos)
        // and text fields keep ImGui's keyboard capture.
        const bool cameraOwnsKeyboard =
            (overViewport || s_viewportFocused || g_editor.isPlaying()) &&
            !ImGui::IsAnyItemActive() && !io.WantTextInput;
        if (cameraOwnsKeyboard) wantCaptureKeyboard = false;
        inputManager.setImGuiCapture(wantCaptureKeyboard, wantCaptureMouse);

        // Camera mode keys 0-4 (Free / Follow / Orbit / Top-down / First-person).
        // Mode is shared with the Camera menu + World Settings combo via uiState.
        if (!wantCaptureKeyboard) {
            if (inputState.isKeyPressed(GLFW_KEY_0)) g_editor.uiState.playCameraMode = 0;
            else if (inputState.isKeyPressed(GLFW_KEY_1)) g_editor.uiState.playCameraMode = 1;
            else if (inputState.isKeyPressed(GLFW_KEY_2)) g_editor.uiState.playCameraMode = 2;
            else if (inputState.isKeyPressed(GLFW_KEY_3)) g_editor.uiState.playCameraMode = 3;
            else if (inputState.isKeyPressed(GLFW_KEY_4)) g_editor.uiState.playCameraMode = 4;
        }
        const int camMode = std::clamp(g_editor.uiState.playCameraMode, 0, kPlayCameraModeCount - 1);

        int windowW, windowH;
        glfwGetWindowSize(window, &windowW, &windowH);
        if (windowH == 0) windowH = 1;

        // ---- Play mode toggles (toolbar button sets g_editor state) -------------
        if (!opts.headless && !wantCaptureKeyboard && inputState.isKeyDown(GLFW_KEY_F5)) {
            g_editor.setPlaying(!g_editor.isPlaying());
        }
        // F8 toggles the Geo tracking panel (signature feature - live GPS feeds,
        // trajectory prediction and storage).
        if (!opts.headless && !wantCaptureKeyboard && inputState.isKeyDown(GLFW_KEY_F8)) {
            g_editor.uiState.showGameMode = !g_editor.uiState.showGameMode;
            // Geo tab is index 3 in the fixed tab bar {Outliner, Layers, World, Geo}.
            if (g_editor.uiState.showGameMode) g_editor.scenePanelConfig.activeTabIndex = 3;
        }

        // ---- Editor keyboard shortcuts (Ctrl combos + Delete) ----------------
        if (!opts.headless && !wantCaptureKeyboard) {
            const bool ctrl = inputState.isKeyDown(GLFW_KEY_LEFT_CONTROL) ||
                              inputState.isKeyDown(GLFW_KEY_RIGHT_CONTROL);
            if (ctrl && inputState.isKeyPressed(GLFW_KEY_Z)) {
                UndoRedo::Undo();
                g_editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
            }
            if (ctrl && inputState.isKeyPressed(GLFW_KEY_Y)) {
                UndoRedo::Redo();
                g_editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
            }
            if (ctrl && inputState.isKeyDown(GLFW_KEY_S)) {
                SceneManager::SaveScene(g_editor.uiState.sceneFile, g_editor.world());
            }
            if (ctrl && inputState.isKeyDown(GLFW_KEY_D)) {
                ecs::EntityID dup = EntityManager::DuplicateEntity(g_editor.selectedEntity());
                if (dup != ecs::INVALID_ENTITY_ID) g_editor.setSelectedEntity(dup);
            }
            if (ctrl && inputState.isKeyDown(GLFW_KEY_N)) {
                g_editor.world().shutdown();
                g_editor.world().init();
                UndoRedo::Clear();
                g_editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
            }
            if (ctrl && inputState.isKeyDown(GLFW_KEY_O)) {
                g_editor.uiState.showOpenScene = true;
            }
            if (inputState.isKeyPressed(GLFW_KEY_DELETE) ||
                inputState.isKeyPressed(GLFW_KEY_BACKSPACE)) {
                EntityManager::DeleteEntity(g_editor.selectedEntity());
                g_editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
            }
        }
        const bool playing = g_editor.isPlaying();
        // The bot is always present in the viewport (loaded at startup). Play
        // mode just hands the player control; exiting keeps the bot visible.
        if (playing && !playWasActive) {
            followCamInit = false;
            std::cout << "[Engine] Entering play mode\n";
        } else if (!playing && playWasActive) {
            std::cout << "[Engine] Exiting play mode\n";
        }
        playWasActive = playing;

        // Headless: auto-enter play mode and drive the scripted cinematic demo.
        if (opts.headless && !playing) {
            g_editor.setPlaying(true);
            continue;  // re-run this frame with play mode active
        }

       // ---- Cinematic camera intro -------------------------------------------
      /*  if (cinematicActive && cam) {
            double elapsed = glfwGetTime() - cinematicStart;
            bool userInput = inputState.isKeyDown(GLFW_KEY_W) || inputState.isKeyDown(GLFW_KEY_S) ||
                             inputState.isKeyDown(GLFW_KEY_A) || inputState.isKeyDown(GLFW_KEY_D) ||
                             inputState.isMouseButtonDown(Input::MouseButton::RIGHT) ||
                             inputState.isMouseButtonDown(Input::MouseButton::LEFT);
            if (userInput || elapsed >= CINEMATIC_DURATION) {
                cinematicActive = false;
            } else {
                float t = (float)(elapsed / CINEMATIC_DURATION);
                float ease = t < 0.5f ? 4.0f * t * t * t
                                      : 1.0f - (float)std::pow(-2.0 * t + 2.0, 3.0) * 0.5f;
                float angle = (float)elapsed * 18.0f;
                float yawRad = glm::radians(angle);
                glm::vec3 pos(glm::cos(yawRad) * CINEMATIC_RADIUS, 0.0f, glm::sin(yawRad) * CINEMATIC_RADIUS);
                pos.y = glm::mix(CINEMATIC_HEIGHT * 1.5f, CINEMATIC_HEIGHT * 0.6f, ease);
                cam->Position = pos;
                cam->Target = glm::vec3(0.0f, 1.5f, 0.0f);
                (void)ease;
            }
        }*/

        // ---- Camera (fly / follow) ---------------------------------------------
        // Mode 0 (Free) uses the fly camera: WASD + right-drag look. Modes 1-4
        // orbit the bot via UpdatePlayCamera below.
        if (camMode == 0) {
            if (!wantCaptureKeyboard) {
                float moveSpeed = 10.0f * dt;
                if (cam) {
                    glm::vec3 forward = glm::normalize(cam->Target + cam->Position);
                    glm::vec3 right = glm::normalize(glm::cross(forward, cam->WorldUp));
                    if (inputState.isKeyDown(GLFW_KEY_W)) { cam->Position -= forward * moveSpeed; cam->Target -= forward * moveSpeed; }
                    if (inputState.isKeyDown(GLFW_KEY_S)) { cam->Position += forward * moveSpeed; cam->Target -= forward * moveSpeed; }
                    if (inputState.isKeyDown(GLFW_KEY_A)) { cam->Position += right * moveSpeed; cam->Target -= right * moveSpeed; }
                    if (inputState.isKeyDown(GLFW_KEY_D)) { cam->Position -= right * moveSpeed; cam->Target += right * moveSpeed; }
                }
            }
            // Orbit: hold right mouse (or trackpad right-button) and drag.
            // ProcessMouseMovement expects ABSOLUTE cursor positions (it
            // computes the delta internally) - passing mouseDelta would only
            // produce second-order jitter and make the orbit feel dead.
            if (!wantCaptureMouse) {
                if (cam) cam->ProcessMouseMovement(inputState.mousePosition.x, inputState.mousePosition.y);
            }
            // Zoom: scroll wheel / trackpad two-finger scroll.
            if (!wantCaptureMouse && inputState.scrollDelta.y != 0.0f) {
                if (cam) cam->ProcessMouseScroll(inputState.scrollDelta.y);
            }
        }

        // ---- Simulate ------------------------------------------------------------
        // The bot always runs in the viewport: outside play mode it gets zero
        // input (idle loop), in play mode it reads the keyboard / cinematic demo.
        const float aspect = (float)windowW / (float)std::max(1, windowH);
        if (play.isLoaded()) {
            CharacterInput in;
            if (playing) {
                if (opts.headless) {
                    const int phase = CinematicDemo::PhaseAt(demoTime);
                    const bool edge = (phase != lastPhase);
                    lastPhase = phase;
                    const CinematicDemo::Input cd = CinematicDemo::At(demoTime, edge);
                    in.moveDirection = cd.moveDirection;
                    in.moveMagnitude = 1.0f;
                    in.sprint = cd.sprint;
                    in.jump = cd.jump;
                    in.crouch = false;
                    in.grounded = true;
                    in.verticalVelocity = 0.0f;
                    demoTime += dt;
                } else {
                    in = KeyboardInput(inputState);
                }
            }
            // else: zero input -> the character idles in place

            auto terrain = [&worldManager](float x, float z) -> float {
                return worldManager.getHeightAt(x, z);
            };
            play.update(dt, in, terrain);

            const AnimatedCharacter& cc = play.character();

            // Unreal-style ground clamp: the follow camera never sinks below
            // the terrain (no more "under the floor" view).
            followCam.groundHeightFn = terrain;

            // Camera modes 1-4 (Follow/Orbit/Top-down/First-person) orbit the
            // bot; mode 0 (Free) uses the fly camera above. camMode is already
            // clamped + driven by keys 0-4 / the Camera menu / World Settings.
            if (camMode >= 1) {
                UpdatePlayCamera(camMode - 1, followCam, cc, dt, aspect, inputState,
                                 followCamInit, lastCamMode, fpPitch);
                lastCamMode = camMode - 1;
            } else {
                lastCamMode = -1;
            }

            if (!haveStart) { startPos = cc.position; haveStart = true; }
            maxSpeed = std::max(maxSpeed, cc.currentSpeed());

            // ---- Frame the bot once it has snapped to the terrain -------------
            // The camera starts aimed at the world origin, but the bot stands at
            // terrain height (y can be ~12+), so it used to be clipped off-frame
            // at the top edge. One-shot: point the free camera at the bot's chest
            // from a comfortable distance (the user can still fly away later).
            static bool s_cameraFramed = false;
            if (!s_cameraFramed && camMode == 0 && cam) {
                s_cameraFramed = true;
                // Unreal-style framing: chest-level target, ~5 units back.
                cam->Target = cc.position + glm::vec3(0.0f, 1.3f, 0.0f);
                cam->Position = cc.position + glm::vec3(0.0f, 2.0f, 5.0f);
            }
        }

        glm::vec3 cameraPosition = (camMode >= 1 && play.isLoaded())
                                       ? followCam.position
                                       : (cam ? cam->Position : glm::vec3(0.0f));
        worldManager.update(cameraPosition, dt);
        world.update(dt);

        // ---- Geo pipeline <-> UI bridge -------------------------------------------
        // Push the live ECS geospatial pipeline state into the GeoAPI facade so the
        // Geo tracking panel + status bar show real GPS data, and push back any
        // panel-driven GPS config changes (mode/speed/noise).
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
            if (opts.headless && frameCount == 120) {
                const geo::GPSStatus gs = geoApi.getGPSStatus();
                printf("[geo-diag] frame=%d fix=%s lat=%.6f lon=%.6f alt=%.1f speed=%.1f\n",
                       frameCount, gs.isValid ? "VALID" : "none",
                       gs.latitude, gs.longitude, gs.altitude, gs.speed);
                fflush(stdout);
            }
        }

        // ---- Render scene into the viewport FBO ----------------------------------
        // beginFrame()/renderScene() do not clear, so clear color + depth first or
        // the FBO keeps stale/undefined contents (blank viewport).
        glBindFramebuffer(GL_FRAMEBUFFER, renderPipeline.getFramebuffer());
        glViewport(0, 0, renderPipeline.getViewportWidth(), renderPipeline.getViewportHeight());
        glClearColor(0.08f, 0.09f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::mat4 view = cam ? cam->GetViewMatrix() : glm::mat4(1.0f);
        if (camMode >= 1 && play.isLoaded()) view = followCam.getViewMatrix();
        const float kCamFov = (camMode >= 1 && play.isLoaded())
                                  ? followCam.config.fov : 60.0f;
        glm::mat4 projection = glm::perspective(
            glm::radians(kCamFov),
            (float)renderPipeline.getViewportWidth() / (float)std::max(1, renderPipeline.getViewportHeight()),
            renderConfig.nearPlane, renderConfig.farPlane);

        renderPipeline.beginFrame();
        renderPipeline.renderScene(view, projection, cameraPosition, kCamFov);
        worldManager.render(view, projection, cameraPosition);
        // The bot is always in the viewport (idle outside play mode).
        if (play.isLoaded()) RenderPlayCharacter(play, playModel.get(), renderPipeline, view, projection, cameraPosition);


        // ---- ImGui editor UI on top of the viewport texture ----------------------
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, windowW, windowH);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (entityCache.dirty) {
            UI::RebuildEntityCache(entityCache, world);
        }

        UI::RenderMenuBar(g_editor, g_editor.uiState.sceneFile);
        if (g_editor.shouldClose()) glfwSetWindowShouldClose(window, true);
        UI::RenderSceneDialogs(g_editor);
        UI::RenderToolbar(g_editor);
        UI::RenderLeftPanel(g_editor);
        UI::RenderRightPanel(g_editor);
        UI::RenderBottomPanel(g_editor, fps);
        // Pass the active view camera (fly in Free mode, follow cam in modes
        // 1-4) so the viewport overlay + gizmo/grid reflect what is rendered.
        const glm::vec3* activeCamPos = (camMode >= 1 && play.isLoaded())
                                            ? &followCam.position
                                            : (cam ? &cam->Position : nullptr);
        const glm::vec3* activeCamTarget = (camMode >= 1 && play.isLoaded())
                                               ? &followCam.target
                                               : (cam ? &cam->Target : nullptr);
        UI::RenderViewport(g_editor, renderPipeline.getFramebufferTexture(), windowW, windowH, window, &io,
                           projection, view,
                           play.isLoaded() ? kPlayCameraModeNames[camMode] : nullptr,
                           activeCamPos, activeCamTarget);
        if (playing && play.isLoaded()) UI::RenderPlayModeHUD(play, kPlayCameraModeNames[camMode]);
        UI::RenderStatusBar(world.getEntityCount(), g_editor.selectedEntity(), fps,
                            g_editor.isPlaying(), g_editor.wasPlaying(), windowW, windowH);
        UI::RenderAboutDialog(g_editor.showAboutRef());

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        renderPipeline.renderUI();
        renderPipeline.endFrame();

        glfwSwapBuffers(window);
        glfwPollEvents();

        // ---- Headless bound -------------------------------------------------------
        if (hidden && frameCount >= opts.frames) {
            glfwSetWindowShouldClose(window, true);
        }


    }

    // ---- Summary ------------------------------------------------------------------
    std::cout << "\n[Engine] Main loop ended after " << frameCount << " frames\n";
    if (play.isLoaded()) {
        const auto& cc = play.character();
        const auto diag = play.debugPoseDiag();
        const float dist = glm::length(cc.position - startPos);
        std::cout << "  character final : " << cc.position.x << ", " << cc.position.y
                  << ", " << cc.position.z << "\n"
                  << "  distance moved  : " << dist << " units\n"
                  << "  max speed       : " << maxSpeed << " m/s\n"
                  << "  final state     : " << AnimationStateToString(cc.state()) << "\n"
                  << "  matcher clip    : " << diag.matcherClip << "\n"
                  << "  active clip     : " << cc.activeClipName() << "\n"
                  << "  motion matching : " << (cc.isMotionMatchingActive() ? "ACTIVE" : "off") << "\n"
                  << "  foot IK         : L=" << (diag.leftLocked ? "LOCK" : "free")
                  << " R=" << (diag.rightLocked ? "LOCK" : "free") << "\n";
    }
    std::cout << "  average fps     : " << (frameCount > 0 ? (float)frameCount / std::max(0.001f, lastTime - 0.0f) : 0.0f) << "\n";

    // ---- Cleanup --------------------------------------------------------------------
    // Tear down play-mode GPU resources while the GL context is still alive
    // (Model's destructor issues GL calls via BoneMatrixBuffer::Shutdown).
    play.shutdown();
    playModel.reset();
    CleanupEditor();
    glctx::setAlive(false);
    renderPipeline.shutdown();
    inputManager.shutdown();
    worldManager.shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    std::cout << "[Engine] Shutdown complete.\n";
    return 0;
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char** argv) {
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
    signal(SIGFPE, crashHandler);

    const AppOptions opts = parseOptions(argc, argv);

    std::cout << "\n"
              << "  ============================================================\n"
              << "    RTT ENGINE - ENGINE + EDITOR APPLICATION\n"
              << "    tests: 535 | systems: ECS, physics, animation, motion\n"
              << "    matching, camera, terrain, memory, geo, editor UI, demo\n"
              << "  ============================================================\n";

    if (opts.help) {
        std::cout << kUsage;
        return 0;
    }

    printSystemInventory();

    if (!opts.skipTests) {
        const int testResult = runTestPhase(argc, argv);
        if (testResult != 0) {
            std::cerr << "\n[Engine] Self-check FAILED - not booting the editor app.\n"
                      << "[Engine] Fix the failing tests, or re-run with --skip-tests\n"
                      << "[Engine] to force-boot anyway.\n";
            return testResult;
        }
        std::cout << "\n[Engine] All tests passed - booting the editor app.\n";
    } else {
        std::cout << "\n[Engine] Self-check skipped (--skip-tests) - booting the editor app.\n";
    }

    if (opts.headless) {
        std::cout << "\n[Engine] Headless mode: bounded run of " << opts.frames << " frames.\n";
    }

    return runEditorApp(opts);
}
