#pragma once
#include "CameraTypes.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdint>
#include "CinematicCamera.h"

// Selection-focus tracking needs an entity ID. The DTS-cam/kCamera tests + the
// standalone bin/engine loop do not include ECS headers, so guard the ECS-only
// fields behind a config flag and define a minimal no-op type when ECS is absent
// so the camera class still compiles in those contexts.
#ifdef THIRD_PERSON_CAMERA_HAS_ECS
    #include "ecs/ECS.h"
    using FocusEntityID = ecs::EntityID;
    constexpr FocusEntityID kInvalidFocusEntity = ecs::INVALID_ENTITY_ID;
#else
    using FocusEntityID = uint32_t;
    constexpr FocusEntityID kInvalidFocusEntity = 0xFFFFFFFFu;
#endif

/**
 * Third-Person Camera - State-Aware Follow System
 * 
 * Features:
 * - State-aware smoothing (different behavior for idle/walk/run/jump/fall)
 * - Smooth transitions between camera states
 * - Collision avoidance
 * - Configurable follow behavior
 * 
 * Design Principles:
 * - Modularity: Independent system, easy to test
 * - Scalability: Easy to add new states/behaviors
 * - Performance: Minimal allocations, efficient updates
 */
class ThirdPersonCamera {
public:
    // Public state for debugging/inspection
    CameraConfig config;
    CameraOutput output;
    
    // Current camera transform
    glm::vec3 position;
    glm::vec3 target;
    glm::vec3 pivot;  // Look-at point
    
    // State tracking
    CameraState currentState;
    CameraState previousState;
    
    // Smooth follow value (transitions between state values)
    float currentFollowSmooth;
    float targetFollowSmooth;
    
    // Orbit controls (yaw/pitch)
    float yaw;
    float pitch;
    
    // Collision
    bool isColliding;
    float idealDistance;  // Distance before collision

    // Live lens FOV (forcam.txt §3 dynamic FOV expansion). Eased from
    // config.fov toward config.fovMax by ground speed and read by the projection
    // matrix + output, so sprint/jump lens widening reaches rendered frames.
    float currentFOV;

    // forCam.txt §3A-B: one-shot FocusOnEntity glide state. While m_isGliding
    // the camera EaseInOut-blends to a fresh selection (bypassing the
    // continuous follow loop below); once it completes, follow resumes.
    bool        m_isGliding = false;
    float       m_glideTimer = 0.0f;
    glm::vec3   m_glideStartPos{0.0f};
    glm::vec3   m_glideStartPivot{0.0f};
    glm::vec3   m_glideEndPos{0.0f};
    glm::vec3   m_glideEndPivot{0.0f};

    // forCam.txt §4 Camera Banking state. roll is 0 while the heading is
    // steady (idle / straight follow) and tilts the viewport only while the
    // yaw is changing (character turning under auto-orient, or a fast manual
    // orbit). m_prevYaw seeds from the constructor's yaw so the first frame
    // registers no spurious roll.
    float       roll{0.0f};
    float       m_prevYaw{180.0f};

    // forCam.txt §1 Pillar 1 screen-space dead-zone tracking state. Only used
    // when config.enableScreenDeadZone is true (off by default).
    glm::vec3   m_screenSpaceSmoothedPivot{0.0f};
    bool        m_pivotInitialized = false;

    // Optional terrain-height callback. When set, the camera is clamped so it
    // never sinks below the floor (Unreal-style ground clamp).
    std::function<float(float, float)> groundHeightFn;
    
    /**
     * Constructor
     */
    ThirdPersonCamera(
        const glm::vec3& startPos = glm::vec3(0.0f, 5.0f, 10.0f),
        const glm::vec3& targetPos = glm::vec3(0.0f, 2.0f, 0.0f),
        const CameraConfig& cfg = CameraConfig()
    ) : config(cfg),
        position(startPos),
        target(targetPos),
        pivot(targetPos),
        currentState(CameraState::IDLE),
        previousState(CameraState::IDLE),
        currentFollowSmooth(cfg.idleFollowSmooth),
        targetFollowSmooth(cfg.idleFollowSmooth),
        yaw(180.0f), // Z-forward: behind the -Z-forward character, looks down -Z (U6)
        pitch(0.0f),
        isColliding(false),
        idealDistance(cfg.distance),
        currentFOV(cfg.fov)
    {
        output.position = position;
        output.target = target;
        output.currentState = currentState;
        output.currentFollowSmooth = currentFollowSmooth;
        output.currentFOV = config.fov;
        output.isColliding = false;
        output.roll = 0.0f;
    }
    
