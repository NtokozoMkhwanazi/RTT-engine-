// ============================================================================
//  test.cpp - RTT Engine + Editor Application
// ============================================================================
//  The single engine entry point. Two phases:
//
//    1. SELF-CHECK: runs the complete Google Test suite (712 tests across 99
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
#include <functional>  // std::function for skeleton debug recursion
#include <cxxabi.h>
#include <unistd.h>   // execvp (engine restart)

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "rhi/RHI.h"                      // Phase B Step A: rhi->renderFrameScene bridge under RTT_RHI_SCENE
#include "ecs/components/AnimatedCharacterComponent.h" // geo Personnel -> animated NPC (sim WALK drive)
#include "ecs/systems/Systems.h"
#include "renderer/Renderer.h"
#include "cameraSystem/flyCamera.h"
#include "cameraSystem/ThirdPersonCamera.h"
#include "cameraSystem/CameraFocusSystem.h"
#include "editor/CameraFocus.h"
#include "shaderSystem/Shader.h"
#include "renderer/DebugRenderer.h"  // g_debugRenderer for skeleton debug
#include "boneSystem/Skeleton.h"    // Skeleton, BoneInfo for skeleton debug
#include <glm/gtc/matrix_inverse.hpp>  // glm::affineInverse
#include "editor/render_pipeline.h"
#include "editor/world_manager.h"
#include "editor/editor_state.h"
#include "editor/config.h"
#include "editor/console.h"
#include "editor/grid_renderer.h"
#include "editor/input_manager.h"
#include "editor/ui_config.h"
#include "editor/ui.h"
#include "editor/phosphor_imgui.h"
#include "editor/editor_theme.h"
#include "editor/fa_imgui.h"
#include "editor/gl_context_lifecycle.h"
#include "editor/undo_redo.h"
#include "editor/scene_manager.h"
#include "editor/entity_manager.h"
#include "editor/shader_manager.h"
#include "editor/PlayModeController.h"
#include "animationSystem/AnimationStateMachine.h"
#include "cameraSystem/CinematicDemo.h"
#include "demo/DemoRecorder.h"
#include "geospatial/GeoAPI.h"
#include "geospatial/GeoHTTPServer.h"
#include "geospatial/GPSTracker.h"
#include "geospatial/SafetyGeofenceSystem.h"
#include "renderer/GPUProfilerAdvanced.h"

// ============================================================================
// Options
// ============================================================================
struct AppOptions {
    bool skipTests = false;   // --skip-tests   / RTT_SKIP_TESTS=1
    bool headless  = false;   // --headless     / RTT_HEADLESS=1
    bool help      = false;
    int  frames    = 900;     // --frames N     / RTT_FRAMES=N  (headless bound)
    int  geoStress = 0;       // --geo-stress N  spawn N animated geo Personnel NPCs (idle/walk/jog/run/sprint)
};

