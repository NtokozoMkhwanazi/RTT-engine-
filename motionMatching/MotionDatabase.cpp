#include "MotionDatabase.h"
#include "../animationSystem/Animator.h"
#include "../boneSystem/Skeleton.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cctype>

// ============================================================================
// MOTION FEATURES IMPLEMENTATION
// ============================================================================

float MotionFeatures::getDifference(const MotionFeatures& other) const {
    float diff = 0.0f;

    // Velocity difference (most important)
    glm::vec3 velDiff = rootVelocity - other.rootVelocity;
    diff += glm::length(velDiff) * 3.0f;

    // Speed difference
    float speedDiff = std::abs(speed - other.speed);
    diff += speedDiff * 2.0f;

    // Direction difference
    float dirDiff = std::abs(moveAngle - other.moveAngle);
    diff += dirDiff * 2.0f;

    // Foot plant state (binary penalty)
    if (leftFootPlanted != other.leftFootPlanted) diff += 1.0f;
    if (rightFootPlanted != other.rightFootPlanted) diff += 1.0f;

    // Crouch state
    if (isCrouching != other.isCrouching) diff += 0.5f;

    return diff;
}

// ============================================================================
// TRAJECTORY IMPLEMENTATION
// ============================================================================

glm::vec3 Trajectory::getPositionAt(float timeOffset) const {
    int pointIndex = static_cast<int>(timeOffset / 0.1f);
    if (pointIndex < 0 || pointIndex >= numPoints) return positions[0];
    return positions[pointIndex];
}

glm::vec3 Trajectory::getVelocityAt(float timeOffset) const {
    int pointIndex = static_cast<int>(timeOffset / 0.1f);
    if (pointIndex < 0 || pointIndex >= numPoints) return velocities[0];
    return velocities[pointIndex];
}

float Trajectory::getDifference(const Trajectory& other) const {
    float diff = 0.0f;
    int comparePoints = std::min(numPoints, other.numPoints);

    for (int i = 0; i < comparePoints; i++) {
        glm::vec3 posDiff = positions[i] - other.positions[i];
        diff += glm::length(posDiff);

        glm::vec3 velDiff = velocities[i] - other.velocities[i];
        diff += glm::length(velDiff) * 0.5f;
    }

    return diff / comparePoints;
}

// ============================================================================
// MOTION DATABASE IMPLEMENTATION (FIXED - Proper shared_ptr ownership)
// ============================================================================

MotionDatabase::MotionDatabase() {
    std::cout << "[MotionDatabase] Constructor called - this=" << this << "\n";
    // Default weights tuned for third-person character
    // CRITICAL: speedWeight must be VERY HIGH to ensure correct animation selection
    weights.velocityWeight = 5.0f;   // HIGH - velocity match is most important
    weights.speedWeight = 20.0f;     // VERY HIGH - speed magnitude is THE most important
    weights.directionWeight = 0.3f;  // Low - direction can be corrected by blending
    weights.trajectoryWeight = 1.0f;
    weights.footPlantWeight = 0.5f;  // Reduced - foot plant shouldn't override speed
    weights.crouchWeight = 0.5f;
}

MotionDatabase::~MotionDatabase() {
    std::cout << "[MotionDatabase] Destructor called - this=" << this << " poses=" << poses.size() << "\n";
    Clear();
    std::cout << "[MotionDatabase] Destructor complete\n";
}

// Move constructor
MotionDatabase::MotionDatabase(MotionDatabase&& other) noexcept
    : poses(std::move(other.poses))
    , motionData(std::move(other.motionData))
    , animations(std::move(other.animations))
    , animationByName(std::move(other.animationByName))
    , weights(other.weights) {
    // other is now in valid but unspecified state
}

// Move assignment
MotionDatabase& MotionDatabase::operator=(MotionDatabase&& other) noexcept {
    if (this != &other) {
        Clear();
        poses = std::move(other.poses);
        motionData = std::move(other.motionData);
        animations = std::move(other.animations);
        animationByName = std::move(other.animationByName);
        weights = other.weights;
    }
    return *this;
}

void MotionDatabase::Clear() {
    poses.clear();
    motionData.Clear();  // Clear SoA data too
    animations.clear();
    animationByName.clear();
}

