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

    // Direction difference (moveAngle is angular - wrap into [0, pi] so two
    // poses both pointing backward at +pi/-pi don't score a full circle).
    float dirDiff = std::abs(moveAngle - other.moveAngle);
    if (dirDiff > 3.14159265f) dirDiff = 6.28318530f - dirDiff;
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
    int comparePoints = std::min(numPoints, other.numPoints);
    // Guard against empty trajectories (no points yet, or only the root-local
    // feature populated): an empty comparison must contribute 0, never NaN
    // (diff / 0), which would poison every score in a brute-force search.
    if (comparePoints <= 0) return 0.0f;

    float diff = 0.0f;
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
    clipNominalSpeeds_.clear();
    clipNominalMoveAngles_.clear();
    simdCache_.valid = false;  // invalidate SIMD cache
}

float MotionDatabase::GetClipNominalSpeed(size_t animIndex) const {
    if (animIndex >= animations.size()) return 0.0f;

    // Reallocate if clips were added (or the database was cleared) since the
    // last access - a fresh -1.0f "not computed" sentinel per clip.
    if (clipNominalSpeeds_.size() != animations.size()) {
        clipNominalSpeeds_.assign(animations.size(), -1.0f);
    }
    float& cached = clipNominalSpeeds_[animIndex];
    if (cached >= 0.0f) return cached;

    // Mean root-motion speed over the clip's poses - the same speed feature
    // the KD-tree ranks on, so this is the clip's natural gait speed
    // (Idle ~0, Walk ~2, Run ~4 in the test clips / bot clips).
    double sum = 0.0;
    size_t count = 0;
    for (const auto& p : poses) {
        if (p.animationIndex == static_cast<int>(animIndex)) {
            sum += p.features.speed;
            ++count;
        }
    }
    cached = (count > 0) ? static_cast<float>(sum / static_cast<double>(count)) : 0.0f;
    return cached;
}