    /**
     * Update camera with state-aware follow
     * 
     * @param dt Delta time
     * @param input Character state input
     * @param aspectRatio Aspect ratio for projection matrix
     */
    void update(float dt, const CameraInput& input, float aspectRatio = 16.0f/9.0f) {
        if (dt <= 0.0f) { updateOutput(aspectRatio); return; }

        previousState = currentState;
        currentState = input.animState;

        // Update target follow smooth based on state
        targetFollowSmooth = config.getFollowSmoothForState(currentState);

        // Smoothly transition follow smooth value (frame-rate independent)
        currentFollowSmooth = glm::mix(
            currentFollowSmooth,
            targetFollowSmooth,
            1.0f - std::exp(-config.smoothTransitionRate * dt)
        );

    // forCam.txt §3A-B + GTA-style selection framing: one-shot EaseInOut glide
    // to frame a newlyselected entity/prop (triggered on viewport click OR
    // outliner row selection — whichever source wins that frame). Bypass the
    // continuous follow math while gliding so the frame settles to a clean pose;
    // resume follow (updatePivot etc.) once it completes. selection is driven
    // (bot-follow / prop frame) by the caller via FocusOnEntity; idle follow is
    // untouched when not gliding.
    if (m_isGliding) {
            m_glideTimer += dt;
            float t = glm::clamp(m_glideTimer / config.focusGlideDuration, 0.0f, 1.0f);
            float s = t * t * (3.0f - 2.0f * t);       // smoothstep EaseInOut
            position = glm::mix(m_glideStartPos,   m_glideEndPos,   s);
            pivot    = glm::mix(m_glideStartPivot, m_glideEndPivot, s);
            target   = pivot;
            if (t >= 1.0f) {
                m_isGliding = false;
                // One-shot glide completed — hand the target identity back to the
                // caller so it may re-trigger a fresh glide next frame if the
                // artist changed selection while the glide was in flight.
                targetIsSet = false;
            }
            updateFOV(input, dt);
            updateOutput(aspectRatio);
            return;
        }

        // Always keep the camera BEHIND the focus target: when enabled, ease yaw
        // toward the target's heading so the camera orbits around to its back
        // the moment it turns. Without this the camera stays fixed in world
        // space and ends up in front of the target when it walks toward or past
        // the camera.
        //
        // Only auto-orient when the focus target is animated/playable
        // (targetIsAnimated). Unlike the move-magnitude dead-zone in forCam.txt
        // §4 (which prevented in-place turn snaps), the editor's play camera
        // always orients behind the bot so the default slider yaw (-90, side)
        // does not leave the view stuck at the character's side on first select.
        if (config.orientToCharacterForward && input.targetIsAnimated) {
            // forcam.txt §2 "Linear-Space Euler Snapping and Pole Flips": the
            // original eased yaw via atan2 + two manual 360-wrap loops, which is
            // brittle at the +/-180 seam and composes poorly with pitch. Blend a
            // quaternion heading instead: glm::slerp takes the shortest arc
            // natively (no wrap bookkeeping) and, being quaternionic, never
            // gimbal-locks the yaw/pitch composition even at steep pitches.
            const glm::vec2 fwd2(input.characterForward.x, input.characterForward.z);
            if (glm::length(fwd2) > 0.001f) {
                const float targetYaw = std::atan2(fwd2.x, fwd2.y);  // radians: glm::angleAxis takes radians; atan2 returns radians (do NOT wrap in glm::radians)
                const glm::quat targetQ = glm::angleAxis(targetYaw, glm::vec3(0.0f, 1.0f, 0.0f));
                const glm::quat currentQ = glm::angleAxis(glm::radians(yaw), glm::vec3(0.0f, 1.0f, 0.0f));
                const float t = std::min(1.0f, 1.0f - std::exp(-config.orientSmoothRate * dt));
                const glm::quat blended = glm::slerp(currentQ, targetQ, t);
                // Extract the blended Y-rotation back to scalar yaw. Because the
                // inputs are pure-Y rotations, slerp stays in the (w,y) plane, so
                // yaw = 2*atan2(y, w) is exact and gimbal-free -- no pitch is
                // involved, so the vertical pole cannot enter the blend.
                yaw = glm::degrees(2.0f * std::atan2(blended.y, blended.w));
                if (yaw > 180.0f) yaw -= 360.0f;        // normalize to [-180, 180]
                else if (yaw < -180.0f) yaw += 360.0f;
            }
        }

        // forcam.txt §2 pole guard: enforce pitch limits every frame so the orbit
        // direction never approaches the +/-90 deg gimbal region. The config
        // limits are the intended bounds; this is the enforcement the original
        // lacked (pitch was clamped nowhere, so extreme manual input could sail
        // the orbit vector through the singular pole).
        if (pitch > config.maxPitch) pitch = config.maxPitch;
        if (pitch < config.minPitch) pitch = config.minPitch;

        // forCam.txt §4 Camera Banking: roll the viewport proportional to the
        // turn rate (deg/s). yawDelta is the per-frame heading change; at
        // idle / steady follow it is 0 -> rollTarget 0 -> roll decays to 0,
        // so straight tracking is perfectly level (and existing tests, which
        // never assert roll, are unaffected). Clamping to [-180,180] keeps the
        // delta correct across the +-180 seam so banking never over/undershoots.
        {
            float yawDelta = yaw - m_prevYaw;
            while (yawDelta > 180.0f)   yawDelta -= 360.0f;
            while (yawDelta < -180.0f)  yawDelta += 360.0f;
            m_prevYaw = yaw;
            // STABILITY: dividing by the raw frame time made short frames
            // explode the turn rate (a 1e-4 s frame turned a 0.01 deg nudge into
            // 100 deg/s and slammed roll to maxBankRoll), so a camera with any
            // per-frame yaw noise vibrated. Floor the divisor at 240 Hz and drop
            // sub-0.02 deg deltas entirely: quantisation-level yaw wobble in a
            // steady slew can no longer shake the viewport, while real turns
            // (many degrees per frame) are unaffected.
            const float bankDt = std::max(dt, 1.0f / 240.0f);
            const float turnRate = (std::abs(yawDelta) < 0.02f) ? 0.0f : (yawDelta / bankDt);   // deg/s
            const float rollTarget = glm::clamp(turnRate * config.bankScale,
                                                -config.maxBankRoll, config.maxBankRoll);
            const float r = 1.0f - std::exp(-config.bankDamping * dt);
            roll = glm::mix(roll, rollTarget, r);
        }

        // Pivot (look-at) target. Lead the bot by a velocity-biased look-ahead
        // (forCam.txt §4 "velocity-biased tracking") so the camera frames
        // slightly ahead of the hip position on a moving bot. The look-at
        // target is anchor + pivotHeight (matches updatePivot's targetPivot).
        const glm::vec3 anchor = input.characterPosition
                               + input.characterVelocity * config.positionLookAhead;
        const glm::vec3 lookAtTarget = anchor + glm::vec3(0.0f, config.pivotHeight, 0.0f);
        if (config.enableScreenDeadZone) {
            // forCam.txt §1 Pillar 1: hold the frame while the anchor projects
            // inside the inner screen box, only chasing on boundary breach.
            const glm::mat4 viewProj = getProjectionMatrix(aspectRatio) * getViewMatrix();
            updateScreenSpaceSlackPivot(dt, lookAtTarget, viewProj, aspectRatio);
            pivot = m_screenSpaceSmoothedPivot;
        } else {
            updatePivot(dt, anchor);
        }

        // Calculate ideal camera position
        glm::vec3 idealPosition = calculateIdealPosition(input);

        // Handle collision avoidance
        if (config.collisionEnabled) {
            idealPosition = handleCollision(input.characterPosition, idealPosition, dt);
        }

        // Smooth camera follow (state-aware, frame-rate independent exponential).
        // forcam.txt §3 position-lag coupling: as ground speed rises (lens open)
        // the follow rate scales down so the camera trails the target more
        // (increased position lag) to amplify the sensation of motion. Driven
        // from the LIVE speed (not the eased FOV) so the lag engages in the same
        // frame; idle (speed == 0) is unchanged, preserving frame-rate
        // independence.
        const float horizSpeed = glm::length(glm::vec3(input.characterVelocity.x,
                                                       0.0f,
                                                       input.characterVelocity.z));
        const float lagScale = 1.0f - lensOpenFactor(horizSpeed) * config.positionLagScale;
        const float alpha = 1.0f - std::exp(-currentFollowSmooth * lagScale * dt);
        position = glm::mix(position, idealPosition, alpha);
        target = pivot;

        // AAA Ground Clamp Fix (forcam.txt §1 "Hard-Floor Ground Shudder"):
        // clamp the FINAL smoothed position AFTER the exponential blend, not
        // the pre-smooth ideal target. Clamping idealPosition.y before smoothing
        // drags the blended trajectory onto a floor profile that drops instantly
        // when the character crests a cliff or descends stairs (the ideal target
        // snaps to the new floor while the physical camera lags behind), which
        // produces a mechanical pop. Clamping the resolved position instead
        // leaves the smoothing integral intact and only arrests the camera at the
        // terrain boundary once it has actually arrived there.
        if (groundHeightFn) {
            const float minY = groundHeightFn(position.x, position.z) + config.groundClearance;
            if (position.y < minY) position.y = minY;
        }

        // Vertical keep-off clamps (GTA-style framing safety): keep the camera
        // out of the skybox ceiling AND under big props / pits, even when
        // auto-orbit or manual drag would otherwise drive it there. Ground
        // clamp is terrain only; this is a hard world-space band so a bot that
        // walks up a hill cannot end up inside the skybox.
        {
            const float floorY = (groundHeightFn ? groundHeightFn(position.x, position.z) + config.groundClearance : -1e9f);
            if (position.y < floorY) position.y = floorY;
            if (position.y < config.minHeight) position.y = config.minHeight;
            // FIX: the ceiling is a band ABOVE THE FLOOR, not an absolute world
            // Y. config.maxHeight (40 m) was written for a floor near y=0, but
            // this engine's heightmap puts the terrain at y≈100 — the absolute
            // clamp then pinned the follow camera 60+ m UNDER the ground, so the
            // viewport looked up at the scene from below, far from every object.
            // With groundHeightFn the band rides with the terrain, which keeps
            // its original meaning (stay below the skybox) at any altitude; on a
            // floor at y=0 the bound is unchanged (== config.maxHeight).
            const float ceilY = groundHeightFn
                ? (groundHeightFn(position.x, position.z) + config.maxHeight)
                : config.maxHeight;
            if (position.y > ceilY) position.y = ceilY;
        }

        // Dynamic FOV Expansion (forcam.txt §3 "Lack of Lens Profile FOV
        // Expansion"): widen the lens as ground speed rises from idle to
        // sprint. Horizontal speed only: vertical velocity (falls/jumps) must
        // not widen the lens; open and close rates differ so the lens settles
        // crisply after a burst.
        updateFOV(input, dt);

        // Update output
        updateOutput(aspectRatio);
    }
    