// CRITICAL FIX: Takes shared_ptr - caller MUST use std::make_shared
void MotionDatabase::AddAnimation(const std::string& name, std::shared_ptr<Animation> anim,
                                   const Skeleton* skeleton) {
    if (!anim || !skeleton) {
        std::cerr << "[MotionDatabase] ERROR: Null animation or skeleton for " << name << "\n";
        return;
    }

    // Validate name
    if (name.empty()) {
        std::cerr << "[MotionDatabase] ERROR: Empty animation name\n";
        return;
    }

    // Validate animation
    if (anim->duration <= 0.0f || anim->duration > 10.0f) {
        std::cerr << "[MotionDatabase] ERROR: Invalid duration " << anim->duration << "s for " << name << "\n";
        return;
    }

    if (anim->boneAnimations.empty()) {
        std::cerr << "[MotionDatabase] ERROR: No bone animations for " << name << "\n";
        return;
    }

    std::cout << "[MotionDatabase::AddAnimation] Adding: " << name
              << " (" << anim->duration << "s, " << anim->boneAnimations.size() << " bones)\n";

    // Create animation entry (database takes ownership via shared_ptr)
    AnimationEntry entry;
    entry.name = anim->name;
    entry.animation = anim;
    entry.startPoseIndex = poses.size();

    // CRITICAL FIX: Extract at 120fps for smooth gait cycle capture
    // 60fps misses subtle foot plant/lift transitions
    // A full gait cycle needs ~100-150 frames for proper foot planting
    float fps = 120.0f;  // Increased from 60 to 120 for better gait capture
    float frameTime = 1.0f / fps;
    int numFrames = static_cast<int>(anim->duration * fps);

    // Increased limit to accommodate full gait cycles at 120fps
    // Walk (2.0s @ 120fps) = 240 frames
    // Run (1.5s @ 120fps) = 180 frames
    const int MAX_FRAMES = 800;  // Increased from 600
    if (numFrames > MAX_FRAMES) {
        std::cerr << "[MotionDatabase] WARNING: Truncating " << name << " from " << numFrames
                  << " to " << MAX_FRAMES << " frames\n";
        numFrames = MAX_FRAMES;
    }

    std::cout << "  Extracting " << numFrames << " frames at " << fps << "fps (HIGH RES for gait cycle)\n";

    // Pre-reserve memory (prevents reallocations)
    size_t expectedPoses = poses.size() + numFrames;
    if (poses.capacity() < expectedPoses) {
        poses.reserve(expectedPoses);
        motionData.Reserve(expectedPoses);
    }

    for (int frame = 0; frame < numFrames; frame++) {
        float time = frame * frameTime;

        // Create pose sample
        PoseSample pose;
        pose.animationIndex = static_cast<int>(animations.size());
        pose.timeInSeconds = time;
        pose.frameNumber = frame;

        // Set links to adjacent frames
        if (frame > 0) {
            pose.prevFrameIndex = static_cast<int>(poses.size()) - 1;
        }
        if (frame < numFrames - 1) {
            pose.nextFrameIndex = static_cast<int>(poses.size()) + 1;
        }

        // Add pose to database first, then extract features
        poses.push_back(pose);

        // Extract motion features for this frame
        ExtractPoseFeatures(poses.size() - 1, anim, time, skeleton);

        // CRITICAL: Also add to cache-friendly SoA storage
        motionData.AddPose(poses.back());
    }

    entry.endPoseIndex = poses.size() - 1;
    animations.push_back(entry);
    animationByName[name] = animations.size() - 1;

    std::cout << "  Added " << numFrames << " pose samples (SoA cache-optimized)\n";
}

