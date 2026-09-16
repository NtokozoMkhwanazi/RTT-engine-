#pragma once
#include "MotionMatchingTypes.h"
#include "../boneSystem/Skeleton.h"
#include <glm/glm.hpp>

// ============================================================================
// FOOT PLANTING SYSTEM
// ============================================================================
// 
// Prevents foot sliding by LOCKING feet to the ground when planted.
// This is what separates AAA motion matching from amateur implementations.
// 
// How it works:
// 1. Detect when foot is "planted" (low velocity + on ground)
// 2. Remember the world position where it planted
// 3. Use IK to keep foot at that position while planted
// 4. Release when foot lifts for next step
// ============================================================================

class FootPlantingSystem {
public:
    FootPlantingSystem();
    ~FootPlantingSystem() = default;
    
    // =========================================================================
    // INITIALIZATION
    // =========================================================================
    
    /**
     * Initialize with skeleton bone indices
     */
    void Initialize(const Skeleton* skeleton);
    
    // =========================================================================
    // FOOT PLANT DETECTION
    // =========================================================================
    
    /**
     * Update foot plant state
     * 
     * Call every frame to detect which feet are planted.
     * 
     * @param leftFootPos Current left foot position (world space)
     * @param rightFootPos Current right foot position (world space)
     * @param leftFootVel Current left foot velocity
     * @param rightFootVel Current right foot velocity
     * @param groundHeight Height of ground at foot position
     * @param config Motion matching configuration
     */
    void Update(const glm::vec3& leftFootPos,
               const glm::vec3& rightFootPos,
               const glm::vec3& leftFootVel,
               const glm::vec3& rightFootVel,
               float groundHeight,
               const MotionMatchingConfig& config,
               float dt);
    
    /**
     * Check if foot is currently planted
     */
    bool IsLeftFootPlanted() const { return leftFoot.isLocked; }
    bool IsRightFootPlanted() const { return rightFoot.isLocked; }
    
    /**
     * Get planted foot position (world space, locked)
     */
    glm::vec3 GetLeftFootPosition() const { return leftFoot.plantedWorldPos; }
    glm::vec3 GetRightFootPosition() const { return rightFoot.plantedWorldPos; }

    /**
     * Set planted foot position (world space, locked). Used by the
     * MotionMatcher pelvis-damping pass to apply low-frequency smoothing
     * on the ground tracking positions during idle loops (Fix 3 from
     * updated todo).
     */
    void SetLeftFootPlantedPosition(const glm::vec3& pos) { leftFoot.plantedWorldPos = pos; }
    void SetRightFootPlantedPosition(const glm::vec3& pos) { rightFoot.plantedWorldPos = pos; }
    
    /**
     * Get lock weight (0 = free, 1 = fully locked, decaying otherwise)
     */
    float GetLeftFootLockWeight() const { return leftFoot.lockWeight; }
    float GetRightFootLockWeight() const { return rightFoot.lockWeight; }
    
    // =========================================================================
    // IK TARGETS
    // =========================================================================
    
    /**
     * Get IK target for foot
     * 
     * If foot is planted, returns locked position.
     * If foot is free, returns current animation position.
     * 
     * @param animFootPos Position from animation
     * @param isLeftFoot true for left foot
     * @return IK target position
     */
    glm::vec3 GetIKTarget(const glm::vec3& animFootPos, bool isLeftFoot) const;
    
    /**
     * Apply foot IK to skeleton
     * 
     * Modifies bone matrices to lock planted feet.
     */
    void ApplyFootIK(class Animator* animator,
                    const glm::mat4& modelMatrix,
                    float dt) const;
    
    // =========================================================================
    // DEBUG
    // =========================================================================
    
    /**
     * Get debug info string
     */
    std::string GetDebugInfo() const;
    
    /**
     * Foot state for debugging
     */
    struct FootDebugInfo {
        bool isLocked;
        glm::vec3 position;
        glm::vec3 plantedWorldPos;
        float velocity;
        float height;
        float plantTimer;
    };
    
    FootDebugInfo GetLeftFootDebug() const { 
        return {leftFoot.debug.isLocked, leftFoot.debug.position, 
                leftFoot.debug.plantedWorldPos, leftFoot.debug.velocity,
                leftFoot.debug.height, leftFoot.debug.plantTimer}; 
    }
    FootDebugInfo GetRightFootDebug() const { 
        return {rightFoot.debug.isLocked, rightFoot.debug.position, 
                rightFoot.debug.plantedWorldPos, rightFoot.debug.velocity,
                rightFoot.debug.height, rightFoot.debug.plantTimer}; 
    }
    
private:
    /**
     * State for one foot
     */
    struct FootState {
        // Current state
        bool isLocked{false};           // Is the foot firmly planted (IK locked)
        float lockWeight{0.0f};         // Smooth blend factor 0→1 (release ramp)
        glm::vec3 position{0.0f};
        glm::vec3 velocity{0.0f};
        
        // Plant data
        glm::vec3 plantedWorldPos{0.0f}; // World-space anchor where foot planted
        float plantTimer{0.0f};           // How long foot has been planted
        float releaseTimer{0.0f};         // Cooldown before can plant again
        
        // Bone indices
        int footBone{-1};
        int toeBone{-1};
        
        // Debug info
        struct {
            bool isLocked;
            glm::vec3 position;
            glm::vec3 plantedWorldPos;
            float velocity;
            float height;
            float plantTimer;
        } debug;
        
        // EMV deadband state (Fix 2 from updated todo: statistically-gated
        // plant confirmation replacing magic-number velocity thresholds).
        float speedMean = 0.0f;
        float speedVariance = 0.0f;

        /**
         * Update foot state
         */
        void Update(const glm::vec3& newPos,
                   const glm::vec3& newVel,
                   float groundHeight,
                   const MotionMatchingConfig& config,
                   float dt);
        
        /**
         * Reset debug info
         */
        void UpdateDebug();
    };
    
    FootState leftFoot;
    FootState rightFoot;
    
    bool initialized{false};
    
    /**
     * Check if foot should plant
     */
    bool ShouldPlant(const glm::vec3& position,
                    const glm::vec3& velocity,
                    float groundHeight,
                    const MotionMatchingConfig& config) const;
    
    /**
     * Check if foot should release
     */
    bool ShouldRelease(const FootState& foot,
                      const glm::vec3& newVel,
                      const MotionMatchingConfig& config) const;
};
