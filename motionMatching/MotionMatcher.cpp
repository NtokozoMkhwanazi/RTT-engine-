#include "MotionMatcher.h"
#include <iostream>
#include <algorithm>
#include <chrono>

MotionMatcher::MotionMatcher() 
    : database(nullptr)
    , skeleton(nullptr)
    , animator(nullptr)
    , currentPoseIndex(-1)
    , currentAnimTime(0.0f)
    , blendProgress(0.0f)
    , blendFromPose(-1)
    , blendToPose(-1)
    , characterPosition(0.0f)
    , characterVelocity(0.0f)
    , characterRotation(0.0f)
    , moveDirection(0.0f, 1.0f)
    , isGrounded(true)
    , isCrouching(false)
    , currentDatabaseName("Default")
    , pendingDatabase(nullptr)
    , databaseBlendProgress(0.0f)
    , databaseBlendDuration(0.0f)
    , isBlendingDatabases(false)
    , staticRootThreshold(0.1f)
    , enableStaticRootFallback(false)  // FORCE DISABLED - always search
    , hasStaticRoot(false) {}

MotionMatcher::~MotionMatcher() {
    // unique_ptr automatically cleans up database
    // Clear any dangling pointers
    animator = nullptr;
    skeleton = nullptr;
}

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

    // Initialize database with unique_ptr
    database = std::make_unique<MotionDatabase>();
    database->Clear();
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
    std::cout << "[MotionMatcher::LoadAnimation] ENTER: name='" << name << "' anim=" << (void*)anim.get() << "\n";
    
    if (!initialized) {
        std::cerr << "[MotionMatcher] ERROR: Not initialized!\n";
        return;
    }
    
    std::cout << "[MotionMatcher::LoadAnimation] Calling database->AddAnimation...\n";

    // Use stored skeleton for feature extraction
    database->AddAnimation(name, anim, skeleton);
    
    std::cout << "[MotionMatcher::LoadAnimation] COMPLETE\n";
}

void MotionMatcher::BuildSearchIndex() {
    if (!initialized) return;

    std::cout << "[MotionMatcher] Building KD-Tree search index...\n";

    // Use currentDatabaseRef if set, otherwise use database
    const MotionDatabase* dbToUse = currentDatabaseRef ? currentDatabaseRef : database.get();
    
    if (!dbToUse) {
        std::cerr << "[MotionMatcher] ERROR: No database available for KD-Tree build!\n";
        return;
    }

    // Build KD-Tree from pose database
    const auto& poses = dbToUse->GetPoses();
    searchTree.Build(poses, 10);  // Max 10 poses per leaf

    MotionKDTree::TreeStats stats = searchTree.GetStats();
    std::cout << "  KD-Tree: " << stats.totalNodes << " nodes, "
              << stats.leafNodes << " leaves, depth=" << stats.maxDepth << "\n";
    std::cout << "  Search speed: O(log n) instead of O(n)\n";
}

void MotionMatcher::SetCurrentDatabase(const MotionDatabase& newDatabase) {
    if (!initialized) {
        std::cerr << "[MotionMatcher] ERROR: Not initialized, cannot switch database!\n";
        return;
    }

    std::cout << "[MotionMatcher] Switching to new database (reference mode)...\n";
    
    // For database switching without copying, we use a raw pointer reference
    // The caller (HybridAnimGraph) owns the database lifetime
    currentDatabaseRef = &newDatabase;
    
    // Rebuild search index with new database
    BuildSearchIndex();
    
    std::cout << "[MotionMatcher] Database switch complete. New database has " 
              << newDatabase.GetPoseCount() << " poses.\n";
}