void MotionDatabase::ExtractPoseFeatures(size_t poseIndex, std::shared_ptr<Animation> anim,
                                          float time, const Skeleton* skeleton) {
    if (poseIndex >= poses.size()) {
        return;
    }
    PoseSample& pose = poses[poseIndex];

    // Store animation info
    pose.features.animationTime = time;
    pose.features.animationDuration = anim->duration;
    pose.features.animationIndex = pose.animationIndex;
    pose.features.frameIndex = pose.frameNumber;

    // Sample animation to get root position at current time
    glm::vec3 rootPos = glm::vec3(0.0f);
    glm::vec3 rootVel = glm::vec3(0.0f);

    // Find root bone animation (search for common root bone names)
    const BoneAnimation* rootBoneAnim = nullptr;

    static const std::vector<std::string> rootBoneNames = {
        "hips", "Hips", "mixamorig:hips", "mixamorig:Hips", 
        "Mixamorig:Hips", "root", "Root", "hip", "Hip", "pelvis", "Pelvis"
    };

    // Search for root bone by name
    for (const auto& boneName : rootBoneNames) {
        auto it = anim->boneAnimations.find(boneName);
        if (it != anim->boneAnimations.end()) {
            rootBoneAnim = &it->second;
            break;
        }
    }

    // Fallback: use first bone if no root found
    if (!rootBoneAnim && !anim->boneAnimations.empty()) {
        rootBoneAnim = &anim->boneAnimations.begin()->second;
    }

    if (rootBoneAnim) {
        // Sample position at current time
        rootPos = rootBoneAnim->InterpolatePosition(time);

        // Calculate velocity from position delta (sample 1/60s ahead)
        float dt = 1.0f / 60.0f;
        glm::vec3 nextPos = rootBoneAnim->InterpolatePosition(time + dt);
        rootVel = (nextPos - rootPos) / dt;
    }

    // Store root position and rotation
    pose.rootPosition = rootPos;
    pose.rootRotationY = 0.0f;

    // Calculate speed from root velocity (XZ plane only)
    float speed = glm::length(glm::vec3(rootVel.x, 0.0f, rootVel.z));
    pose.features.speed = speed;
    pose.features.rootVelocity = rootVel;

    // Debug: print speed for first frame of each animation
    if (poseIndex == 0 || (pose.frameNumber == 0)) {
        std::cout << "  [Pose " << poseIndex << "] " << anim->name << " speed=" << speed 
                  << " vel=(" << rootVel.x << "," << rootVel.z << ")\n";
    }

    // Calculate move angle from velocity direction
    if (speed > 0.001f) {
        pose.features.moveAngle = atan2(rootVel.x, rootVel.z);
    } else {
        pose.features.moveAngle = 0.0f;
    }

    // =========================================================================
    // FOOT PLANTING DETECTION - CRITICAL FOR PREVENTING FOOTSKATING
    // =========================================================================
    
    // Find foot bones
    const BoneAnimation* leftFootAnim = nullptr;
    const BoneAnimation* rightFootAnim = nullptr;
    
    static const std::vector<std::string> leftFootNames = {"leftfoot", "LeftFoot", "mixamorig:LeftFoot"};
    static const std::vector<std::string> rightFootNames = {"rightfoot", "RightFoot", "mixamorig:RightFoot"};
    
    for (const auto& name : leftFootNames) {
        auto it = anim->boneAnimations.find(name);
        if (it != anim->boneAnimations.end()) {
            leftFootAnim = &it->second;
            break;
        }
    }
    
    for (const auto& name : rightFootNames) {
        auto it = anim->boneAnimations.find(name);
        if (it != anim->boneAnimations.end()) {
            rightFootAnim = &it->second;
            break;
        }
    }
    
    // Calculate foot positions and velocities
    glm::vec3 leftFootPos(0.0f), leftFootVel(0.0f);
    glm::vec3 rightFootPos(0.0f), rightFootVel(0.0f);
    
    if (leftFootAnim) {
        leftFootPos = leftFootAnim->InterpolatePosition(time);
        glm::vec3 nextPos = leftFootAnim->InterpolatePosition(time + 0.016f);
        leftFootVel = (nextPos - leftFootPos) / 0.016f;
    }
    
    if (rightFootAnim) {
        rightFootPos = rightFootAnim->InterpolatePosition(time);
        glm::vec3 nextPos = rightFootAnim->InterpolatePosition(time + 0.016f);
        rightFootVel = (nextPos - rightFootPos) / 0.016f;
    }
    
    // Store foot heights
    pose.leftFootHeight = leftFootPos.y;
    pose.rightFootHeight = rightFootPos.y;
    
    // Detect foot planting based on velocity and height
    // Foot is planted when:
    // 1. Near ground (low Y position relative to ankle)
    // 2. Moving slowly (low horizontal velocity)
    float footPlantVelThreshold = 0.25f;  // Velocity threshold for planting
    float footPlantHeightThreshold = 0.15f;  // Height threshold relative to min foot height
    
    // Calculate horizontal foot speed (ignore vertical motion)
    float leftFootSpeed = glm::length(glm::vec2(leftFootVel.x, leftFootVel.z));
    float rightFootSpeed = glm::length(glm::vec2(rightFootVel.x, rightFootVel.z));
    
    // Simple foot plant detection
    // Note: For more accurate detection, we would analyze the full animation
    // to find the minimum foot height and use that as reference
    pose.leftFootPlanted = (leftFootSpeed < footPlantVelThreshold && 
                            leftFootPos.y < footPlantHeightThreshold &&
                            leftFootVel.y < 0.1f);  // Not moving up
                            
    pose.rightFootPlanted = (rightFootSpeed < footPlantVelThreshold && 
                             rightFootPos.y < footPlantHeightThreshold &&
                             rightFootVel.y < 0.1f);  // Not moving up
    
    // Store foot velocities for motion matching
    pose.features.leftFootVel = leftFootVel;
    pose.features.rightFootVel = rightFootVel;
    pose.features.leftFootPos = leftFootPos;
    pose.features.rightFootPos = rightFootPos;
}

