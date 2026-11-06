#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include <cmath>

class flyCamera {
public:
    // Camera attributes
    glm::vec3 Position;
    glm::vec3 Target;
    glm::vec3 Up;
    glm::vec3 Right;
    glm::vec3 WorldUp;

    // Euler angles (orbit)
    float Yaw;
    float Pitch;

    // Camera options (order matches constructor initialization list)
    float FieldOfView;
    float Zoom;
    float MouseSensitivity;
    float DistanceToTarget;
    float MinDistance;
    float MaxDistance;

    // Mouse state
    float LastX, LastY;
    bool FirstMouse;

    // Reset the first-mouse seed so the next ProcessMouseMovement /
    // ProcessMouseMovementAbsolute call pans from the cursor's CURRENT position
    // instead of snapping to the stale LastX/LastY (the violent "mouse hover
    // doesn't work" jump when the viewport regains focus after ImGui captured
    // the cursor over a panel). The editor input router calls this on
    // viewport-focus-enter.
    void resetFirstMouse() { FirstMouse = true; }

    // Smooth follow
    float followSpeed = 5.0f;
    float rotationSpeed = 2.0f;

    // Camera shake
    struct ShakeEffect {
        float intensity = 0.0f;
        float duration = 0.0f;
        float decayRate = 1.0f;
        glm::vec3 direction = glm::vec3(1.0f);
    };
    ShakeEffect currentShake;

    // Constructor
    flyCamera(glm::vec3 startPos = glm::vec3(0.0f, 2.0f, 10.0f),
              glm::vec3 up = glm::vec3(0.0f,1.0f,0.0f),
              float yaw = -90.0f,
              float pitch = 20.0f,
              float distance = 8.0f)
        : WorldUp(up), Yaw(yaw), Pitch(pitch),
          FieldOfView(60.0f), Zoom(60.0f),
          MouseSensitivity(0.2f),
          DistanceToTarget(distance), MinDistance(0.5f), MaxDistance(150.0f)
    {
        Position = startPos;
        Target = glm::vec3(0.0f);
        FirstMouse = true;
        LastX = 400; LastY = 300;
        
        // Calculate orbit position from Yaw, Pitch, and DistanceToTarget
        // This positions the camera correctly for orbit camera behavior
        updateCameraVectors();
    }

    // Get view matrix
    glm::mat4 GetViewMatrix() {
        return glm::lookAt(Position, Target, Up);
    }

    // Get projection matrix
    glm::mat4 GetProjectionMatrix(float aspectRatio) {
        return glm::perspective(glm::radians(FieldOfView), aspectRatio, 0.1f, 1000.0f);
    }

    // Smooth third-person follow
    void FollowPlayerSmooth(const glm::vec3& playerPos, float deltaTime) {
        Target = playerPos;

        glm::vec3 offset;
        offset.x = DistanceToTarget * cos(glm::radians(Pitch)) * sin(glm::radians(Yaw));
        offset.y = DistanceToTarget * sin(glm::radians(Pitch));
        offset.z = DistanceToTarget * cos(glm::radians(Pitch)) * cos(glm::radians(Yaw));

        glm::vec3 desiredPos = Target - offset; // minus ensures proper behind position
        Position = glm::mix(Position, desiredPos, followSpeed * deltaTime);

        updateCameraVectors();
    }

    // Instant follow (optional)
    void FollowPlayer(const glm::vec3& playerPos) {
        Target = playerPos;
        glm::vec3 offset;
        offset.x = DistanceToTarget * cos(glm::radians(Pitch)) * sin(glm::radians(Yaw));
        offset.y = DistanceToTarget * sin(glm::radians(Pitch));
        offset.z = DistanceToTarget * cos(glm::radians(Pitch)) * cos(glm::radians(Yaw));

        Position = Target - offset;
        updateCameraVectors();
    }

    // Mouse rotation
    void ProcessMouseMovement(float xpos, float ypos) {
        if (FirstMouse) { LastX = xpos; LastY = ypos; FirstMouse = false; }

        float xoffset = xpos - LastX;
        float yoffset = LastY - ypos; // reversed
        LastX = xpos;
        LastY = ypos;

        xoffset *= MouseSensitivity;
        yoffset *= MouseSensitivity;

        Yaw += xoffset * rotationSpeed;
        Pitch += yoffset * rotationSpeed;

        // Clamp pitch
        if (Pitch > 89.0f) Pitch = 89.0f;
        if (Pitch < -89.0f) Pitch = -89.0f;

        updateCameraVectors();
    }

    // Scroll zoom. World-space, decoupled from any model: the original additive
    // step (`DistanceToTarget -= yoffset`) yanked the camera ~1 unit per notch
    // and the [1,50] band couldn't frame a 100-unit terrain chunk -- the "zoom
    // in / zoom out doesn't work" complaint. Use a distance-scaled exponential
    // zoom (`dist * exp(-wheel*0.12)`, matching the Vulkan viewport) so near =
    // fine / far = coarse, and widen the band to [0.5,150] so the whole world
    // can be framed (needed to visually debug gizmo/forward/foliage/POM).
    void ProcessMouseScroll(float yoffset) {
        if (yoffset == 0.0f) return;
        DistanceToTarget = glm::clamp(
            DistanceToTarget * std::exp(-yoffset * 0.12f),
            MinDistance, MaxDistance);
        updateCameraVectors();
    }