void MotionMatcher::Update(float dt,
                            const CharacterInput& input,
                            const CharacterState& charState) {
    std::cout << "[MotionMatcher::Update] ENTER dt=" << dt << " input.mag=" << input.moveMagnitude << "\n";
    
    if (!initialized) return;

    // Update character state
    characterPosition = charState.position;
    characterVelocity = charState.velocity;
    characterRotation = charState.rotation;
    moveDirection = charState.moveDirection;
    isGrounded = charState.grounded;
    isCrouching = charState.crouching;

    std::cout << "[MotionMatcher::Update] charVelocity=(" << characterVelocity.x << "," << characterVelocity.z << ") speed=" << glm::length(characterVelocity) << "\n";

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
    // Update database blending first
    UpdateDatabaseBlend(dt);

    // Check for static root
    bool isStatic = CheckStaticRoot();

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

    // Debug: print query speed every 30 frames
    static int frameCount = 0;
    if (++frameCount % 30 == 0) {
        std::cout << "[MM Query] speed=" << query.speed
                  << " vel=(" << query.rootVelocity.x << "," << query.rootVelocity.z << ")\n";
    }

    // Handle static root fallback
    if (isStatic && enableStaticRootFallback) {
        if (currentPoseIndex >= 0) {
            const PoseSample& pose = database->GetPose(currentPoseIndex);
            std::shared_ptr<Animation> currentAnim = database->GetAnimation(pose.animationIndex);

            if (currentAnim) {
                float animSpeed = currentAnim->speed > 0 ? currentAnim->speed : 1.0f;
                currentAnimTime += dt * animSpeed;
                currentAnimTime = fmod(currentAnimTime, currentAnim->duration);
                if (currentAnimTime < 0) currentAnimTime += currentAnim->duration;

                animator->Play(currentAnim.get());
                animator->SetCurrentTime(currentAnimTime);
            }
        }
        return;
    }

    // Search using KD-Tree
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
        results = database->SearchWithBlending(
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
        const PoseSample& targetPose = database->GetPose(results.best.poseIndex);
        std::shared_ptr<Animation> targetAnim = database->GetAnimation(targetPose.animationIndex);

        // CRITICAL FIX: Check if we're staying in the same animation
        bool sameAnimation = false;
        int currentAnimIndex = -1;
        if (currentPoseIndex >= 0 && currentPoseIndex < (int)database->GetPoseCount()) {
            currentAnimIndex = database->GetPose(currentPoseIndex).animationIndex;
            sameAnimation = (targetPose.animationIndex == currentAnimIndex);
        }

        // Update current animation time
        if (blendFromPose < 0 || blendProgress >= 1.0f) {
            if (sameAnimation && currentPoseIndex >= 0 && currentPoseIndex < (int)database->GetPoseCount()) {
                // STAYING in same animation - read time from animator, don't fight with it!
                // Animator::Update() advances time naturally via UpdateAnimationBlending()
                
                const PoseSample& currentPose = database->GetPose(currentPoseIndex);
                std::shared_ptr<Animation> currentAnim = database->GetAnimation(currentPose.animationIndex);

                if (currentAnim) {
                    // Get current time from animator (it's advancing time correctly)
                    float animatorTime = animator->GetCurrentTime();
                    
                    // Use animator's time for pose selection
                    currentAnimTime = animatorTime;

                    // Check if animation looped (for foot plant release and pose index reset)
                    bool aboutToLoop = (currentAnimTime > currentAnim->duration * 0.95f);

                    // Handle looping - reset our internal pose tracker
                    if (aboutToLoop) {
                        // Animation is about to loop - reset pose index to start
                        for (size_t i = 0; i < database->GetPoseCount(); i++) {
                            const PoseSample& p = database->GetPose(i);
                            if (p.animationIndex == currentPose.animationIndex) {
                                currentPoseIndex = static_cast<int>(i);
                                break;
                            }
                        }
                    } else {
                        // Normal case - advance to next pose based on time
                        // Find the pose that matches current animator time
                        float targetTime = currentAnimTime;
                        int bestPoseIdx = currentPoseIndex;
                        float bestTimeDiff = 999999.0f;
                        
                        // Search nearby poses for best time match
                        for (int searchDir = 0; searchDir <= 1; searchDir++) {
                            int searchPoseIdx = searchDir == 0 ? currentPose.nextFrameIndex : 
                                                                 currentPose.prevFrameIndex;
                            
                            while (searchPoseIdx >= 0 && searchPoseIdx < (int)database->GetPoseCount()) {
                                const PoseSample& searchPose = database->GetPose(searchPoseIdx);
                                if (searchPose.animationIndex != currentPose.animationIndex) break;
                                
                                float timeDiff = std::abs(searchPose.timeInSeconds - targetTime);
                                if (timeDiff < bestTimeDiff) {
                                    bestTimeDiff = timeDiff;
                                    bestPoseIdx = searchPoseIdx;
                                }
                                
                                searchPoseIdx = searchDir == 0 ? searchPose.nextFrameIndex : 
                                                                 searchPose.prevFrameIndex;
                            }
                        }
                        
                        currentPoseIndex = bestPoseIdx;
                    }
                }
            } else {
                // SWITCHING to new animation - jump to target pose
                currentPoseIndex = results.best.poseIndex;
                currentAnimTime = targetPose.timeInSeconds;
                blendFromPose = -1;
                blendToPose = -1;
                blendProgress = 1.0f;

                if (targetAnim) {
                    // Set animation speed ONCE when switching (not every frame!)
                    // Use 1.0 for real-time playback (animator dt already handles timing)
                    // DON'T use extractionFps/ticksPerSecond - that's for pose extraction, not playback
                    if (targetAnim->speed <= 0.0f || targetAnim->speed > 10.0f) {
                        targetAnim->speed = 1.0f;  // Real-time playback
                    }

                    // Only call Play() if this is a different animation
                    if (currentAnimationPtr != targetAnim.get()) {
                        animator->Play(targetAnim.get());
                        currentAnimationPtr = targetAnim.get();
                        // Set time directly when switching
                        animator->SetCurrentTime(currentAnimTime);
                        std::cout << "[MotionMatcher] Switched to: " << targetAnim->name 
                                  << " @ " << currentAnimTime << "s (speed=" << targetAnim->speed << ")\n";
                    } else {
                        // Same animation but jumped to different time (e.g., from blend)
                        animator->SetCurrentTime(currentAnimTime);
                    }
                }
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
                const PoseSample& fromPose = database->GetPose(blendFromPose);
                float fromTime = fromPose.timeInSeconds;
                float toTime = targetPose.timeInSeconds;
                currentAnimTime = fromTime + (toTime - fromTime) * blendProgress;
            }
        }
    }
}

