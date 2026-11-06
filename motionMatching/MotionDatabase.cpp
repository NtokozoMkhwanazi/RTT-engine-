#include "MotionDatabase.h"
#include "../animationSystem/Animator.h"
#include "../boneSystem/Skeleton.h"
#include <iostream>
#include <algorithm>
#include <chrono>
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
        weights = std::move(other.weights);  // FIX: was a copy, not a move
        clipNominalSpeeds_ = std::move(other.clipNominalSpeeds_);
        clipNominalMoveAngles_ = std::move(other.clipNominalMoveAngles_);
        simdCache_ = std::move(other.simdCache_);

        // Ensure moved-from object is left in a valid, clean state.
        other.simdCache_.valid = false;
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

        // Calculate velocity from position delta (sample 1/120s ahead)
        // Synchronized with the high-resolution 120fps keyframe sampling to
        // avoid temporal smearing in root velocity vectors.
        const float velocityExtractionDt = 1.0f / 120.0f;
        glm::vec3 nextPos = rootBoneAnim->InterpolatePosition(time + velocityExtractionDt);
        rootVel = (nextPos - rootPos) / velocityExtractionDt;

        // Yaw rate of the root bone over the extraction window (rad/s). Feeds
        // the ghost-node turn-in-place score (forAnime.txt L1131-1138): a pose
        // that spins in place carries high angular velocity and wins the
        // turn-in-place search pass. Guarded to the existing root bone only;
        // time is wrapped into [0,duration) for the look-ahead sample (mirrors
        // the root-relative trajectory look-ahead @404).
        glm::quat rNow  = rootBoneAnim->InterpolateRotation(time);
        glm::quat rNext = rootBoneAnim->InterpolateRotation(fmod(time + velocityExtractionDt, anim->duration));
        auto yawOf = [](const glm::quat& q) -> float {
            return 2.0f * atan2(q.y, q.w);
        };
        float yawDelta = yawOf(rNext) - yawOf(rNow);
        while (yawDelta > 3.14159265f)  yawDelta -= 6.28318530f;
        while (yawDelta < -3.14159265f) yawDelta += 6.28318530f;
        pose.features.angularVelocity = yawDelta / velocityExtractionDt;
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

namespace {
// Process-wide switch that pins SearchCacheOptimized to its scalar reference
// loop (see MotionDatabase::SetForceScalarSearch).
bool gForceScalarSearch = false;
// Process-wide switch for the root-relative future-path term in the flat
// search (see MotionDatabase::SetFlatSearchUsesTrajectory).
bool gFlatSearchUsesTrajectory = true;
}  // namespace

void MotionDatabase::SetForceScalarSearch(bool force) { gForceScalarSearch = force; }
bool MotionDatabase::IsForceScalarSearch() { return gForceScalarSearch; }
void MotionDatabase::SetFlatSearchUsesTrajectory(bool on) { gFlatSearchUsesTrajectory = on; }
bool MotionDatabase::FlatSearchUsesTrajectory() { return gFlatSearchUsesTrajectory; }

float MotionDatabase::CalculateTrajectoryTerm(const PoseSample& pose,
                                              const MotionFeatures& query) const {
    if (!gFlatSearchUsesTrajectory) return 0.0f;
    if (pose.trajectory.localNumPoints <= 0) return 0.0f;

    const int cmp = std::max(0, std::min(kTrajectorySteps, query.futureCount));
    if (cmp == 0) return 0.0f;

    float wSum = 0.0f;
    float acc = 0.0f;
    for (int k = 0; k < cmp; ++k) {
        const float w = trajectoryFalloff_[k];
        // query.futureLocal is (x, z) stored as (x, y); the pose's path is a
        // glm::vec3 whose x/z carry the same two components (the KD tree makes
        // the identical x -> x, y -> z mapping).
        const float dx = pose.trajectory.localPositions[k].x - query.futureLocal[k].x;
        const float dz = pose.trajectory.localPositions[k].z - query.futureLocal[k].y;
        acc += w * std::sqrt(dx * dx + dz * dz);
        wSum += w;
    }
    if (wSum <= 0.0f) return 0.0f;
    return weights.trajectoryWeight * acc / wSum;
}

