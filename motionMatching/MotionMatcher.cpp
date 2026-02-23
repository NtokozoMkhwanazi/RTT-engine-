#include "MotionMatcher.h"
#include <iostream>
#include <algorithm>
#include <chrono>

MotionMatcher::MotionMatcher() : animator(nullptr) {}

MotionMatcher::~MotionMatcher() {}

void MotionMatcher::Initialize(const Skeleton* skeleton, Animator* inAnimator) {
    if (!skeleton) {
        std::cerr << "[MotionMatcher] ERROR: Null skeleton!\n";
        return;
    }

    animator = inAnimator;
    this->skeleton = skeleton;  // Store skeleton for feature extraction

    if (!animator) {
        std::cerr << "[MotionMatcher] WARNING: Null animator!\n";
    }

    std::cout << "[MotionMatcher] Initializing...\n";

    // Initialize subsystems
    database.Clear();
    footPlanting.Initialize(skeleton);
    
    // Default configuration tuned for third-person character
    config.maxSearchResults = 10;
    config.searchRadius = 2.0f;
    config.useTrajectoryMatching = true;
    config.blendDuration = 0.1f;
    config.numBlendPoses = 2;
    config.footPlantThreshold = 0.05f;
    config.footPlantHeightThreshold = 0.1f;
    config.enableFootLocking = true;
    config.trajectoryDuration = 0.5f;
    config.trajectoryPoints = 5;
    config.useSpatialIndex = true;
    config.spatialIndexRebuildFrames = 60;
    
    initialized = true;
    std::cout << "[MotionMatcher] Initialized successfully!\n";
}

void MotionMatcher::LoadAnimation(const std::string& name, std::shared_ptr<Animation> anim) {
    if (!initialized) {
        std::cerr << "[MotionMatcher] ERROR: Not initialized!\n";
        return;
    }

    // Use stored skeleton for feature extraction
    database.AddAnimation(name, anim, skeleton);
}

void MotionMatcher::BuildSearchIndex() {
    if (!initialized) return;
    
    std::cout << "[MotionMatcher] Building KD-Tree search index...\n";
    
    // Build KD-Tree from pose database
    const auto& poses = database.GetPoses();
    searchTree.Build(poses, 10);  // Max 10 poses per leaf
    
    MotionKDTree::TreeStats stats = searchTree.GetStats();
    std::cout << "  KD-Tree: " << stats.totalNodes << " nodes, "
              << stats.leafNodes << " leaves, depth=" << stats.maxDepth << "\n";
    std::cout << "  Search speed: O(log n) instead of O(n)\n";
}

void MotionMatcher::Update(float dt,
                            const CharacterInput& input,
                            const CharacterState& charState) {
    if (!initialized) return;
    
    // Update character state
    characterPosition = charState.position;
    characterVelocity = charState.velocity;
    characterRotation = charState.rotation;
    moveDirection = charState.moveDirection;
    isGrounded = charState.grounded;
    isCrouching = charState.crouching;
    
    // 1. Predict future trajectory
    UpdateTrajectory();
    
    // 2. Search database and blend to best pose
    SearchAndBlend(dt);
    
    // 3. Apply foot IK to prevent sliding
    if (config.enableFootLocking) {
        ApplyFootIK(dt);
    }
    
    // 4. Update debug info
    UpdateDebugInfo();
}

void MotionMatcher::Update(float dt, const CharacterState& charState) {
    if (!initialized) return;
    
    // Update character state
    characterPosition = charState.position;
    characterVelocity = charState.velocity;
    characterRotation = charState.rotation;
    moveDirection = charState.moveDirection;
    isGrounded = charState.grounded;
    isCrouching = charState.crouching;
    
    // 1. Predict future trajectory
    UpdateTrajectory();
    
    // 2. Search database and blend to best pose
    SearchAndBlend(dt);
    
    // 3. Apply foot IK to prevent sliding
    if (config.enableFootLocking) {
        ApplyFootIK(dt);
    }
    
    // 4. Update debug info
    UpdateDebugInfo();
}

void MotionMatcher::Update(float dt,
                            const glm::vec3& position,
                            const glm::vec3& velocity,
                            const glm::vec2& moveDir,
                            float rotation,
                            bool grounded,
                            bool crouching) {
    CharacterState state;
    state.position = position;
    state.velocity = velocity;
    state.rotation = rotation;
    state.moveDirection = moveDir;
    state.grounded = grounded;
    state.crouching = crouching;

    Update(dt, state);
}

void MotionMatcher::UpdateTrajectory() {
    // Build motion query from current state
    MotionFeatures query;
    query.rootVelocity = characterVelocity;
    query.speed = glm::length(characterVelocity);
    query.moveDirection = moveDirection;
    query.facingAngle = characterRotation;
    query.isGrounded = isGrounded;
    query.isCrouching = isCrouching;
    
    // Predict future trajectory
    debug.predictedTrajectory = trajectoryPredictor.Predict(
        characterPosition,
        characterVelocity,
        characterRotation,
        moveDirection,
        config
    );
}