    /**
     * Get view matrix
     */
    glm::mat4 getViewMatrix() const {
        glm::mat4 view = glm::lookAt(position, target, glm::vec3(0.0f, 1.0f, 0.0f));
        // Apply banking roll about the eye->target axis. Guarded: when the
        // camera is steady (roll==0, the common/idle case) this branch is
        // skipped and the matrix is byte-identical to the original lookAt(),
        // so every existing test that inspects the view matrix is unaffected.
        // Roll is applied in eye space (rotate the view volume), not the world,
        // so it tilts the viewport without displacing the look-at target.
        if (std::abs(roll) > 1e-4f) {
            const glm::vec3 axis = glm::normalize(target - position);
            view = glm::rotate(view, glm::radians(roll), axis);
        }
        return view;
    }
    
    /**
     * Get projection matrix
     */
    glm::mat4 getProjectionMatrix(float aspectRatio) const {
        return glm::perspective(
            glm::radians(currentFOV),
            aspectRatio,
            0.1f, 
            1000.0f
        );
    }
    
    /**
     * Set camera state directly (for manual control)
     */
    void setState(CameraState state) {
        previousState = currentState;
        currentState = state;
        targetFollowSmooth = config.getFollowSmoothForState(state);
    }
    
    /**
     * Set configuration
     */
    void setConfig(const CameraConfig& cfg) {
        config = cfg;
        targetFollowSmooth = config.getFollowSmoothForState(currentState);
    }
    