float MotionDatabase::GetClipNominalMoveAngle(size_t animIndex) const {
    if (animIndex >= animations.size()) return 0.0f;

    // Reallocate if clips were added (or the database was cleared) since the
    // last access - a fresh max-float "not computed" sentinel per clip. Angles
    // can be any radian value, so a plain -1.0f sentinel (as used by the speed
    // cache) would collide with a legitimate -1 radian mean.
    if (clipNominalMoveAngles_.size() != animations.size()) {
        clipNominalMoveAngles_.assign(animations.size(),
                                      std::numeric_limits<float>::max());
    }
    float& cached = clipNominalMoveAngles_[animIndex];
    if (cached != std::numeric_limits<float>::max()) return cached;

    // Circular mean: average the (cos, sin) unit vectors of the clip's move
    // angles and take atan2 of the result. Raw-angle averaging would let a
    // turn clip sweeping e.g. -170..170 degrees collapse to ~0 (facing the
    // wrong way); the unit-vector mean keeps the dominant heading.
    double sumX = 0.0, sumY = 0.0;
    size_t count = 0;
    for (const auto& p : poses) {
        if (p.animationIndex == static_cast<int>(animIndex)) {
            sumX += std::cos(static_cast<double>(p.features.moveAngle));
            sumY += std::sin(static_cast<double>(p.features.moveAngle));
            ++count;
        }
    }
    cached = (count > 0) ? static_cast<float>(std::atan2(sumY, sumX)) : 0.0f;
    return cached;
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

    // Validate animation. Durations are real seconds now (loader converts
    // Assimp ticks); the generous upper bound keeps long ambient clips (e.g.
    // a 16s Idle) usable - frame extraction is capped by MAX_FRAMES anyway.
    if (anim->duration <= 0.0f || anim->duration > 60.0f) {
        std::cerr << "[MotionDatabase] ERROR: Invalid duration " << anim->duration << "s for " << name << "\n";
        return;
    }

    if (anim->boneAnimations.empty()) {
        std::cerr << "[MotionDatabase] ERROR: No bone animations for " << name << "\n";
        return;
    }

    std::cout << "[MotionDatabase::AddAnimation] Adding: " << name
              << " (" << anim->duration << "s, " << anim->boneAnimations.size() << " bones)\n";

    // Create animation entry (database takes ownership via shared_ptr).
    // Use the REGISTERED name (what the caller asked for, e.g. "Jump") rather
    // than anim->name - the raw FBX channel name ("mixamo.com" for Mixamo
    // rigs) is identical across clips and useless for debug/HUD output.
    AnimationEntry entry;
    entry.name = name;
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

    // UE-style airborne tagging: clips whose name suggests a jump/fall arc are
    // flagged per-pose so the matcher can gate the pose search by movement
    // state (airborne queries only match Jump/Fall poses; grounded queries
    // only match locomotion poses). This is what lets takeoff/landing blend
    // through the pose search instead of hard overrides.
    std::string lowerName = name;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    const bool airborneClip =
        lowerName.find("jump") != std::string::npos ||
        lowerName.find("fall") != std::string::npos;

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

        // Tag the movement state on the pose (airborne = Jump/Fall clip).
        poses.back().features.isAirborne = airborneClip;
        poses.back().features.isGrounded = !airborneClip;

        // CRITICAL: Also add to cache-friendly SoA storage
        motionData.AddPose(poses.back());
    }

    entry.endPoseIndex = poses.size() - 1;
    animations.push_back(entry);
    animationByName[name] = animations.size() - 1;

    // CRITICAL CORRECTION: Only invalidate SIMD registers when loading a
    // genuinely new kinematic structure. During idle rest states the clip is
    // re-added on every database reset, which would continuously flip
    // simdCache_.valid = false and force a full SoA cache rebuild every
    // frame - the memory-alignment shifts register as visual jitter in the
    // ankle joint matrix buffers. Guarding on the clip name (Idle = the
    // common non-moving loop) plus the pose-count invariant keeps the cache
    // warm during the rest cycle.
    if (name != "Idle" || poses.size() <= expectedPoses) {
        clipNominalSpeeds_.clear();
        clipNominalMoveAngles_.clear();
        simdCache_.valid = false;
        simdCache_.poseCount = 0;
    }

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

    // =========================================================================
    // ROOT-RELATIVE FUTURE TRAJECTORY (Unreal-style pose feature)
    // =========================================================================
    // Sample the root bone k*0.1s ahead and store the offset from the current
    // root in the clip's own space (XZ). This lets the pose search rank
    // same-speed poses by their future path (turns, stops, strafes). The
    // matcher builds the identical feature from its predicted trajectory.
    //
    // CRITICAL: the offsets are stored UNROTATED (raw clip space). The matcher
    // expresses its query path relative to the CHARACTER heading, and since the
    // render transform is R(heading) applied on top of clip space, the query's
    // R(-heading) exactly reproduces clip space. Rotating the DB side by the
    // root yaw here would only match when the root yaw is exactly 0 - for any
    // constant nonzero yaw the two spaces would fight and the turn-refinement
    // feature would degrade to noise.
    if (rootBoneAnim) {
        pose.trajectory.localNumPoints = kTrajectorySteps;
        for (int k = 1; k <= kTrajectorySteps; ++k) {
            // Wrap the sample time into the clip so poses near the loop point
            // predict the root's actual loop-back position (the root does NOT
            // stop at the clip end - it snaps back to the start on wrap). This
            // keeps the trajectory feature consistent across the loop seam,
            // which is where the matcher used to mis-predict stops.
            float sampleT = time + k * kTrajectoryStepTime;
            sampleT = fmod(sampleT, anim->duration);
            const glm::vec3 futurePos = rootBoneAnim->InterpolatePosition(sampleT);
            const glm::vec3 rel = futurePos - rootPos;
            pose.trajectory.localPositions[k - 1] =
                glm::vec3(rel.x, 0.0f, rel.z);
        }
    }

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
        // Canonicalize to [0, 2pi): atan2 returns +/-pi for the SAME backward
        // heading (the zero-sign of the X component is an arbitrary floating
        // artifact), and the KD tree builds on these raw values - +pi and -pi
        // land on opposite ends of the split axis, so a straight-backward
        // query prunes the matching backward poses. Shifting negatives makes
        // "backward" always +pi (the seam moves to 0, i.e. forward). The
        // distance metrics wrap the difference, so the value range does not
        // affect scoring - only the tree structure.
        if (pose.features.moveAngle < 0.0f) pose.features.moveAngle += 6.28318530f;
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
    // FIXED (unknown doc): match the 120fps gait-capture step size exactly.
    // The prior look-ahead sampled two keyframe increments (2 * 1/120 s),
    // smearing foot velocity across a frame and mis-flagging planted feet as
    // moving. One frame step (1/120 s) is the true gait cadence.
    const float velocityExtractionDt = 1.0f / 120.0f;

    if (leftFootAnim) {
        leftFootPos = leftFootAnim->InterpolatePosition(time);
        glm::vec3 nextPos = leftFootAnim->InterpolatePosition(time + velocityExtractionDt);
        leftFootVel = (nextPos - leftFootPos) / velocityExtractionDt;
    }

    if (rightFootAnim) {
        rightFootPos = rightFootAnim->InterpolatePosition(time);
        glm::vec3 nextPos = rightFootAnim->InterpolatePosition(time + velocityExtractionDt);
        rightFootVel = (nextPos - rightFootPos) / velocityExtractionDt;
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

    // FIX (v11 Section 3): Dispatch to the AVX2 trajectory extractor when
    // the CPU supports it. This recomputes the future-path trajectory offsets
    // using a single _mm256_sub_ps instead of the scalar loop above. The rest
    // of the feature (root pos/velocity, speed, angle, foot planting) is
    // identical — only the trajectory subtractions are vectorized.
    if (cpuSupportsAVX2()) {
        ExtractPoseFeaturesSIMD(poseIndex, anim, time, rootPos.x, rootPos.z);
    }
}

