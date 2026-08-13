#include "MotionMatcher.h"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <limits>

MotionMatcher::MotionMatcher() 
    : database(nullptr)
    , skeleton(nullptr)
    , animator(nullptr)
    , currentPoseIndex(-1)
    , currentAnimTime(0.0f)
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
    if (verbose)
        std::cout << "[MotionMatcher::Update] ENTER dt=" << dt
                  << " input.mag=" << input.moveMagnitude << "\n";
    
    if (!initialized) return;

    // Update character state
    characterPosition = charState.position;
    characterVelocity = charState.velocity;
    characterRotation = charState.rotation;
    moveDirection = charState.moveDirection;
    isGrounded = charState.grounded;
    isCrouching = charState.crouching;
    isJumping = charState.jumping;

    if (verbose)
        std::cout << "[MotionMatcher::Update] charVelocity=(" << characterVelocity.x
                  << "," << characterVelocity.z << ") speed=" << glm::length(characterVelocity) << "\n";

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
    isJumping = charState.jumping;
    
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
    // Predict future trajectory. The pose-search query itself is built inside
    // SearchAndBlend() each frame - this is only the predicted path (used both
    // for the KD-tree future-path feature and the debug overlay).
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

    // Build motion query.
    //
    // ROOT-FRAME TRANSFORM: the database stores every pose's velocity,
    // moveAngle and future path in the CLIP's own frame (raw root space,
    // forward = +Z). The query's velocity is world-space, and the character's
    // local forward is -Z - the OPPOSITE of the clips' +Z root motion. So the
    // world velocity is rotated into clip space (R(-heading)) and negated,
    // making all three features heading-independent. Without this the velocity
    // sign flipped when moving in -Z and the matcher re-selected Idle
    // (footskate) while +Z movement worked by coincidence.
    MotionFeatures query;
    const float cosR = std::cos(-characterRotation);
    const float sinR = std::sin(-characterRotation);
    const glm::vec3 clipVel(
        -(characterVelocity.x * cosR - characterVelocity.z * sinR),
        characterVelocity.y,   // vertical kept as-is (airborne ascent/descent)
        -(characterVelocity.x * sinR + characterVelocity.z * cosR));
    query.rootVelocity = clipVel;
    // XZ-only speed (vertical velocity must not inflate the speed feature).
    query.speed = glm::length(glm::vec2(clipVel.x, clipVel.z));
    query.moveDirection = moveDirection;
    query.facingAngle = characterRotation;
    // Movement-vs-facing angle (strafing/backpedal discrimination): atan2 of
    // the clip-frame velocity - the same reference the database stores.
    query.moveAngle = std::atan2(clipVel.x, clipVel.z);
    query.isGrounded = isGrounded;
    query.isCrouching = isCrouching;
    query.isAirborne = isJumping || !isGrounded;

    // Root-local future path (Unreal-style trajectory feature): the predicted
    // world trajectory expressed as offsets from the character's root, rotated
    // into the clip frame (and negated, same as the velocity) - identical to
    // how the database stores each pose's trajectory, so the search ranks
    // same-speed poses by their future path (turns/stops/strafes), not just
    // current velocity.
    query.futureCount = kTrajectorySteps;
    for (int k = 1; k <= kTrajectorySteps; ++k) {
        const glm::vec3 wp =
            debug.predictedTrajectory.getPositionAt(k * kTrajectoryStepTime);
        const glm::vec3 rel = wp - characterPosition;
        query.futureLocal[k - 1] =
            glm::vec2(-(rel.x * cosR - rel.z * sinR),
                      -(rel.x * sinR + rel.z * cosR));
    }

    // UE-style gait-phase continuity: carry the CURRENT pose's foot-contact
    // state into the query so the search prefers poses in the same gait phase
    // (planted stays planted, swing stays swing). Before this the query's
    // foot-plant features defaulted to false, so the KD-tree distance added
    // (1-0)^2 * footPlantWeight per planted foot - it systematically penalized
    // planted poses and preferred mid-swing poses, which is footskating. The
    // FootPlantingSystem already feeds the plant/lift detection; this makes
    // the POSE SEARCH respect it (UE's motion-phase feature).
    //
    // Read from the SAME effective database the search tree was built from
    // (currentDatabaseRef when a non-owning switch is active, else the owned
    // database) - the tree indices refer to that pose array, so the phase
    // lookup must use it too.
    const MotionDatabase* effectiveDb =
        currentDatabaseRef ? currentDatabaseRef : database.get();
    if (effectiveDb && currentPoseIndex >= 0 &&
        currentPoseIndex < static_cast<int>(effectiveDb->GetPoseCount())) {
        const PoseSample& curPose = effectiveDb->GetPose(currentPoseIndex);
        query.leftFootPlanted = curPose.features.leftFootPlanted;
        query.rightFootPlanted = curPose.features.rightFootPlanted;
    }

    // Debug: print query speed every 30 frames (gated by verbose so the
    // editor character doesn't flood the log)
    static int frameCount = 0;
    if (verbose && ++frameCount % 30 == 0) {
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
        // Ask for enough candidates to cover the best pose in the CURRENT clip
        // and the best pose in any OTHER clip (persistence needs both).
        auto kdResults = searchTree.FindKNearest(query, 8);

        // UE-style movement-state gate: an airborne query may only match
        // airborne poses (Jump/Fall), a grounded query only grounded poses.
        // This is what lets takeoff and landing route through the pose search
        // - without it an airborne query's near-zero XZ speed would match Idle
        // and the character would freeze mid-air. Only enforced when the
        // database actually contains airborne poses (a locomotion-only DB
        // keeps its current behavior). If gating empties the top-k (rare - it
        // needs every near pose to come from the wrong state), re-search the
        // full database restricted to the required state instead of silently
        // using the wrong-state set; a Jump/Fall pose must never play while
        // the character is grounded (footskate) and vice versa (freeze).
        const bool airborneQuery = isJumping || !isGrounded;
        bool usedFilteredFallback = false;
        if (database->HasAirbornePoses() && !kdResults.empty()) {
            std::vector<KDTSearchResult> gated;
            gated.reserve(kdResults.size());
            for (const auto& r : kdResults) {
                const bool poseAirborne =
                    database->GetPose(r.poseIndex).features.isAirborne;
                if (poseAirborne == airborneQuery) gated.push_back(r);
            }
            if (!gated.empty()) {
                kdResults = std::move(gated);
            } else {
                // Rare: every top-k candidate is in the wrong movement state.
                // Re-search the full database restricted to the required state.
                // NOTE: the scores returned here use the database's internal
                // metric (speed weight ~2, not the KD tree's 15), so
                // r.distance is NOT comparable to KD distances below. That is
                // fine: the persistence hysteresis is skipped on this path
                // (usedFilteredFallback), so the metric mix never feeds the
                // 0.85-fraction clip-switch comparison.
                auto filtered = database->SearchCacheOptimized(
                    query, debug.predictedTrajectory, 8,
                    airborneQuery ? 1 : 0);
                if (!filtered.empty()) {
                    kdResults.clear();
                    kdResults.reserve(filtered.size());
                    for (const auto& f : filtered) {
                        KDTSearchResult r;
                        r.poseIndex = f.first;
                        r.distance = f.second;
                        r.score = 1.0f / (1.0f + f.second);
                        kdResults.push_back(r);
                    }
                    usedFilteredFallback = true;
                }
            }
        }

        int bestIdx = kdResults.empty() ? -1 : kdResults[0].poseIndex;
        float bestDist = kdResults.empty() ? std::numeric_limits<float>::max()
                                           : kdResults[0].distance;

        // Unreal-style persistence: when the best candidate is in a DIFFERENT
        // clip than the one playing, only switch if it is clearly better than
        // the best candidate in the CURRENT clip. Without this hysteresis the
        // matcher flickers between clips with overlapping feature
        // neighborhoods (idle<->crouch at rest, walk<->run near the band).
        if (bestIdx >= 0 && !usedFilteredFallback && currentPoseIndex >= 0 &&
            currentPoseIndex < (int)database->GetPoseCount()) {
            const int curAnimIdx = database->GetPose(currentPoseIndex).animationIndex;
            int bestSameIdx = -1;
            float bestSameDist = std::numeric_limits<float>::max();
            int bestOtherIdx = -1;
            float bestOtherDist = std::numeric_limits<float>::max();
            for (const auto& r : kdResults) {
                const int ai = database->GetPose(r.poseIndex).animationIndex;
                if (ai == curAnimIdx) {
                    if (r.distance < bestSameDist) {
                        bestSameDist = r.distance;
                        bestSameIdx = r.poseIndex;
                    }
                } else {
                    if (r.distance < bestOtherDist) {
                        bestOtherDist = r.distance;
                        bestOtherIdx = r.poseIndex;
                    }
                }
            }
            // A different clip must beat the current clip by 15% (or be the
            // only candidate) before we switch - stay put otherwise.
            if (bestOtherIdx >= 0 &&
                (bestSameIdx < 0 || bestOtherDist < bestSameDist * 0.85f)) {
                bestIdx = bestOtherIdx;
                bestDist = bestOtherDist;
            } else if (bestSameIdx >= 0) {
                bestIdx = bestSameIdx;  // pose-follow in the current clip
                bestDist = bestSameDist;
            }
        }

        if (bestIdx >= 0) {
            results.best.poseIndex = bestIdx;
            results.best.score = 1.0f / (1.0f + bestDist);
            results.best.blendWeight = 1.0f;

            // Second best (for blending)
            if (kdResults.size() > 1) {
                results.second.poseIndex = kdResults[1].poseIndex;
                results.second.score = kdResults[1].score;
                results.second.blendWeight = 0.0f;
            }
        }
    } else {
        // Fallback to brute force if tree not built. Apply the same movement-
        // state gate as the KD path so airborne queries can't match Idle.
        const bool airborneQuery = isJumping || !isGrounded;
        results = database->SearchWithBlending(
            query,
            debug.predictedTrajectory,
            config
        );
        if (database->HasAirbornePoses() && results.best.isValid()) {
            const bool poseAirborne =
                database->GetPose(results.best.poseIndex).features.isAirborne;
            if (poseAirborne != airborneQuery) {
                // Re-search restricted to the required state.
                auto gated = database->SearchCacheOptimized(
                    query, debug.predictedTrajectory,
                    config.maxSearchResults, airborneQuery ? 1 : 0);
                if (!gated.empty()) {
                    results.best.poseIndex = gated[0].first;
                    results.best.score = gated[0].second;
                }
            }
        }
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

        if (sameAnimation && currentPoseIndex >= 0 &&
            currentPoseIndex < (int)database->GetPoseCount()) {
            // STAYING in the same clip - pose following: advance the matcher's
            // own clip clock and track the nearest database pose. The animator
            // layer started at this same time (Play/BlendToAt) and advances on
            // its own, so we never fight it with SetCurrentTime. We also don't
            // read GetCurrentTime() here: mid-crossfade that would return the
            // fading layer's time and desync the pose tracker.
            const PoseSample& currentPose = database->GetPose(currentPoseIndex);
            std::shared_ptr<Animation> currentAnim =
                database->GetAnimation(currentPose.animationIndex);

            if (currentAnim) {
                // Resync the clip clock with the animator whenever the single
                // authoritative layer IS this clip (no crossfade in flight).
                // This heals any desync left by jump/crouch/rest overrides or
                // preview clips that drove the animator directly. Mid-
                // crossfade (2+ layers) we advance locally, because
                // GetCurrentTime() would return the fading layer's time.
                if (animator &&
                    animator->GetActiveAnimationLayerCount() <= 1 &&
                    animator->GetCurrentAnimation() == currentAnim.get()) {
                    currentAnimTime = animator->GetCurrentTime();
                } else {
                    const float speedMul =
                        currentAnim->speed > 0.0f ? currentAnim->speed : 1.0f;
                    currentAnimTime += dt * speedMul;
                    if (currentAnimTime >= currentAnim->duration) {
                        currentAnimTime = fmod(currentAnimTime, currentAnim->duration);
                    }
                }

                // Nearest database pose to the current clip time.
                float bestTimeDiff = std::numeric_limits<float>::max();
                int bestPoseIdx = currentPoseIndex;
                for (size_t i = 0; i < database->GetPoseCount(); ++i) {
                    const PoseSample& p = database->GetPose(i);
                    if (p.animationIndex != currentPose.animationIndex) continue;
                    const float d = std::abs(p.timeInSeconds - currentAnimTime);
                    if (d < bestTimeDiff) {
                        bestTimeDiff = d;
                        bestPoseIdx = static_cast<int>(i);
                    }
                }
                currentPoseIndex = bestPoseIdx;
            }
        } else if (targetAnim) {
            // SWITCHING to a different clip - crossfade (Unreal-style): the
            // animator fades the old pose out while the newly matched pose
            // fades in at its matched time. The hard Play() that popped on
            // every switch is gone.
            currentPoseIndex = results.best.poseIndex;
            currentAnimTime = targetPose.timeInSeconds;

            // Set animation speed ONCE when switching (not every frame!)
            // Use 1.0 for real-time playback (animator dt already handles
            // timing); extractionFps/ticksPerSecond are for pose extraction,
            // not playback.
            if (targetAnim->speed <= 0.0f || targetAnim->speed > 10.0f) {
                targetAnim->speed = 1.0f;  // Real-time playback
            }

            if (currentAnimationPtr != targetAnim.get()) {
                // Crossfade into the matched pose instead of Play()-popping.
                animator->BlendToAt(targetAnim.get(), currentAnimTime,
                                    glm::max(config.blendDuration, 0.05f));
                currentAnimationPtr = targetAnim.get();
                if (verbose)
                    std::cout << "[MotionMatcher] Switched to: " << targetAnim->name
                              << " @ " << currentAnimTime << "s (speed=" << targetAnim->speed << ")\n";
            } else {
                // Same animation but jumped to a different time
                animator->SetCurrentTime(currentAnimTime);
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

    // WORLD-space foot IK. The character's model matrix (translate * rotate *
    // scale, set each frame by the caller) maps the model-space bone positions
    // into world space where a planted foot is genuinely stationary while the
    // body walks over it - in model space it slides backward at the body's
    // speed and every stationary/plant check failed, so no foot ever locked
    // (visible footskating).
    const glm::mat4 modelMatrix = m_modelMatrix;
    const float modelScale =
        glm::max(glm::length(glm::vec3(modelMatrix[0])), 0.0001f);

    // Foot world positions (this frame's pose) + velocities vs last frame.
    const glm::vec3 leftFootPos =
        animator->GetBoneWorldPosition(leftFootBone, modelMatrix);
    const glm::vec3 rightFootPos =
        animator->GetBoneWorldPosition(rightFootBone, modelMatrix);

    glm::vec3 leftFootVel(0.0f);
    glm::vec3 rightFootVel(0.0f);
    if (leftFootBone < (int)animator->prevBoneWorldPos.size() &&
        rightFootBone < (int)animator->prevBoneWorldPos.size()) {
        const glm::vec3 leftPrev =
            glm::vec3(modelMatrix * glm::vec4(animator->prevBoneWorldPos[leftFootBone], 1.0f));
        const glm::vec3 rightPrev =
            glm::vec3(modelMatrix * glm::vec4(animator->prevBoneWorldPos[rightFootBone], 1.0f));
        leftFootVel = leftFootPos - leftPrev;
        rightFootVel = rightFootPos - rightPrev;
    }

    // Update foot planting system (world space + world floor height).
    footPlanting.Update(leftFootPos, rightFootPos, leftFootVel, rightFootVel, m_floorHeight, config);

    // Apply foot IK through animator
    // The animator has a complete foot IK system - we just need to enable it
    if (!animator->footIKSettings.enabled) {
        // Configure foot IK if not already enabled
        Animator::FootIKSettings ikSettings;
        ikSettings.enabled = true;
        ikSettings.floorHeight = m_floorHeight;  // world-space terrain height
        ikSettings.ikStrength = 1.0f;
        ikSettings.footLockBlend = 5.0f;  // Fast lock when planted
        ikSettings.footLockReleaseSpeed = 3.0f;  // Fast release when lifting
        // Max ~40cm world-space correction (the foot lock offsets are now
        // computed in world units, so 0.4 m is the right scale).
        ikSettings.maxIKDistance = 0.4f;
        ikSettings.leftFootBone = leftFootBone;
        ikSettings.rightFootBone = rightFootBone;
        animator->SetFootIKSettings(ikSettings);
    }

    // Push the live floor height every frame - it was only written on first
    // enable, so a corrected floor never reached the plant check.
    animator->SetFloorHeight(m_floorHeight);

    // Tell the animator the world scale so the world-space offsets it computes
    // can be converted back to model space for the skinning matrices.
    animator->SetIKWorldScale(modelScale);

    // Determine if character is moving (for foot lock release)
    bool isMoving = (glm::length(characterVelocity) > 0.1f);

    // Update animator's foot IK system (world-space foot evaluation)
    animator->UpdateFootIK(dt, modelMatrix, isMoving);
}

void MotionMatcher::UpdateDebugInfo() {
    debug.currentAnimationIndex = currentPoseIndex >= 0 ?
        database->GetPose(currentPoseIndex).animationIndex : -1;
    debug.currentAnimationTime = currentAnimTime;

    // Use the REGISTERED clip name ("Jump") not the raw FBX channel name
    // ("mixamo.com" - identical across Mixamo clips).
    debug.currentAnimationName = currentPoseIndex >= 0
        ? database->GetAnimationName(
              database->GetPose(currentPoseIndex).animationIndex)
        : "None";

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
        pendingDatabaseIsRef = false;
        pendingDatabaseRef = nullptr;

        // Rebuild search index
        BuildSearchIndex();
    } else {
        // Blended switch - store pending database
        pendingDatabase = std::move(newDatabase);
        pendingDatabaseRef = nullptr;
        pendingDatabaseIsRef = false;
        databaseBlendDuration = blendDuration;
        databaseBlendProgress = 0.0f;
        isBlendingDatabases = true;
    }
}

void MotionMatcher::SetDatabase(const MotionDatabase& newDatabase, float blendDuration) {
    if (!initialized) {
        std::cerr << "[MotionMatcher] ERROR: Not initialized!\n";
        return;
    }

    std::cout << "[MotionMatcher] Switching database (non-owning): " << currentDatabaseName
              << " -> External (blend=" << blendDuration << "s)\n";

    if (blendDuration <= 0.0f) {
        // Instant switch — point currentDatabaseRef at the external storage.
        // The external owner retains ownership; we just read through the pointer.
        currentDatabaseRef = &newDatabase;
        currentDatabaseName = "External";
        databaseBlendProgress = 0.0f;
        isBlendingDatabases = false;
        pendingDatabaseIsRef = false;
        pendingDatabaseRef = nullptr;
    } else {
        // Blended switch — store a raw pointer to the external database and
        // adopt it into currentDatabaseRef when the blend timer elapses.
        // The caller MUST keep `newDatabase` alive until then.
        pendingDatabaseRef = &newDatabase;
        pendingDatabase.reset();
        pendingDatabaseIsRef = true;
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
        // Blend complete. There are TWO pending paths: the owning one moves
        // pendingDatabase into `database`, and the non-owning one (external
        // storage owned by the caller, e.g. the crouch DB in stateDatabases)
        // adopts pendingDatabaseRef into currentDatabaseRef. Before, only the
        // owning path was handled: with a ref pending, `pendingDatabase` is
        // null, so `database = std::move(pendingDatabase)` nulled the ONLY
        // database and the next search dereferenced a null pointer (crash
        // once the crouch blend finished).
        if (pendingDatabaseIsRef && pendingDatabaseRef) {
            currentDatabaseRef = pendingDatabaseRef;
            currentDatabaseName = "External";
            pendingDatabaseRef = nullptr;
            pendingDatabaseIsRef = false;
        } else if (pendingDatabase) {
            database = std::move(pendingDatabase);
            currentDatabaseName = "Blended";
        }
        pendingDatabase.reset();
        databaseBlendProgress = 0.0f;
        isBlendingDatabases = false;

        // Rebuild search index
        BuildSearchIndex();

        std::cout << "[MotionMatcher] Database blend complete\n";
    }
}
