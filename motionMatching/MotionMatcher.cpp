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
    config.footLiftVelocityThreshold = 0.5f;   // Forced release above walking-step speed
    config.footPlantedHeightThreshold = 0.1f;  // Within 10cm of floor = grounded
    config.footPlantVelocityThreshold = 0.05f; // Below walk speed = plantable
    config.footLockReleaseDuration = 0.15f;   // 150ms ramp to fade ankle offset
    config.trajectoryDuration = 0.5f;
    config.trajectoryPoints = 5;
    config.useSpatialIndex = true;
    config.spatialIndexRebuildFrames = 60;
    // speedBandFactor tuned for standard mocap speed bands:
    // walk~1.5, jog~3.5, run~6.0. A 0.5 factor gives a band of ~50% of the
    // clip's nominal speed (+1.0 m/s floor), which correctly triggers gait
    // switches at band boundaries without premature tripping during normal
    // speed variation within a gait.
    config.speedBandFactor = 0.5f;
    config.directionBandRadians = 1.5707963267948966f;  // pi/2 = 90 degrees

    // Re-tune the matching metrics so angle/foot state cannot overpower
    // velocity. Per the structural audit: binary foot-plant flags (weight 2.0
    // → 0.2) and raw angular deltas were overpowering the speed channel
    // (15.0 → 25.0). Direction weight raised from 0.5 to 4.0, velocity
    // components from 1.0 to 5.0 each, trajectory scale 1.5x.
    SetSearchWeights(
        25.0f,   // speed   (was 15.0 — velocity magnitude must dominate)
        5.0f,    // velX    (was 1.0  — keep horizontal direction stable)
        5.0f,    // velZ    (was 1.0  — keep forward direction stable)
        4.0f,    // direction (was 0.5 — prevent backward-clip selection)
        0.2f,    // footPlant (was 2.0 — subtle tie-breaker only)
        1.5f,    // trajectoryScale (was 1.0, then 8.0 — 1.5 is the audited value)
        2.0f);   // verticalVelocity (unchanged)

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

void MotionMatcher::SetDatabaseExplicit(const MotionDatabase& newDatabase,
                                         const MotionKDTree& preBuiltTree) {
    if (!initialized) {
        std::cerr << "[MotionMatcher] ERROR: Not initialized, cannot switch database!\n";
        return;
    }

    // Zero-allocation reference swap: no tree rebuild, no memory allocation.
    currentDatabaseRef = &newDatabase;
    activeSearchTreeRef = &preBuiltTree;  // Use pre-built tree instead of searchTree
    currentDatabaseName = "External_WarmSwap";
    currentPoseIndex = -1;
    currentAnimationPtr = nullptr;

    std::cout << "[MotionMatcher] Warm-swap database switch complete. New database has "
              << newDatabase.GetPoseCount() << " poses.\n";
}

// FIX (v12 Section 2): Restore primary database + tree pointers so the
// matcher returns to the owned locomotion DB after a state switch (e.g.
// Crouch→Locomotion).  Without this, activeSearchTreeRef stays pointed at
// the crouch KD-Tree and the matcher keeps searching crouch poses during
// locomotion.
void MotionMatcher::ResetDatabaseExplicit() {
    currentDatabaseRef = nullptr;   // falls back to database.get() (primary)
    activeSearchTreeRef = nullptr;  // falls back to &searchTree (primary)
    currentDatabaseName = "Primary";
    currentPoseIndex = -1;
    currentAnimationPtr = nullptr;
}

// =========================================================================
// Structural Motion Graph: Transition Clip Playback
// =========================================================================

void MotionMatcher::PlayTransitionClip(
    std::shared_ptr<Animation> clip,
    const MotionDatabase& targetDatabase,
    const MotionKDTree& preBuiltTree,
    int targetClipIndex) {

    if (!initialized || !clip || !animator) {
        std::cerr << "[MotionMatcher] ERROR: Cannot play transition clip (not initialized or null clip/animator)\n";
        return;
    }

    // Store transition state
    transitionClip = clip;
    transitionClipActive = true;
    transitionClipTime = 0.0f;
    transitionClipDuration = clip->duration;
    transitionTargetDB = &targetDatabase;
    transitionTargetTree = &preBuiltTree;
    transitionTargetDBName = "TransitionTarget";
    transitionTargetPoseIndex = targetClipIndex;

    // Exit clip lock mode — transition clip playback handles the motion now
    clipLockActive = false;

    // Start playing the transition clip immediately on the animator.
    // The clip is a self-contained Animation with all joints blended via
    // slerp + linear root interpolation with 2D alignment — no runtime
    // patches needed.
    animator->BlendToAt(clip.get(), 0.0f, 0.01f);  // Near-instant start into clip

    std::cout << "[MotionMatcher] Playing transition clip: " << clip->name
              << " (duration=" << clip->duration << "s, "
              << clip->boneAnimations.size() << " bones)\n";
}

void MotionMatcher::CompleteTransitionNow() {
    if (!transitionClipActive) return;

    // Swap to the target database — this is now a SEAMLESS handoff because
    // the transition clip ended at the target pose, so the KD-tree search
    // on the next frame will find poses very close to where the clip left off.
    if (transitionTargetDB) {
        currentDatabaseRef = transitionTargetDB;
        currentDatabaseName = transitionTargetDBName;
    }
    if (transitionTargetTree) {
        activeSearchTreeRef = transitionTargetTree;
    }

    // Resume pose tracking from the target clip
    currentPoseIndex = transitionTargetPoseIndex;
    currentAnimationPtr = nullptr;  // Force re-selection on next search

    // Clear transition state
    transitionClip.reset();
    transitionClipActive = false;
    transitionClipTime = 0.0f;
    transitionClipDuration = 0.0f;
    transitionTargetDB = nullptr;
    transitionTargetTree = nullptr;

    std::cout << "[MotionMatcher] Transition clip complete — resumed search in "
              << currentDatabaseName << "\n";
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
    characterWorldVelocity = charState.worldVelocity;
    characterRotation = charState.rotation;
    moveDirection = charState.moveDirection;
    isGrounded = charState.grounded;
    isCrouching = charState.crouching;
    isJumping = charState.jumping;

    if (verbose)
        std::cout << "[MotionMatcher::Update] charVelocity=(" << characterVelocity.x
                  << "," << characterVelocity.z << ") speed=" << glm::length(characterVelocity)
                  << " worldSpeed=" << glm::length(characterWorldVelocity) << "\n";

    // 1. Predict future trajectory
    UpdateTrajectory();

    // 2. Search database and blend to best pose
    SearchAndBlend(dt);

    // 3. Apply foot IK to prevent sliding (DEFERRED — called by the
    //    AnimatedCharacter after Animator::Update so the IK runs against
    //    THIS frame's bone positions, eliminating the 1-frame lag
    //    jitter that stretched legs during pose switches.)
    if (config.enableFootLocking) {
        // ApplyFootIK is invoked separately by the caller after the skeleton
        // is evaluated — see AnimatedCharacter::update().
    }

    // 4. Update debug info
    UpdateDebugInfo();
}