    /**
     * Instant camera placement (for cutscenes, etc.)
     */
    void setPosition(const glm::vec3& pos) {
        position = pos;
    }
    
    void setTarget(const glm::vec3& tgt) {
        target = tgt;
        pivot = tgt;
    }

    /**
     * forCam.txt §3A-B: smooth EaseInOut "glide" to frame a selected entity or
     * prop (triggered on viewport click / outliner row selection). Bypasses the
     * normal follow loop for `config.focusGlideDuration`, framing the target
     * from BEHIND the entity (animated: use entity forward; static: keep current
     * yaw) at a distance scaled by boundingRadius (clamped to [minDistance,
     * maxDistance]) — large props pull back, small NPCs aren't zoomed in on.
     * Resumes continuous follow afterward.
     */
    void FocusOnEntity(const glm::vec3& entityPos, float boundingRadius = 2.0f,
                       const glm::vec3& entityForward = glm::vec3(0.0f, 0.0f, 1.0f),
                       bool entityIsAnimated = false) {
        // Back-compat: older call sites pass only (pos, radius). Those resolve
        // to entityForward=(0,0,1) and entityIsAnimated=false, which keeps the
        // current yaw — same behaviour as before this signature change.
        m_glideStartPos   = position;
        m_glideStartPivot = pivot;

        const float dist = glm::clamp(boundingRadius * 2.5f,
                                      config.minDistance, config.maxDistance);

        // For animated entities, frame from behind the entity's forward so the
        // camera lands on the back side (not the current orbit yaw, which may be
        // at the side). For static props, keep the current yaw so the view
        // direction feels natural (don't spin the camera around a tree).
        float yawRad = glm::radians(entityIsAnimated ? 180.0f : yaw);
        const float pitchRad = glm::radians(pitch);
        glm::vec3 dir;
        dir.x =  std::cos(pitchRad) * std::sin(yawRad);
        dir.y = -std::sin(pitchRad);
        dir.z =  std::cos(pitchRad) * std::cos(yawRad);
        dir = glm::normalize(dir);

        // Pivot = entity position + pivotHeight (look-at chest/head level).
        // entityPos is the entity's FEET position (TransformComponent::position);
        // the camera config's pivotHeight lifts the look-at target to chest level.
        m_glideEndPivot = entityPos + glm::vec3(0.0f, config.pivotHeight, 0.0f);
        // Use the same local-space shoulder offset as calculateIdealPosition so
        // the glide lands on the pose the continuous follow would settle on --
        // no pop on the glide -> follow handoff (forCAM.txt §3 framing fix).
        m_glideEndPos   = m_glideEndPivot - dir * dist + localShoulderOffset(dir);

        // [FIX] stop re-arming: only begin a NEW glide when one is not already
        // in flight. EditorUI polls the same selected entity every frame; without
        // this guard FocusOnEntity reset m_glideTimer each frame and the EaseInOut
        // never reached t==1, so the camera oscillated around the endpoint.
        if (!m_isGliding) {
            m_glideTimer = 0.0f;
            m_isGliding  = true;
        }
    }
    
