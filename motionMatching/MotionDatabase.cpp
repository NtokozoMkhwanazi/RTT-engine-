#include "MotionDatabase.h"
#include "../animationSystem/Animator.h"
#include "../boneSystem/Skeleton.h"
#include <iostream>
#include <algorithm>
#include <cmath>

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
// MOTION DATABASE IMPLEMENTATION
// ============================================================================

MotionDatabase::MotionDatabase() {
    // Default weights tuned for third-person character
    weights.velocityWeight = 3.0f;
    weights.directionWeight = 2.0f;
    weights.trajectoryWeight = 1.5f;
    weights.footPlantWeight = 1.0f;
    weights.crouchWeight = 0.5f;
}

MotionDatabase::~MotionDatabase() {
    Clear();
}

void MotionDatabase::Clear() {
    poses.clear();
    animations.clear();
    animationByName.clear();
}

void MotionDatabase::AddAnimation(const std::string& name, std::shared_ptr<Animation> anim,
                                   const Skeleton* skeleton) {
    if (!anim || !skeleton) {
        std::cerr << "[MotionDatabase] ERROR: Null animation or skeleton for " << name << "\n";
        return;
    }

    std::cout << "[MotionDatabase] Adding animation: " << name
              << " (" << anim->duration << "s, " << anim->boneAnimations.size() << " bones)\n";

    // Create animation entry (database takes ownership via shared_ptr)
    AnimationEntry entry;
    entry.name = name;
    entry.animation = anim;  // Store shared_ptr
    entry.startPoseIndex = poses.size();
    
    // Find foot bones for plant detection
    int leftFootBone = skeleton->GetBoneIndex("leftfoot");
    int rightFootBone = skeleton->GetBoneIndex("rightfoot");
    int leftToeBone = skeleton->GetBoneIndex("lefttoebase");
    int rightToeBone = skeleton->GetBoneIndex("righttoebase");
    
    // Extract pose for every frame (at 30fps)
    float fps = 30.0f;
    float frameTime = 1.0f / fps;
    int numFrames = static_cast<int>(anim->duration * fps);
    
    for (int frame = 0; frame < numFrames; frame++) {
        float time = frame * frameTime;

        // Create pose sample
        PoseSample pose;
        pose.animationIndex = animations.size();
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

        // Extract motion features for this frame (after pose is in vector)
        ExtractPoseFeatures(poses.size() - 1, anim, time, skeleton);
    }
    
    entry.endPoseIndex = poses.size() - 1;
    animations.push_back(entry);
    animationByName[name] = animations.size() - 1;
    
    std::cout << "  Added " << numFrames << " pose samples\n";
}

void MotionDatabase::ExtractPoseFeatures(size_t poseIndex, std::shared_ptr<Animation> anim,
                                          float time, const Skeleton* skeleton) {
    PoseSample& pose = poses[poseIndex];

    // For now, we'll use simplified feature extraction
    // In a full implementation, we'd:
    // 1. Sample the animation at this time
    // 2. Calculate root velocity from position delta
    // 3. Detect foot plants from foot bone velocity
    // 4. Calculate facing angle from root rotation

    // Simplified: Just store time and basic info
    pose.features.animationTime = time;
    pose.features.animationDuration = anim->duration;
    pose.features.animationIndex = pose.animationIndex;
    pose.features.frameIndex = pose.frameNumber;

    // Root position/rotation would come from sampled animation
    pose.rootPosition = glm::vec3(0.0f);  // Would be: sampledPose.rootPos
    pose.rootRotationY = 0.0f;             // Would be: sampledPose.rootRot.y

    // Foot heights would come from foot bone positions
    pose.leftFootHeight = 0.0f;
    pose.rightFootHeight = 0.0f;

    // For now, assume no foot plants (will be calculated at runtime)
    pose.leftFootPlanted = false;
    pose.rightFootPlanted = false;

    // Speed would be calculated from root velocity
    pose.features.speed = 0.0f;  // Would be: glm::length(rootVelocity)
    pose.features.rootVelocity = glm::vec3(0.0f);
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
    
    // Find top N candidates
    std::vector<SearchResult> candidates;
    candidates.reserve(config.maxSearchResults);
    
    for (size_t i = 0; i < poses.size(); i++) {
        const PoseSample& pose = poses[i];
        float score = CalculatePoseScore(pose, query, trajectory, config);
        
        // Insert into sorted candidates
        if (candidates.empty() || score < candidates.back().score) {
            SearchResult result;
            result.poseIndex = static_cast<int>(i);
            result.score = score;
            
            // Insert in sorted order
            auto it = std::lower_bound(candidates.begin(), candidates.end(), result,
                [](const SearchResult& a, const SearchResult& b) {
                    return a.score < b.score;
                });
            candidates.insert(it, result);
            
            // Keep only top N
            if (candidates.size() > config.maxSearchResults) {
                candidates.pop_back();
            }
        }
    }
    
    // Get best and second best for blending
    if (candidates.size() > 0) {
        results.best = candidates[0];
        results.best.blendWeight = 1.0f;
    }
    if (candidates.size() > 1) {
        results.second = candidates[1];
        results.second.blendWeight = 0.0f;  // Will be calculated by matcher
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
    score += speedDiff * weights.velocityWeight;
    
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
    
    return info;
}