void MotionMatcher::SearchAndBlend(float dt) {
    // Start timing
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Build motion query
    MotionFeatures query;
    query.rootVelocity = characterVelocity;
    query.speed = glm::length(characterVelocity);
    query.moveDirection = moveDirection;
    query.facingAngle = characterRotation;
    query.isGrounded = isGrounded;
    query.isCrouching = isCrouching;

    // Search using KD-Tree (FAST!)
    SearchResults results;
    results.totalSearched = searchTree.GetPoseCount();

    if (searchTree.IsBuilt()) {
        // Find top 3 nearest neighbors
        auto kdResults = searchTree.FindKNearest(query, 3);

        if (!kdResults.empty()) {
            // Best match
            results.best.poseIndex = kdResults[0].poseIndex;
            results.best.score = kdResults[0].score;
            results.best.blendWeight = 1.0f;

            // Second best (for blending)
            if (kdResults.size() > 1) {
                results.second.poseIndex = kdResults[1].poseIndex;
                results.second.score = kdResults[1].score;
                results.second.blendWeight = 0.0f;
            }
        }
    } else {
        // Fallback to brute force if tree not built
        results = database.SearchWithBlending(
            query,
            debug.predictedTrajectory,
            config
        );
    }

    // End timing
    auto endTime = std::chrono::high_resolution_clock::now();
    debug.searchTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

    // Update debug info
    debug.currentResult = results.best;
    debug.posesSearched = results.totalSearched;

    if (results.best.isValid()) {
        // Get target pose
        const PoseSample& targetPose = database.GetPose(results.best.poseIndex);
        std::shared_ptr<Animation> targetAnim = database.GetAnimation(targetPose.animationIndex);

        // Update current animation time
        if (blendFromPose < 0 || blendProgress >= 1.0f) {
            // Not blending - jump to new pose
            currentPoseIndex = results.best.poseIndex;
            currentAnimTime = targetPose.timeInSeconds;
            blendFromPose = -1;
            blendToPose = -1;
            blendProgress = 1.0f;

            // CRITICAL: Tell animator to play this animation!
            if (targetAnim) {
                animator->Play(targetAnim.get());
                animator->SetCurrentTime(currentAnimTime);
            }
        } else {
            // Blending - interpolate
            blendProgress += dt / config.blendDuration;

            if (blendProgress >= 1.0f) {
                // Blend complete
                currentPoseIndex = blendToPose;
                currentAnimTime = targetPose.timeInSeconds;
                blendFromPose = -1;
                blendToPose = -1;
                blendProgress = 1.0f;
            } else {
                // Still blending - interpolate animation time
                const PoseSample& fromPose = database.GetPose(blendFromPose);
                float fromTime = fromPose.timeInSeconds;
                float toTime = targetPose.timeInSeconds;
                currentAnimTime = fromTime + (toTime - fromTime) * blendProgress;
            }
        }
    }
}

void MotionMatcher::ApplyFootIK(float dt) {
    // Get foot positions from current animation pose
    // In a full implementation, we'd sample the skeleton
    
    // For now, this is a placeholder
    // The foot planting system would:
    // 1. Get left/right foot positions from animation
    // 2. Update foot planting state
    // 3. Apply IK to lock planted feet
    
    // footPlanting.Update(leftFootPos, rightFootPos, leftFootVel, rightFootVel, 0.0f, config);
}

void MotionMatcher::UpdateDebugInfo() {
    debug.currentAnimationIndex = currentPoseIndex >= 0 ?
        database.GetPose(currentPoseIndex).animationIndex : -1;
    debug.currentAnimationTime = currentAnimTime;

    auto anim = GetCurrentAnimation();
    debug.currentAnimationName = anim ? anim->name : "None";

    debug.leftFootPlanted = footPlanting.IsLeftFootPlanted();
    debug.rightFootPlanted = footPlanting.IsRightFootPlanted();
}

std::shared_ptr<Animation> MotionMatcher::GetCurrentAnimation() const {
    if (currentPoseIndex < 0) return nullptr;

    const PoseSample& pose = database.GetPose(currentPoseIndex);
    return database.GetAnimation(pose.animationIndex);
}

void MotionMatcher::PrintDebugInfo() const {
    std::cout << "\n=== MOTION MATCHING DEBUG ===\n";
    std::cout << "Current pose: " << currentPoseIndex << "\n";
    std::cout << "Animation: " << debug.currentAnimationName
              << " @ " << currentAnimTime << "s\n";
    std::cout << "Search: " << debug.posesSearched << " poses, "
              << debug.searchTimeMs << "ms\n";
    std::cout << "Score: " << debug.currentResult.score << "\n";
    std::cout << "Feet: L=" << (debug.leftFootPlanted ? "PLANTED" : "FREE")
              << " R=" << (debug.rightFootPlanted ? "PLANTED" : "FREE") << "\n";
    
    // Print query features for debugging
    std::cout << "\n[QUERY FEATURES]\n";
    std::cout << "  Speed: " << characterVelocity.z << " m/s\n";
    std::cout << "  Velocity: (" << characterVelocity.x << ", " 
              << characterVelocity.y << ", " << characterVelocity.z << ")\n";
    std::cout << "  Grounded: " << (isGrounded ? "YES" : "NO") << "\n";
    std::cout << "  Crouching: " << (isCrouching ? "YES" : "NO") << "\n";
}

std::string MotionMatcher::GetDatabaseStats() const {
    return database.GetDebugInfo();
}