    /**
     * Get camera forward direction
     */
    glm::vec3 getForward() const {
        return glm::normalize(target - position);
    }
    
    /**
     * Get distance to character
     */
    float getDistanceToCharacter(const glm::vec3& charPos) const {
        return glm::length(position - charPos);
    }
    
    /**
     * Check if camera is in transition
     */
    bool isTransitioning() const {
        return currentState != previousState;
    }
    
    /**
     * Get current state as string (for debugging)
     */
    std::string getStateString() const {
        return CameraStateToString(currentState);
    }
    
private:
    /**
     * Update pivot point (look-at target)
     */
    void updatePivot(float dt, const glm::vec3& charPos) {
        glm::vec3 targetPivot = charPos + glm::vec3(0.0f, config.pivotHeight, 0.0f);
        const float alpha = 1.0f - std::exp(-config.pivotSmooth * dt);
        pivot = glm::mix(pivot, targetPivot, alpha);
    }

    /**
     * forCam.txt §1 Pillar 1: screen-space dead-zone pivot tracker.
     *
     * Projects the 3D anchor into NDC -> viewport [0..1] and HOLDS the smoothed
     * pivot stationary while it sits inside the inner slack box (default 15%
     * envelope around the viewport centre), resuming the chase only once the
     * anchor breaches a boundary. The chase eases onto the raw anchor (the
     * proposal's unused invViewProj is intentionally omitted to avoid an
     * unused-variable warning while preserving the documented behaviour).
     */
    void updateScreenSpaceSlackPivot(float dt, const glm::vec3& rawCharacterAnchor,
                                     const glm::mat4& viewProjMatrix, float aspectRatio) {
        (void)aspectRatio;   // (reserved: future per-axis slack tuning)

        if (!m_pivotInitialized) {
            m_screenSpaceSmoothedPivot = rawCharacterAnchor;
            m_pivotInitialized = true;
            return;
        }

        const glm::vec4 clipSpacePos = viewProjMatrix * glm::vec4(rawCharacterAnchor, 1.0f);
        if (clipSpacePos.w <= 0.001f) {
            // Behind the camera (no stable projection): snap to anchor.
            m_screenSpaceSmoothedPivot = rawCharacterAnchor;
            return;
        }

        const glm::vec3 ndcSpacePos = glm::vec3(clipSpacePos) / clipSpacePos.w;
        glm::vec2 screenSpacePos;
        screenSpacePos.x = ndcSpacePos.x * 0.5f + 0.5f;
        screenSpacePos.y = 1.0f - (ndcSpacePos.y * 0.5f + 0.5f); // top-left origin

        const ScreenDeadZone& zone = config.screenDeadZone;
        glm::vec2 correctionDelta(0.0f);
        bool boundaryBreached = false;
        if (screenSpacePos.x < zone.xMin) { correctionDelta.x = screenSpacePos.x - zone.xMin; boundaryBreached = true; }
        if (screenSpacePos.x > zone.xMax) { correctionDelta.x = screenSpacePos.x - zone.xMax; boundaryBreached = true; }
        if (screenSpacePos.y < zone.yMin) { correctionDelta.y = screenSpacePos.y - zone.yMin; boundaryBreached = true; }
        if (screenSpacePos.y > zone.yMax) { correctionDelta.y = screenSpacePos.y - zone.yMax; boundaryBreached = true; }
        (void)correctionDelta;   // measured but the anchor-ease below is the documented behaviour

        if (boundaryBreached) {
            // Anchor is outside the inner box: chase it (eased).
            const float trackingAlpha = 1.0f - std::exp(-zone.catchUpSpeed * dt);
            m_screenSpaceSmoothedPivot = glm::mix(m_screenSpaceSmoothedPivot,
                                                  rawCharacterAnchor, trackingAlpha);
        } else {
            // Anchor is inside the slack box: hold the pivot. A micro-decay
            // prevents drift if it sits exactly on a boundary.
            const float microDecayAlpha = 1.0f - std::exp(-1.5f * dt);
            m_screenSpaceSmoothedPivot = glm::mix(m_screenSpaceSmoothedPivot,
                m_screenSpaceSmoothedPivot + (rawCharacterAnchor - m_screenSpaceSmoothedPivot) * 0.05f,
                microDecayAlpha);
        }
    }