SearchResult MotionDatabase::Search(const MotionFeatures& query,
                                     const Trajectory& trajectory) const {
    SearchResult best;
    best.score = std::numeric_limits<float>::max();

    // Search all poses (brute force - we'll optimize with KD-tree later)
    for (size_t i = 0; i < poses.size(); i++) {
        const PoseSample& pose = poses[i];

        // Calculate match score
        float score = CalculatePoseScore(pose, query, trajectory, MotionMatchingConfig());

        if (score < best.score) {
            best.score = score;
            best.poseIndex = static_cast<int>(i);
            best.blendWeight = 1.0f;
        }
    }

    return best;
}

SearchResults MotionDatabase::SearchWithBlending(const MotionFeatures& query,
                                                  const Trajectory& trajectory,
                                                  const MotionMatchingConfig& config) const {
    SearchResults results;
    results.totalSearched = static_cast<int>(poses.size());

    // Find top N candidates using cache-optimized search
    auto candidates = SearchCacheOptimized(query, trajectory, config.maxSearchResults);

    // Get best and second best for blending
    if (candidates.size() > 0) {
        results.best.poseIndex = candidates[0].first;
        results.best.score = candidates[0].second;
        results.best.blendWeight = 1.0f;
    }
    if (candidates.size() > 1) {
        results.second.poseIndex = candidates[1].first;
        results.second.score = candidates[1].second;
        results.second.blendWeight = 0.0f;  // Will be calculated by matcher
    }

    return results;
}

// CRITICAL: Cache-optimized search using SoA layout
std::vector<std::pair<int, float>> MotionDatabase::SearchCacheOptimized(
    const MotionFeatures& query,
    const Trajectory& trajectory,
    int maxCandidates) const {
    
    std::vector<std::pair<int, float>> candidates;
    candidates.reserve(maxCandidates);

    const size_t poseCount = motionData.GetPoseCount();
    
    // Search using SoA layout (cache-friendly)
    for (size_t i = 0; i < poseCount; i++) {
        // Calculate score using cache-optimized method
        float score = CalculatePoseScoreCacheOptimized(i, query, trajectory, MotionMatchingConfig());

        // Insert into sorted candidates
        if (candidates.empty() || score < candidates.back().second) {
            std::pair<int, float> result;
            result.first = static_cast<int>(i);
            result.second = score;

            // Insert in sorted order
            auto it = std::lower_bound(candidates.begin(), candidates.end(), result,
                [](const std::pair<int, float>& a, const std::pair<int, float>& b) {
                    return a.second < b.second;
                });
            candidates.insert(it, result);

            // Keep only top N
            if (static_cast<int>(candidates.size()) > maxCandidates) {
                candidates.pop_back();
            }
        }
    }

    return candidates;
}