void MotionMatcher::Update(float dt, const CharacterState& charState) {
    if (!initialized) return;
    
    // Update character state
    characterPosition = charState.position;
    characterVelocity = charState.velocity;
    characterWorldVelocity = charState.worldVelocity;
    characterRotation = charState.rotation;
    moveDirection = charState.moveDirection;
    isGrounded = charState.grounded;
    isCrouching = charState.crouching;
    isJumping = charState.jumping;
    
    // 1. Predict future trajectory
    UpdateTrajectory();
    
    // 2. Search database and blend to best pose
    SearchAndBlend(dt);
    
    // 3. Apply foot IK to prevent sliding (DEFERRED — called by the
    //    AnimatedCharacter after Animator::Update so the IK runs against
    //    THIS frame's bone positions, eliminating the 1-frame lag
    //    jitter that stretched legs during pose switches.)
    if (config.enableFootLocking) {
        // ApplyFootIK is invoked separately by the caller after the skeleton
        // is evaluated — see AnimatedCharacter::update().
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

int MotionMatcher::SelectPoseWithPersistence(
    const MotionDatabase* db, int currentPoseIndex, const MotionFeatures& query,
    const std::vector<KDTSearchResult>& candidates, float& outBestDist) const {
    // Nothing to persist against - first frame or empty candidate list.
    if (!db || candidates.empty()) {
        if (!candidates.empty()) outBestDist = candidates[0].distance;
        return candidates.empty() ? -1 : candidates[0].poseIndex;
    }
    if (currentPoseIndex < 0 ||
        currentPoseIndex >= static_cast<int>(db->GetPoseCount())) {
        outBestDist = candidates[0].distance;
        return candidates[0].poseIndex;
    }

    // Split the candidates into the CURRENT clip's poses and everyone else's.
    const int curAnimIdx = db->GetPose(currentPoseIndex).animationIndex;
    int bestSameIdx = -1;
    float bestSameDist = std::numeric_limits<float>::max();
    int bestOtherIdx = -1;
    float bestOtherDist = std::numeric_limits<float>::max();
    for (const auto& r : candidates) {
        const int ai = db->GetPose(r.poseIndex).animationIndex;
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

    // [Advanced - feet/footskate guard] Directional flip penalty on the best
    // "other-clip" candidate. If bestOther's clip-frame root velocity is
    // geometrically OPPPOSED to the query (their normalized dot is below
    // -directionFlipDotThreshold, i.e. more than ~90deg apart), it matched only
    // on scalar speed - metric-space collapse, e.g. a backward RunLookBack clip
    // on a forward-left W+A strafe, which plants the feet on the wrong
    // animation. Inflate ONLY bestOther's distance (the current clip is never
    // touched) so the 0.85 persistence comparison can't be won by a
    // geometrically-flipped clip. Genuine reversals where bestOther (Back)
    // aligns with a backpedal query are unaffected and still switch.
    // Idle/zero-velocity poses are exempt.
    if (bestOtherIdx >= 0 && config.directionalFlipMultiplier > 1.0f) {
        const glm::vec3& ov = db->GetPose(bestOtherIdx).features.rootVelocity;
        const glm::vec3& qv = query.rootVelocity;
        const float oLen = glm::length(ov);
        const float qLen = glm::length(qv);
        if (oLen > 1e-5f && qLen > 1e-5f &&
            glm::dot(ov / oLen, qv / qLen) < -config.directionFlipDotThreshold) {
            bestOtherDist *= config.directionalFlipMultiplier;
        }
    }

    // Speed-band awareness: when the query speed has LEFT the current clip's
    // nominal gait band (run<->walk, walk<->stop, walk<->sprint...), the 15%
    // margin only delays the switch - the character keeps playing the wrong
    // gait (footskate) until the pose search finally wins by the margin. Drop
    // the margin and switch to the best other-clip candidate at once. Band =
    // nominalSpeed * config.speedBandFactor with a 1 m/s floor so Idle
    // (~0 m/s) never trips; set speedBandFactor < 0 to disable.
    const float clipSpeed = db->GetClipNominalSpeed(curAnimIdx);
    bool speedOutsideBand = false;
    if (config.speedBandFactor >= 0.0f) {
        const float speedBand = std::max(1.0f, clipSpeed * config.speedBandFactor);
        speedOutsideBand = std::abs(query.speed - clipSpeed) > speedBand;
    }

    // Direction-away awareness: the direction analog of the speed band. When
    // the query is moving more than config.directionBandRadians away from the
    // current clip's nominal move direction (reversal / backpedal / moving
    // toward the camera), the 15% margin would keep playing the old heading
    // while the character is already turned around - drop the margin and play
    // the best other-clip candidate at once. Only meaningful for clips that
    // actually move (Idle's nominal angle is a meaningless 0 and would trip
    // spuriously); set directionBandRadians < 0 to disable.
    bool directionOutsideBand = false;
    if (config.directionBandRadians >= 0.0f && clipSpeed > 0.5f) {
        float dirDiff =
            std::abs(query.moveAngle - db->GetClipNominalMoveAngle(curAnimIdx));
        if (dirDiff > glm::pi<float>()) dirDiff = 2.0f * glm::pi<float>() - dirDiff;
        directionOutsideBand = dirDiff > config.directionBandRadians;
    }

    // If the current clip produced no candidate in the KD top-k window, its
    // true best pose sat just outside the returned set: another clip can flood
    // the near set with near-identical poses (a constant-velocity walk yields
    // many equidistant poses), pushing the current clip's reversal pose out.
    // Persist the 15% margin by sampling the current clip's TRUE best pose in
    // the KD metric - otherwise bestSameDist stays at maxfloat and the 0.85
    // comparison is meaningless, force-switching away from the current clip
    // even when the other clip is only marginally better.
    if (bestSameIdx < 0) {
        KDTSearchResult clipBest = searchTree.FindBestInClip(query, curAnimIdx);
        if (clipBest.poseIndex >= 0) {
            bestSameIdx = clipBest.poseIndex;
            bestSameDist = clipBest.distance;
        }
    }

    // Pose-level persistence (companion to the clip-level persistence above).
    // bestSameIdx is the min-distance pose in the current clip. On a
    // constant-velocity clip (walk/idle) that minimum is shared by MANY
    // near-equidistant frames, so the naive min flips every frame -> matched
    // clip time jitters ~+-half a frame -> the foot IK chases the shifting root
    // and the ankles stretch/release each flicker ("feet vibrate / stretch on
    // idle", "walk vibrate"). Hold the CURRENT pose unless a same-clip
    // candidate genuinely beats it by config.poseHoldMargin; the current clip
    // is the authority here, so the bestOther switch (below) is untouched.
    if (bestSameIdx >= 0 && bestSameIdx != currentPoseIndex) {
        float currentPoseDist = std::numeric_limits<float>::max();
        bool foundInPool = false;
        for (const auto& r : candidates) {
            if (r.poseIndex == currentPoseIndex) {
                currentPoseDist = r.distance;
                foundInPool = true;
                break;
            }
        }
        // When the active frame is evicted from the Top-K window (a dense
        // 120fps clip floods the near-set with near-identical poses on a W+A
        // curve), the loop above leaves currentPoseDist at maxfloat and the
        // hold margin collapses -> persistence dies and the matched pose flips
        // 2-3 frames every tick (violent leg chatter: currentAnimTime jitters,
        // bone matrices pop, SolveLegIK fights a shifting root). Sample the
        // active frame's TRUE feature distance in the KD metric so the hold is
        // judged on real distance: a close-but-evicted frame (~= bestSameDist)
        // holds; a genuinely-shy one loses the margin and advances. The
        // bestOther switch below is untouched.
        if (!foundInPool) {
            std::vector<float> queryVec = searchTree.GetFeatureVector(query);
            std::vector<float> curPoseVec =
                searchTree.GetFeatureVector(db->GetPose(currentPoseIndex));
            currentPoseDist = searchTree.CalculateDistance(queryVec, curPoseVec);
        }
        // Hold the active pose unless a same-clip candidate genuinely beats it
        // by config.poseHoldMargin; the current clip is the authority here.
        if (bestSameDist >= currentPoseDist * config.poseHoldMargin) {
            bestSameIdx = currentPoseIndex;
            bestSameDist = currentPoseDist;
        }
    }

    // A different clip must beat the current clip by 15% (or be the only
    // candidate) before we switch - unless a band tripped, then switch at once.
    if (bestOtherIdx >= 0 &&
        (speedOutsideBand || directionOutsideBand ||
         bestOtherDist < bestSameDist * 0.85f)) {
        outBestDist = bestOtherDist;
        return bestOtherIdx;
    }
    if (bestSameIdx >= 0) {
        outBestDist = bestSameDist;  // pose-follow in the current clip
        return bestSameIdx;
    }
    outBestDist = candidates[0].distance;
    return candidates[0].poseIndex;
}

void MotionMatcher::SearchAndBlend(float dt) {
    // Update database blending first
    UpdateDatabaseBlend(dt);

    // ---- Structural Motion Graph: Transition Clip Playback ----
    // If a pre-computed transition clip is playing (instead of the instant
    // SetDatabaseExplicit swap), drive the Animator from the transition clip
    // and skip the KD-tree search entirely. When the clip finishes, swap
    // to the target database and resume normal motion matching.
    if (transitionClipActive && transitionClip) {
        transitionClipTime += dt;
        float clipProgress = transitionClipTime / transitionClipDuration;

        // Play the transition clip at the matching time
        if (animator) {
            float playTime = fmod(transitionClipTime, transitionClipDuration);
            if (playTime < 0.0f) playTime += transitionClipDuration;
            animator->BlendToAt(transitionClip.get(), playTime, 0.05f);
        }

        // Check for completion
        if (transitionClipTime >= transitionClipDuration) {
            CompleteTransitionNow();
        }

        // Update debug info
        UpdateDebugInfo();
        return;
    }

    // ---- Clip Lock Mode ----
    // When the character is at rest (idle), let the current clip play through
    // its natural cycle without pose-search interruptions ("cut mid clip"
    // effect). Simply skip the KD-tree search — the animator's Update(dt)
    // handles natural time advancement. We must NOT call SetCurrentTime here
    // because that would DOUBLE-advance (matcher advances + animator Update
    // advances), causing 2× playback speed and visible time jumps.
    if (clipLockActive) {
        UpdateDebugInfo();
        return;
    }

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
    //
    // ── Phase 1 (Retargeting): Scale-Normalize ──────────────────────────────
    // The velocity is ALREADY in model-space asset units when it arrives from
    // AnimatedCharacter::driveMotionMatching (which divides by `scale` once).
    // Applying m_characterScale here again would double-divide by scale,
    // inflating query.speed by ~1/scale² (e.g. 8850× for scale=0.01), causing
    // the KD-Tree to always match Run/Sprint poses. Fix (v12 S3): NO additional
    // scaleFactor — the caller handles the world→model conversion.
    const float scaleFactor = 1.0f;
    MotionFeatures query;
    const float cosR = std::cos(-characterRotation);
    const float sinR = std::sin(-characterRotation);
    const glm::vec3 clipVel(
        -(characterVelocity.x * cosR - characterVelocity.z * sinR) * scaleFactor,
        characterVelocity.y * scaleFactor,   // vertical kept as-is (airborne)
        -(characterVelocity.x * sinR + characterVelocity.z * cosR) * scaleFactor);
    query.rootVelocity = clipVel;
    // XZ-only speed (vertical velocity must not inflate the speed feature).
    query.speed = glm::length(glm::vec2(clipVel.x, clipVel.z));
    query.moveDirection = moveDirection;
    query.facingAngle = characterRotation;

    // FIX (v7 todo Step 1 — "Hard Standstill Deadband"):
    // Force an absolute standstill deadband when the player isn't touching the
    // stick AND the capsule velocity is below the foot-lift threshold (0.5 m/s,
    // i.e. below walking speed). This wipes all velocity, angle, and trajectory
    // noise so the KD-Tree matches clean Idle poses and breaks the clip-
    // persistence local-minimum trap (15% margin prevents the trailing gait
    // clip from holding the slot forever, which is why the leg stays stretched
    // until you move again).
    const bool playerIsDecelerating = glm::length(moveDirection) < 0.01f;
    const bool capsuleIsNearlyStatic = glm::length(characterVelocity) < config.footLiftVelocityThreshold;
    if (playerIsDecelerating && capsuleIsNearlyStatic) {
        query.rootVelocity = glm::vec3(0.0f);
        query.speed = 0.0f;
        query.moveAngle = 0.0f;
    } else {
        // Movement-vs-facing angle (strafing/backpedal discrimination): atan2 of
        // the clip-frame velocity - the same reference the database stores.
        // Canonicalized to [0, 2pi) exactly like the database's poses (atan2 can
        // return +/-pi for the same backward heading; the KD tree splits on these
        // raw values, so +pi vs -pi would land on opposite ends of the axis and
        // prune matching backward poses).
        if (query.speed > 0.001f) {
            query.moveAngle = std::atan2(clipVel.x, clipVel.z);
            if (query.moveAngle < 0.0f) query.moveAngle += 6.28318530f;
        } else {
            query.moveAngle = 0.0f;
        }
    }
    query.isGrounded = isGrounded;
    query.isCrouching = isCrouching;
    query.isAirborne = isJumping || !isGrounded;

    // Root-local future path (Unreal-style trajectory feature): the predicted
    // world trajectory expressed as offsets from the character's root.
    // TrajectoryPredictor::Predict() already bakes the character's heading into
    // the integrated world positions via worldInputDir, so the relative offsets
    // are ALREADY in model space — applying cosR/sinR here would double-rotate
    // and diverge from the database's local-space tracking attributes.
    query.futureCount = kTrajectorySteps;
    for (int k = 1; k <= kTrajectorySteps; ++k) {
        // FIX (v7 todo Step 2 — "Purge Trajectory Predictor on Rest"):
        // When at a standalone rest state, lock future trajectory offsets to
        // zero so the KD-Tree doesn't match turning/moving frames when standing
        // still. This stops the trailing momentum from projecting a ghost path
        // that the search matches against walk/turn clips.
        if (playerIsDecelerating && capsuleIsNearlyStatic) {
            query.futureLocal[k - 1] = glm::vec2(0.0f);
        } else {
            const glm::vec3 wp =
                debug.predictedTrajectory.getPositionAt(k * kTrajectoryStepTime);
            const glm::vec3 rel = wp - characterPosition;
            // CRITICAL CORRECTION: Remove the negative signs to match the positive
            // clip-coordinate extraction in MotionDatabase::ExtractPoseFeatures [File 32].
            // Predict() already bakes worldInputDir (which accounts for heading) into
            // the integrated world positions, so rel = wp - characterPosition is
            // already in the same model-space convention the database uses for its
            // trajectory features. Negating here flips the query against the database:
            // walking forward matches backward-leaning clips, stretching the ankles.
            query.futureLocal[k - 1] = glm::vec2(rel.x, rel.z);
        }
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
    // database) - the tree indices refer to that pose array, so every pose
    // lookup below must use it too (a non-owning context switch would
    // otherwise index the OWNED database with the new context's pose indices
    // - different array, garbage or out-of-bounds).
    const MotionDatabase* db = EffectiveDatabase();
    if (db && currentPoseIndex >= 0 &&
        currentPoseIndex < static_cast<int>(db->GetPoseCount())) {
        const PoseSample& curPose = db->GetPose(currentPoseIndex);
        query.leftFootPlanted = curPose.features.leftFootPlanted;
        query.rightFootPlanted = curPose.features.rightFootPlanted;
        // FIX (v7 todo Step 3 — "Align Gait-Phase Tokens to Grounded Stance"):
        // On deceleration, overwrite the foot-plant state to true (both feet
        // grounded) so the KD-Tree doesn't penalize clean Idle poses with an
        // asymmetric phase feature carried over from the trailing gait cycle.
        if (playerIsDecelerating && capsuleIsNearlyStatic) {
            query.leftFootPlanted = true;
            query.rightFootPlanted = true;
        }
        // Persist the gait phase so a context switch (which resets
        // currentPoseIndex) can carry it into the new database's first query.
        lastFootPlantL_ = query.leftFootPlanted;
        lastFootPlantR_ = query.rightFootPlanted;
    } else if (currentPoseIndex < 0) {
        // First query in a fresh context (pose index reset on the database
        // switch): carry the last known foot-contact state over so the entry
        // pose lands in the same gait phase instead of an arbitrary
        // mid-swing pose (pose mismatch on crouch/capoeira switches).
        // FIX (v7 todo Step 3): Force historical tokens to true on rest so a
        // fresh-context query carries both-feet-planted into the standstill
        // search instead of an asymmetric mid-swing state from the walk cycle.
        if (playerIsDecelerating && capsuleIsNearlyStatic) {
            lastFootPlantL_ = true;
            lastFootPlantR_ = true;
        }
        query.leftFootPlanted = lastFootPlantL_;
        query.rightFootPlanted = lastFootPlantR_;
    }

    // Debug: print query speed every 30 frames (gated by verbose so the
    // editor character doesn't flood the log)
    static int frameCount = 0;
    if (verbose && ++frameCount % 30 == 0) {
        std::cout << "[MM Query] speed=" << query.speed
                  << " vel=(" << query.rootVelocity.x << "," << query.rootVelocity.z << ")\n";
    }



    // Handle static root fallback OR rest-deadband sequential stepping.
    // enableStaticRootFallback is disabled by default (FORCE DISABLED - always
    // search for best pose), but when the character is at rest the KD-tree
    // search flickers between near-equidistant idle poses every frame
    // (floating-point noise at 120fps), micro-popping the root and vibrating
    // the legs. When the standstill deadband is active, bypass the search
    // entirely and step through poses sequentially.
    if (isStatic && (enableStaticRootFallback ||
                     (playerIsDecelerating && capsuleIsNearlyStatic))) {
        if (currentPoseIndex >= 0) {
            const PoseSample& pose = db->GetPose(currentPoseIndex);
            std::shared_ptr<Animation> currentAnim = db->GetAnimation(pose.animationIndex);

            if (currentAnim) {
                float animSpeed = currentAnim->speed > 0 ? currentAnim->speed : 1.0f;
                currentAnimTime += dt * animSpeed;
                currentAnimTime = fmod(currentAnimTime, currentAnim->duration);
                if (currentAnimTime < 0) currentAnimTime += currentAnim->duration;

                // FIX (v9 todo — "Stance Phase Clock Skew"):
                // When standing still, derive currentPoseIndex from the ACTUAL
                // elapsed playback time of the idle clip, not from sequential
                // nextFrameIndex stepping. The database is extracted at 120fps
                // while the Animator plays at 30fps; naive +1-per-frame stepping
                // causes a 4x clock skew that triggers alternating foot pops
                // (the MM tracker sees a foot-lift phase that the Animator
                // hasn't actually reached). Tying the index to currentAnimTime
                // keeps the MM tracker in sync with the visual playback timeline.
                if (playerIsDecelerating && capsuleIsNearlyStatic) {
                    constexpr float kExtractionFps = 120.0f;
                    int calculatedFrame = static_cast<int>(currentAnimTime * kExtractionFps);
                    size_t clipStartIdx, clipEndIdx;
                    db->GetPoseRange(pose.animationIndex, clipStartIdx, clipEndIdx);
                    size_t frameCount = clipEndIdx - clipStartIdx + 1;
                    if (frameCount > 0) {
                        currentPoseIndex =
                            static_cast<int>(clipStartIdx +
                                             (calculatedFrame % frameCount));
                    }
                }

                // Sync currentAnimationPtr for consistency (static root path)
                currentAnimationPtr = currentAnim.get();
                animator->Play(currentAnim.get());
                animator->SetCurrentTime(currentAnimTime);
            }
        }
        return;
    }

    // Search using KD-Tree
    SearchResults results;
    // FIX (v11 Section 2): Use the pre-built tree reference if one was set via
    // SetDatabaseExplicit (zero-allocation warm-swap), otherwise fall back to
    // the locally-built searchTree. This eliminates the KD-Tree rebuild stall
    // during state switches (CrouchWalk ↔ Walk/Run).
    const MotionKDTree* currentTree = activeSearchTreeRef ? activeSearchTreeRef : &searchTree;
    results.totalSearched = currentTree->GetPoseCount();

    if (currentTree->IsBuilt()) {
        // Ask for enough candidates to cover the best pose in the CURRENT clip
        // and the best pose in any OTHER clip (persistence needs both).
        auto kdResults = currentTree->FindKNearest(query, 8);

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
        if (db->HasAirbornePoses() && !kdResults.empty()) {
            std::vector<KDTSearchResult> gated;
            gated.reserve(kdResults.size());
            for (const auto& r : kdResults) {
                const bool poseAirborne =
                    db->GetPose(r.poseIndex).features.isAirborne;
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
                //
                // FIX (v12 SIMD-Wireup): use the AVX2-accelerated SIMD search
                // path (SearchSIMD → ExecuteAVX2PoseSearch) instead of the
                // scalar SearchCacheOptimized.  The SIMD path falls back to
                // scalar automatically when AVX2 is unavailable, and internally
                // applies the airborne filter, re-searching scalar-only if the
                // AVX2 top-N didn't contain enough matching poses.
                auto filtered = db->SearchSIMD(
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

        // Unreal-style persistence with band override (shared with the
        // brute-force fallback path below, so both behave identically): when
        // the best candidate is in a DIFFERENT clip than the one playing, only
        // switch if it is clearly better than the best candidate in the
        // CURRENT clip (15% margin) - unless the query has left the current
        // clip's nominal speed band (gait change) or is moving away from its
        // nominal heading (reversal/backpedal), in which case the margin is
        // dropped and the switch happens at once. Skipped on the filtered
        // fallback: its distances come from the database's metric mix (not the
        // KD tree's), so they are not comparable to the tree's feature-space
        // distances that the 0.85-fraction comparison relies on.
        if (!usedFilteredFallback) {
            const int chosen =
                SelectPoseWithPersistence(db, currentPoseIndex, query, kdResults, bestDist);
            if (chosen >= 0) bestIdx = chosen;
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
        // Fallback to brute force if tree not built. Apply the SAME logic as
        // the KD path: the movement-state gate (airborne queries can't match
        // Idle) and the clip persistence + speed/direction band override.
        // Before, this path always jumped to the global best - it could
        // flicker between overlapping clips (idle<->crouch at rest, walk<->run
        // near the band) that the KD path's hysteresis holds steady.
        const bool airborneQuery = isJumping || !isGrounded;
        results.totalSearched = static_cast<int>(db->GetPoseCount());
        const int gate = db->HasAirbornePoses() ? (airborneQuery ? 1 : 0) : -1;
        // FIX (v12 SIMD-Wireup): use SIMD-accelerated search instead of scalar.
        auto cands = db->SearchSIMD(
            query, debug.predictedTrajectory,
            config.maxSearchResults, gate);
        if (!cands.empty()) {
            std::vector<KDTSearchResult> kdResults;
            kdResults.reserve(cands.size());
            for (const auto& f : cands) {
                KDTSearchResult r;
                r.poseIndex = f.first;
                r.distance = f.second;
                r.score = 1.0f / (1.0f + f.second);
                kdResults.push_back(r);
            }
            float bestDist = kdResults[0].distance;
            const int chosen =
                SelectPoseWithPersistence(db, currentPoseIndex, query, kdResults, bestDist);
            if (chosen >= 0) {
                results.best.poseIndex = chosen;
                results.best.score = 1.0f / (1.0f + bestDist);
                results.best.blendWeight = 1.0f;

                // Second best (for blending)
                if (kdResults.size() > 1) {
                    results.second.poseIndex = kdResults[1].poseIndex;
                    results.second.score = kdResults[1].score;
                    results.second.blendWeight = 0.0f;
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
        const PoseSample& targetPose = db->GetPose(results.best.poseIndex);
        std::shared_ptr<Animation> targetAnim = db->GetAnimation(targetPose.animationIndex);

        // CRITICAL FIX: Check if we're staying in the same animation
        bool sameAnimation = false;
        int currentAnimIndex = -1;
        if (currentPoseIndex >= 0 && currentPoseIndex < (int)db->GetPoseCount()) {
            currentAnimIndex = db->GetPose(currentPoseIndex).animationIndex;
            sameAnimation = (targetPose.animationIndex == currentAnimIndex);
        }

        if (sameAnimation && currentPoseIndex >= 0 &&
            currentPoseIndex < (int)db->GetPoseCount()) {
            // STAYING in the same clip - pose following: advance the matcher's
            // own clip clock and track the nearest database pose. The animator
            // layer started at this same time (Play/BlendToAt) and advances on
            // its own, so we never fight it with SetCurrentTime. We also don't
            // read GetCurrentTime() here: mid-crossfade that would return the
            // fading layer's time and desync the pose tracker.
            const PoseSample& currentPose = db->GetPose(currentPoseIndex);
            std::shared_ptr<Animation> currentAnim =
                db->GetAnimation(currentPose.animationIndex);

            if (currentAnim) {
                // Sync currentAnimationPtr — needed by clip-lock readiness
                // checks and the safety-net override's matcherOnIt comparison.
                // Previously this was only set in the "switch clip" branch,
                // leaving it null after a transition into the same clip (e.g.
                // Loco→Crouch transition ends, then idle search stays in
                // Crouch clip), which permanently blocked clip-lock engagement.
                currentAnimationPtr = currentAnim.get();
                // The matcher owns its own clip clock and advances it strictly
                // by dt * speedMul -- it NEVER mirrors the animator's live
                // playback time. The animator's clock is fmod-wrapped at
                // animDuration and mutates mid-frame during blends, so reading
                // it here created a circular synchronization loop: MotionMatcher
                // pulled a time that was half-advanced between the matcher and
                // the (mutating) matrix-blend pass, which micro-flickered the
                // KD-tree query between two adjacent poses every single frame
                // -- the violent feet jitter on idle/walk (see todo file,
                // "The Diagnostics: Why it is Vibrating" #1). Advancing locally
                // with a unified step-forward eliminates the two-clock fight.
                // Wherever the matcher DOES retarget the time (BlendToAt /
                // SetCurrentTime in the switch branch below) it writes
                // currentAnimTime into the animator, so the matcher's clock
                // stays authoritative and the two stay in phase as long as the
                // playback speed matches.
                const float speedMul =
                    currentAnim->speed > 0.0f ? currentAnim->speed : 1.0f;
                currentAnimTime += dt * speedMul;
                if (currentAnimTime >= currentAnim->duration) {
                    currentAnimTime = fmod(currentAnimTime, currentAnim->duration);
                }

                // Nearest database pose to the current clip time.
                //
                // FIX (pose-time jitter): At low speeds, dt*speedMul is tiny
                // relative to the pose sample interval, so the naive nearest-
                // time lookup flips back and forth between two adjacent poses
                // every frame — the bone matrices pop half a pose's worth,
                // chattering the IK root. Add hysteresis: only switch to a new
                // pose when it is closer by at least kPoseSwitchTimeBias
                // (half the sample spacing), not by a rounding error.
                float bestTimeDiff = std::numeric_limits<float>::max();
                int bestPoseIdx = currentPoseIndex;
                for (size_t i = 0; i < db->GetPoseCount(); ++i) {
                    const PoseSample& p = db->GetPose(i);
                    if (p.animationIndex != currentPose.animationIndex) continue;
                    const float d = std::abs(p.timeInSeconds - currentAnimTime);
                    if (d < bestTimeDiff) {
                        bestTimeDiff = d;
                        bestPoseIdx = static_cast<int>(i);
                    }
                }
                // Hysteresis: hold the current pose unless a different pose
                // is closer by at least kPoseSwitchTimeBias (avoids the
                // ±half-sample flip at low speeds). The sample interval is
                // estimated from the animation's frame rate (default 30 fps →
                // ~0.033s); use half of that as the switch bias.
                float sampleInterval = currentAnim->ticksPerSecond > 0.0f
                    ? (1.0f / currentAnim->ticksPerSecond)
                    : (1.0f / 30.0f);
                float kPoseSwitchTimeBias = sampleInterval * 0.5f;
                float currentTimeDiff = bestTimeDiff;
                if (currentPoseIndex >= 0 && currentPoseIndex < (int)db->GetPoseCount()) {
                    const PoseSample& curP = db->GetPose(currentPoseIndex);
                    if (curP.animationIndex == currentPose.animationIndex) {
                        currentTimeDiff = std::abs(curP.timeInSeconds - currentAnimTime);
                    }
                }
                // Only advance if the nearest pose is meaningfully closer than
                // the current pose (not just a rounding error at the midpoint).
                if (bestPoseIdx != currentPoseIndex &&
                    bestTimeDiff < currentTimeDiff - kPoseSwitchTimeBias) {
                    currentPoseIndex = bestPoseIdx;
                }
                // If the current pose is still the nearest (or within the
                // hysteresis band), keep it — avoids the flip-flop.
            }
        } else if (targetAnim) {
            // SWITCHING to a different clip - crossfade (Unreal-style): the
            // animator fades the old pose out while the newly matched pose
            // fades in at its matched time. The hard Play() that popped on
            // every switch is gone.
            //
            // LANDING/TAKEOFF: crossing the airborne<->grounded boundary
            // blends between very different poses (tucked Fall -> full-stance
            // Walk), so it gets a longer dedicated crossfade than a normal
            // locomotion switch - the generic 0.1s made landings pop.
            const bool prevAirborne =
                (currentPoseIndex >= 0 &&
                 currentPoseIndex < (int)db->GetPoseCount())
                    ? db->GetPose(currentPoseIndex).features.isAirborne
                    : targetPose.features.isAirborne;
            const bool targetAirborne = targetPose.features.isAirborne;
            float switchBlend = glm::max(config.blendDuration, 0.05f);
            if (prevAirborne != targetAirborne) {
                switchBlend = targetAirborne
                                  ? glm::max(config.takeoffBlendDuration, 0.05f)
                                  : glm::max(config.landingBlendDuration, 0.05f);
            }

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
                animator->BlendToAt(targetAnim.get(), currentAnimTime, switchBlend);
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

    // ── Leg Stretching Fix: Stride Warping ──────────────────────────────
    // When the character's actual world speed doesn't match the animation's
    // native speed, the feet slide (skating) — the animation foot plants where
    // the clip thinks it should be, but the body has moved further (or less)
    // in real space. Stride warping adjusts the SWINGING foot's IK target
    // along the character's forward axis by the speed ratio so the foot
    // lands where the body actually expects it, eliminating foot slide.
    //
    // Only SWINGING (unplanted) feet are adjusted — a planted foot is locked
    // to the ground and must not move (that would re-introduce sliding).
    if (animator && db && results.best.isValid()) {
        const PoseSample& targetPose = db->GetPose(results.best.poseIndex);
        std::shared_ptr<Animation> targetAnim =
            db->GetAnimation(targetPose.animationIndex);

        // Native speed of the currently playing animation (the clip's own
        // ground velocity in its local space).
        float nativeAnimSpeed = 1.0f;
        if (targetAnim && targetAnim->speed > 0.0f) {
            nativeAnimSpeed = targetAnim->speed;
        }

        // Actual body speed in world XZ plane (scale-normalized to match
        // the database's native unit space).
        glm::vec3 horizVel = glm::vec3(characterWorldVelocity.x, 0.0f, characterWorldVelocity.z);
        float actualSpeed = glm::length(horizVel) / m_characterScale;

        // Warp ratio: actual / native. 1.0 = perfect match (no adjustment).
        float strideWarpScale = 1.0f;
        if (nativeAnimSpeed > 0.05f) {
            strideWarpScale = actualSpeed / nativeAnimSpeed;
        }
        // Clamp to anatomically plausible bounds — beyond 70% or 140% the
        // foot arc deforms unnaturally.
        strideWarpScale = glm::clamp(strideWarpScale, 0.7f, 1.4f);

        // Only act when there's a meaningful speed mismatch (avoids jitter
        // from tiny floating-point differences near 1.0).
        if (std::abs(strideWarpScale - 1.0f) > 0.001f && dt > 0.0f) {
            // Character forward direction in world space (from model matrix).
            // Model matrix row 2 is the +Z basis (forward).
            glm::vec3 characterForward = glm::vec3(m_modelMatrix[2].x, m_modelMatrix[2].y, m_modelMatrix[2].z);
            characterForward = glm::normalize(characterForward);

            // Per-foot adjustment along the forward axis. We nudge the target
            // position of SWINGING feet so their landing point matches the
            // body's actual travel distance this frame. The factor of dt
            // converts the per-second warp into a per-frame displacement.
            float warpDisplacement = (strideWarpScale - 1.0f) * actualSpeed * dt;

            // Left foot: only adjust if NOT planted (planted feet are locked
            // to world ground and must stay put).
            if (!footPlanting.IsLeftFootPlanted()) {
                animator->leftFootIK.targetPosition += characterForward * warpDisplacement;
            }
            // Right foot
            if (!footPlanting.IsRightFootPlanted()) {
                animator->rightFootIK.targetPosition += characterForward * warpDisplacement;
            }

            if (verbose && (frameCount % 30 == 0)) {
                std::cout << "[StrideWarp] scale=" << strideWarpScale
                          << " actualSpeed=" << actualSpeed
                          << " nativeSpeed=" << nativeAnimSpeed
                          << " warpDisp=" << warpDisplacement << "\n";
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

    // Leg (thigh / shin) bones for the two-bone knee bend. Names follow the
    // AnimationRetargeting bone map: leftupleg/rightupleg (thigh),
    // leftleg/rightleg (shin). Falls back to the Left/Right variants.
    int leftUpLegBone = skeleton->GetBoneIndex("leftupleg");
    int rightUpLegBone = skeleton->GetBoneIndex("rightupleg");
    int leftLegBone = skeleton->GetBoneIndex("leftleg");
    int rightLegBone = skeleton->GetBoneIndex("rightleg");
    if (leftUpLegBone < 0) leftUpLegBone = skeleton->GetBoneIndex("LeftUpLeg");
    if (rightUpLegBone < 0) rightUpLegBone = skeleton->GetBoneIndex("RightUpLeg");
    if (leftLegBone < 0) leftLegBone = skeleton->GetBoneIndex("LeftLeg");
    if (rightLegBone < 0) rightLegBone = skeleton->GetBoneIndex("RightLeg");

    // WORLD-space foot IK. The character's model matrix (translate * rotate *
    // scale, set each frame by the caller) maps the model-space bone positions
    // into world space where a planted foot is genuinely stationary while the
    // body walks over it - in model space it slides backward at the body's
    // speed and every stationary/plant check failed, so no foot ever locked
    // (visible footskating).
    const glm::mat4 modelMatrix = m_modelMatrix;
    // GENUINE world scale of the model (length of the X basis vector of the
    // model matrix). This is the factor that converts the model-space bone
    // positions (currBoneWorldPos) into world metres, and the inverse of what
    // converts world-space IK deltas back into model units. It must NOT carry an
    // arbitrary multiplier: a 25x inflation here made the cached leg reach ~15 m
    // (not ~0.6 m), which silently zeroed the pelvis-drop deficit (knee crouch
    // never applied) and made the over-stride threshold unreachable, so the low-
    // speed walk/jog legs never absorbed and the feet came off.
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
        // CRITICAL: prevBoneWorldPos is in LOCAL/MODEL space. Transform it
        // by LAST FRAME's model matrix (m_prevModelMatrix), not the current
        // one. Using the current frame's matrix computes the previous foot
        // position as if the body had already completed this frame's motion,
        // collapsing the velocity signal and triggering foot plants while
        // the character is still moving — the leg-stretching / rubber-leg bug.
        const glm::vec3 leftPrev =
            glm::vec3(m_prevModelMatrix * glm::vec4(animator->prevBoneWorldPos[leftFootBone], 1.0f));
        const glm::vec3 rightPrev =
            glm::vec3(m_prevModelMatrix * glm::vec4(animator->prevBoneWorldPos[rightFootBone], 1.0f));
        leftFootVel = leftFootPos - leftPrev;
        rightFootVel = rightFootPos - rightPrev;
    }

    // FIX (v6 todo Possibility 1 — "Ghost Momentum Trap"):
    // When the character comes to rest, the capsule's world position stops
    // changing (m_modelMatrix == m_prevModelMatrix), but prevBoneWorldPos still
    // holds LOCAL bone data from the last MOVING animation frame. The world-
    // space velocity calculation above (leftFootPos - leftPrev) then measures
    // the walk→idle pose delta through a stationary matrix, producing a spurious
    // velocity spike opposite to the previous motion. The Schmitt trigger sees
    // this as the foot "breaking away" from the plant, releases the lock, and
    // the matcher selects drifting poses — stretching the leg.
    //
    // Guard: if the capsule hasn't translated in world space (resting on the
    // ground), the foot is planted and stationary. Trust the LOCAL bone delta
    // (curr - prev in the same model space) instead of the world-space rebuild,
    // which is immune to the matrix-mismatch artifact.
    {
        const glm::vec3 modelDelta =
            glm::vec3(m_modelMatrix[3]) - glm::vec3(m_prevModelMatrix[3]);
        const float capsuleWorldDisp =
            glm::length(glm::vec2(modelDelta.x, modelDelta.z));
        if (capsuleWorldDisp < 0.001f) {
            // Standing still — use local bone deltas directly.
            if (leftFootBone  < (int)animator->currBoneWorldPos.size() &&
                leftFootBone  < (int)animator->prevBoneWorldPos.size() &&
                rightFootBone < (int)animator->currBoneWorldPos.size() &&
                rightFootBone < (int)animator->prevBoneWorldPos.size()) {
                leftFootVel  = animator->currBoneWorldPos[leftFootBone]
                             - animator->prevBoneWorldPos[leftFootBone];
                rightFootVel = animator->currBoneWorldPos[rightFootBone]
                             - animator->prevBoneWorldPos[rightFootBone];
            }
        }
    }

    // Update foot planting system (world space + world floor height).
    // Forward the REAL frame delta-time instead of letting FootPlantingSystem
    // fall back to a hardcoded 0.016f tick — at 120fps that 16ms fallback causes
    // sub-frame timer lag (plant/release detection runs at the wrong rate,
    // mis-timed locks, feet that don't sync to the render loop velocity).
    footPlanting.Update(leftFootPos, rightFootPos, leftFootVel, rightFootVel, m_floorHeight, config, dt);

    // Apply foot IK through animator
    // The animator has a complete foot IK system - we just need to enable it
    if (!animator->footIKSettings.enabled) {
        // Configure foot IK if not already enabled
        Animator::FootIKSettings ikSettings;
        ikSettings.enabled = true;
        ikSettings.floorHeight = m_floorHeight;  // world-space terrain height
        ikSettings.ikStrength = 1.0f;
        ikSettings.footLockBlend = 15.0f;   // Full lock within ~4 frames at 60fps (was 5.0f — too slow, foot never fully locked mid-stance at low speeds)
        ikSettings.footLockReleaseSpeed = 3.0f;  // Fast release when lifting
        // Tight ankle-offset budget (world m): the knee bend + soft-extension do
        // the plant, so keep the ankle translate small to avoid the visible
        // ankle-offset stretch at 70% scale. 0.2 m trims the residual.
        ikSettings.maxIKDistance = 0.2f;
        ikSettings.leftFootBone = leftFootBone;
        ikSettings.rightFootBone = rightFootBone;
        ikSettings.leftUpLegBone = leftUpLegBone;
        ikSettings.rightUpLegBone = rightUpLegBone;
        ikSettings.leftLegBone = leftLegBone;
        ikSettings.rightLegBone = rightLegBone;
        ikSettings.kneeBendWeight = 1.0f;
        animator->SetFootIKSettings(ikSettings);
    }

    // FIX (v8 todo Fix 2): Dynamically toggle rest damping based on actual
    // physical movement. When standing perfectly still, turn on the 10Hz
    // low-pass rotation filter to flatten sub-degree knee jitter (the analytical
    // IK solver's cosA bounces against the 175° knee-clamp wall on flat ground
    // due to numerical compression near 180°). Turn it off during movement for
    // raw responsiveness.
    {
        const bool mmPlayerDecelerating = glm::length(moveDirection) < 0.01f;
        const bool mmCapsuleStatic =
            glm::length(characterVelocity) < config.footLiftVelocityThreshold;
        animator->footIKSettings.restDampingEnabled =
            (mmPlayerDecelerating && mmCapsuleStatic);
    }

    // Push the live floor height every frame - it was only written on first
    // enable, so a corrected floor never reached the plant check.
    animator->SetFloorHeight(m_floorHeight);

    // Velocity-robust releases: forward the character's horizontal world
    // velocity to the animator so the over-stride release threshold in the
    // IK solver scales with body speed (prevents false releases on high-speed
    // turns).
    animator->SetCharacterVelocity(characterWorldVelocity);

    // Tell the animator the world scale so the world-space offsets it computes
    // can be converted back to model space for the skinning matrices.
    animator->SetIKWorldScale(modelScale);
    // Forward the terrain heightmap so the foot IK can tilt feet to ground
    // slopes (nil = flat foot). Set every frame alongside floorHeight/scale.
    animator->SetTerrainHeightFn(m_terrainFn);

    // PELVIS HEIGHT ADJUSTMENT (knee-bend fix): measure how far each locked
    // foot's leg is from reaching its target, then drop the Hips bone so the
    // two-bone knee bend carries the foot down instead of stretching the
    // ankle. Must run AFTER bone evaluation (currBoneWorldPos is fresh) and
    // BEFORE the lock-resolution IK solve (the IK reads the pelvis-adjusted
    // pose). The pelvis drop is applied with a 1-frame lag
    // (currentPelvisDropY is consumed in the next frame's EvaluateNode),
    // masked by asymmetric smoothing.
    animator->CalculatePelvisAdjustment(dt, modelMatrix);

    // ====================================================================
    // SMOOTH PLANTED POSITION TRACKING (Fix 3 from updated todo)
    // During idle loops, sub-millimetre root hip anchor drift from motion-
    // matching database frame swaps causes the planted foot positions to jitter.
    // Apply a low-frequency 14 Hz low-pass envelope on the locked world anchors
    // so the IK targets settle into single, silent state lines. In locomotion
    // the buffers reset instantly (no lag / foot sliding).
    // ====================================================================
    {
        static glm::vec3 dampedPlantedWorldL(0.0f);
        static glm::vec3 dampedPlantedWorldR(0.0f);

        float capsuleSpeed = glm::length(characterWorldVelocity);
        if (capsuleSpeed < 0.5f) {  // Extended from 0.05f to cover low walk speeds
            float standDampingAlpha = glm::clamp(14.0f * dt, 0.0f, 1.0f);
            dampedPlantedWorldL = glm::mix(dampedPlantedWorldL,
                                           footPlanting.GetLeftFootPosition(),
                                           standDampingAlpha);
            dampedPlantedWorldR = glm::mix(dampedPlantedWorldR,
                                           footPlanting.GetRightFootPosition(),
                                           standDampingAlpha);
            // Feed the smoothed coordinates back into the tracking solver
            footPlanting.SetLeftFootPlantedPosition(dampedPlantedWorldL);
            footPlanting.SetRightFootPlantedPosition(dampedPlantedWorldR);
        } else {
            // Reset tracking buffers instantly during locomotion
            dampedPlantedWorldL = footPlanting.GetLeftFootPosition();
            dampedPlantedWorldR = footPlanting.GetRightFootPosition();
        }
    }

    // ====================================================================
    // LOCK RESOLUTION PASS — replaces Animator::UpdateFootIK.
    //
    // The old path called animator->UpdateFootIK() which had its OWN internal
    // foot locking that could stay forcefully engaged during fast gait cycles
    // (the "ghost lock"): feet stayed pinned to old ground coordinates while
    // the body strode forward, elastically stretching the leg past its
    // anatomical max reach (ankleOffset.y hit -1.73 m, hip2foot=0.73 m vs
    // maxReach=0.61 m — reach=NO).
    //
    // New flow: FootPlantingSystem::FootState::Update now does velocity-based
    // lock dissolution — a foot moving faster than footLiftVelocityThreshold
    // breaks its lock instantly. This pass then uses footPlanting's
    // isLocked/lockWeight/plantedWorldPos as the authoritative lock state and
    // drives SolveLegIK directly, so the IK target is ALWAYS in sync with the
    // actual planting state. No more stale anchors.
    // ====================================================================

    // Reset leg IK state to identity so a freed foot contributes no residual
    // rotation/translation this frame (SET semantics, not ADD).
    const int legBones[] = {
        leftUpLegBone, rightUpLegBone, leftLegBone, rightLegBone
    };
    for (int b : legBones) {
        if (b >= 0 && b < (int)animator->ikRotations.size())
            animator->ikRotations[b] = glm::mat4(1.0f);
    }
    const int footBones[] = { leftFootBone, rightFootBone };
    for (int b : footBones) {
        if (b >= 0 && b < (int)animator->ikOffsets.size())
            animator->ikOffsets[b] = glm::vec3(0.0f);
        if (b >= 0 && b < (int)animator->ikFootTilt.size())
            animator->ikFootTilt[b] = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }

    // Static smoothing state for the velocity-scaled low-pass filter (Fix 2
    // from updated todo: eliminates micro-vibration by tying the interpolation
    // frequency to character speed — heavy smoothing at rest, pure passthrough
    // at high velocity).
    static glm::vec3 stableAnkleOffsetL(0.0f);
    static glm::vec3 stableAnkleOffsetR(0.0f);
    static glm::quat stableThighQuatL(1.0f, 0.0f, 0.0f, 0.0f);
    static glm::quat stableShinQuatL(1.0f, 0.0f, 0.0f, 0.0f);
    static glm::quat stableThighQuatR(1.0f, 0.0f, 0.0f, 0.0f);
    static glm::quat stableShinQuatR(1.0f, 0.0f, 0.0f, 0.0f);

    // DYNAMIC INTERPOLATION ENVELOPE: smooth all output transforms based on
    // capsule speed. At 0 m/s (idle) blendAlpha = 4*dt ≈ heavy smoothing;
    // at >= 2 m/s (walk) blendAlpha ramps up to 45*dt ≈ near-raw. The speed
    // threshold was 5.0 m/s (only sprint exceeded it) which left walk speeds
    // in the worst damping zone. Lowered to 2.0 m/s so the full smoothing
    // range covers idle→walk→run.
    float capsuleSpeed = glm::length(characterWorldVelocity);
    float blendTrackingSpeed = glm::mix(4.0f, 45.0f,
                                        glm::clamp(capsuleSpeed / 2.0f, 0.0f, 1.0f));
    float blendAlpha = glm::clamp(blendTrackingSpeed * dt, 0.0f, 1.0f);

    // ====================================================================
    // DYNAMIC PELVIS DAMPING SINK (Fix 2 from updated todo)
    // Solves the reach=NO error (d=0.678 > maxReach=0.606) by computing the
    // exact leg-length deficit demanded by the floor anchors and smoothly
    // dropping the pelvis by that amount. The leg chain bends naturally
    // instead of the ankleOffset stretching the leg bone past its max reach
    // (the violent vibration source from telemetry logs).
    // ====================================================================
    // 1. Gather raw animated hip (thigh-root) world positions
    glm::vec3 hipLeftWorld =
        glm::vec3(modelMatrix * animator->globalBoneMatrices[leftUpLegBone] *
                  glm::vec4(0, 0, 0, 1.0f));
    glm::vec3 hipRightWorld =
        glm::vec3(modelMatrix * animator->globalBoneMatrices[rightUpLegBone] *
                  glm::vec4(0, 0, 0, 1.0f));

    float legReachL = animator->GetLeftLegMaxReach();
    float legReachR = animator->GetRightLegMaxReach();

    // Target distances from hip to the locked foot anchor
    float targetDistL = 0.0f, targetDistR = 0.0f;
    float lengthDeficitL = 0.0f, lengthDeficitR = 0.0f;

    if (footPlanting.IsLeftFootPlanted()) {
        targetDistL = glm::distance(hipLeftWorld, footPlanting.GetLeftFootPosition());
        if (targetDistL > (legReachL * 0.96f)) {
            lengthDeficitL = targetDistL - (legReachL * 0.96f);
        }
    }
    if (footPlanting.IsRightFootPlanted()) {
        targetDistR = glm::distance(hipRightWorld, footPlanting.GetRightFootPosition());
        if (targetDistR > (legReachR * 0.96f)) {
            lengthDeficitR = targetDistR - (legReachR * 0.96f);
        }
    }

    // Take the worst-case deficit and clamp to an absolute 12cm drop cap
    float requiredPelvisSink = glm::max(lengthDeficitL, lengthDeficitR);
    requiredPelvisSink = glm::clamp(requiredPelvisSink, 0.0f, 0.12f);

    // FIX (v10 todo — "Pelvis Sink Trampoline"):
    // When the character is at rest, the hip position is read from
    // globalBoneMatrices which already includes the previous frame's pelvis
    // drop. Sub-millimetre float noise creates a tiny length deficit, which
    // the low-pass filter pushes the hips down, which on the next frame
    // recalculates a slightly different deficit — a vertical oscillation
    // feedback loop. Inject a 5mm deadzone floor: if the deficit is below
    // 5mm while stationary, wipe it to zero to break the trampoline.
    {
        const bool mmAtRest =
            glm::length(moveDirection) < 0.01f &&
            glm::length(characterVelocity) < config.footLiftVelocityThreshold;
        if (mmAtRest && requiredPelvisSink < 0.005f) {
            requiredPelvisSink = 0.0f;
        }
    }

    // Low-pass filter height corrections (absorbs microscale database snaps)
    static float filteredPelvisSink = 0.0f;
    float absorptionRate = (capsuleSpeed < 0.1f) ? 8.0f : 22.0f;
    filteredPelvisSink = glm::mix(filteredPelvisSink, requiredPelvisSink,
                                  absorptionRate * dt);
    // FIX (v10 todo): Hard-snap filteredPelvisSink to absolute zero when at rest
    // and below the 1mm deadzone floor. This fully kills any residual
    // sub-frame oscillation from the low-pass filter's exponential tail.
    {
        const bool mmAtRest =
            glm::length(moveDirection) < 0.01f &&
            glm::length(characterVelocity) < config.footLiftVelocityThreshold;
        if (mmAtRest && filteredPelvisSink < 0.001f) {
            filteredPelvisSink = 0.0f;
        }
    }

    // Construct the stabilized model matrix (pelvis shifted down) and feed
    // it into the IK solvers so the two-bone knee bend carries the foot
    // down naturally instead of stretching the ankleOffset past max reach.
    const glm::mat4 stabilizedModelMatrix =
        glm::translate(modelMatrix, glm::vec3(0.0f, -filteredPelvisSink, 0.0f));

    // --- Left Foot Lock Resolution Pass --
    {
        glm::mat4 outThighRotL(1.0f), outShinRotL(1.0f);
        glm::vec3 outAnkleOffsetL(0.0f);
        glm::vec3 worldAnkleEndL(0.0f), worldKneeEndL(0.0f);

        if (footPlanting.IsLeftFootPlanted()) {
            // Foot is firmly planted: drive the IK to the locked world anchor.
            glm::vec3 lockedTargetWorld = footPlanting.GetLeftFootPosition();
            animator->SolveLegIK(leftUpLegBone, leftLegBone, leftFootBone,
                                 stabilizedModelMatrix, lockedTargetWorld,
                                 outThighRotL, outShinRotL, outAnkleOffsetL,
                                 worldAnkleEndL, worldKneeEndL);
        } else if (footPlanting.GetLeftFootLockWeight() > 0.001f) {
            // Blending out of a lock: interpolate between the current animated
            // ankle position and the locked anchor so the target slides back
            // down into the animated local bone coordinates instead of floating
            // behind the capsule.
            glm::vec3 animatedAnkleWorld =
                glm::vec3(modelMatrix * animator->globalBoneMatrices[leftFootBone] *
                          glm::vec4(0, 0, 0, 1.0f));
            glm::vec3 blendedTargetWorld = glm::mix(
                animatedAnkleWorld,
                footPlanting.GetLeftFootPosition(),
                footPlanting.GetLeftFootLockWeight());

            animator->SolveLegIK(leftUpLegBone, leftLegBone, leftFootBone,
                                 stabilizedModelMatrix, blendedTargetWorld,
                                 outThighRotL, outShinRotL, outAnkleOffsetL,
                                 worldAnkleEndL, worldKneeEndL);
        } else {
            // Pure swing phase: bypass world targeting entirely to let the
            // animation clip play natively — no knee bend, no ankle translate.
            outThighRotL  = glm::mat4(1.0f);
            outShinRotL   = glm::mat4(1.0f);
            outAnkleOffsetL = glm::vec3(0.0f);
        }

        // Inject the resolved IK into the animator's skeleton bone registry.
        // Blend the knee-bend rotation by lockWeight (slerp from identity) so
        // the leg fades out smoothly instead of popping when the foot lifts.
        float wL = footPlanting.GetLeftFootLockWeight();
        glm::quat qThighL = glm::slerp(glm::quat(1.0f, 0, 0, 0),
                                       glm::quat_cast(outThighRotL), wL);
        glm::quat qShinL  = glm::slerp(glm::quat(1.0f, 0, 0, 0),
                                       glm::quat_cast(outShinRotL), wL);

        // VELOCITY-SCALED LOW-PASS: smooth the lockWeight-blended quaternions
        // to isolate micro-vibrations. blendAlpha is heavy at idle (kills
        // ground-normal flutter) and light during locomotion (no lag).
        stableThighQuatL = glm::slerp(stableThighQuatL, qThighL, blendAlpha);
        stableShinQuatL  = glm::slerp(stableShinQuatL,  qShinL,  blendAlpha);

        // Apply ankle-offset deadzone + low-pass: if the drift since last frame
        // is under 5 mm, freeze the value; otherwise low-pass filter.
        // Threshold was 0.01f world m/s (idle only); extended to 1.0f so walk
        // speeds still get the micro-jitter kill.
        if (glm::length(characterWorldVelocity) < 1.0f) {
            float driftL = glm::distance(outAnkleOffsetL, stableAnkleOffsetL);
            if (driftL < 0.005f) {
                outAnkleOffsetL = stableAnkleOffsetL;
            } else {
                stableAnkleOffsetL = glm::mix(stableAnkleOffsetL,
                                                outAnkleOffsetL, 8.0f * dt);
                outAnkleOffsetL = stableAnkleOffsetL;
            }
        } else {
            stableAnkleOffsetL = outAnkleOffsetL;
        }

        if (leftUpLegBone >= 0 && leftUpLegBone < (int)animator->ikRotations.size())
            animator->ikRotations[leftUpLegBone] = glm::mat4_cast(stableThighQuatL);
        if (leftLegBone >= 0 && leftLegBone < (int)animator->ikRotations.size())
            animator->ikRotations[leftLegBone]  = glm::mat4_cast(stableShinQuatL);
        if (leftFootBone >= 0 && leftFootBone < (int)animator->ikOffsets.size())
            animator->ikOffsets[leftFootBone]   = outAnkleOffsetL * wL;
    }

    // --- Right Foot Lock Resolution Pass ---
    {
        glm::mat4 outThighRotR(1.0f), outShinRotR(1.0f);
        glm::vec3 outAnkleOffsetR(0.0f);
        glm::vec3 worldAnkleEndR(0.0f), worldKneeEndR(0.0f);

        if (footPlanting.IsRightFootPlanted()) {
            glm::vec3 lockedTargetWorld = footPlanting.GetRightFootPosition();
            animator->SolveLegIK(rightUpLegBone, rightLegBone, rightFootBone,
                                 stabilizedModelMatrix, lockedTargetWorld,
                                 outThighRotR, outShinRotR, outAnkleOffsetR,
                                 worldAnkleEndR, worldKneeEndR);
        } else if (footPlanting.GetRightFootLockWeight() > 0.001f) {
            glm::vec3 animatedAnkleWorld =
                glm::vec3(modelMatrix * animator->globalBoneMatrices[rightFootBone] *
                          glm::vec4(0, 0, 0, 1.0f));
            glm::vec3 blendedTargetWorld = glm::mix(
                animatedAnkleWorld,
                footPlanting.GetRightFootPosition(),
                footPlanting.GetRightFootLockWeight());

            animator->SolveLegIK(rightUpLegBone, rightLegBone, rightFootBone,
                                 stabilizedModelMatrix, blendedTargetWorld,
                                 outThighRotR, outShinRotR, outAnkleOffsetR,
                                 worldAnkleEndR, worldKneeEndR);
        } else {
            outThighRotR   = glm::mat4(1.0f);
            outShinRotR    = glm::mat4(1.0f);
            outAnkleOffsetR = glm::vec3(0.0f);
        }

        float wR = footPlanting.GetRightFootLockWeight();
        glm::quat qThighR = glm::slerp(glm::quat(1.0f, 0, 0, 0),
                                       glm::quat_cast(outThighRotR), wR);
        glm::quat qShinR  = glm::slerp(glm::quat(1.0f, 0, 0, 0),
                                       glm::quat_cast(outShinRotR), wR);

        // VELOCITY-SCALED LOW-PASS (right leg) — same envelope as left.
        stableThighQuatR = glm::slerp(stableThighQuatR, qThighR, blendAlpha);
        stableShinQuatR  = glm::slerp(stableShinQuatR,  qShinR,  blendAlpha);

        // Ankle-offset deadzone (right) — freeze below 5 mm at rest.
        if (glm::length(characterWorldVelocity) < 1.0f) {
            float driftR = glm::distance(outAnkleOffsetR, stableAnkleOffsetR);
            if (driftR < 0.005f) {
                outAnkleOffsetR = stableAnkleOffsetR;
            } else {
                stableAnkleOffsetR = glm::mix(stableAnkleOffsetR,
                                                outAnkleOffsetR, 8.0f * dt);
                outAnkleOffsetR = stableAnkleOffsetR;
            }
        } else {
            stableAnkleOffsetR = outAnkleOffsetR;
        }

        if (rightUpLegBone >= 0 && rightUpLegBone < (int)animator->ikRotations.size())
            animator->ikRotations[rightUpLegBone] = glm::mat4_cast(stableThighQuatR);
        if (rightLegBone >= 0 && rightLegBone < (int)animator->ikRotations.size())
            animator->ikRotations[rightLegBone]  = glm::mat4_cast(stableShinQuatR);
        if (rightFootBone >= 0 && rightFootBone < (int)animator->ikOffsets.size())
            animator->ikOffsets[rightFootBone]   = outAnkleOffsetR * wR;
    }

    // One-time verification: the two-bone path found the leg bones and the
    // bone chain is populated (currBoneWorldPos holds the pose from the
    // lock-resolution pass above - valid from the first frame Animator::Update
    // ran).
    static bool loggedLegIK = false;
    if (!loggedLegIK && leftFootBone >= 0 && leftUpLegBone >= 0) {
        float L1 = 0.0f, L2 = 0.0f;
        const auto& bp = animator->currBoneWorldPos;
        if (leftUpLegBone < (int)bp.size() && leftLegBone < (int)bp.size() &&
            leftFootBone < (int)bp.size()) {
            glm::vec3 h = glm::vec3(modelMatrix * glm::vec4(bp[leftUpLegBone], 1.0f));
            glm::vec3 k = glm::vec3(modelMatrix * glm::vec4(bp[leftLegBone], 1.0f));
            glm::vec3 a = glm::vec3(modelMatrix * glm::vec4(bp[leftFootBone], 1.0f));
            L1 = glm::length(k - h);
            L2 = glm::length(a - k);
        }
        // Only log once the bone chain is actually populated (L1>0); otherwise
        // retry on a later frame instead of printing zeros from frame 0.
        if (L1 > 0.0f) {
            loggedLegIK = true;
            std::cout << "[TwoBone] leg bones L/R thigh="<<leftUpLegBone<<"/"<<rightUpLegBone
                      << " shin="<<leftLegBone<<"/"<<rightLegBone
                      << " ankle="<<leftFootBone<<"/"<<rightFootBone
                      << " scale="<<modelScale
                      << " L1="<<L1<<" L2="<<L2
                      << " kneeWeight="<<animator->footIKSettings.kneeBendWeight<<"\n";
        }
    }
}

void MotionMatcher::UpdateDebugInfo() {
    const MotionDatabase* db = EffectiveDatabase();
    if (!db) {
        // No database loaded yet (pre-LoadAnimation / after a failed switch):
        // do NOT dereference null in GetPose/GetAnimationName below.
        debug.currentAnimationIndex = -1;
        debug.currentAnimationName = "None";
        debug.leftFootPlanted = false;
        debug.rightFootPlanted = false;
        debug.currentAnimationTime = currentAnimTime;
        return;
    }
    debug.currentAnimationIndex = currentPoseIndex >= 0 ?
        db->GetPose(currentPoseIndex).animationIndex : -1;
    debug.currentAnimationTime = currentAnimTime;

    // Use the REGISTERED clip name ("Jump") not the raw FBX channel name
    // ("mixamo.com" - identical across Mixamo clips).
    debug.currentAnimationName = currentPoseIndex >= 0
        ? db->GetAnimationName(
              db->GetPose(currentPoseIndex).animationIndex)
        : "None";

    debug.leftFootPlanted = footPlanting.IsLeftFootPlanted();
    debug.rightFootPlanted = footPlanting.IsRightFootPlanted();
}

std::shared_ptr<Animation> MotionMatcher::GetCurrentAnimation() const {
    if (currentPoseIndex < 0) return nullptr;

    const MotionDatabase* db = EffectiveDatabase();
    if (!db) return nullptr;
    const PoseSample& pose = db->GetPose(currentPoseIndex);
    return db->GetAnimation(pose.animationIndex);
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
    const MotionDatabase* db = EffectiveDatabase();
    return db ? db->GetDebugInfo() : std::string();
}

MotionMatcher::Stats MotionMatcher::GetStats() const {
    Stats s;

    // KD-Tree geometry
    MotionKDTree::TreeStats ts = searchTree.GetStats();
    s.kdTreeNodes       = ts.totalNodes;
    s.kdTreeLeaves      = ts.leafNodes;
    s.kdTreeInternal    = ts.internalNodes;
    s.kdTreeMaxDepth    = ts.maxDepth;
    s.kdTreeAvgLeafSize = ts.avgLeafSize;

    // Database scale
    const MotionDatabase* db = EffectiveDatabase();
    if (db) {
        s.poseCount      = db->GetPoseCount();
        s.animationCount = db->GetAnimationCount();
    }

    // Last-frame search metrics
    s.posesSearched   = debug.posesSearched;
    s.searchTimeMs    = debug.searchTimeMs;
    s.searchScore     = debug.currentResult.score;

    // SIMD availability (compile-time + runtime detection)
#if defined(__AVX2__)
    extern bool cpuSupportsAVX2();  // from MotionKDTreeSIMD.h (included via MotionDatabase.h)
    s.simdAvailable = true;
    s.simdActive    = cpuSupportsAVX2();
#else
    s.simdAvailable = false;
    s.simdActive    = false;
#endif

    // Current pose info — use the CLIP-FRAME speed (the matcher rotates
    // world velocity into clip space before search), not the raw model-space
    // characterVelocity.z which can be huge (un-scaled model units).
    s.currentClip     = debug.currentAnimationName;
    s.currentClipTime = currentAnimTime;
    // Speed = magnitude of the horizontal velocity (XZ plane in model space)
    s.querySpeed      = glm::length(glm::vec2(characterVelocity.x, characterVelocity.z));

    return s;
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

        // Pose state indexes the previous database's array - reset it so the
        // first search in the new context starts fresh instead of comparing
        // against a stale pose index into a different pose array.
        currentPoseIndex = -1;
        currentAnimationPtr = nullptr;

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

        // Same reset as the owning path: the pose index belongs to the old
        // database's array, and the tree must be rebuilt over the new one
        // (the old SetCurrentDatabase did this; the ref SetDatabase didn't).
        currentPoseIndex = -1;
        currentAnimationPtr = nullptr;
        BuildSearchIndex();
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

    // Validate the ACTIVE database (the non-owning context switch when set),
    // not the owned fallback - the search runs against the active one.
    const MotionDatabase* db = EffectiveDatabase();
    if (!db) return false;
    const auto& poses = db->GetPoses();
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

    const MotionDatabase* db = EffectiveDatabase();
    const PoseSample& pose = db->GetPose(currentPoseIndex);
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
            // Release previous internal assets to prevent memory accumulation
            // leaks when transitioning from an owned database to an external ref.
            if (database) {
                database->Clear();
            }
            pendingDatabaseRef = nullptr;
            pendingDatabaseIsRef = false;
        } else if (pendingDatabase) {
            database = std::move(pendingDatabase);
            currentDatabaseName = "Blended";
        }
        pendingDatabase.reset();
        databaseBlendProgress = 0.0f;
        isBlendingDatabases = false;

        // Pose state still indexes the OLD database's array and the tree was
        // just rebuilt over the new one - reset it so the first search in the
        // new context starts with a fresh match instead of a stale pose index
        // into a different pose array.
        currentPoseIndex = -1;
        currentAnimationPtr = nullptr;

        // Rebuild search index
        BuildSearchIndex();

        std::cout << "[MotionMatcher] Database blend complete\n";
    }
}