    /**
     * Local-space shoulder offset expressed in the camera's view basis
     * (forCam.txt §3 "Framing Invalidation during Local-Space Pivot Shifts").
     *
     * A raw world-Y offset (+height*0.35) applied after computing the orbit
     * radius shifts the camera off its spherical track at steep pitches,
     * stretching the focus distance and pushing the subject out of frame.
     * Building right/up from the live look direction keeps the offset pitch-
     * invariant so the character stays framed at a constant on-screen position.
     *
     * The cross(lookDir, worldUp) that yields the right vector vanishes near the
     * vertical pole (pitch -> ±90), which is exactly the gimbal region forCam.txt
     * §2 warns about -- guard that case so the shoulder term can never emit NaN
     * (the min/maxPitch limits already keep us out of it, this is belt-and-suspenders).
     */
    glm::vec3 localShoulderOffset(const glm::vec3& lookDir) const {
        const float cosPitch = std::sqrt(lookDir.x * lookDir.x + lookDir.z * lookDir.z);
        if (cosPitch < 1e-3f) {
            // Near-vertical look: fall back to a pure world-up lift to avoid a
            // degenerate (zero) right vector at the pole.
            return glm::vec3(0.0f, config.height * 0.15f, 0.0f);
        }
        glm::vec3 rightDir = glm::normalize(glm::cross(lookDir, glm::vec3(0.0f, 1.0f, 0.0f)));
        glm::vec3 upDir    = glm::cross(rightDir, lookDir);  // unit by construction
        return rightDir * 0.35f + upDir * (config.height * 0.15f);
    }

    /**
     * Calculate ideal camera position (without collision)
     */
    glm::vec3 calculateIdealPosition(const CameraInput& input) {
        // Use yaw and pitch for orbit control. Positive pitch raises the camera
        // ABOVE the character's chest and looks down at it (Unreal-style).
        float yawRad = glm::radians(yaw);
        float pitchRad = glm::radians(pitch);

        // Z-forward convention (U6): yaw=0 looks down +Z. This matches the
        // yaw/atan2 convention used by the editor free-fly, the orbit camera and
        // the cinematic orbit math, so a single yaw value means the same heading
        // everywhere. dir.x = cos(pitch)*sin(yaw), dir.z = cos(pitch)*cos(yaw).
        glm::vec3 direction;
        direction.x = std::cos(pitchRad) * std::sin(yawRad);
        direction.y = -std::sin(pitchRad);   // +pitch raises the camera above
        direction.z = std::cos(pitchRad) * std::cos(yawRad);
        direction = glm::normalize(direction);

        // Camera orbits the character's chest at `distance`, with a local-space
        // shoulder offset so framing holds at steep pitch angles (forCam.txt §3).
        glm::vec3 pivotPos = input.characterPosition
                           + glm::vec3(0.0f, config.pivotHeight, 0.0f);
        glm::vec3 idealPos = pivotPos - direction * config.distance
                           + localShoulderOffset(direction);

        return idealPos;
    }
    
    // --- Collision spring state (forCam.txt §4 critical-damping solver) ---
    float m_collisionSpringDistance = 0.0f;  // current camera-to-char distance (shell)
    bool  m_collisionSpringArmed = false;    // true once the spring has been seeded