float MotionDatabase::CalculatePoseScore(const PoseSample& pose,
                                          const MotionFeatures& query,
                                          const Trajectory& trajectory,
                                          const MotionMatchingConfig& config) const {
    float score = 0.0f;

    // 1. Velocity match (most important)
    glm::vec3 velDiff = pose.features.rootVelocity - query.rootVelocity;
    score += glm::length(velDiff) * weights.velocityWeight;

    // 2. Speed match
    float speedDiff = std::abs(pose.features.speed - query.speed);
    score += speedDiff * weights.speedWeight;

    // 3. Direction match
    float dirDiff = std::abs(pose.features.moveAngle - query.moveAngle);
    score += dirDiff * weights.directionWeight;

    // 4. Trajectory match (if enabled)
    if (config.useTrajectoryMatching) {
        float trajDiff = pose.trajectory.getDifference(trajectory);
        score += trajDiff * weights.trajectoryWeight;
    }

    // 5. Foot plant match
    if (pose.leftFootPlanted != query.leftFootPlanted) {
        score += weights.footPlantWeight;
    }
    if (pose.rightFootPlanted != query.rightFootPlanted) {
        score += weights.footPlantWeight;
    }

    // 6. Crouch state match
    if (pose.features.isCrouching != query.isCrouching) {
        score += weights.crouchWeight;
    }

    return score;
}

// CRITICAL: Cache-optimized score calculation using SoA layout
float MotionDatabase::CalculatePoseScoreCacheOptimized(size_t poseIndex,
                                                       const MotionFeatures& query,
                                                       const Trajectory& trajectory,
                                                       const MotionMatchingConfig& config) const {
    float score = 0.0f;

    // Access features from SoA storage (contiguous memory = cache-friendly)
    const float speed = motionData.GetSpeed(poseIndex);
    const glm::vec3& velocity = motionData.GetVelocity(poseIndex);
    const float moveAngle = motionData.GetMoveAngle(poseIndex);
    const bool leftFootPlanted = motionData.IsLeftFootPlanted(poseIndex);
    const bool rightFootPlanted = motionData.IsRightFootPlanted(poseIndex);

    // 1. Velocity match (most important)
    glm::vec3 velDiff = velocity - query.rootVelocity;
    score += glm::length(velDiff) * weights.velocityWeight;

    // 2. Speed match
    float speedDiff = std::abs(speed - query.speed);
    score += speedDiff * weights.speedWeight;

    // 3. Direction match
    float dirDiff = std::abs(moveAngle - query.moveAngle);
    score += dirDiff * weights.directionWeight;

    // 4. Trajectory match (if enabled)
    if (config.useTrajectoryMatching && poseIndex < poses.size()) {
        float trajDiff = poses[poseIndex].trajectory.getDifference(trajectory);
        score += trajDiff * weights.trajectoryWeight;
    }

    // 5. Foot plant match
    if (leftFootPlanted != query.leftFootPlanted) {
        score += weights.footPlantWeight;
    }
    if (rightFootPlanted != query.rightFootPlanted) {
        score += weights.footPlantWeight;
    }

    // 6. Crouch state match (would need to add to SoA if used frequently)
    if (poseIndex < poses.size() && poses[poseIndex].features.isCrouching != query.isCrouching) {
        score += weights.crouchWeight;
    }

    return score;
}

std::shared_ptr<Animation> MotionDatabase::GetAnimation(size_t index) const {
    if (index >= animations.size()) return nullptr;
    return animations[index].animation;
}

std::shared_ptr<Animation> MotionDatabase::GetAnimation(const std::string& name) const {
    auto it = animationByName.find(name);
    if (it == animationByName.end()) return nullptr;
    return animations[it->second].animation;
}

void MotionDatabase::PrintStats() const {
    std::cout << "\n=== MOTION DATABASE STATS ===\n";
    std::cout << "Total poses: " << poses.size() << "\n";
    std::cout << "Total animations: " << animations.size() << "\n";
    std::cout << "Cache-friendly SoA data: " << (motionData.GetPoseCount() == poses.size() ? "YES" : "NO") << "\n";

    for (const auto& anim : animations) {
        size_t poseCount = anim.endPoseIndex - anim.startPoseIndex + 1;
        std::cout << "  " << anim.name << ": " << poseCount << " poses\n";
    }
}

std::string MotionDatabase::GetDebugInfo() const {
    std::string info = "Motion Database: " + std::to_string(poses.size()) + " poses, " +
           std::to_string(animations.size()) + " animations";

    // Include animation names
    info += " [";
    for (size_t i = 0; i < animations.size(); i++) {
        if (i > 0) info += ", ";
        info += animations[i].name;
    }
    info += "]";
    
    info += " (SoA: " + std::to_string(motionData.GetPoseCount()) + " poses)";

    return info;
}