void MotionMatcher::ApplyFootIK(float dt) {
    if (!animator || !skeleton) return;

    // Get current foot positions and velocities from animator
    int leftFootBone = skeleton->GetBoneIndex("leftfoot");
    int rightFootBone = skeleton->GetBoneIndex("rightfoot");

    if (leftFootBone < 0 || rightFootBone < 0) {
        // Try alternative bone names
        leftFootBone = skeleton->GetBoneIndex("LeftFoot");
        rightFootBone = skeleton->GetBoneIndex("RightFoot");
    }

    if (leftFootBone < 0 || rightFootBone < 0) {
        std::cerr << "[MotionMatcher::ApplyFootIK] WARNING: Could not find foot bones!\n";
        return;
    }

    // Get foot positions in world space (using identity model matrix for now)
    glm::mat4 modelMatrix(1.0f);
    glm::vec3 leftFootPos = animator->GetBoneWorldPosition(leftFootBone, modelMatrix);
    glm::vec3 rightFootPos = animator->GetBoneWorldPosition(rightFootBone, modelMatrix);

    // Calculate foot velocities from position delta
    glm::vec3 leftFootVel = leftFootPos - glm::vec3(0.0f);  // Will be set properly below
    glm::vec3 rightFootVel = rightFootPos - glm::vec3(0.0f);

    // Use animator's previous bone positions for velocity calculation
    if (leftFootBone < (int)animator->prevBoneWorldPos.size() &&
        rightFootBone < (int)animator->prevBoneWorldPos.size()) {
        leftFootVel = leftFootPos - animator->prevBoneWorldPos[leftFootBone];
        rightFootVel = rightFootPos - animator->prevBoneWorldPos[rightFootBone];
    }

    // Update foot planting system
    footPlanting.Update(leftFootPos, rightFootPos, leftFootVel, rightFootVel, 0.0f, config);

    // Apply foot IK through animator
    // The animator has a complete foot IK system - we just need to enable it
    if (!animator->footIKSettings.enabled) {
        // Configure foot IK if not already enabled
        Animator::FootIKSettings ikSettings;
        ikSettings.enabled = true;
        ikSettings.floorHeight = 0.0f;
        ikSettings.ikStrength = 1.0f;
        ikSettings.footLockBlend = 5.0f;  // Fast lock when planted
        ikSettings.footLockReleaseSpeed = 3.0f;  // Fast release when lifting
        ikSettings.maxIKDistance = 0.15f;  // Max 15cm correction
        ikSettings.leftFootBone = leftFootBone;
        ikSettings.rightFootBone = rightFootBone;
        animator->SetFootIKSettings(ikSettings);
    }

    // Determine if character is moving (for foot lock release)
    bool isMoving = (glm::length(characterVelocity) > 0.1f);

    // Update animator's foot IK system
    animator->UpdateFootIK(dt, modelMatrix, isMoving);
}