// Real, measured A/B of the two flat-search implementations on this database.
// Used by the PlayMode HUD so the displayed "SIMD x.x×" figure is a
// measurement of the live path rather than a compile-time assumption.
double MotionDatabase::MeasureFlatSearchSpeedup(int iterations) const {
    if (iterations <= 0 || poses.empty() || !cpuSupportsAVX2()) return 0.0;

    // Build a deterministic query load straight from the database: every N-th
    // pose contributes its own current features + future path, so the search
    // runs against the feature distribution it will actually see.
    std::vector<MotionFeatures> queries;
    const size_t stride = std::max<size_t>(1, poses.size() / 32);
    for (size_t i = 0; i < poses.size(); i += stride) {
        const PoseSample& p = poses[i];
        MotionFeatures q = p.features;
        q.futureCount = std::max(0, std::min(kTrajectorySteps, p.trajectory.localNumPoints));
        for (int k = 0; k < q.futureCount; ++k) {
            q.futureLocal[k] = glm::vec2(p.trajectory.localPositions[k].x,
                                         p.trajectory.localPositions[k].z);
        }
        queries.push_back(q);
    }
    if (queries.empty()) return 0.0;

    const Trajectory trajectory{};
    const auto run = [&](bool forceScalar) {
        const bool saved = gForceScalarSearch;
        gForceScalarSearch = forceScalar;
        for (const MotionFeatures& q : queries) {
            (void)SearchCacheOptimized(q, trajectory, 10, 0);   // warm-up
        }
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (int it = 0; it < iterations; ++it) {
            for (const MotionFeatures& q : queries) {
                (void)SearchCacheOptimized(q, trajectory, 10, 0);
            }
        }
        const auto t1 = std::chrono::high_resolution_clock::now();
        gForceScalarSearch = saved;
        return std::chrono::duration<double, std::micro>(t1 - t0).count();
    };

    const double scalarUs = run(true);
    const double avx2Us = run(false);
    if (avx2Us <= 0.0) return 0.0;
    return scalarUs / avx2Us;
}

// Cache-optimized search. The AVX2 kernel computes the identical metric as
// SearchCacheOptimizedScalar below (that is what tests/test_motion_simd.cpp
// asserts), so this is a pure implementation swap: same weights, same state
// gate, same candidate ordering rules.
std::vector<std::pair<int, float>> MotionDatabase::SearchCacheOptimized(
    const MotionFeatures& query,
    const Trajectory& trajectory,
    int maxCandidates,
    int airborneFilter) const {
    if (!gForceScalarSearch && !poses.empty() && cpuSupportsAVX2()) {
        return SearchSIMD(query, trajectory, maxCandidates, airborneFilter);
    }
    return SearchCacheOptimizedScalar(query, trajectory, maxCandidates, airborneFilter);
}

// CRITICAL: Cache-optimized search using SoA layout (scalar reference)
std::vector<std::pair<int, float>> MotionDatabase::SearchCacheOptimizedScalar(
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
    auto& soa = simdCache_.soa;

    // Every channel must exist at the real length BEFORE padding, so
    // EnforceVectorPadding can extend them all to the same multiple of 8.
    soa.poseCount = n;
    soa.speeds.resize(n);
    soa.velocitiesX.resize(n);
    soa.velocitiesY.resize(n);
    soa.velocitiesZ.resize(n);
    soa.moveAngles.resize(n);
    soa.leftFootPlanted.resize(n);
    soa.rightFootPlanted.resize(n);
    soa.crouching.resize(n);
    soa.airborne.resize(n);
    soa.originalIndex.resize(n);

    soa.trajectoryValid.resize(n);
    for (int k = 0; k < kTrajectorySteps; ++k) {
        soa.trajX[k].resize(n);
        soa.trajZ[k].resize(n);
    }

    for (size_t i = 0; i < n; ++i) {
        const auto& f = poses[i].features;
        soa.speeds[i] = f.speed;
        soa.velocitiesX[i] = f.rootVelocity.x;
        soa.velocitiesY[i] = f.rootVelocity.y;
        soa.velocitiesZ[i] = f.rootVelocity.z;
        soa.moveAngles[i] = f.moveAngle;
        soa.leftFootPlanted[i] = poses[i].leftFootPlanted ? 1.0f : 0.0f;
        soa.rightFootPlanted[i] = poses[i].rightFootPlanted ? 1.0f : 0.0f;
        soa.crouching[i] = f.isCrouching ? 1.0f : 0.0f;
        soa.airborne[i] = f.isAirborne ? 1.0f : 0.0f;
        soa.originalIndex[i] = static_cast<int>(i);

        // Root-relative future path (mirrors the KD tree's dims 7..14). A pose
        // with no path at all is flagged so the kernel zeroes its traj weight,
        // keeping the AVX2 score identical to CalculateTrajectoryTerm.
        const Trajectory& tr = poses[i].trajectory;
        soa.trajectoryValid[i] = (tr.localNumPoints > 0) ? 1.0f : 0.0f;
        for (int k = 0; k < kTrajectorySteps; ++k) {
            const bool ok = (k < tr.localNumPoints);
            soa.trajX[k][i] = ok ? tr.localPositions[k].x : 0.0f;
            soa.trajZ[k][i] = ok ? tr.localPositions[k].z : 0.0f;
        }
    }
    soa.EnforceVectorPadding();
    simdCache_.valid = true;
    simdCache_.poseCount = n;
}