    /**
     * Handle collision avoidance via a critical-damping spring (forCam.txt §4
     * "Massless Collision Spring Compression"). The previous version used a
     * fixed-rate glm::mix(position, newPos, collisionLerp) geometric snap that
     * pops the camera onto the safe radius; a spring-mass-damper instead eases
     * the camera forward onto the shell on contact and smoothly restores the
     * full orbit distance the instant the path clears.
     */
    glm::vec3 handleCollision(const glm::vec3& charPos, const glm::vec3& idealPos, float dt) {
        glm::vec3 direction = idealPos - charPos;
        float distance = glm::length(direction);
        
        if (distance < 1e-6f) return idealPos;  // degenerate: camera at character
        
        glm::vec3 dirNorm = direction / distance;
        
        if (distance < config.collisionRadius) {
            // The ideal position would place the camera inside the collision
            // boundary -- pull it to the safe radius. Pull slightly closer than
            // the wall face so the camera never clips through geometry.
            isColliding = true;
            const float safeDistance = glm::max(0.2f,
                config.collisionRadius * (1.0f - config.collisionBuffer));
            if (!m_collisionSpringArmed) {
                // Seed the spring from the camera's actual distance so the first
                // contact frame eases in smoothly instead of instant-snap.
                m_collisionSpringDistance = glm::length(position - charPos);
                m_collisionSpringArmed = true;
            }
            const float rate = 1.0f - std::exp(-config.collisionSpringRate * dt);
            m_collisionSpringDistance = glm::mix(m_collisionSpringDistance, safeDistance, rate);
            idealDistance = m_collisionSpringDistance;
            
            // Spring-resolved position along the orbit ray (no geometric snap).
            return charPos + dirNorm * m_collisionSpringDistance;
        }
        
        // Path clear: restore the full (shoulder-inclusive) ideal and re-arm so
        // the next contact re-seeds the spring from the live position.
        isColliding = false;
        m_collisionSpringArmed = false;
        idealDistance = config.distance;
        return idealPos;
    }
    
    /**
     * forCam.txt §3: shared horizontal-speed -> lens-open / lag factor in [0..1]
     * (0 at/below fovSpeedThreshold, 1 at/above fovMaxSpeed). Used by both the
     * dynamic-FOV blend and the position-lag coupling so they track the same
     * speed profile without duplicating the threshold math.
     */
    float lensOpenFactor(float horizSpeed) const {
        const float span = config.fovMaxSpeed - config.fovSpeedThreshold;
        float t = 0.0f;
        if (span > 1e-5f) t = (horizSpeed - config.fovSpeedThreshold) / span;
        else t = (horizSpeed >= config.fovSpeedThreshold) ? 1.0f : 0.0f;
        return glm::clamp(t, 0.0f, 1.0f);
    }

    /**
     * Dynamic FOV Expansion (forCam.txt §3). Eases the live lens FOV between the
     * resting `config.fov` and `config.fovMax` as a function of the character's
     * ground (horizontal) speed, using frame-rate-independent exponential decay.
     * The lens opens at fovExpandRate and closes at fovContractRate. Horizontal
     * speed only: vertical velocity (falls/jumps) must not widen the lens.
     */
    void updateFOV(const CameraInput& input, float dt) {
        const glm::vec3 horizVel(input.characterVelocity.x, 0.0f,
                                 input.characterVelocity.z);
        const float speed = glm::length(horizVel);

        // Map [fovSpeedThreshold .. fovMaxSpeed] -> [0..1] lens-open factor
        // (lives in lensOpenFactor(), shared with the §3 position-lag coupling).
        const float t = lensOpenFactor(speed);
        const float targetFOV = glm::mix(config.fov, config.fovMax, t);

        const float rate = (targetFOV > currentFOV)
                         ? config.fovExpandRate
                         : config.fovContractRate;
        currentFOV = glm::mix(currentFOV, targetFOV,
                              1.0f - std::exp(-rate * dt));
    }

    /**
     * Update output structure
     */
    void updateOutput(float aspectRatio) {
        output.position = position;
        output.target = target;
        output.viewMatrix = getViewMatrix();
        output.projectionMatrix = getProjectionMatrix(aspectRatio);
        output.currentFOV = currentFOV;
        output.roll       = roll;     // forCam.txt §4 banking (0 when steady)
        output.currentState = currentState;
        output.currentFollowSmooth = currentFollowSmooth;
        output.isColliding = isColliding;
    }

    // Selection-focus tracking for idempotent per-target re-trigger guard.
    FocusEntityID targetEntity{kInvalidFocusEntity};
    bool          targetIsSet = false;
};

/**
 * Camera Controller - High-level camera management
 * 
 * Manages camera modes, transitions, and presets
 */