    // Camera shake effect
    void AddShake(float intensity, float duration, const glm::vec3& direction = glm::vec3(1.0f)) {
        currentShake.intensity = intensity;
        currentShake.duration = duration;
        currentShake.decayRate = intensity / duration;
        currentShake.direction = direction;
    }

    // Update camera shake
    void UpdateShake(float deltaTime) {
        if (currentShake.duration > 0.0f) {
            currentShake.duration -= deltaTime;
            if (currentShake.duration <= 0.0f) {
                currentShake.intensity = 0.0f;
            } else {
                currentShake.intensity -= currentShake.decayRate * deltaTime;
            }
            
            // Apply shake offset
            if (currentShake.intensity > 0.0f) {
                // Generate a pseudo-random offset based on time
                float time = glfwGetTime();
                glm::vec3 shakeOffset;
                shakeOffset.x = (sin(time * 100.0f) * 0.5f - 0.25f) * currentShake.intensity * currentShake.direction.x;
                shakeOffset.y = (cos(time * 123.0f) * 0.5f - 0.25f) * currentShake.intensity * currentShake.direction.y;
                shakeOffset.z = (sin(time * 147.0f) * 0.5f - 0.25f) * currentShake.intensity * currentShake.direction.z;
                
                Position += shakeOffset;
                Target += shakeOffset;
            }
        }
    }

    // Set field of view
    void SetFieldOfView(float fov) {
        FieldOfView = glm::clamp(fov, 1.0f, 120.0f);
    }

    // Cinematic camera modes
    enum class CameraMode {
        FREE_LOOK,
        THIRD_PERSON,
        FIRST_PERSON,
        ORBITAL
    };

    void SetCameraMode(CameraMode mode) {
        cameraMode = mode;
    }

    // ---- Engine-style focus / smooth-follow (forCam.txt Steps A & B) ----
    // The camera is an independent entity driven by a Target / Anchor point
    // rather than an ego-centric eye. FocusOnEntity initiates a smooth glide
    // to frame a selected object; UpdateEngineCamera advances the glide each
    // frame (call BEFORE normal input processing so focus isn't disrupted).
    glm::vec3 desiredBotPosition = glm::vec3(0.0f);  // THIRD_PERSON follow target
    glm::vec3 InterpolatedTarget = glm::vec3(0.0f);   // smoothly tracks focus target
    bool IsGlidingToFocus = false;
    float GlideDuration = 0.4f;   // seconds to complete a focus glide
    float GlideTimer = 0.0f;

    glm::vec3 GlideStartPos;
    glm::vec3 GlideStartTgt;
    glm::vec3 GlideEndPos;
    glm::vec3 GlideEndTgt;

    // Call this whenever an Entity or Prop is clicked in the viewport or outliner.
    // Frames the object at a distance appropriate for its bounding radius.
    // For animated targets the camera frames from behind the entity's forward;
    // for static props it keeps the current yaw so the view direction feels natural.
    void FollowOnEntity(const glm::vec3& entityPos, float boundingRadius = 2.0f,
                        const glm::vec3& entityForward = glm::vec3(0.0f, 0.0f, 1.0f),
                        bool entityIsAnimated = false) {
        // [FIX] gate on a changed target ID so the same selection can't re-arm the
        // glide every poll frame (which left the camera vibrating around the end).
        // Provided as a dedicated routine so editor code can pass the selection ID;
        // FocusOnEntity stays on the existing per-frame-unaware call sites.
        FocusOnEntity(entityPos, boundingRadius, entityForward, entityIsAnimated);
    }
    void FocusOnEntity(const glm::vec3& entityPos, float boundingRadius = 2.0f,
                       const glm::vec3& entityForward = glm::vec3(0.0f, 0.0f, 1.0f),
                       bool entityIsAnimated = false) {
        GlideStartPos = Position;
        GlideStartTgt = Target;

        // Perfect framing distance based on object size (larger objects pull back).
        DistanceToTarget = glm::clamp(boundingRadius * 2.5f, MinDistance, MaxDistance);

        // For animated entities frame from behind the entity's forward; for static
        // props keep the current yaw so we don't spin the camera around a tree.
        const float useYaw = entityIsAnimated ? 0.0f : Yaw;  // yaw=0 -> behind -Z-forward entity
        const float cosPitch = cos(glm::radians(Pitch));
        const float sinPitch = sin(glm::radians(Pitch));
        const float cosYaw = cos(glm::radians(useYaw));
        const float sinYaw = sin(glm::radians(useYaw));
        glm::vec3 offset;
        offset.x = DistanceToTarget * cosYaw * cosPitch;
        offset.y = DistanceToTarget * sinPitch;
        offset.z = DistanceToTarget * sinYaw * cosPitch;

        GlideEndTgt = entityPos;
        GlideEndPos = entityPos - offset;  // frame behind the chosen angle

        // [FIX] stop re-arming: only begin a NEW glide when one is not already
        // in flight. EditorUI polls the same selected entity every frame; without
        // this guard FocusOnEntity reset GlideTimer each frame and the EaseInOut
        // never reached t==1, so the camera oscillated around the endpoint.
        if (!IsGlidingToFocus) {
            GlideTimer = 0.0f;
            IsGlidingToFocus = true;
        }
    }