std::vector<std::pair<int, float>> MotionDatabase::SearchSIMD(
    const MotionFeatures& query,
    const Trajectory& trajectory,
    int maxCandidates,
    int airborneFilter) const {
    // The trajectory weighting in CalculatePoseScoreCacheOptimized is inert for
    // database poses (Trajectory::numPoints is only filled by the query-side
    // predictor), so the AVX2 metric intentionally omits it - see the note at
    // the top of MotionKDTreeSIMD.h. The parameter is kept for signature parity.
    (void)trajectory;

    // Rebuild SoA cache if the database changed
    RebuildSIMDCacheIfNeeded();

    if (simdCache_.soa.GetPoseCount() == 0)
        return {};

    // ExecuteAVX2PoseSearch is compiled with target("avx2,fma"), so the compiler
    // may emit AVX2/FMA instructions beyond the intrinsic loop itself. Never
    // enter it on a CPU without AVX2 - fall back to the scalar reference, which
    // is the implementation the kernel mirrors.
    if (!cpuSupportsAVX2()) {
        return SearchCacheOptimizedScalar(query, trajectory, maxCandidates, airborneFilter);
    }

    // Map the MotionFeatures query to the SIMD query format
    SIMDFeaturesQuery q;
    q.speed     = query.speed;
    q.velX      = query.rootVelocity.x;
    q.velY      = query.rootVelocity.y;
    q.velZ      = query.rootVelocity.z;
    q.moveAngle = query.moveAngle;
    q.leftFoot  = query.leftFootPlanted ? 1.0f : 0.0f;
    q.rightFoot = query.rightFootPlanted ? 1.0f : 0.0f;
    q.crouch    = query.isCrouching ? 1.0f : 0.0f;

    // Root-relative future path of the query (root-local), scored by the kernel
    // exactly like CalculateTrajectoryTerm scores it in the scalar reference.
    q.trajCount = std::max(0, std::min(kTrajectorySteps, query.futureCount));
    for (int k = 0; k < q.trajCount; ++k) {
        q.trajX[k] = query.futureLocal[k].x;
        q.trajZ[k] = query.futureLocal[k].y;
    }

    SIMDWeightsConfig w;
    w.speed     = weights.speedWeight;
    w.velocity  = weights.velocityWeight;
    w.direction = weights.directionWeight;
    w.footPlant = weights.footPlantWeight;
    w.crouch    = weights.crouchWeight;
    // Zero disables the term in the kernel (and skips the loads entirely).
    w.trajectory = gFlatSearchUsesTrajectory ? weights.trajectoryWeight : 0.0f;
    for (int k = 0; k < kTrajectorySteps; ++k) w.trajectoryFalloff[k] = trajectoryFalloff_[k];

    // Run the AVX2 8-wide search
    return ExecuteAVX2PoseSearch(simdCache_.soa, q, w, maxCandidates, airborneFilter);
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

    // 4. Trajectory match (if enabled).
    //
    // This used to be `poses[i].trajectory.getDifference(trajectory)`, which
    // reads Trajectory::positions/velocities - fields that are NEVER populated
    // for database poses (only Trajectory::localPositions is), so the term was
    // silently always zero. CalculateTrajectoryTerm scores the root-relative
    // future path that is actually filled in, with the same falloff weights as
    // the KD tree, and the AVX2 kernel mirrors it exactly. (The unused
    // `trajectory` parameter is kept for signature/API stability.)
    (void)trajectory;
    if (config.useTrajectoryMatching && poseIndex < poses.size()) {
        score += CalculateTrajectoryTerm(poses[poseIndex], query);
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

    // 7. Ghost-node turn-in-place angular-velocity match (forAnime.txt
    // L1131-1138). Only active when the query trajectory was produced by
    // PredictWithGhostNodes (hasGhostNodes flag); for ordinary motion the flag
    // is false -> a no-op, so the scalar ranking is bit-identical to the AVX2
    // kernel (MotionKDTreeSIMD.h) and existing tests are unaffected. When the
    // flag is set, velocities[0].y carries the packed target yaw rate and poses
    // whose inherent yaw rate diverges from it are penalized so turn-in-place
    // clips win the search.
    if (trajectory.hasGhostNodes) {
        const float angularSpeedTarget = trajectory.velocities[0].y;
        const float poseAngularVelocity = motionData.GetAngularVelocity(poseIndex);
        score += std::abs(poseAngularVelocity - angularSpeedTarget) * 15.0f;
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