static const char* kUsage =
    "Usage: engine [options]\n"
    "  --skip-tests      Skip the Google Test self-check phase\n"
    "  --headless        Bounded run with a hidden window (auto-exit; no display\n"
    "                    falls back to a GL-free logic sim)\n"
    "  --frames N        Headless frame budget (default 900)\n"
    "  --geo-stress N    Spawn N animated geo Personnel NPCs (Shaw.fbx), each\n"
    "                    geoDriven to a distinct FSM band (idle/walk/jog/run/sprint)\n"
    "                    to stress the geo->animated-NPC pipeline\n"
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
        else if (a == "--geo-stress" && i + 1 < argc) { o.geoStress = std::atoi(argv[++i]); if (o.geoStress < 0) o.geoStress = 0; }
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
              << "  (712 tests across 99 suites)\n"
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
    if (!ctrl.load("assets/Male Locomotion Pack/Shaw.fbx", "assets")) {
        std::cerr << "[Engine] FAILED to load assets/Male Locomotion Pack/Shaw.fbx - is the working dir the repo root?\n";
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
// Viewport camera modes - Free / Third-person / First-person
// ============================================================================
// Collapsed to THREE world-space cameras (was Free/Follow/Orbit/Top-Down/
// First-Person). Mode ids match editor.uiState.playCameraMode, the Camera
// menu and the World Settings combo: 0 = Free (fly camera), 1 = Third-person
// (follow + free-orbit folded), 2 = First-person. Orbit folded into
// Third-person (right-drag free yaw + scroll zoom); Top-Down / Cinematic are
// dropped from the UI (Cinematic stays engine-internal for the demo/tests).
static const char* kPlayCameraModeNames[] = {"Free", "Third-person", "First-person"};
static constexpr int kPlayCameraModeCount = 3;

static void UpdatePlayCamera(int mode, ThirdPersonCamera& cam,
                             const EntityFocus& focus, float dt, float aspect,
                             const Input::InputState& in, bool& firstInit,
                             int prevMode, float& fpPitch, float& zoomOffset) {
    // The follow camera tracks the SELECTED entity (NPC / prop) when one is
    // selected, else the playable character; `focus` is resolved upstream from
    // the ECS selection + live bot state. This decouples the camera from the
    // AnimatedCharacter object itself.
    const glm::vec3 focusPos = focus.position;
    const glm::vec3 focusForward = (glm::length(focus.forward) > 1e-4f)
        ? glm::normalize(focus.forward) : glm::vec3(0.0f, 0.0f, 1.0f);
    const bool animated = focus.animated;
    const bool drag = in.isMouseButtonDown(Input::MouseButton::RIGHT);

    CameraInput camIn;
    camIn.characterPosition = focusPos;
    camIn.characterVelocity = focus.velocity;
    camIn.moveMagnitude = focus.moveMagnitude;
    camIn.isGrounded = true;
    camIn.characterForward = focusForward;
    camIn.animState = focus.animState;
    camIn.targetIsAnimated = animated;

    // ---- First-person: head-height view decoupled from the focus model. Heading
    // (yaw) comes from the Yaw slider (world-space, no model dependency); pitch
    // rests at the Pitch slider + right-drag tilts. Mirrors Third-person's
    // slider binding so all viewport cams share Camera Settings.
    if (mode == 3) {
        const float yawR = glm::radians(g_editor.uiState.playCameraYaw);
        fpPitch = drag ? std::clamp(fpPitch + in.mouseDelta.y * 0.1f, -75.0f, 75.0f)
                 : glm::mix(fpPitch, g_editor.uiState.playCameraPitch,
                            std::min(1.0f, 1.0f - std::exp(-3.0f * dt)));
        const float p = glm::radians(fpPitch);
        const glm::vec3 fwd = glm::normalize(
            glm::vec3(std::sin(yawR) * std::cos(p),
                       std::sin(p),
                       std::cos(yawR) * std::cos(p)));
        cam.position = focusPos + glm::vec3(0.0f, 1.8f, 0.0f);  // eye height
        cam.target = cam.position + fwd * 10.0f;
        return;
    }

    // ---- Follow / Orbit / Top-down share the state-aware follow camera ------
    if (firstInit) {
        // Start slightly above and behind the focus target, chest-level target.
        // The bot renders at 70% height (~1.26 m), so the framing target
        // sits proportionally higher (chest/shoulders of the larger bot).
        cam.position = focusPos + glm::vec3(0.0f, 2.2f * 0.7f, 3.5f * 0.7f);
        cam.target = focusPos + glm::vec3(0.0f, 1.3f * 0.7f, 0.0f);
        cam.currentState = camIn.animState;
        cam.yaw = g_editor.uiState.playCameraYaw;
        // Wire the World Settings Distance / Pitch sliders to the initial
        // camera pose instead of hardcoded 4.0f / 10.0f. The zoom offset
        // (accumulated from scroll in Follow/Orbit modes below) adds on top.
        cam.pitch = g_editor.uiState.playCameraPitch;
        cam.config.distance = g_editor.uiState.playCameraDistance + zoomOffset;
        cam.config.height = 1.6f * 0.7f;
        cam.config.pivotHeight = 1.3f * 0.7f;
        firstInit = false;
    }

    // Resting distance always tracks the slider + accumulated zoom offset,
    // so changing the Distance slider mid-play moves the camera immediately.
    cam.config.distance = g_editor.uiState.playCameraDistance + zoomOffset;

    if (mode == 1) {  // Orbit: free mouse orbit + scroll zoom
        if (drag) {
            cam.yaw -= in.mouseDelta.x ;
            // Drag up = camera higher (Unreal orbit convention)
            cam.pitch = std::clamp(cam.pitch - in.mouseDelta.y * 0.3f,
                                   cam.config.minPitch, cam.config.maxPitch);
        }
        zoomOffset = std::clamp(zoomOffset - in.scrollDelta.y * 1.5f, -7.0f, 7.0f);
    } else if (mode == 2) {  // Top-down: aerial overview, rotate around the character
        if (drag) cam.yaw -= in.mouseDelta.x;
        cam.pitch = 80.0f;
        zoomOffset = 0.0f;  // top-down is a fixed framing
    } else {  // Third-person (Follow + Orbit folded): right-drag freely yaws /
              // pitches the world-space cam (decoupled from the model); idle it
              // eases yaw toward the Yaw slider / pitch toward the Pitch slider;
              // scroll zooms Distance. A static prop (tree) just sits framed.
        if (drag) {
            // Free world-space yaw orbit (right-drag); yaw slider eases back when idle.
            cam.yaw -= in.mouseDelta.x;
            // Drag up = look from higher (Unreal convention)
            cam.pitch = std::clamp(cam.pitch - in.mouseDelta.y ,
                                   -20.0f, 55.0f);
        } else {
            // Idle: ease yaw toward the Yaw slider + pitch toward the Pitch slider
            // (was: hardcoded -90 yaw / 10 pitch rest). Right-drag breaks the ease.
            cam.yaw = glm::mix(cam.yaw, g_editor.uiState.playCameraYaw,
                               std::min(1.0f, 1.0f - std::exp(-4.0f * dt)));
            // Ease back to the slider-defined resting pitch (was: hardcoded 10.0f)
            cam.pitch = glm::mix(cam.pitch, g_editor.uiState.playCameraPitch,
                                 std::min(1.0f, 1.0f - std::exp(-3.0f * dt)));
        }
        zoomOffset = std::clamp(zoomOffset - in.scrollDelta.y * 1.5f, -7.0f, 7.0f);
    }

    cam.update(dt, camIn, aspect);
}

static CharacterInput KeyboardInput(const Input::InputState& ks) {
    CharacterInput input;
    glm::vec2 move(0.0f);
    // World-space move direction (x->X, y->Z); the bot turns to face it and
    // the follow camera orbits behind. Local forward is -Z, so W = forward
    // (-Z, away from the camera), S = backward, A = strafe left (-X), D =
    // right. A/D were previously swapped (A strafed right, D left).
    if (ks.isKeyDown(GLFW_KEY_W)) move.y -= 1.0f;
    if (ks.isKeyDown(GLFW_KEY_S)) move.y += 1.0f;
    if (ks.isKeyDown(GLFW_KEY_A)) move.x -= 1.0f;
    if (ks.isKeyDown(GLFW_KEY_D)) move.x += 1.0f;
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

[[maybe_unused]] static void RenderPlayCharacter(Editor::PlayModeController& play, Model* playModel,
                                Render::RenderPipeline& pipeline,
                                const glm::mat4& view, const glm::mat4& proj,
                                const glm::vec3& cameraPos) {
    if (!playModel || !play.isLoaded()) return;
    Shader* shader = pipeline.getModelShader();
    if (!shader) return;

    AnimatedCharacter& cc = play.character();
    // modelMatrix() applies the 180-degree facing flip (the bot is authored
    // facing +Z while the logic treats local -Z as forward) so the rendered
    // model faces AWAY from the follow camera. Without it the camera saw the
    // bot's front and it looked like it was walking backward.
    const glm::mat4 model = cc.modelMatrix();

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

    // ---- Skeleton debug overlay ----
    // When editor.uiState.showSkeletonDebug is on, draw the bone hierarchy
    // as wireframe lines using the final bone matrices. Bones are in model
    // space, so we transform by the character's model matrix.
    if (g_editor.uiState.showSkeletonDebug && cc.skeleton() && animator) {
        const glm::mat4& modelMat = cc.modelMatrix();
        [[maybe_unused]] const auto& bones = cc.skeleton()->bones;
        [[maybe_unused]] const auto& boneMaps = cc.skeleton()->boneMapping;
        const auto& finalMats = cc.debugFinalBones();
        if (!finalMats.empty()) {
            // Walk the AssimpNodeData hierarchy to draw parent-child bone lines
            // We use the bindTransform (local) to compute world positions.
            const Skeleton* skel = cc.skeleton();
            // Recursively draw the skeleton from the root node
            std::function<void(const AssimpNodeData&, const glm::mat4&)>
                drawBoneTree = [&](const AssimpNodeData& node, const glm::mat4& parentWorld) {
                    glm::mat4 localTransform = node.transform;
                    // If this node maps to a bone, use the final bone matrix
                    // (already includes animation) instead of bindTransform.
                    glm::mat4 worldTransform = parentWorld * localTransform;

                    if (node.boneIndex >= 0 && node.boneIndex < (int)finalMats.size()) {
                        // Use the animated bone transform for this node
                        // The final bone matrices are in model space (they already
                        // include the model matrix offset, so we need to use them
                        // directly with the character's model matrix).
                        const glm::mat4& boneModel = finalMats[node.boneIndex];
                        glm::vec3 bonePos = modelMat * boneModel * glm::vec4(0, 0, 0, 1);
                        // Draw from parent's position to this bone's position
                        // (parent already drawn in the recursive call)
                        // For root bone, just draw a small marker
                        if (node.boneIndex == skel->rootBoneIndex) {
                            // Root bone: draw a small cross
                            const float sz = 0.1f;
                            g_debugRenderer.drawLine(bonePos - glm::vec3(sz,0,0), bonePos + glm::vec3(sz,0,0), glm::vec3(1,0.3f,0.3f));
                            g_debugRenderer.drawLine(bonePos - glm::vec3(0,0,sz), bonePos + glm::vec3(0,0,sz), glm::vec3(1,0.3f,0.3f));
                            g_debugRenderer.drawLine(bonePos - glm::vec3(0,sz,0), bonePos + glm::vec3(0,sz,0), glm::vec3(1,0.3f,0.3f));
                        }
                        // Draw bone axes (colored cross at each joint)
                        if (g_editor.uiState.showBoneAxes) {
                            const float axisSz = 0.05f;
                            g_debugRenderer.drawLine(bonePos, bonePos + glm::vec3(axisSz, 0, 0), glm::vec3(1, 0.2f, 0.2f));
                            g_debugRenderer.drawLine(bonePos, bonePos + glm::vec3(0, axisSz, 0), glm::vec3(0.2f, 1, 0.2f));
                            g_debugRenderer.drawLine(bonePos, bonePos + glm::vec3(0, 0, axisSz), glm::vec3(0.2f, 0.2f, 1));
                        }
                        // Store current position for children to draw from
                        glm::vec3 parentPos = bonePos;
                        for (const auto& child : node.children) {
                            glm::vec3 childPos = modelMat * finalMats[child.boneIndex >= 0 ? child.boneIndex : 0] * glm::vec4(0,0,0,1);
                            g_debugRenderer.drawLine(parentPos, childPos, glm::vec3(0.9f, 0.9f, 0.2f));
                            drawBoneTree(child, worldTransform);
                        }
                    } else {
                        // Non-bone node: still traverse children
                        for (const auto& child : node.children) {
                            drawBoneTree(child, worldTransform);
                        }
                    }
                };
            drawBoneTree(skel->rootNode, modelMat);
        }
    }
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

    // Headless-ness is now AUTHORITATIVELY flag-driven: only `--headless`
    // (or RTT_HEADLESS=1) puts the engine into the bounded windowless run.
    // `--skip-tests` alone requests a real, visible editor window. Previously
    // this also auto-headless'd whenever $DISPLAY was unset, which silently
    // turned `make run` (= ./bin/engine --skip-tests) into a windowless run on
    // any non-X shell — the exact complaint. With a display present the window
    // is created normally below; on a display-less host glfwCreateWindow()
    // simply returns NULL and runEditorApp() falls back to the GL-free logic sim
    // (runLogicSim) with a clear message, instead of quietly going headless.
    const bool hidden = opts.headless;
    glfwWindowHint(GLFW_VISIBLE, hidden ? GLFW_FALSE : GLFW_TRUE);
    // Request the latest released OpenGL version. 4.6 is the high-water mark
    // (no 4.7 has ever been released) and promotes several image-quality
    // features to core (anisotropic filter, ARB_gl_spirv, DSA). BUT pinning a
    // single 4.6 context hard-rejects older drivers that are otherwise
    // supported — the engine has a documented 4.5-quality path (see the
    // warning + Renderer.cpp's glMultiDrawElements fallback), so we MUST
    // negotiate DOWN, not just bail out: try 4.6 core, then 4.5/4.4/3.3 core,
    // and only if EVERY tier is refused by GLFW do we drop to the GL-free logic
    // sim. This is what lets `make run` / `make run-headless` actually open a GL
    // window on GL 4.5 hardware (e.g. Mesa/llvmpipe, older GPUs) instead of
    // silently falling through to the headless sim on a 4.5-only / no-display
    // box. (Previously: glfwWindowHint(4.6) => glfwCreateWindow NULL => logic
    // sim, which masked every non-4.6 GPU as "headless".)
    auto& renderConfig = Config::getRenderConfig();
    struct GLRequest { int major, minor; int profile; const char* label; };
    const GLRequest requests[] = {
        {4, 6, GLFW_OPENGL_CORE_PROFILE, "4.6 core"},
        {4, 5, GLFW_OPENGL_CORE_PROFILE, "4.5 core"},
    };
    GLFWwindow* window = nullptr;
    [[maybe_unused]] int achievedMajor = 0, achievedMinor = 0;
    const int kMsaaSamples = 4;   // 4x MSAA (resolved below)
    for (const auto& req : requests) {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, req.major);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, req.minor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, req.profile);
        glfwWindowHint(GLFW_SAMPLES, kMsaaSamples);
        window = glfwCreateWindow(renderConfig.defaultWindowWidth,
                                  renderConfig.defaultWindowHeight,
                                  "RTT Engine - Editor", nullptr, nullptr);
        if (window) {
            achievedMajor = req.major;
            achievedMinor = req.minor;
            std::cout << "[Engine] GLFW window + GL context created ("
                      << req.label << " requested)";
            if (req.major < 4 || (req.major == 4 && req.minor < 6)) {
                std::cout << " - downgraded from 4.6 (engine 4.5-quality path active)";
            }
            std::cout << "\n";
            break;
        }
    }
    if (!window) {
        // Headless fallback: a visible editor window could not be created —
        // most commonly because there is no display server on this host (no
        // $DISPLAY / no XWayland compositor), or the GPU can't grant a 4.6
        // context. This is NOT a crash: we drop to the GL-free logic sim
        // (GeospatialSystem still ticks, twins still update, just no viewport)
        // so the engine boots and exits cleanly. To get the *viewport* you need
        // a real GL context; for the bounded windowless editor path, run on a
        // graphical session or pass --headless (which also needs a display for
        // the hidden GLFW window). The previous "Failed to create GLFW window"
        // wording was logged as an error and read like a crash — this path is
        // the intended headless fallback, so log it as informational.
        std::cout << "[Engine] No graphical display / GL context available on "
                  << "this host - running headless (GL-free logic sim, "
                  << opts.frames << " frames). The Geo viewport itself needs a "
                  << "real display; use --headless on a graphical session for "
                  << "the bounded windowless editor run.\n";
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
    std::cout << "[Engine] Requested OpenGL 4.6 core profile context.\n";
    glctx::setAlive(true);

    // ---- Graphics quality: toggle the GL capabilities a 4.6 core context
    //      exposes that directly improve output quality. Every call below uses
    //      only enums present in the bundled (3.3) loader, so they are safe
    //      regardless of the loader's GLVERSION flags:
    //
    //  * GL_FRAMEBUFFER_SRGB - gamma-correct compositing to the window. The
    //    renderer draws linear HDR (GL_RGBA16F G-buffer then tone-maps in the
    //    post-composite shader); without sRGB framebuffer encoding the final
    //    image lands on the display with the wrong (flat, washed-out) gamma.
    //  * GL_MULTISAMPLE - activates the 4x MSAA samples requested above via
    //    GLFW_SAMPLES; without glEnable(GL_MULTISAMPLE) the sample buffer is
    //    never resolved, so edges stay aliased.
    //  * texture / mipmap quality hints - prefer the highest-quality decode
    //    for compressed textures and the sharpest generated mipmaps.
    glEnable(GL_FRAMEBUFFER_SRGB);
    glEnable(GL_MULTISAMPLE);
    glHint(GL_TEXTURE_COMPRESSION_HINT, GL_NICEST);
    // GL_GENERATE_MIPMAP_HINT is core since GL 1.4, but the bundled 4.6 glad
    // core generator only emits its _SGIS alias. Define the canonical core
    // alias so this hint compiles regardless of loader.
    #ifndef GL_GENERATE_MIPMAP_HINT
    #define GL_GENERATE_MIPMAP_HINT 0x8192
    #endif
    glHint(GL_GENERATE_MIPMAP_HINT,    GL_NICEST);

    // Sanity check: warn (but don't abort) if the driver couldn't actually
    // hand us a 4.6 context - the logic sim can still boot in that case.
    if (GLVersion.major < 4 || (GLVersion.major == 4 && GLVersion.minor < 6)) {
        std::cout << "[Engine] WARNING: driver reported OpenGL " << GLVersion.major
                  << "." << GLVersion.minor << " (4.6 requested). Falling back to\n"
                  << "         the 4.5-quality path - 4.6 image-quality features"
                  << " may be limited.\n";
    }

    // ---- Editor state (shaders, meshes, grid, ECS world, camera) -----------
    std::cout << "[Engine] Initializing editor..." << std::endl;
    // verify.txt: load the artist-authored engine_config.json BEFORE creating
    // the WorldManager so heightmapPath / terrain_material paths come from the
    // file, not the struct defaults (test.cpp previously skipped loadConfig and
    // silently used the shipped Perlin default heightmap + hardcoded textures).
    Config::loadConfig("engine_config.json");
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

    // Seed live-tracked AssetKind mesh twins so the dashboard physics[] panel,
    // safety geofence, and prediction cone reflect ECS entities (not just the
    // walker). Personnel + Heavy Machinery are placed ~9 m apart (< 15 m safety
    // cylinder) so a proximity alert fires on the dashboard on the first tick.
    // External sim data later updates them by entityId via the ingest path.
    {
        auto& geoApi = g_editor.geospatialSystem().getGeoAPI();
        const geo::GeoConfig cfg = geoApi.getConfig();
        const double lat0 = cfg.originLat, lon0 = cfg.originLon, alt0 = cfg.originAlt;

        // Personnel + Heavy Machinery at ~9 m apart (< 15 m safety cylinder) so a
        // proximity alert fires on the first tick.
        auto personnel = EntityManager::CreateGeospatialTwin(geo::AssetKind::Personnel,
                                                             glm::vec3(0, 0, 0),
                                                             lat0, lon0, alt0);
        // The Personnel geo twin is now an ANIMATED NPC (Shaw.fbx + locomotion
        // clips) — not a T-pose static bot. With no live feed in this headless
        // test we inject a simulated telemetry speed so the FSM transitions
        // IDLE -> WALK, proving a geo-tracked NPC animates from feed velocity
        // (the same geoDriven path GeospatialSystem::update drives for a real
        // GPS/REST feed: position = live lat/lon/ENU fix, moveDir/dir(heading),
        // moveSpeed = speed/maxWalk). A real feed overrides this sim drive.
        if (personnel.isValid()) {
            if (auto* npc = g_editor.world().getComponentArchetype<ecs::AnimatedCharacterComponent>(personnel)) {
                npc->geoDriven = true;
                npc->geoMoveDir = glm::vec2(0.0f, 1.0f);   // forward walk
                npc->geoMoveSpeed = 0.5f;                  // ~1 m/s -> WALK band (< 2.0 cap)
                std::cout << "[Engine] Geo Personnel twin " << std::to_string(personnel.id)
                          << " -> animated NPC (Shaw.fbx), sim WALK drive\n";
            } else {
                std::cout << "[Engine] Geo Personnel twin missing AnimatedCharacterComponent\n";
            }
        }

        // --geo-stress N: spawn N geo Personnel NPCs (animated Shaw + locomotion),
        // each geoDriven to a distinct locomotion band, to prove the geo->NPC
        // pipeline (entity registration, skinned render, per-NPC FSM) tiles under
        // load: idle(0.0) / walk(0.4) / jog(0.9) / run(1.5) / sprint(2.5).
        // geoMoveSpeed>1.0 promotes the FSM to RUN via input.sprint. Each is a
        // geo twin (entity_<id>) on the GeoTracked layer, rendered skinned by
        // renderAnimated() — NOT a T-pose static mesh.
        if (opts.geoStress > 0) {
            static const float stressSpeeds[] = {0.0f, 0.4f, 0.9f, 1.5f, 2.5f};
            std::cout << "[Engine] Geo stress: spawning " << opts.geoStress
                      << " animated Personnel NPCs (idle/walk/jog/run/sprint)\n";
            for (int i = 0; i < opts.geoStress; ++i) {
                const float xoff = 5.0f + i * 3.0f;
                // Place each stress twin 1 index further from the origin so the
                // first stress twin (i==0 -> lon0+0.0002) does not collide with
                // the HeavyMachinery twin (lon0+0.0001), which would otherwise
                // trip a spurious 0.000000m CRITICAL. The intentional machinery
                // demo (Personnel 0 at lon0 ~ 9m from the machinery) is preserved.
                auto npc = EntityManager::CreateGeospatialTwin(geo::AssetKind::Personnel,
                    glm::vec3(xoff, 0.0f, 0.0f),
                    lat0, lon0 + (i + 2) * 0.0001, alt0);
                if (!npc.isValid()) continue;
                if (auto* ac = g_editor.world().getComponentArchetype<ecs::AnimatedCharacterComponent>(npc)) {
                    ac->geoDriven = true;
                    ac->geoMoveDir = glm::vec2(0.0f, 1.0f);
                    ac->geoMoveSpeed = stressSpeeds[i % 5];
                    const char* band = (ac->geoMoveSpeed == 0.0f) ? "idle"
                                      : (ac->geoMoveSpeed > 1.0f) ? "run"
                                      : "walk";
                    std::cout << "[Engine] Stress NPC " << std::to_string(npc.id)
                              << " -> " << band << " (geoSpeed=" << ac->geoMoveSpeed << ")\n";
                }
            }
        }
        auto machinery  = EntityManager::CreateGeospatialTwin(geo::AssetKind::HeavyMachinery,
                                                            glm::vec3(3, 0, 0),
                                                            lat0, lon0 + 0.0001, alt0);

        // Seed each twin's own 3D Kalman with a 2-point trajectory so the dashboard
        // draws a leading prediction cone per twin (Todo 9/10 per-twin cones).
        // Small drift keeps the cone near the spawn dot; positions stay fixed so
        // the proximity geofence still trips on the spawn separation.
        auto& twins = g_editor.geospatialSystem();
        twins.addTwinObservation(personnel.id, lat0,        lon0,             alt0, 1.4, 0.0,  0.0);
        twins.addTwinObservation(personnel.id, lat0+0.00001, lon0,             alt0, 1.4, 0.0,  2.0);
        twins.addTwinObservation(machinery.id, lat0,        lon0 + 0.0001,    alt0, 0.5, 90.0, 0.0);
        twins.addTwinObservation(machinery.id, lat0,        lon0 + 0.00011,   alt0, 0.5, 90.0, 2.0);
    }

    // ---- Live geo web dashboard (http://localhost:8123/) ---------------------
    // Host the GeoHTTPServer against the GeoAPI facade so the modernized
    // DefaultPage viewer polls live telemetry. The server is declared at
    // function scope so it spans the main loop; its destructor stops the
    // accept thread on function exit (after the loop + cleanup).
    geo::GeoHTTPServer geoHttpServer;
    geo::SafetyGeofenceSystem safetyFence;   // mine-safety proximity checks (Todo 5)
    {
        auto& geoApi = g_editor.geospatialSystem().getGeoAPI();
        geoHttpServer.setFeedHandlers(
            [&geoApi](const std::string& url, const std::string& type) {
                return geoApi.addFeed(url, type)
                    ? std::string("{\"ok\":true}")
                    : std::string("{\"ok\":false,\"error\":\"add failed\"}");
            },
            [&geoApi](size_t index) {
                geoApi.removeFeed(index);
                return std::string("{\"ok\":true}");
            });
        if (!geoHttpServer.start(geo::GeoHTTPServer::kDefaultPort,
                                 [&geoApi]() { return geoApi.buildFeedJson(); })) {
            std::cerr << "[Engine] Geo web dashboard: failed to bind port "
                      << geo::GeoHTTPServer::kDefaultPort
                      << " (already in use?)" << std::endl;
        } else {
            std::cout << "[Engine] Geo web dashboard on http://localhost:"
                      << geo::GeoHTTPServer::kDefaultPort << "/" << std::endl;
        }
        // Route proximity alerts to the editor console (warning=1, critical=2)
        // AND surface them on the web dashboard via GeoAPI::recordAlert so they
        // are visible in the browser, not only the editor log.
        safetyFence.setAlertSink([&geoApi](const geo::SafetyGeofenceSystem::ProximityAlert& a) {
            const std::string msg =
                "GEO SAFETY: personnel " + std::to_string(a.entityA) +
                " within " + std::to_string(a.horizontalMeters) + "m of machinery " +
                std::to_string(a.entityB) + (a.level == 2 ? " [CRITICAL]" : " [WARNING]");
            geoApi.recordAlert(geo::LiveAlert{
                a.entityA, a.entityB, a.kindA, a.kindB,
                a.distanceMeters, a.horizontalMeters, a.verticalDeltaM,
                a.timestamp, a.level, msg,
                a.escapeBearingDeg, a.evacLatitude, a.evacLongitude,
                a.evacAltitude, a.evacuationDistance});
            // Evacuation trail (forGeo.txt §2): record the computed escape
            // vector as a time-series point tagged assetKind="evacuation", so
            // the interlock path is persisted (memory ring buffer + InfluxDB)
            // alongside the standard machine tracking profiles and is
            // queryable as a verifiable evacuation-history trail.
            TimeSeriesPoint evac{};
            evac.timestamp = a.timestamp;
            evac.latitude  = a.evacLatitude;
            evac.longitude = a.evacLongitude;
            evac.altitude  = a.evacAltitude;
            evac.entityId  = "personnel_" + std::to_string(a.entityA);
            evac.assetKind = "evacuation";
            geoApi.getTimeSeriesDB().record(evac);
            EditorConsole::Log(msg, a.level);
        });
        safetyFence.setSafetyRadiusMeters(15.0f);
        safetyFence.setPairCooldownSeconds(1.0);
    }

    // Persist / restore geo panel state so GPS mode selection survives
    // between editor sessions.
    LoadGeoPanelState(g_editor.geoPanelState);

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
    // Match ImGui's GLSL shaders to the ACHIEVED GL context, not the 4.6 we
    // request. The engine negotiates down to 4.5 on older drivers / Mesa
    // (llvmpipe), and ImGui's GL3 backend compiles a standalone shader whose
    // GLSL version MUST match the context — "#version 460" only compiles on a
    // 4.6 context, so on a 4.5 context it aborts the engine. Derive it from the
    // actual version: 4.6+ -> 460, otherwise 450 (both ≥4.5 here; the engine's
    // own shaders are #version 430, so this stays consistent).
    const char* imguiGlsl = (GLVersion.major > 4 ||
                             (GLVersion.major == 4 && GLVersion.minor >= 6))
                            ? "#version 460" : "#version 450";
    ImGui_ImplOpenGL3_Init(imguiGlsl);

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
    g_editor.uiState.uiScale = UIConfig::gUIScale;
    // Sync grid config from global UIConfig to editor state (so the World
    // Settings panel + GridRenderer both use the persisted values on startup).
    g_editor.uiState.gridSize = UIConfig::gGridSize;
    g_editor.uiState.gridSpacing = UIConfig::gGridSpacing;
    // Init the GridRenderer with the persisted grid size/spacing.
    GridRenderer::Init(g_editor.uiState.gridSize, g_editor.uiState.gridSpacing);
    EditorTheme::SetThemeMode(UIConfig::gThemeMode == 1 ? EditorTheme::ThemeMode::Light
                                                        : EditorTheme::ThemeMode::Dark);
    EditorTheme::ApplyTheme();

    // Apply the persisted UI scale (default 1.0 = no zoom).
    EditorTheme::SetUIScale(g_editor.uiState.uiScale);

    flyCamera* cam = g_editor.camera();
    if (cam) {
        cam->Position = glm::vec3(0.0f, 10.0f, 20.0f);
        cam->Target = glm::vec3(0.0f, 0.0f, 0.0f);
        // Wire the World Settings Distance / Pitch sliders to the fly camera's
        // resting orbit params (was: left at constructor defaults 8.0f / 20.0f).
        cam->DistanceToTarget = g_editor.uiState.playCameraDistance;
        cam->Pitch = g_editor.uiState.playCameraPitch;
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

    // ---- Terrain mode (default engine start) --------------------------------
    // The terrain with rocks_ground heightmap + POM textures is the default
    // scene. Arena mode is DISABLED so the terrain path runs (heightmap +
    // splat textures + POM) and WorldObjects load from JSON config.
    // (Previously setArenaMode(true) here — a stale copy-paste from the arena
    //  self-check test. The comment always said "disabled"; the code now
    //  matches. This was why "Load Ground" produced nothing: render() early-
    //  returned at the arena check before ever reaching Terrain::render.)
    worldManager.setArenaMode(false);

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
    // Zoom offset from the World Settings Distance slider. The slider sets the
    // resting distance; scroll zoom offsets from it and persists across frames
    // so the two never fight (was: scroll directly mutated cam.config.distance,
    // clobbering the slider value and never recovering it). Reset on play-enter.
    float followCamZoomOffset = 0.0f;
    bool playWasActive = false;
    double demoTime = 0.0;
    int lastPhase = -1;

    // ---- The bot lives in the viewport permanently -----------------------------
    // Load the character + its locomotion clips once at startup so it stands
    // (idle animation) in the editor viewport. Play mode hands the player
    // control; camera modes 1-4 orbit it.
    if (!play.load("assets/Male Locomotion Pack/Shaw.fbx", "assets")) {
        std::cerr << "[Engine] Failed to load character: assets/Male Locomotion Pack/Shaw.fbx (is the working dir the repo root?)\n";
    }
    // Spawn the bot ON the physics surface (terrain + tree/rock tops) instead
    // of floating at y=0 above a valley / tree line: floor-snap to the surface
    // so the first rendered frame already rests on the ground.
    if (play.isLoaded()) {
        AnimatedCharacter& ch = play.character();
        const float surf = worldManager.getSurfaceHeightAt(ch.position.x, ch.position.z);
        ch.position.y = surf;
        ch.grounded = true;
        std::cout << "[Spawn] bot clamped to surface y=" << surf
                  << " at (" << ch.position.x << "," << ch.position.z << ")\n";
    }
    playModel = std::make_unique<Model>("assets/Male Locomotion Pack/Shaw.fbx");
    if (playModel->GetMeshCount() == 0) playModel.reset();

    // Character visibility + model/clips config live on Editor state (hoisted
    // from a runEditorApp-local bool) so the viewport stand-in renders outside
    // Play Mode and the inspector panel can drive it. lastApplied* track what's
    // currently loaded so the panel's path edits reload only when changed.
    std::string lastAppliedModel   = g_editor.uiState.playCharacterModel;
    std::string lastAppliedClipsDir = g_editor.uiState.playClipsDir;

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
    [[maybe_unused]] double cinematicStart = -1.0;
    bool cinematicActive = !opts.headless;
    [[maybe_unused]] constexpr double CINEMATIC_DURATION = 6.0;
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
    const double loopStartTime = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        // Headless: fixed 60 Hz timestep for a deterministic bounded run.
        float now = static_cast<float>(glfwGetTime());
        float dt = opts.headless ? (1.0f / 60.0f) : (now - lastTime);
        lastTime = now;
        // FIX (forAnime.txt Fix 2): Cap dt to a smooth 30 FPS step (0.0333s)
        // during massive frame drops. The existing 0.1f cap was too lenient —
        // exponential formulas (1 - exp(-smooth * dt)) can still generate
        // extreme values and cause the camera / IK foot solver to "leap",
        // triggering the Temporal Desync Loop.
        if (dt > 0.1f) dt = 0.0333f;
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
        // Keep the 3D viewport inert while a panel splitter is hovered / being
        // dragged so the camera can't hijack a click on a splitter strip —
        // notably the right/Details splitter, which sits flush on the viewport's
        // right edge. Without these guards, a click meant for the Details
        // splitter is handed to the camera (wantCaptureMouse=false) instead of
        // the splitter, so the right panel "doesn't drag". Mirrors the gate in
        // EditorApplication::run (editor_main.cpp:1451); test.cpp/bin/engine
        // was the only path missing it.
        const bool splitterDragging = g_editor.uiState.isDraggingLeftSplit
                                    || g_editor.uiState.isDraggingRightSplit
                                    || g_editor.uiState.isDraggingBottomSplit;
        const bool overViewport = (!splitterDragging)
                                && !UI::IsSplitterHovering()
                                && UI::IsViewport3DHovered() && !io.WantTextInput;
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

        // Camera mode keys 0-2 (Free / Third-person / First-person).
        // Mode is shared with the Camera menu + World Settings combo via uiState.
        if (!wantCaptureKeyboard) {
            if (inputState.isKeyPressed(GLFW_KEY_0)) g_editor.uiState.playCameraMode = 0;
            else if (inputState.isKeyPressed(GLFW_KEY_1)) g_editor.uiState.playCameraMode = 1;
            else if (inputState.isKeyPressed(GLFW_KEY_2)) g_editor.uiState.playCameraMode = 2;
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
        // F6 toggles the play character's visibility — temporarily "remove"
        // the bot from the viewport so you can inspect / build the arena
        // without the model getting in the way. The character is still
        // simulated (so physics / camera follow keep working); only the
        // render draw-call is suppressed.
        if (!opts.headless && !wantCaptureKeyboard && inputState.isKeyPressed(GLFW_KEY_F6)) {
            g_editor.setShowCharacter(!g_editor.showCharacter());
            std::cout << "[Engine] Character " << (g_editor.showCharacter() ? "visible" : "hidden") << "\n";
        }
        // F7 toggles the arena stage itself (managed by WorldManager).
        // Enabling arena mode on demand lazily creates + initializes the
        // ArenaScene (WorldManager owns the single instance), so getArena()
        // is always safe to dereference afterwards. Previously this dereferenced
        // a null unique_ptr when arena mode was off (latent crash).
        if (!opts.headless && !wantCaptureKeyboard && inputState.isKeyPressed(GLFW_KEY_F7)) {
            if (!worldManager.isArenaMode()) {
                worldManager.setArenaMode(true);
            }
            if (worldManager.isArenaMode()) {
                bool visible = worldManager.getArena().isVisible();
                worldManager.getArena().setVisible(!visible);
                std::cout << "[Engine] Arena "
                          << (worldManager.getArena().isVisible() ? "visible" : "hidden")
                          << "\n";
            }
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
            if (inputState.isKeyPressed(GLFW_KEY_R) && !opts.headless && !wantCaptureKeyboard) {
                // Hot-reload: re-init shaders + textures from disk.
                g_editor.reloadStatus = "reloading shaders...";
                ShaderManager::CleanupShaders();
                ShaderManager::InitShaders();
                g_editor.reloadStatus = "shaders reloaded";
                EditorConsole::Log("Hot-reload: shaders recompiled");
            }
            if (ctrl && inputState.isKeyPressed(GLFW_KEY_COMMA)) {
                g_editor.uiState.showPreferences = true;
            }
            if (ctrl && inputState.isKeyDown(GLFW_KEY_S)) {
                SceneManager::SaveScene(g_editor.uiState.sceneFile, g_editor.world());
                UIConfig::gUIScale = g_editor.uiState.uiScale;
                UIConfig::SaveConfig(g_editor.uiState.showOutliner, g_editor.uiState.showDetails);
            }
            if (inputState.isKeyPressed(GLFW_KEY_DELETE) ||
                inputState.isKeyPressed(GLFW_KEY_BACKSPACE)) {
                EntityManager::DeleteEntity(g_editor.selectedEntity());
                g_editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
            }
            // UI zoom: Ctrl+= / Ctrl+- / Ctrl+0 (browser-style zoom)
            if (ctrl && inputState.isKeyPressed(GLFW_KEY_EQUAL)) {
                g_editor.uiState.uiScale = std::min(g_editor.uiState.uiScale + 0.1f, 2.0f);
                EditorTheme::SetUIScale(g_editor.uiState.uiScale);
            }
            if (ctrl && inputState.isKeyPressed(GLFW_KEY_MINUS)) {
                g_editor.uiState.uiScale = std::max(g_editor.uiState.uiScale - 0.1f, 0.5f);
                EditorTheme::SetUIScale(g_editor.uiState.uiScale);
            }
            if (ctrl && inputState.isKeyPressed(GLFW_KEY_0)) {
                g_editor.uiState.uiScale = 1.0f;
                EditorTheme::SetUIScale(1.0f);
            }
        }
        const bool playing = g_editor.isPlaying();
        // The bot is always present in the viewport (loaded at startup). Play
        // mode just hands the player control; exiting keeps the bot visible.
        if (playing && !playWasActive) {
            followCamInit = false;
            followCamZoomOffset = 0.0f;  // reset zoom offset from slider on play-enter
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
                    // Free-fly camera (mode 0): WASD translates the eye AND the
                    // target together along the view frame so the orientation is
                    // preserved (W = forward into the scene, S = back, A/D strafe
                    // left/right). Matches the Vulkan viewport's free-fly math.
                    glm::vec3 forward = glm::normalize(cam->Target - cam->Position);
                    glm::vec3 right = glm::normalize(glm::cross(forward, cam->WorldUp));
                    if (inputState.isKeyDown(GLFW_KEY_W)) { cam->Position += forward * moveSpeed; cam->Target += forward * moveSpeed; }
                    if (inputState.isKeyDown(GLFW_KEY_S)) { cam->Position -= forward * moveSpeed; cam->Target -= forward * moveSpeed; }
                    if (inputState.isKeyDown(GLFW_KEY_A)) { cam->Position -= right * moveSpeed; cam->Target -= right * moveSpeed; }
                    if (inputState.isKeyDown(GLFW_KEY_D)) { cam->Position += right * moveSpeed; cam->Target += right * moveSpeed; }
                }
            }
            // Orbit: hold right mouse (or trackpad right-button) and drag.
            // ProcessMouseMovement expects ABSOLUTE cursor positions (it
            // computes the delta internally) - passing mouseDelta would only
            // produce second-order jitter and make the orbit feel dead.
            if (!wantCaptureMouse) {
                if (cam) {
                    // Re-seed the first-mouse position whenever the viewport
                    // takes mouse ownership back from ImGui -- otherwise the
                    // first right-drag after a click on a panel snaps from the
                    // stale (400,300) seed the cam was initialised with ("mouse
                    // hover doesn't work"). overViewport is in scope here (same
                    // ImGui-capture block as wantCaptureMouse above).
                    static bool s_prevFreeHovered = false;
                    if (overViewport && !s_prevFreeHovered) cam->resetFirstMouse();
                    s_prevFreeHovered = overViewport;
                    cam->ProcessMouseMovement(inputState.mousePosition.x, inputState.mousePosition.y);
                }
            }
            // Zoom: scroll wheel / trackpad two-finger scroll.
            if (!wantCaptureMouse && inputState.scrollDelta.y != 0.0f) {
                if (cam) cam->ProcessMouseScroll(inputState.scrollDelta.y);
            }
        }

        // Update the free camera's smooth focus glide (forCam.txt Step B).
        // Called AFTER input processing so the glide isn't disrupted, BEFORE
        // the view matrix is fetched for rendering below.
        if (cam && camMode == 0) {
            cam->UpdateEngineCamera(dt);
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

            // Surface height INCLUDING static world-object colliders
            // (boulder/rock/trunk tops), so the character stands ON solid
            // objects the same way it stands on terrain.
            auto terrain = [&worldManager](float x, float z) -> float {
                return worldManager.getSurfaceHeightAt(x, z);
            };
            // Handle rewind (toolbar button) — reset the animation clip time
            // to 0 so the current clip replays from the start.
            if (g_editor.wantRewind()) {
                g_editor.setWantRewind(false);
                if (play.isLoaded()) {
                    play.character().rewind();
                }
            }

            // Apply playback speed from the slider (0.25x–4x). Scales the
            // simulation dt so the character plays faster/slower.
            const float playbackSpeed = play.isLoaded()
                ? play.character().playbackSpeed : 1.0f;
            play.update(dt * playbackSpeed, in, terrain);

            // Solid world objects (boulders/rocks/trees) carry static physics
            // colliders: resolve the character's capsule against them so it
            // cannot walk through - the feet get pushed out of any overlap and
            // the velocity component heading into the surface is killed
            // (slides along / stops at the rock). Mirrors the editor's
            // updatePlayMode path.
            if (worldManager.getPhysicsWorld()) {
                AnimatedCharacter& ccM = play.character();
                glm::vec3 feet = ccM.position;
                glm::vec3 vel = ccM.velocity;
                if (worldManager.resolveCharacterCollision(feet, 0.12f, 0.54f, vel, ccM.grounded)) {
                    ccM.position = feet;
                    ccM.velocity = vel;
                }
            }

            const AnimatedCharacter& cc = play.character();

            // Unreal-style ground clamp: the follow camera never sinks below
            // the terrain (no more "under the floor" view).
            followCam.groundHeightFn = terrain;

            // Camera modes 1-2 (Third-person / First-person) follow the bot;
            // mode 0 (Free) uses the fly camera above. camMode is already
            // clamped + driven by keys 0-2 / the Camera menu / World Settings.
            if (camMode >= 1) {
                // Camera follows the SELECTED entity (NPC/prop) when one is
                // selected, else the live bot — decoupling the follow cam from
                // the character object (selection-driven focus).
                EntityFocus charFocus;
                charFocus.position = cc.position;
                charFocus.forward = glm::vec3(-std::sin(cc.heading), 0.0f, -std::cos(cc.heading));
                charFocus.animState = AnimStateToCamState(cc.state());
                charFocus.moveMagnitude = cc.currentSpeed();
                charFocus.velocity = cc.velocity;
                charFocus.animated = true;
                charFocus.valid = true;
                EntityFocus focus = ::Editor::resolveCameraFocus(
                    g_editor.world(),
                    EntityManager::ValidateOrClear(g_editor.selectedEntity()),
                    ecs::INVALID_ENTITY_ID,   // engine has no play-character twin entity
                    charFocus);
                // 3 cameras: Free(0) uses the fly cam above; Third-person(1) maps
                // to UpdatePlayCamera Follow (mode 0); First-person(2) maps to the
                // First-person branch (mode 3). Orbit/Top-down UC modes unused.
                const int ucMode = (camMode == 2) ? 3 : 0;
                UpdatePlayCamera(ucMode, followCam, focus, dt, aspect, inputState,
                                 followCamInit, lastCamMode, fpPitch, followCamZoomOffset);
                lastCamMode = ucMode;
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

        // Geo panel toggles: push into GeoVisualizationSystem BEFORE the ECS tick
        // so this frame's point marker / trajectory / prediction cone / Monte
        // Carlo / uncertainty rendering honors the checkboxes. With Prediction +
        // Monte Carlo + Uncertainty unchecked, only the raw-telemetry twin
        // meshes remain -> clear A↔B proximity, no Kalman/ML drift on the eye.
        {
            const auto& gs = g_editor.geoPanelState;
            g_editor.geospatialSystem().getVisualizationSystem().setToggles(
                gs.showPoints, gs.showTrajectory, gs.showPrediction,
                gs.showUncertainty, gs.showMonteCarlo);
        }

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

            // Drive the 3D Kalman live: feed the fix (with altitude) into the
            // model + refresh cached predictions on the configured interval so
            // the dashboard's prediction cone + next-fix card update each frame.
            geoApi.updatePredictions();

            // Todo 5: mine-safety proximity geofence - emit alerts when a
            // Personnel twin enters the 15m concentric safety cylinder around
            // HeavyMachinery. Runs every frame; per-pair cooldown prevents
            // editor-console flooding.
            safetyFence.checkMineSafetyConcentric(geoApi.getSnapshots(),
                                                   geoApi.getGPSStatus().timestamp);
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
        //
        // verify.txt Fix 3: Sync the viewport FBO size to the WINDOW size every
        // frame. Previously test.cpp/bin-engine never called applyPending-
        // ViewportResize(), so the FBO was stuck at the 1280x720 constructor
        // default while the window was 1920x1080. The deferred G-Buffer /
        // PostProcess / SSAO textures were allocated at 1280x720, so the
        // entire scene (terrain + ECS) rendered into a tiny 1280x720 region
        // in the corner of the 1920x1080 framebuffer — the "screen within a
        // screen" symptom. Growing anything past the FBO edge was clipped.
        renderPipeline.setPendingViewportSize(windowW, windowH);
        renderPipeline.applyPendingViewportResize();
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

        // ---- Wireframe toggle (was: missing entirely in test.cpp) ----
        // The renderMode combo (Lit/Wire/Normals/Unlit) and the viewport
        // toolbar Wireframe button both drive EditorState::showWireframe().
        // Previously test.cpp/bin/engine read renderMode() only for the flat
        // (Normals=2 / Unlit=3) shaders but NEVER applied glPolygonMode for
        // Wire mode (renderMode==1), so flipping "Wireframe" had zero visual
        // effect in the engine viewport. Wire it here: GL_LINE for Wire mode,
        // GL_FILL for everything else. Restore GL_FILL after the scene draw so
        // the ImGui HUD (drawn after this block) is never wireframed.
        const bool wf = g_editor.showWireframe();
        renderPipeline.setWireframeMode(wf);
        if (wf) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        else    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        if (std::getenv("RTT_RHI_SCENE")) {
            // Phase B Step A: render the editor scene through the RHI instead of
            // the GL RenderPipeline. The bridge builds an OffscreenScene (the
            // forward + skinned + deferred subset with GL<->Vulkan pixel parity);
            // the backends NO-OP the still-to-be-ported passes (CSM/SSAO/SSR/
            // skybox/bloom) until Phase B, so this is forward-safe: with the env
            // var unset, the GL RenderPipeline path below is unchanged.
            static std::unique_ptr<RHI::IRHI> s_rhi;
            static bool s_rhiTried = false;
            if (!s_rhiTried) {
                s_rhiTried = true;
                RHI::GraphicsAPI api = RHI::activeGraphicsApi();
                if (std::getenv("RTT_VULKAN")) api = RHI::GraphicsAPI::Vulkan;
                s_rhi = RHI::createRHI(api);
                if (s_rhi) {
                    const auto& rc = Config::getRenderConfig();
                    if (!s_rhi->available() ||
                        !s_rhi->initialize(rc.defaultWindowWidth, rc.defaultWindowHeight,
                                           "RTT Engine - RHI")) {
                        s_rhi.reset();   // RHI unavailable (headless) -> GL fallback
                    }
                }
            }
            if (s_rhi && s_rhi->beginFrame()) {
                // Phase B terrain port: wire WorldManager::getTerrain() into the
                // offscreen bridge so world/Terrain chunks become OffscreenMesh
                // meshes rendered by BOTH backends -> terrain visible on
                // rhivulkan (parity with the GL offscreen path).
                renderPipeline.setTerrain(worldManager.getTerrain());
                renderPipeline.setVegetation(worldManager.getVegetation());
                RHI::OffscreenScene scene = renderPipeline.toOffscreenScene(
                    view, projection, cameraPosition, kCamFov);
                s_rhi->renderFrameScene(scene);
                s_rhi->endFrame();
            } else if (!s_rhi) {
                // Headless: no RHI (GLFW/Vulkan unavailable). Drop to the GL
                // editor path so the engine still renders instead of going dark.
                renderPipeline.beginFrame();
                renderPipeline.renderScene(view, projection, cameraPosition, kCamFov);
            }
            // Terrain / play-char / HUD overlays are Phase B — intentionally
            // skipped under RTT_RHI_SCENE until ported to the RHI.
        } else {
            renderPipeline.beginFrame();
            renderPipeline.renderScene(view, projection, cameraPosition, kCamFov);
            // WorldManager handles arena rendering in arena mode (and terrain in
            // regular mode). The arena renders on top of the skybox.
            // renderScene()'s deferred path unbinds the viewport FBO to 0 at its end
            // (glBindFramebuffer(GL_FRAMEBUFFER,0)). Rebind the viewport FBO here so
            // the terrain (renderSplat, which does NOT push its own FBO) + the forward
            // ECS pass below render INTO the viewport texture the editor/display reads
            // (m_framebufferTexture) instead of leaking into the default window FBO.
            // This is the "Fix 3" bind-before-draw contract, applied per-section.
            glBindFramebuffer(GL_FRAMEBUFFER, renderPipeline.getFramebuffer());
            glViewport(0, 0, renderPipeline.getViewportWidth(), renderPipeline.getViewportHeight());
            worldManager.render(view, projection, cameraPosition);
        }

        // Lazy-apply any play-character model/clips change requested from the
        // Character inspector (general: any model path + clips dir, not geo-only).
        // Reloads the skinned character + render model; safe and only fires
        // when the path differs from what's loaded.
        if (play.isLoaded()) {
            const std::string wantModel = g_editor.uiState.playCharacterModel;
            const std::string wantClips = g_editor.uiState.playClipsDir;
            if (wantModel != lastAppliedModel || wantClips != lastAppliedClipsDir) {
                lastAppliedModel   = wantModel;
                lastAppliedClipsDir = wantClips;
                play.load(wantModel, wantClips);               // reload skeleton + clips
                playModel = std::make_unique<Model>(wantModel);
                if (!playModel || playModel->GetMeshCount() == 0) playModel.reset();
                if (play.isLoaded()) {
                    AnimatedCharacter& ch = play.character();
                    const float surf = worldManager.getSurfaceHeightAt(ch.position.x, ch.position.z);
                    ch.position.y = surf; ch.grounded = true;
                }
            }
        }
        // The bot is always in the viewport (idle outside play mode). F6 / the
        // Character panel toggles showCharacter (EditorState) to render it
        // while building the arena — not only in Play Mode.
        // A3: render the play character via the ECS skinned path — the SAME Vulkan
        // skinning pipeline NPC twins use (editor_application.cpp:1284 +
        // ModelRenderSystem::renderAnimated) — instead of the standalone GL
        // RenderPlayCharacter draw. The entity borrows PlayModeController's
        // AnimatedCharacter (externallyTicked so the NPC wander/geo FSM never
        // drives the player) + the live play Model; renderAnimated() draws it via
        // ch->modelMatrix() + Model::Draw(shader, animator), identical to
        // RenderPlayCharacter. Flag-gated (active = showCharacter) + forward-safe:
        // absent the ECS world, createEntityWithComponents returns INVALID.
        static ecs::Entity playCharEntity = ecs::Entity{ecs::INVALID_ENTITY_ID};
        if (!playCharEntity.isValid() && play.isLoaded() && playModel && playModel->GetMeshCount() > 0) {
            playCharEntity = g_editor.world().createEntityWithComponents<
                ecs::TransformComponent, ecs::AnimatedCharacterComponent,
                ecs::NameComponent>();
        }
        if (playCharEntity.isValid()) {
            if (auto* ac = g_editor.world().getComponentArchetype<ecs::AnimatedCharacterComponent>(playCharEntity)) {
                // Re-borrow every frame so a hot-reload (new playModel / reloaded
                // skeleton) is reflected without recreating the entity.
                ac->externalCharacter = play.isLoaded() ? &play.character() : nullptr;
                ac->externalModel     = playModel.get();
                ac->externallyTicked  = true;
                ac->active            = g_editor.showCharacter();   // F6 / Character panel toggle
            }
            // Sync transform for camera-focus / gizmo targeting; renderAnimated()
            // uses ch->modelMatrix() (character-owned), NOT this transform, for
            // the actual draw — so this is best-effort focus data only.
            if (auto* t = g_editor.world().getComponentArchetype<ecs::TransformComponent>(playCharEntity)) {
                if (play.isLoaded()) {
                    const AnimatedCharacter& ch = play.character();
                    t->position = ch.position;
                    t->setEulerAngles(glm::vec3(0.0f, ch.heading, 0.0f));
                }
            }
        }

        // ----- Grid + selection gizmo -----
        // Drawn INTO the viewport FBO (still bound here) so they appear on the
        // FBO texture that ImGui::Image displays via RenderViewport. Rendering
        // them later (in the UI pass, after the FBO is unbound) puts them on
        // the default framebuffer, which ImGui's viewport image then draws on
        // top of — hiding both.
        GridRenderer::Draw(view, projection, ShaderManager::GetMainShaderProgram());

        if (g_editor.showGizmo() && g_editor.gizmoType() != GizmoRenderer::GizmoType::None) {
            ecs::EntityID selected = EntityManager::ValidateOrClear(g_editor.selectedEntity());
            if (selected != ecs::INVALID_ENTITY_ID) {
                ecs::World& w = g_editor.world();
                ecs::TransformComponent* t =
                    w.getComponentArchetype<ecs::TransformComponent>(selected);
                if (t) {
                    glm::vec3 pos = t->position;
                    glm::quat rot = (g_editor.spaceType() == GizmoRenderer::SpaceType::World)
                                       ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                                       : glm::quat(t->rotation);
                    GizmoRenderer::Draw(pos, 1.0f, rot, view, projection,
                                        ShaderManager::GetGizmoShaderProgram(),
                                        g_editor.gizmoType());
                }
            }
        }

        // ----- Forward pass: render ECS mesh/model entities -----
        // In renderSceneOnly() this runs for the editor_app path via
        // EditorApplication; here (test.cpp / bin/engine) the main loop is
        // self-contained, so the ECS forward pass that draws everything placed
        // via the spawner / Content Browser / Add menu must be called
        // explicitly HERE — before the FBO is unbound. Without it, only the
        // deferred terrain/skybox/vegetation render; all spawned primitives,
        // models, and characters are invisible.
        {
            // Push the Layers-panel visibility into both forward renderers so
            // the per-layer cull reflects the UI immediately.
            bool lv[8];
            for (int i = 0; i < 8; ++i)
                lv[i] = g_editor.uiState.layerVisible[i];
            g_editor.renderSystem().setLayerVisibility(lv);
            g_editor.modelRenderSystem().setLayerVisibility(lv);

            const int kViewportMode = g_editor.renderMode();
            const bool kFlatMode = (kViewportMode == 2 || kViewportMode == 3);
            if (kFlatMode) {
                GLuint prog = (kViewportMode == 3)
                    ? ShaderManager::GetUnlitShaderProgram()
                    : ShaderManager::GetNormalsShaderProgram();
                g_editor.renderSystem().setDefaultShaderProgram(prog ? prog : ShaderManager::GetMainShaderProgram());
                g_editor.modelRenderSystem().setDefaultShaderProgram(prog ? prog : ShaderManager::GetMainShaderProgram());
            }

            g_editor.renderSystem().render();
            g_editor.modelRenderSystem().setSkinShader(renderPipeline.getModelShader());
            g_editor.modelRenderSystem().setCameraPosition(cameraPosition);
            g_editor.modelRenderSystem().setViewProjection(view, projection);
            g_editor.modelRenderSystem().update(ImGui::GetIO().DeltaTime);
            g_editor.modelRenderSystem().render();

            Renderer& fwd = g_editor.renderer();
            fwd.SetCameraMatrices(view, projection);
            fwd.SetLights(nullptr, 0, cameraPosition);
            fwd.SetFrustumCulling(worldManager.isCullingEnabled());
            // verify.txt Fix: Pin the forward renderer viewport to the live panel
            // size. Without this, fwd.Render() falls back to its 800x600 default
            // viewport (see editor_application.cpp:973) — Content Browser models
            // render in a tiny 800x600 sub-rect of the FBO and get clipped at the
            // "small screen" boundaries ("screen within a screen" symptom).
            fwd.SetViewport(0, 0,
                            renderPipeline.getViewportWidth(),
                            renderPipeline.getViewportHeight());
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            fwd.SubmitBatches();
            fwd.Render();

            if (kFlatMode) {
                GLuint prog = ShaderManager::GetMainShaderProgram();
                g_editor.renderSystem().setDefaultShaderProgram(prog);
                g_editor.modelRenderSystem().setDefaultShaderProgram(prog);
            }

            g_editor.modelRenderSystem().renderAnimated();
            fwd.ClearBatches();
        }

        // Restore polygon mode + depth mask after the 3D scene so the ImGui
        // HUD (drawn on the default framebuffer below) is never wireframed
        // and depth writes are sane for the forward UI pass.
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDepthMask(GL_TRUE);

        // ---- ImGui editor UI on top of the viewport texture ----
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
        // Restart request (toolbar button) — shut the loop down so main() can
        // re-exec the process. Mirrors the shouldClose() handshake above.
        if (g_editor.wantRestart()) glfwSetWindowShouldClose(window, true);
        UI::RenderSceneDialogs(g_editor);
        UI::RenderToolbar(g_editor);
        UI::RenderLeftPanel(g_editor);
        // The play-mode debug UI lives in the Details panel (not as a
        // floating overlay on the viewport) so the 3D view stays clear and
        // the character remains visible while playing.
        UI::RenderRightPanel(g_editor, &play, kPlayCameraModeNames[camMode]);
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
        UI::RenderStatusBar(g_editor, world.getEntityCount(), g_editor.selectedEntity(), fps,
                            g_editor.isPlaying(), g_editor.wasPlaying(), windowW, windowH,
                            activeCamPos, activeCamTarget,
                            play.isLoaded() ? kPlayCameraModeNames[camMode] : nullptr);
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
    // Average FPS over the MAIN LOOP only (excluding startup: the -O0 debug
    // build spends tens of seconds loading clips / building databases before
    // the loop, which used to drag this number down to ~5 fps and hide the
    // real frame cost).
    const double loopElapsed = lastTime - loopStartTime;
    std::cout << "  average fps     : " << (frameCount > 0 ? (float)frameCount / std::max(0.001f, (float)loopElapsed) : 0.0f) << "\n";

    // ---- Cleanup --------------------------------------------------------------------
    // Tear down play-mode GPU resources while the GL context is still alive
    // (Model's destructor issues GL calls via BoneMatrixBuffer::Shutdown).
    play.shutdown();
    playModel.reset();

    // Persist geo panel state + UI config so the next session restores the
    // last GPS mode, panel toggles, grid settings, etc.
    SaveGeoPanelState(g_editor.geoPanelState);
    UIConfig::gUIScale = g_editor.uiState.uiScale;
    UIConfig::SaveConfig(g_editor.uiState.showOutliner, g_editor.uiState.showDetails);

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
              << "    tests: 712 | systems: ECS, physics, animation, motion\n"
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

    const int appExitCode = runEditorApp(opts);

    // Engine restart: a clean process re-exec. runEditorApp has already torn
    // down ImGui/GLFW + all subsystems (shutdown complete), so execvp replaces
    // this image with a fresh launch using the original command line. A faithful
    // argv restart preserves the user's launch flags (e.g. --skip-tests for a
    // fast app restart); if execvp fails we fall back to the app's exit code.
    if (!opts.headless && g_editor.wantRestart()) {
        std::cout << "\n[Engine] Restart requested — re-executing process.\n";
        execvp(argv[0], argv);
        // execvp only returns on failure (e.g. argv[0] not executable on PATH).
        std::perror("[Engine] Restart execvp failed");
        return (appExitCode != 0) ? appExitCode : 1;
    }

    return appExitCode;
}
