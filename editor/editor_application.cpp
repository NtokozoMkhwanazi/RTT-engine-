#include <glad/glad.h>
#include "editor_application.h"
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>

#include <imgui.h>
#include "editor/ui.h"
#include "console.h"
#include "profiler.h"
#include "lighting/CVar.h"
#include "renderer/GPUProfiler.h"
#include "modelSystem/Model.h"
#include "shaderSystem/Shader.h"

namespace Editor {

EditorApplication& EditorApplication::getInstance() {
    static EditorApplication instance;
    return instance;
}

EditorApplication::EditorApplication()
    : m_renderPipeline(Render::RenderPipeline::getInstance())
    , m_inputManager(Input::InputManager::getInstance())
    , m_worldManager(World::WorldManager::getInstance())
    , m_resourceManager(Resources::ResourceManager::getInstance()) {
}

EditorApplication::~EditorApplication() {
    shutdown();
}

bool EditorApplication::initialize(int windowWidth, int windowHeight) {
    m_windowWidth = windowWidth;
    m_windowHeight = windowHeight;
    m_state = AppState::Initializing;

    // Redirect all std::cout / std::cerr output into the EditorConsole so
    // existing log messages appear in the Output Log panel (previously the
    // panel was always empty because nobody called EditorConsole::Log()).
    // Installed BEFORE any other log output so startup messages are captured.
    EditorConsole::InstallStdoutRedirect();

    std::cout << "[EditorApplication] Initializing..." << std::endl;

    // Window creation should be done in main() for now as per test_refactored.cpp
    m_window = glfwGetCurrentContext();
    if (!m_window) {
        std::cerr << "[EditorApplication] Error: No GLFW context found!" << std::endl;
        return false;
    }

    if (!m_renderPipeline.initialize()) return false;

    // --- Profiling CVars (artists tune live via config/cvars.ini) ---
    // profiling.enabled   : 0=off, 1=on (toggles CPU scope timers + console flush)
    // profiling.logInterval: seconds between periodic console stat dumps
    // profiling.consoleVerbose: 0=silent, 1=periodic flush, 2=verbose (every frame)
    CVar::Instance().registerFloat("profiling.enabled", 1.0f);
    CVar::Instance().registerFloat("profiling.logInterval", 5.0f);
    CVar::Instance().registerFloat("profiling.consoleVerbose", 1.0f);
    CVar::Instance().registerFloat("profiling.categoryMemory", 1.0f);
    CVar::Instance().registerFloat("profiling.categoryKDTree", 1.0f);
    CVar::Instance().registerFloat("profiling.categorySIMD", 1.0f);

    // Register a GPU profiler provider so GPU timing data also flows into
    // the Profiler snapshot + console flush.
    Profiler::Instance().registerProvider([](Profiler& p) {
        const auto& gpu = GPUProfiler::getInstance();
        p.setStat("GPU.FrameTimeMs", gpu.getFrameTimeMs(), "ms");
        p.setStat("GPU.AvgFrameTimeMs", gpu.getAverageFrameTimeMs(), "ms");
        p.setStat("GPU.FPS", static_cast<double>(gpu.getFPS()), "fps");
        for (const auto& r : gpu.getAllResults()) {
            p.setStat("GPU." + r.name + ".AvgMs", r.avgGpuTimeMs, "ms");
            p.setStat("GPU." + r.name + ".MinMs",  r.minGpuTimeMs, "ms");
            p.setStat("GPU." + r.name + ".MaxMs",  r.maxGpuTimeMs, "ms");
        }
    });

    // Register world-manager render as a callback inside renderScene()'s
    // geometry pass. This ensures terrain/vegetation render to the correct
    // FBO (deferred G-Buffer or forward HDR target) BEFORE post-processing,
    // instead of being drawn as a raw overlay on top of the composited image.
    m_renderPipeline.setSceneRenderCallback(
        [this](const glm::mat4& view, const glm::mat4& proj,
               const glm::vec3& camPos, const LightingEnvironment& le) {
            m_worldManager.render(view, proj, camPos,
                                  m_renderPipeline.getShadowMapper(), le);

            // Render NPC targets as colored spheres so they're visible
            // in the viewport during combat testing. Gated on m_showNPCSpheres
            // (default false) so they don't render in-engine as stray spheres.
            if (m_showNPCSpheres) {
                renderNPCs(view, proj, camPos);
            }
        });
    
    m_inputManager.initialize(m_window);
    
    // Arena mode — the arena is the default engine start scene (skybox + cube
    // walls + ground), replacing terrain/water/trees as the priority viewport.
    // Clear separation: WorldManager owns the arena and renders it when active.
    m_worldManager.setArenaMode(true);
    if (!m_worldManager.initialize(Config::getTerrainConfig(), Config::getVegetationConfig())) {
        std::cerr << "[EditorApplication] WorldManager initialization had issues" << std::endl;
    }

    // The follow camera must always stay BEHIND the bot: auto-orient it to
    // the bot's heading so it orbits around to the back the moment the bot
    // turns, instead of ending up in front of it ("the bot passes the
    // camera" visual glitch).
    m_playCamera.config.orientToCharacterForward = true;

    // Eye height scales with the bot: a 1.26 m-tall bot (70% visual scale)
    // needs the camera near its shoulders, not 1.6 m above its feet.
    // (Tracks AnimatedCharacter::visualScale — update both together.)
    m_playCamera.config.height = 1.6f * 0.7f;       // ~1.12 m over the feet
    m_playCamera.config.pivotHeight = 1.3f * 0.7f;  // ~0.91 m

    // The bot lives in the viewport permanently (idle animation outside play
    // mode), mirroring bin/engine. Play mode just hands the player control.
    if (!m_playController.load(m_playModelPath, m_playClipsDir)) {
        std::cerr << "[EditorApplication] Failed to load character '"
                  << m_playModelPath << "' at startup" << std::endl;
    } else if (m_playController.isLoaded()) {
        // Spawn the bot ON the terrain surface right away (physics floor snap)
        // so the idle viewport bot never hovers above a valley / tree line.
        AnimatedCharacter& ch = m_playController.character();
        const float surf = m_worldManager.getSurfaceHeightAt(ch.position.x, ch.position.z);
        ch.position.y = surf;
        ch.grounded = true;
        std::cout << "[Spawn] bot clamped to surface y=" << surf
                  << " at (" << ch.position.x << "," << ch.position.z << ")\n";
    }
    m_playRenderModel = std::make_unique<Model>(m_playModelPath);
    if (m_playRenderModel->GetMeshCount() == 0) {
        std::cerr << "[EditorApplication] Startup character '"
                  << m_playModelPath << "' has no meshes" << std::endl;
        m_playRenderModel.reset();
    }

    m_state = AppState::Running;
    std::cout << "[EditorApplication] Initialized successfully" << std::endl;
    return true;
}

int EditorApplication::run() {
    m_lastTime = glfwGetTime();
    
    while (m_state != AppState::ShuttingDown && !glfwWindowShouldClose(m_window)) {
        float currentTime = glfwGetTime();
        float dt = currentTime - m_lastTime;
        m_lastTime = currentTime;

        processInput(dt);
        update(dt);
        render(dt);

        glfwPollEvents();
    }
    return 0;
}

void EditorApplication::shutdown() {
    if (m_state == AppState::ShuttingDown) return;

    // Restore original stdout/stderr before tearing down subsystems so the
    // console streambufs don't dangle during static-destruction.
    EditorConsole::UninstallStdoutRedirect();

    m_state = AppState::ShuttingDown;
    m_playController.shutdown();
    m_playRenderModel.reset();
    m_worldManager.shutdown();
    m_inputManager.shutdown();
    m_renderPipeline.shutdown();

    // Drop the model/shader/texture cache while the GL context is still
    // alive. ResourceManager is a Meyers singleton - if its cache survives to
    // static-destruction time it is destroyed AFTER main() has torn the
    // context down, and Mesh/Model destructors issue GL calls (a SIGSEGV at
    // exit, see crash.log). Clearing here makes the GL teardown order
    // deterministic.
    m_resourceManager.clearCache();
}

void EditorApplication::processInput(float dt) {
    m_inputManager.update();

    // With the editor UI panels visible, ImGui captures the keyboard/mouse
    // over any window - including the viewport. Release that capture while
    // the cursor is over the viewport (and always for the keyboard during
    // play mode) so WASD + right-drag + scroll keep driving the character /
    // follow camera, exactly like the engine's viewport input routing. Text
    // fields and active widgets keep ImGui's capture.
    if (ImGui::GetCurrentContext()) {
        ImGuiIO& io = ImGui::GetIO();
        const bool overViewport = UI::IsViewport3DHovered() && !io.WantTextInput;
        bool captureKb = io.WantCaptureKeyboard;
        bool captureMouse = io.WantCaptureMouse;
        if (overViewport) captureMouse = false;
        if ((overViewport || m_isPlaying) && !ImGui::IsAnyItemActive() && !io.WantTextInput) {
            captureKb = false;
        }
        m_inputManager.setImGuiCapture(captureKb, captureMouse);
    }
}

glm::vec3 EditorApplication::worldUpdateCameraPos(bool playLoaded,
                                                   const ThirdPersonCamera& playCam) {
    // The camera is updated later in this frame (after character physics, per
    // the fixed pipeline order), so use its position from the previous frame -
    // one frame of lag is imperceptible for streaming/LOD.
    return playLoaded ? playCam.position : glm::vec3(0.0f);
}

void EditorApplication::update(float dt) {
    // The terrain streams chunks and computes LOD relative to this position,
    // so it must follow the actual camera, not the world origin.
    m_worldManager.update(worldUpdateCameraPos(m_playController.isLoaded(), m_playCamera), dt);

    // Drive the play-mode character. The bot is always loaded (idle outside
    // play mode); in play mode it reads the editor keyboard state.
    if (m_playController.isLoaded()) {
        CharacterInput input;
        if (m_isPlaying) {
            const auto& ks = m_inputManager.getInputState();
            glm::vec2 move(0.0f);
            // moveDirection is WORLD space (x->X, y->Z); the character turns
            // to face it and the follow camera orbits behind. The bot's local
            // forward is -Z, so W must push it AWAY from the camera (-Z), S
            // toward it (+Z), A strafe left (-X), D right (+X). Previously
            // W/S were inverted: pressing W walked the bot BACKWARD (toward
            // the camera) until it spun around to face +Z.
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
            // Motion-matching context switching (contextual databases):
            // 1 Locomotion, 2 Crouch, 3 Combat, 4 Capoeira, 5 Dance.
            // Edge-triggered; crouch (C) also auto-switches Locomotion<->Crouch.
            if (ks.isKeyPressed(GLFW_KEY_1)) input.motionContext = (int)AnimatedCharacter::MotionContext::LOCOMOTION;
            else if (ks.isKeyPressed(GLFW_KEY_2)) input.motionContext = (int)AnimatedCharacter::MotionContext::CROUCH;
            else if (ks.isKeyPressed(GLFW_KEY_3)) input.motionContext = (int)AnimatedCharacter::MotionContext::COMBAT;
            else if (ks.isKeyPressed(GLFW_KEY_4)) input.motionContext = (int)AnimatedCharacter::MotionContext::CAPOEIRA;
            else if (ks.isKeyPressed(GLFW_KEY_5)) input.motionContext = (int)AnimatedCharacter::MotionContext::DANCE;
            // Combat attack trigger (left mouse or J key)
            input.attack = ks.isKeyDown(GLFW_MOUSE_BUTTON_LEFT) || ks.isKeyDown(GLFW_KEY_J);
        }
        updatePlayMode(dt, input);
    }
}

void EditorApplication::updatePlayMode(float dt, const CharacterInput& input) {
    if (!m_playController.isLoaded()) return;

    // Surface height function - the character snaps to the world floor. Uses
    // getSurfaceHeightAt (max of terrain AND boulder/rock/trunk tops) so the
    // character can also stand ON solid world objects, not just walk into
    // them.
    auto terrain = [this](float x, float z) -> float {
        return m_worldManager.getSurfaceHeightAt(x, z);
    };
    // ---- Physics collision resolution (BEFORE animation) ----
    // FIX (v5 todo Area 3): Resolve solid-object collisions (capsule vs.
    // boulders/rocks/trees) BEFORE running the animation update. Previously
    // physics ran AFTER animation, which meant the foot IK targets (world-space
    // planted positions in FootPlantingSystem) were locked to the PRE-physics
    // capsule position.  Physics then overwrote cc.position / cc.velocity,
    // shifting the root while the feet stayed pinned — the analytical IK
    // solver in SolveLegIK stretched the leg chain to bridge the gap,
    // causing the structural leg extension / hyper-extension (foot stretching
    // + sliding).  Running physics first means the character's position and
    // velocity are already corrected when the trajectory prediction, skeleton
    // evaluation, and foot IK all run inside AnimatedCharacter::update, so the
    // IK targets are consistent with the physics-corrected root.
    //
    // Note: the animation update's velocity computation (input-driven acceleration)
    // runs after this and may partially overwrite the physics-corrected velocity,
    // but the position correction — which is the primary source of the stretch —
    // is fully retained.
    if (m_worldManager.getPhysicsWorld()) {
        AnimatedCharacter& cc = m_playController.character();
        glm::vec3 feet = cc.position;
        glm::vec3 vel = cc.velocity;
        const float radius = 0.4f * cc.visualScale;     // capsule radius (m) - 70% of 0.4, tracks render scale
        const float totalHeight = 1.8f * cc.visualScale; // capsule height (m) - 70% of 1.8, matches the rendered bot
        m_worldManager.resolveCharacterCollision(feet, radius, totalHeight, vel, cc.grounded);
        // Apply the physics-corrected position unconditionally so the animation
        // update starts from the resolved capsule placement.
        cc.position = feet;
        cc.velocity = vel;
    }

    // ---- Animation update (AFTER physics correction) ----
    m_playController.update(dt, input, terrain);

    // ---- diagnostic: confirm the character's base sits ON the terrain surface ----
    {
        static int botDbg = 0;
        if (++botDbg % 60 == 0) {
            const AnimatedCharacter& dbg = m_playController.character();
            const float th = terrain(dbg.position.x, dbg.position.z);
            std::cout << "[Bot] pos.y=" << dbg.position.y
                      << " terrain=" << th
                      << " diff=" << (dbg.position.y - th)
                      << " grounded=" << (dbg.grounded ? "YES" : "NO") << "\n";
        }
    }

    // Drive the third-person follow camera from the character's state.
    const AnimatedCharacter& cc = m_playController.character();
    CameraInput camIn;
    camIn.characterPosition = cc.position;
    camIn.characterVelocity = cc.velocity;
    camIn.moveMagnitude = cc.currentSpeed();
    camIn.isGrounded = cc.grounded;
    camIn.characterForward = glm::vec3(-std::sin(cc.heading), 0.0f, -std::cos(cc.heading));
    camIn.animState = CameraStateForAnimationState(cc.state());
    const float aspect = (float)m_windowWidth / (float)std::max(1, m_windowHeight);

    // Manual look while the yaw auto-orients behind the bot: right-drag tilts
    // the camera up/down, scroll zooms the follow distance (mirrors the
    // engine's Follow-mode controls).
    const auto& in = m_inputManager.getInputState();
    if (in.isMouseButtonDown(Input::MouseButton::RIGHT)) {
        m_playCamera.pitch = glm::clamp(m_playCamera.pitch - in.mouseDelta.y, -20.0f, 55.0f);
    } else {
        m_playCamera.pitch = glm::mix(m_playCamera.pitch, 10.0f,
                                      std::min(1.0f, 1.0f - std::exp(-3.0f * dt)));
    }
    if (in.scrollDelta.y != 0.0f) {
        // Faster scroll zoom (3.0f per tick) with a wider range: zoom well
        // up close to the bot (down to 0.5 m) or pull back out to 9.0 m.
        m_playCamera.config.distance = glm::clamp(
            m_playCamera.config.distance - in.scrollDelta.y * 3.0f, 0.5f, 9.0f);
    }

    // Ground clamp: the camera never sinks below the terrain.
    m_playCamera.groundHeightFn = terrain;
    m_playCamera.update(dt, camIn, aspect);
}

CameraState EditorApplication::CameraStateForAnimationState(AnimationState s) const {
    switch (s) {
        case AnimationState::IDLE:       return CameraState::IDLE;
        case AnimationState::WALK:       return CameraState::WALK;
        case AnimationState::RUN:        return CameraState::RUN;
        case AnimationState::JUMP:       return CameraState::JUMP;
        case AnimationState::FALL:       return CameraState::FALL;
        case AnimationState::CROUCH:
        case AnimationState::CROUCH_WALK: return CameraState::CROUCH;
        default: return CameraState::IDLE;
    }
}

void EditorApplication::render(float dt) {
    PROFILE_CPU_SCOPE("EditorApplication::render");

    // Respect the live profiling CVar (artists toggle via cvars.ini)
    bool profEnabled = CVar::Instance().getFloat("profiling.enabled", 1.0f) > 0.5f;
    Profiler::Instance().setEnabled(profEnabled);
    Profiler::Instance().setLogIntervalSeconds(
        CVar::Instance().getFloat("profiling.logInterval", 5.0f));

    Profiler::Instance().beginFrame();

    renderSceneOnly(dt);
    if (m_uiFrameHook) {
        // The UI hook drives the ImGui frame (panels over the viewport FBO
        // texture) and performs the final buffer swap itself.
        m_uiFrameHook(dt);
    } else {
        glfwSwapBuffers(m_window);
    }
    m_frameCount++;

    Profiler::Instance().endFrame();
}

void EditorApplication::renderSceneOnly(float dt) {
    m_renderPipeline.beginFrame(dt);

    // #3: resolve the canonical lighting env ONCE per frame (beginFrame owns
    // the per-frame update); thread it into every renderer instead of letting
    // each renderer call LightingEnvironment::Instance().
    const LightingEnvironment& LEnv = LightingEnvironment::Instance();

    glm::mat4 view(1.0f);
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), (float)m_windowWidth/m_windowHeight, 0.1f, 1000.0f);
    glm::vec3 cameraPos(0.0f);
    float fov = 60.0f;
    if (m_playController.isLoaded()) {
        const float aspect = (float)m_windowWidth / (float)std::max(1, m_windowHeight);
        view = m_playCamera.getViewMatrix();
        proj = m_playCamera.getProjectionMatrix(aspect);
        cameraPos = m_playCamera.position;
        fov = m_playCamera.config.fov;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_renderPipeline.getFramebuffer());
    glViewport(0, 0, m_renderPipeline.getViewportWidth(), m_renderPipeline.getViewportHeight());
    glClearColor(0.08f, 0.09f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_renderPipeline.renderScene(view, proj, cameraPos, fov, LEnv);
    // NOTE: m_worldManager.render() is now invoked via the sceneRenderCallback
    // registered during initialization — it runs INSIDE renderScene()'s
    // geometry pass, bound to the correct FBO before post-processing. The
    // old standalone call here drew terrain as a raw overlay on top of the
    // post-processed framebuffer (no bloom/ACES, depth mismatch).
    renderPlayCharacter(view, proj, cameraPos);

    // Expose this frame's camera state to the UI hook (viewport overlay).
    m_lastView = view;
    m_lastProjection = proj;
    m_lastCameraPos = cameraPos;
    m_lastCameraTarget = m_playController.isLoaded() ? m_playCamera.target : glm::vec3(0.0f);

    if (!m_uiFrameHook) {
        // No ImGui UI loop to display the viewport texture: blit the scene
        // FBO to the default framebuffer before the buffer swap.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_renderPipeline.getFramebuffer());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, m_renderPipeline.getViewportWidth(), m_renderPipeline.getViewportHeight(),
                          0, 0, m_windowWidth, m_windowHeight,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    // With a UI hook the ImGui overlay draws over the WINDOW's default
    // framebuffer, so unbind the viewport FBO before the UI pass.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_windowWidth, m_windowHeight);

    m_renderPipeline.renderUI();
    m_renderPipeline.endFrame();
}

void EditorApplication::renderPlayCharacter(const glm::mat4& view, const glm::mat4& proj,
                                             const glm::vec3& cameraPos) {
    if (!m_playController.isLoaded()) return;
    if (!m_playRenderModel) return;

    Shader* shader = m_renderPipeline.getModelShader();
    if (!shader) return;

    AnimatedCharacter& cc = m_playController.character();

    // World transform: position + heading + scale. modelMatrix() applies the
    // 180-degree facing flip so the bot renders facing its logical forward
    // (the follow camera sits behind it, so the model must face away).
    const glm::mat4 model = cc.modelMatrix();

    shader->use();
    shader->setMat4("projection", proj);
    shader->setMat4("view", view);
    shader->setMat4("model", model);
    shader->setVec3("lightPos", glm::vec3(10.0f, 15.0f, 10.0f));
    shader->setVec3("viewPos", cameraPos);
    shader->setInt("uDisableInstancing", 1);
    shader->setInt("uDisableSkinning", 0);
    shader->setInt("uShowDebug", 0);

    Animator* animator = cc.animator();

    // FIX (v5 todo Area 1): Use the actual bone count instead of a hardcoded
    // 256 so the shader's uPaletteSize guard matches the number of valid
    // matrices bound in the UBO. The old code always set 256, which on strict
    // drivers could let GetBone() read past the glBindBufferRange boundary
    // (which is sized to currentBoneCount*64) during runtime database switches
    // (e.g. locomotion 60-bone rig → crouch 45-bone rig), yielding undefined/
    // zero matrices for bone IDs that exceed the active skeleton.
    if (animator) {
        shader->setInt("uPaletteSize", static_cast<int>(animator->GetFinalBoneMatrices().size()));
    } else {
        shader->setInt("uPaletteSize", 256);
    }

    if (animator) {
        // Phase 2: Batched GPU Transfers — upload ALL characters' bone matrices
        // in a single UpdateBatched() call (one glBufferSubData per frame) rather
        // than N per-character Update() calls. Collect the bone spans, call the
        // shared buffer, then draw. Falls back to per-model upload if no
        // characters are active.
        const auto& bones = animator->GetFinalBoneMatrices();
        if (!bones.empty()) {
            std::vector<const std::vector<glm::mat4>*> batches;
            batches.push_back(&bones);
            m_renderPipeline.uploadBatchedBoneMatrices(batches);
        }
        m_playRenderModel->Draw(*shader, *animator);
    } else m_playRenderModel->DrawStatic(*shader);
}

void EditorApplication::renderNPCs(const glm::mat4& view, const glm::mat4& projection,
                                    const glm::vec3& cameraPos) {
    if (!m_npcSphereInitialized) {
        Shader* shader = m_renderPipeline.getDefaultShader();
        if (!shader) return;

        const int sectors = 16;
        const int stacks = 12;
        const float radius = 0.5f;

        std::vector<glm::vec3> vertices;
        std::vector<unsigned int> indices;

        for (int stack = 0; stack <= stacks; ++stack) {
            float phi = glm::pi<float>() * stack / stacks;
            for (int sector = 0; sector < sectors; ++sector) {
                float theta = 2.0f * glm::pi<float>() * sector / sectors;
                glm::vec3 p(
                    radius * sin(phi) * cos(theta),
                    radius * cos(phi),
                    radius * sin(phi) * sin(theta));
                vertices.push_back(p);
            }
        }
        for (int stack = 0; stack < stacks; ++stack) {
            for (int sector = 0; sector < sectors; ++sector) {
                int a = stack * sectors + sector;
                int b = (stack + 1) * sectors + sector;
                int c = (stack + 1) * sectors + (sector + 1) % sectors;
                int d = stack * sectors + (sector + 1) % sectors;
                indices.push_back(a); indices.push_back(b); indices.push_back(d);
                indices.push_back(b); indices.push_back(c); indices.push_back(d);
            }
        }

        glGenVertexArrays(1, &m_npcSphereVAO);
        glGenBuffers(1, &m_npcSphereVBO);
        GLuint ebo; glGenBuffers(1, &ebo);
        m_npcSphereIndexCount = (GLuint)indices.size();

        glBindVertexArray(m_npcSphereVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_npcSphereVBO);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(glm::vec3),
                     vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int),
                     indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
        glBindVertexArray(0);
        m_npcSphereInitialized = true;
    }

    Shader* shader = m_renderPipeline.getDefaultShader();
    if (!shader) return;

    shader->use();
    shader->setMat4("projection", projection);
    shader->setMat4("view", view);
    shader->setVec3("viewPos", cameraPos);
    shader->setInt("uDisableInstancing", 1);
    shader->setInt("uDisableSkinning", 1);
    shader->setInt("uShowDebug", 0);
    shader->setVec3("lightPos", glm::vec3(10.0f, 15.0f, 10.0f));

    glBindVertexArray(m_npcSphereVAO);

    for (const auto& npc : m_worldManager.npcs()) {
        if (!npc.alive) continue;

        glm::mat4 model(1.0f);
        model = glm::translate(model, npc.position);

        glm::vec3 color = npc.isEnemy
            ? glm::vec3(0.9f, 0.2f, 0.2f)
            : glm::vec3(0.2f, 0.5f, 0.9f);

        shader->setMat4("model", model);
        shader->setVec3("color", color);

        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glEnable(GL_LINE_SMOOTH);
        shader->setVec3("uBaseColor", color);
        glDrawElements(GL_TRIANGLES, m_npcSphereIndexCount, GL_UNSIGNED_INT, 0);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    glBindVertexArray(0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

bool EditorApplication::loadScene(const std::string& scenePath) {
    std::cout << "[EditorApplication] Loading scene: " << scenePath << std::endl;
    return true;
}

bool EditorApplication::saveScene(const std::string& scenePath) {
    std::cout << "[EditorApplication] Saving scene: " << scenePath << std::endl;
    return true;
}

bool EditorApplication::newScene() {
    std::cout << "[EditorApplication] Creating new scene" << std::endl;
    return true;
}

void EditorApplication::enterPlayMode() {
    if (m_isPlaying) return;
    // The character is already loaded at startup - play mode just hands the
    // player control, so the bot stays visible when exiting.
    if (!m_playController.isLoaded()) {
        if (!m_playController.load(m_playModelPath, m_playClipsDir)) {
            std::cerr << "[EditorApplication] Play Mode: failed to load character '"
                      << m_playModelPath << "'" << std::endl;
            m_isPlaying = false;
            return;
        }
    }
    if (m_playController.isLoaded()) {
        // Spawn the bot ON the terrain surface (physics floor snap) so it
        // doesn't start hovering above a valley / tree line at y=0.
        AnimatedCharacter& ch = m_playController.character();
        const float surf = m_worldManager.getSurfaceHeightAt(ch.position.x, ch.position.z);
        ch.position.y = surf;
        ch.grounded = true;
        std::cout << "[Spawn] bot clamped to surface y=" << surf
                  << " at (" << ch.position.x << "," << ch.position.z << ")\n";

        // Spawn NPC training dummies around the bot so combat targeting
        // (Intent Matrix + LOS) has real targets in the viewport.
        m_worldManager.spawnNPC(ch.position + glm::vec3(5.0f, 0.0f, -3.0f), true);
        m_worldManager.spawnNPC(ch.position + glm::vec3(-4.0f, 0.0f, 6.0f), true);
        m_worldManager.spawnNPC(ch.position + glm::vec3(2.0f, 0.0f, 8.0f), true);
        std::cout << "[Spawn] NPC training dummies spawned ("
                  << m_worldManager.npcCount() << " total)\n";
    }
    if (!m_playRenderModel) {
        m_playRenderModel = std::make_unique<Model>(m_playModelPath);
        if (m_playRenderModel->GetMeshCount() == 0) {
            std::cerr << "[EditorApplication] Play Mode: render model '"
                      << m_playModelPath << "' has no meshes" << std::endl;
            m_playRenderModel.reset();
        }
    }
    m_isPlaying = true;
    std::cout << "[EditorApplication] Entering Play Mode (character ready, "
              << m_playController.character().boneCount() << " bones)" << std::endl;
}

void EditorApplication::exitPlayMode() {
    if (!m_isPlaying) return;
    m_isPlaying = false;
    std::cout << "[EditorApplication] Exiting Play Mode" << std::endl;
}

void EditorApplication::togglePlayMode() {
    if (m_isPlaying) exitPlayMode();
    else enterPlayMode();
}

void EditorApplication::onWindowResize(int width, int height) {
    m_windowWidth = width;
    m_windowHeight = height;
    m_renderPipeline.resizeViewport(width, height);
}

void EditorApplication::onKeyInput(int key, int action) {
    // Handle key input
}

void EditorApplication::createDefaultScene() {
    // Create default scene
}

} // namespace Editor