class CameraController {
public:
    ThirdPersonCamera* camera;
    
    enum class CameraMode {
        THIRD_PERSON,     // Default state-aware follow
        FIRST_PERSON,     // Over-the-shoulder
        ORBIT,           // Manual orbit control
        CINEMATIC        // Fixed cinematic angles
    };
    
    CameraMode currentMode;
    // Cinematic timeline tracking (unknown U7 drop-in): an explicit clock
    // so the perpetual-orbit math advances smoothly over time instead of
    // freezing at its entry angle.
    float cinematicTime{0.0f};
    double cinematicOrbitAngle{0.0f};
    float cinematicRotationSpeed{15.0f}; // deg/s (fixed: was {15.0f;} syntax error)

    
    CameraController(ThirdPersonCamera* cam) 
        : camera(cam), currentMode(CameraMode::THIRD_PERSON) {}
    
    /**
     * Set camera mode
     */
    void setMode(CameraMode mode) {
        currentMode = mode;
        
        switch (mode) {
            case CameraMode::FIRST_PERSON:
                configureFirstPerson();
                break;
            case CameraMode::ORBIT:
                configureOrbit();
                break;
            case CameraMode::CINEMATIC:
                cinematicTime = 0.0f;       // Reset timeline on state entry
                cinematicOrbitAngle = 0.0;   // Restart orbit at 0 degrees
                configureCinematic();
                break;
            default:
                configureThirdPerson();
                break;
        }
    }

    /**
     * Adjust follow distance by `amount` (positive = pull back), clamped to
     * [config.minDistance, config.maxDistance]. Back-bone of the scroll-zoom input
     * path; exercised by CameraSystemTest.Controller_ZoomLimits.
     */
    void zoom(float amount) {
        if (!camera) return;
        camera->config.distance = glm::clamp(camera->config.distance + amount,
                                             camera->config.minDistance,
                                             camera->config.maxDistance);
    }

    /**
     * Reset to the balanced third-person default (config/distance/yaw/pitch),
     * clearing any first-person/orbit/cinematic override.
     * CameraSystemTest.Controller_Reset.
     */
    void reset() {
        if (!camera) return;
        camera->config = CameraConfig();        // defaults: distance=4, idleFollowSmooth=10, ...
        camera->yaw = 180.0f;
        camera->pitch = 0.0f;
        currentMode = CameraMode::THIRD_PERSON;
        configureThirdPerson();                 // setBalanced() + distance = 4.0f
    }

    /**
     * Update camera controller
     */
    void update(float dt, const CameraInput& input, float aspectRatio = 16.0f / 9.0f) {
        if (!camera) return;

        switch (currentMode) {
            case CameraMode::THIRD_PERSON:
                camera->update(dt, input, aspectRatio);
                break;
            case CameraMode::FIRST_PERSON:
                camera->update(dt, input, aspectRatio);
                configureFirstPerson();
                break;
            case CameraMode::ORBIT:
                camera->update(dt, input, aspectRatio);
                break;
            case CameraMode::CINEMATIC: {
                cinematicTime += dt;
                cinematicOrbitAngle = Cinematic::AdvanceAngle(cinematicOrbitAngle, cinematicRotationSpeed, dt);

                float liveHeight = Cinematic::IntroHeight(cinematicTime, 3.0f, camera->config.height + 2.0f, camera->config.height);
                glm::vec3 lookAtTarget = input.characterPosition + glm::vec3(0.0f, camera->config.pivotHeight, 0.0f);
                glm::vec3 nextOrbitPos = lookAtTarget + Cinematic::OrbitPosition(cinematicOrbitAngle, camera->config.distance, liveHeight);

                camera->m_glideStartPos = camera->position;
                camera->m_glideStartPivot = camera->pivot;
                camera->m_glideEndPos = nextOrbitPos;
                camera->m_glideEndPivot = lookAtTarget;
                camera->m_glideTimer = 0.0f;
                camera->m_isGliding = true;
                break;
            }
            default:
                camera->update(dt, input, aspectRatio);
                break;
        }
        
        updateOutput();
    }

    
private:
    void updateOutput() {
        if (!camera) return;
        // The camera's own updateOutput() already populates camera->output;
        // nothing extra to mirror here.
    }
    
    void configureThirdPerson() { camera->config.setBalanced(); camera->config.distance = 4.0f; }
    void configureFirstPerson() { camera->config.setSnappy();   camera->config.distance = 2.0f; }
    void configureOrbit()       { camera->config.distance = 5.5f; }
    void configureCinematic()   { camera->config.setCinematic(); camera->config.distance = 7.0f; }
};