void MotionMatcher::UpdateDebugInfo() {
    debug.currentAnimationIndex = currentPoseIndex >= 0 ?
        database->GetPose(currentPoseIndex).animationIndex : -1;
    debug.currentAnimationTime = currentAnimTime;

    auto anim = GetCurrentAnimation();
    debug.currentAnimationName = anim ? anim->name : "None";

    debug.leftFootPlanted = footPlanting.IsLeftFootPlanted();
    debug.rightFootPlanted = footPlanting.IsRightFootPlanted();
}

std::shared_ptr<Animation> MotionMatcher::GetCurrentAnimation() const {
    if (currentPoseIndex < 0) return nullptr;

    const PoseSample& pose = database->GetPose(currentPoseIndex);
    return database->GetAnimation(pose.animationIndex);
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
    return database->GetDebugInfo();
}

// ============================================================================
// DATABASE SWITCHING IMPLEMENTATION
// ============================================================================

void MotionMatcher::SetDatabase(std::unique_ptr<MotionDatabase> newDatabase, float blendDuration) {
    if (!initialized || !newDatabase) {
        std::cerr << "[MotionMatcher] ERROR: Not initialized or null database!\n";
        return;
    }

    std::cout << "[MotionMatcher] Switching database: " << currentDatabaseName
              << " -> New (blend=" << blendDuration << "s)\n";

    if (blendDuration <= 0.0f) {
        // Instant switch
        database = std::move(newDatabase);
        currentDatabaseName = "New";
        databaseBlendProgress = 0.0f;
        isBlendingDatabases = false;

        // Rebuild search index
        BuildSearchIndex();
    } else {
        // Blended switch - store pending database
        pendingDatabase = std::move(newDatabase);
        databaseBlendDuration = blendDuration;
        databaseBlendProgress = 0.0f;
        isBlendingDatabases = true;
    }
}

bool MotionMatcher::IsDatabaseValidForMM() const {
    if (!initialized) return false;

    const auto& poses = database->GetPoses();
    if (poses.empty()) return false;

    // Check if database has sufficient root motion
    int dynamicPoses = 0;
    int staticPoses = 0;

    for (const auto& pose : poses) {
        float speed = glm::length(pose.features.rootVelocity);
        if (speed > staticRootThreshold) {
            dynamicPoses++;
        } else {
            staticPoses++;
        }
    }

    // Database is valid if majority of poses have root motion
    float dynamicRatio = static_cast<float>(dynamicPoses) / poses.size();
    bool isValid = dynamicRatio > 0.5f;

    if (!isValid && enableStaticRootFallback) {
        std::cout << "[MotionMatcher] WARNING: Database has " << staticPoses << "/"
                  << poses.size() << " static root poses (" << (1.0f - dynamicRatio) * 100
                  << "%). MM may not work well. Consider using FSM.\n";
    }

    return isValid;
}

bool MotionMatcher::HasStaticRoot() const {
    return hasStaticRoot;
}

bool MotionMatcher::CheckStaticRoot() {
    if (currentPoseIndex < 0) {
        hasStaticRoot = false;
        return false;
    }

    const PoseSample& pose = database->GetPose(currentPoseIndex);
    float speed = glm::length(pose.features.rootVelocity);

    hasStaticRoot = (speed < staticRootThreshold);
    return hasStaticRoot;
}

void MotionMatcher::UpdateDatabaseBlend(float dt) {
    if (!isBlendingDatabases) return;

    databaseBlendProgress += dt / databaseBlendDuration;

    if (databaseBlendProgress >= 1.0f) {
        // Blend complete
        database = std::move(pendingDatabase);
        currentDatabaseName = "Blended";
        databaseBlendProgress = 0.0f;
        isBlendingDatabases = false;

        // Rebuild search index
        BuildSearchIndex();

        std::cout << "[MotionMatcher] Database blend complete\n";
    }
}