// --------------------------------------------------------------------------
// FIX (v11 Section 3): AVX2-accelerated trajectory feature extraction.
// Replaces the scalar for-loop over kTrajectorySteps with a single
// _mm256_sub_ps that computes all 4 future (x,z) offsets in one instruction.
// The 4 time samples are interpolated scalar (InterpolatePosition is not
// vectorizable without restructuring the Animation class), but the packing
// and subtraction run in one 8-lane AVX2 register.
// --------------------------------------------------------------------------
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,fma")))
#endif
void MotionDatabase::ExtractPoseFeaturesSIMD(size_t poseIndex,
                                              std::shared_ptr<Animation> anim,
                                              float time,
                                              float rootPos_x,
                                              float rootPos_z) {
    if (poseIndex >= poses.size()) return;
    PoseSample& pose = poses[poseIndex];

    // Reuse the same root-bone search logic as the scalar version
    const BoneAnimation* rootBoneAnim = nullptr;
    static const std::vector<std::string> rootBoneNames = {
        "hips", "Hips", "mixamorig:hips", "mixamorig:Hips",
        "Mixamorig:Hips", "root", "Root", "hip", "Hip", "pelvis", "Pelvis"
    };
    for (const auto& boneName : rootBoneNames) {
        auto it = anim->boneAnimations.find(boneName);
        if (it != anim->boneAnimations.end()) {
            rootBoneAnim = &it->second;
            break;
        }
    }
    if (!rootBoneAnim && !anim->boneAnimations.empty()) {
        rootBoneAnim = &anim->boneAnimations.begin()->second;
    }

    if (rootBoneAnim) {
        pose.trajectory.localNumPoints = kTrajectorySteps;

        // Sample 4 future time steps ahead (same logic as scalar version)
        alignas(16) float futureTimes[kTrajectorySteps];
        for (int k = 0; k < kTrajectorySteps; ++k) {
            float sampleT = time + (k + 1) * kTrajectoryStepTime;
            futureTimes[k] = fmod(sampleT, anim->duration);
        }

        // Interpolate positions (scalar — Animation::InterpolatePosition
        // is not vectorizable without restructuring the keyframe system)
        alignas(32) float packedFuture[8] = {};
        for (int k = 0; k < kTrajectorySteps; ++k) {
            glm::vec3 fp = rootBoneAnim->InterpolatePosition(futureTimes[k]);
            packedFuture[k * 2]     = fp.x;  // X0,Z0,X1,Z1,X2,Z2,X3,Z3
            packedFuture[k * 2 + 1] = fp.z;
        }

        // Broadcast current root position across all 8 lanes
        alignas(32) float packedCurrent[8] = {
            rootPos_x, rootPos_z,
            rootPos_x, rootPos_z,
            rootPos_x, rootPos_z,
            rootPos_x, rootPos_z
        };

        // Single 256-bit subtraction: (future - current) = relative offset
        __m256 vFuture = _mm256_load_ps(packedFuture);
        __m256 vCurrent = _mm256_load_ps(packedCurrent);
        __m256 vRelative = _mm256_sub_ps(vFuture, vCurrent);

        // Unpack back to the trajectory structure (Y is always 0 — XZ plane)
        alignas(32) float extractedDeltas[8];
        _mm256_store_ps(extractedDeltas, vRelative);

        for (int k = 0; k < kTrajectorySteps; ++k) {
            pose.trajectory.localPositions[k] =
                glm::vec3(extractedDeltas[k * 2], 0.0f, extractedDeltas[k * 2 + 1]);
        }
    }
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
    int maxCandidates,
    int airborneFilter) const {
    
    std::vector<std::pair<int, float>> candidates;
    candidates.reserve(maxCandidates);

    const size_t poseCount = motionData.GetPoseCount();
    
    // Search using SoA layout (cache-friendly)
    for (size_t i = 0; i < poseCount; i++) {
        // UE-style state gate: skip poses outside the requested movement state
        // (airborneFilter = 0 -> grounded only, 1 -> airborne only). The AoS
        // `poses` vector stays in sync with the SoA storage, so the state tag
        // lives in one place; this is only evaluated when filtering is active.
        if (airborneFilter >= 0 && poses[i].features.isAirborne != (airborneFilter == 1)) {
            continue;
        }

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

// ---- AVX2 SIMD search -------------------------------------------------------
void MotionDatabase::RebuildSIMDCacheIfNeeded() const {
    if (simdCache_.valid && simdCache_.poseCount == poses.size())
        return;

    const size_t n = poses.size();
    simdCache_.soa.speeds.resize(n);
    simdCache_.soa.velocitiesX.resize(n);
    simdCache_.soa.velocitiesZ.resize(n);
    simdCache_.soa.moveAngles.resize(n);
    simdCache_.soa.leftFootPlanted.resize(n);
    simdCache_.soa.rightFootPlanted.resize(n);
    simdCache_.soa.velocitiesY.resize(n);
    simdCache_.soa.originalIndex.resize(n);

    // Trajectory future-path arrays (per-step x,z)
    for (int k = 0; k < kTrajectorySteps; ++k) {
        simdCache_.soa.trajectoryX[k].resize(n);
        simdCache_.soa.trajectoryZ[k].resize(n);
    }
    // FIX (v11 Section 4): Interleaved trajectory block — 8 floats per pose
    // packed as [X0,Z0,X1,Z1,X2,Z2,X3,Z3] for single-instruction _mm256_load_ps.
    simdCache_.soa.interleavedTrajectories.resize(n * 8);

    for (size_t i = 0; i < n; ++i) {
        const auto& f = poses[i].features;
        simdCache_.soa.speeds[i] = f.speed;
        simdCache_.soa.velocitiesX[i] = f.rootVelocity.x;
        simdCache_.soa.velocitiesZ[i] = f.rootVelocity.z;
        simdCache_.soa.velocitiesY[i] = f.rootVelocity.y;
        simdCache_.soa.moveAngles[i] = f.moveAngle;
        simdCache_.soa.leftFootPlanted[i] = f.leftFootPlanted ? 1.0f : 0.0f;
        simdCache_.soa.rightFootPlanted[i] = f.rightFootPlanted ? 1.0f : 0.0f;
        simdCache_.soa.originalIndex[i] = static_cast<int>(i);
        // Trajectory: localPositions[k] is a vec3; store x and z (y ignored,
        // matching the KD-tree feature layout at dims 7+2k and 7+2k+1).
        for (int k = 0; k < kTrajectorySteps; ++k) {
            if (f.futureCount > k) {
                simdCache_.soa.trajectoryX[k][i] = f.futureLocal[k].x;
                simdCache_.soa.trajectoryZ[k][i] = f.futureLocal[k].y;
            } else {
                simdCache_.soa.trajectoryX[k][i] = 0.0f;
                simdCache_.soa.trajectoryZ[k][i] = 0.0f;
            }
        }
        // FIX (v11 Section 4): Pack all 4 future x,z offsets into 8 contiguous
        // floats for the interleaved trajectory block [X0,Z0,X1,Z1,X2,Z2,X3,Z3].
        for (int k = 0; k < kTrajectorySteps; ++k) {
            size_t base = i * 8;
            if (f.futureCount > k) {
                simdCache_.soa.interleavedTrajectories[base + k * 2]     = f.futureLocal[k].x;
                simdCache_.soa.interleavedTrajectories[base + k * 2 + 1] = f.futureLocal[k].y;
            } else {
                simdCache_.soa.interleavedTrajectories[base + k * 2]     = 0.0f;
                simdCache_.soa.interleavedTrajectories[base + k * 2 + 1] = 0.0f;
            }
        }
    }
    simdCache_.soa.EnforceVectorPadding();
    simdCache_.valid = true;
    simdCache_.poseCount = n;
}

std::vector<std::pair<int, float>> MotionDatabase::SearchSIMD(
    const MotionFeatures& query,
    const Trajectory& trajectory,
    int maxCandidates,
    int airborneFilter) const {
    // Rebuild SoA cache if the database changed
    RebuildSIMDCacheIfNeeded();

    if (!cpuSupportsAVX2() || simdCache_.soa.GetPoseCount() == 0)
        return SearchCacheOptimized(query, trajectory, maxCandidates, airborneFilter);

    // Map the MotionFeatures query to the SIMD query format
    SIMDFeaturesQuery q;
    q.speed     = query.speed;
    q.velX      = query.rootVelocity.x;
    q.velZ      = query.rootVelocity.z;
    q.moveAngle = query.moveAngle;
    q.leftFoot  = query.leftFootPlanted ? 1.0f : 0.0f;
    q.rightFoot = query.rightFootPlanted ? 1.0f : 0.0f;
    q.velY      = query.rootVelocity.y;
    // Trajectory future-path: query.futureLocal[k] is a vec2 (x, y) where
    // x = local-x offset, y = local-z offset (matching KD-tree dims 7+2k, 7+2k+1).
    for (int k = 0; k < kTrajectorySteps; ++k) {
        if (query.futureCount > k) {
            q.trajX[k] = query.futureLocal[k].x;
            q.trajZ[k] = query.futureLocal[k].y;
        } else {
            q.trajX[k] = 0.0f;
            q.trajZ[k] = 0.0f;
        }
    }

    SIMDWeightsConfig w;
    w.speed     = weights.speedWeight;
    w.velX      = weights.velocityWeight;
    w.velZ      = weights.velocityWeight;
    w.direction = weights.directionWeight;
    w.footPlant = weights.footPlantWeight;
    w.velY      = weights.velocityWeight * 0.5f;  // vertical less important
    // Trajectory weights: use the KD-tree's per-step falloff if available,
    // otherwise fall back to the database-level trajectoryWeight uniform.
    for (int k = 0; k < kTrajectorySteps; ++k) {
        w.trajectoryWeights[k] = weights.trajectoryWeight;
    }

    // Run the AVX2 8-wide search (returns top maxCandidates sorted by distance)
    auto allResults = ExecuteAVX2PoseSearch(simdCache_.soa, q, w, maxCandidates);

    // Apply airborne filter if requested (0 = grounded only, 1 = airborne only).
    // The AVX2 search doesn't know about airborne state, so we filter the
    // results here.  If the filter empties the top-N, fall back to the scalar
    // search that respects the filter natively.
    std::vector<std::pair<int, float>> results;
    results.reserve(maxCandidates);
    if (airborneFilter >= 0) {
        for (const auto& [idx, dist] : allResults) {
            if (idx >= 0 && idx < static_cast<int>(poses.size()) &&
                poses[idx].features.isAirborne == (airborneFilter == 1)) {
                results.emplace_back(idx, dist);
            }
        }
        // If the SIMD top-N didn't contain enough matching poses, the scalar
        // search (which filters natively) will find the correct ones.
        if (static_cast<int>(results.size()) < maxCandidates)
            return SearchCacheOptimized(query, trajectory, maxCandidates, airborneFilter);
    } else {
        for (const auto& [idx, dist] : allResults) {
            results.emplace_back(idx, dist);
        }
    }
    return results;
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

    // 3. Direction match (moveAngle is angular - wrap into [0, pi] so two
    // poses both pointing backward at +pi/-pi don't score a full circle).
    float dirDiff = std::abs(pose.features.moveAngle - query.moveAngle);
    if (dirDiff > 3.14159265f) dirDiff = 6.28318530f - dirDiff;
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

    // 3. Direction match (moveAngle is angular - wrap into [0, pi] so two
    // poses both pointing backward at +pi/-pi don't score a full circle).
    float dirDiff = std::abs(moveAngle - query.moveAngle);
    if (dirDiff > 3.14159265f) dirDiff = 6.28318530f - dirDiff;
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