    // Frame-rate independent blend loop. Advances the focus glide (if active),
    // then applies exponential-decay tracking for third-person follow mode.
    void UpdateEngineCamera(float deltaTime) {
        if (IsGlidingToFocus) {
            GlideTimer += deltaTime;
            const float t = glm::clamp(GlideTimer / GlideDuration, 0.0f, 1.0f);
            // Ease-in-out smoothstep removes mechanical pops
            const float smoothT = t * t * (3.0f - 2.0f * t);
            Position = glm::mix(GlideStartPos, GlideEndPos, smoothT);
            Target = glm::mix(GlideStartTgt, GlideEndTgt, smoothT);
            if (t >= 1.0f) {
                IsGlidingToFocus = false;
            }
            return;  // bypass normal inputs while focusing
        }

        // Normal follow logic for gameplay bots (exponential decay)
        if (cameraMode == CameraMode::THIRD_PERSON) {
            const float blendFactor = 1.0f - std::exp(-followSpeed * deltaTime);
            Target = glm::mix(Target, desiredBotPosition, blendFactor);
            updateCameraVectors();
        }
    }

    // ---- Collision detection for camera ----
    void SetCollisionEnabled(bool enabled) {
        collisionEnabled = enabled;
    }

    void SetCollisionDistance(float distance) {
        collisionDistance = distance;
    }

    // Update camera with collision detection
    void UpdateWithCollision(const std::vector<glm::vec3>& collisionPoints) {
        if (!collisionEnabled) return;

        // Check for collisions with nearby objects
        for (const auto& point : collisionPoints) {
            float dist = glm::distance(Position, point);
            if (dist < collisionDistance) {
                // Move camera away from collision point
                glm::vec3 direction = glm::normalize(Position - point);
                Position = point + direction * collisionDistance;
            }
        }

        updateCameraVectors();
    }

    // Reposition the orbit camera around its current target (Orbit/ThirdPerson
    // modes). Keeps yaw/pitch/distance and heals a degenerate WorldUp.
    void RepositionOrbit() {
        if (glm::length(WorldUp) < 1e-6f) WorldUp = glm::vec3(0.0f, 1.0f, 0.0f);
        updateCameraVectors();
    }

    // Mouse rotation - takes absolute positions from GLFW (PUBLIC)
    void ProcessMouseMovementAbsolute(float xpos, float ypos) {
        if (FirstMouse) { LastX = xpos; LastY = ypos; FirstMouse = false; }

        float xoffset = xpos - LastX;
        float yoffset = LastY - ypos; // reversed
        LastX = xpos;
        LastY = ypos;

        xoffset *= MouseSensitivity;
        yoffset *= MouseSensitivity;

        Yaw += xoffset * rotationSpeed;
        Pitch += yoffset * rotationSpeed;

        // Clamp pitch
        if (Pitch > 89.0f) Pitch = 89.0f;
        if (Pitch < -89.0f) Pitch = -89.0f;

        updateCameraVectors();
    }

private:
    CameraMode cameraMode = CameraMode::FIRST_PERSON;
    bool collisionEnabled = false;
    float collisionDistance = 0.5f;

    void updateCameraVectors() {
        // Heal a degenerate WorldUp (the NaN-poisoning bug): fall back to +Y.
        if (glm::length(WorldUp) < 1e-6f) WorldUp = glm::vec3(0.0f, 1.0f, 0.0f);

        // Calculate new position based on Yaw, Pitch, and DistanceToTarget
        // Yaw=-90 means looking down +Z, Pitch=0 means level
        float cosPitch = cos(glm::radians(Pitch));
        float sinPitch = sin(glm::radians(Pitch));
        float cosYaw = cos(glm::radians(Yaw));
        float sinYaw = sin(glm::radians(Yaw));

        glm::vec3 offset;
        offset.x = DistanceToTarget * cosYaw * cosPitch;
        offset.y = DistanceToTarget * sinPitch;
        offset.z = DistanceToTarget * sinYaw * cosPitch;

        Position = Target - offset;

        // Calculate right and up vectors (guard against NaN from parallel cross product).
        glm::vec3 front = Target - Position;
        if (glm::length(front) < 1e-6f) front = glm::vec3(0.0f, 0.0f, -1.0f);
        front = glm::normalize(front);
        if (glm::abs(glm::dot(front, WorldUp)) > 0.999f)
            Right = glm::normalize(glm::cross(front, glm::vec3(1.0f, 0.0f, 0.0f)));
        else
            Right = glm::normalize(glm::cross(front, WorldUp));
        Up = glm::normalize(glm::cross(Right, front));
    }
};

