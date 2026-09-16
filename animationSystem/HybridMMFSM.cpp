#include "HybridMMFSM.h"
#include <iostream>
#include <string>
#include <algorithm>
#include <glm/gtc/quaternion.hpp>
#include "../editor/config.h"  // InertializationConfig for live editor tuning

// ============================================================================
// HYBRID MM + FSM IMPLEMENTATION (FIXED - Proper shared_ptr ownership)
// ============================================================================

HybridMMFSM::HybridMMFSM() {
    std::cout << "[HybridMMFSM] Constructor called - this=" << this << "\n";
}

HybridMMFSM::~HybridMMFSM() {
    std::cout << "[HybridMMFSM] Destructor called - this=" << this << "\n";
    // Clear all databases (shared_ptr will properly clean up animations)
    std::cout << "[HybridMMFSM] Clearing " << hybridStateSlots.size() << " state databases\n";
    hybridStateSlots.clear();
    std::cout << "[HybridMMFSM] Destructor complete\n";
}

void HybridMMFSM::Initialize(const Skeleton* skel, Animator* anim) {
    if (!skel) {
        std::cerr << "[HybridMMFSM] ERROR: Null skeleton!\n";
        return;
    }

    skeleton = skel;
    animator = anim;

    // Initialize motion matcher for locomotion
    motionMatcher.Initialize(skeleton, animator);
    mmActive = true;

    // Pre-create databases for each state
    hybridStateSlots[HybridState::LOCOMOTION].database = std::make_unique<MotionDatabase>();
    hybridStateSlots[HybridState::CROUCH_WALK].database = std::make_unique<MotionDatabase>();
    hybridStateSlots[HybridState::COMBAT].database = std::make_unique<MotionDatabase>();

    std::cout << "[HybridMMFSM] Initialized - MM for locomotion, FSM for states\n";
    std::cout << "  ✓ Proper unique_ptr ownership enabled\n";
    std::cout << "  ✓ Inertialization blending ready\n";
    std::cout << "  ✓ State-specific databases created\n";
}

// CRITICAL FIX: Takes shared_ptr - caller MUST use std::make_shared
void HybridMMFSM::LoadLocomotionAnimation(const std::string& name, std::shared_ptr<Animation> anim) {
    std::cout << "[LoadLocomotionAnimation] ENTER: name='" << name << "' anim=" << (void*)anim.get() << "\n";
    
    if (!anim) {
        std::cerr << "[HybridMMFSM] ERROR: Null locomotion animation: " << name << "\n";
        return;
    }
    
    // Validate name
    if (name.empty()) {
        std::cerr << "[HybridMMFSM] ERROR: Empty locomotion animation name\n";
        return;
    }
    
    std::cout << "[LoadLocomotionAnimation] anim->name='" << anim->name << "' duration=" << anim->duration << "\n";

    // Validate animation before loading
    if (anim->duration <= 0.0f || anim->duration > 10.0f) {
        std::cerr << "[HybridMMFSM] ERROR: Invalid duration " << anim->duration
                  << "s for " << name << "\n";
        return;
    }

    if (anim->boneAnimations.empty()) {
        std::cerr << "[HybridMMFSM] ERROR: No bone animations for " << name << "\n";
        return;
    }

    std::cout << "[LoadLocomotionAnimation] Calling motionMatcher.LoadAnimation...\n";
    // MotionMatcher takes shared_ptr - proper ownership transfer
    motionMatcher.LoadAnimation(name, anim);

    std::cout << "[HybridMMFSM] Loaded locomotion: " << name
              << " (duration=" << anim->duration << "s, bones=" << anim->boneAnimations.size() << ")\n";
}

// CRITICAL FIX: Takes shared_ptr - caller MUST use std::make_shared
void HybridMMFSM::LoadStateAnimation(HybridState state, const std::string& name, std::shared_ptr<Animation> anim) {
    if (!anim) {
        std::cerr << "[HybridMMFSM] ERROR: Null state animation: " 
                  << HybridStateToString(state) << " - " << name << "\n";
        return;
    }

    // Validate animation
    if (anim->duration <= 0.0f || anim->duration > 10.0f) {
        std::cerr << "[HybridMMFSM] ERROR: Invalid duration " << anim->duration 
                  << "s for " << HybridStateToString(state) << " - " << name << "\n";
        return;
    }

    if (anim->boneAnimations.empty()) {
        std::cerr << "[HybridMMFSM] ERROR: No bone animations for " 
                  << HybridStateToString(state) << " - " << name << "\n";
        return;
    }

    // Create database for this state if it doesn't exist
    auto& slot = hybridStateSlots[state];
    if (!slot.database) {
        slot.database = std::make_unique<MotionDatabase>();
        std::cout << "[HybridMMFSM] Created database for state: " << HybridStateToString(state) << "\n";
    }

    // Add animation to state's database - database takes ownership via shared_ptr
    slot.database->AddAnimation(name, anim, skeleton);
    slot.isBuilt = false;
    
    std::cout << "[HybridMMFSM] Loaded state animation: " << HybridStateToString(state) 
              << " - " << name << " (duration=" << anim->duration << "s)\n";
}

void HybridMMFSM::BuildDatabases() {
    // Build MM database for locomotion
    if (mmActive) {
        motionMatcher.BuildSearchIndex();
        std::cout << "[HybridMMFSM] Locomotion MM database built\n";
    }

    // FIX (v11 Section 2): Pre-build and warm EVERY state-specific KD-Tree
    // at load time. Previously this rebuilt the tree each frame during state
    // switches, causing FPS dips. Now each state owns a persistent tree that
    // is swapped in by reference at runtime (zero allocation).
    for (auto& [state, slot] : hybridStateSlots) {
        if (slot.database && slot.database->GetPoseCount() > 0) {
            std::cout << "[Engine Performance] Pre-building warm KD-Tree for State: "
                      << HybridStateToString(state) << " ("
                      << slot.database->GetPoseCount() << " poses)\n";

            slot.searchTree = std::make_unique<MotionKDTree>();
            slot.searchTree->BuildWithSAH(slot.database->GetPoses(), 10);
            slot.isBuilt = true;

            // FIX (v12 Section 2): Pre-bake the SIMD SoA cache at load time
            // so runtime database switches via SetDatabaseExplicit never
            // trigger mid-frame heap allocations in RebuildSIMDCacheIfNeeded.
            slot.WarmSIMDCache();
        }
    }

    // ---- Structural Motion Graph: Pre-compute transition clips ----
    // Instead of instant database swaps at runtime, pre-compute smooth
    // transition clips between all state pairs. Each clip is generated using
    // the paper's window-based similarity metric (§3.1), 2D rigid alignment
    // (equation 1), and C1-continuous slerp blending (equations 5-7).
    // This eliminates the need for stature offset / pose blend patches.
    std::cout << "[HybridMMFSM] Building structural motion transition graph...\n";
    // BuildTransitions expects a map<int, MotionDatabase*>.
    // Convert from our HybridState-keyed hybridStateSlots.
    std::map<int, MotionDatabase*> dbPtrMap;
    for (auto& [state, slot] : hybridStateSlots) {
        if (slot.database && slot.database->GetPoseCount() > 0) {
            dbPtrMap[static_cast<int>(state)] = slot.database.get();
        }
    }
    transitionGraph.BuildTransitions(dbPtrMap, skeleton);

    // Restore the active database back to the primary locomotion DB.
    // FIX (v12 Section 2): Use ResetDatabaseExplicit instead of
    // SetCurrentDatabase — the latter calls BuildSearchIndex() which rebuilds
    // the KD-tree we just built above, wasting a full allocation mid-init.
    if (mmActive && motionMatcher.GetDatabase()) {
        motionMatcher.ResetDatabaseExplicit();
    }
}

void HybridMMFSM::AddTransition(HybridState from, HybridState to, float duration, 
                                 std::function<bool()> condition, bool useInertialization) {
    HybridTransition t;
    t.from = from;
    t.to = to;
    t.blendDuration = duration;
    t.inertializationDuration = duration * 1.5f;  // Inertialization is typically longer
    t.condition = condition;
    t.useInertialization = useInertialization;
    transitions.push_back(t);
    
    std::cout << "[HybridMMFSM] Added transition: " << HybridStateToString(from) 
              << " -> " << HybridStateToString(to) 
              << " (blend=" << duration << "s, inertialization=" << (useInertialization ? "ON" : "OFF") << ")\n";
}

void HybridMMFSM::AddTransitionWithInertialization(HybridState from, HybridState to, 
                                                    float blendDuration, float inertializationDuration,
                                                    std::function<bool()> condition) {
    HybridTransition t;
    t.from = from;
    t.to = to;
    t.blendDuration = blendDuration;
    t.inertializationDuration = inertializationDuration;
    t.condition = condition;
    t.useInertialization = true;
    transitions.push_back(t);
    
    std::cout << "[HybridMMFSM] Added inertialized transition: " << HybridStateToString(from) 
              << " -> " << HybridStateToString(to) 
              << " (blend=" << blendDuration << "s, inertial=" << inertializationDuration << "s)\n";
}

void HybridMMFSM::Update(float dt, const HybridMMFSMState& state) {
    characterState = state;

    // Update state machine first
    UpdateStateMachine(dt);

    // Update inertialization if active
    if (inertialization.active) {
        UpdateInertialization(dt);
    }

    // =========================================================================
    // CONTINUOUS ANIMATION FLOW (replaces Standstill Posture Lock)
    //
    // Previously: captured a frozen skeleton snapshot on the first frame of
    // rest and replayed it for all subsequent idle frames, bypassing the live
    // IK solver to prevent sub-millimetre knee jitter ("IK Loop Churn").
    //
    // Now: the structural motion graph handles ALL state transitions via
    // pre-computed transition clips (slerp + 2D alignment). Idle poses are
    // selected from the motion database by the MotionMatcher each frame.
    // Foot IK runs continuously with sub-frame damping (ApplyFootIK) to
    // eliminate jitter without freezing — all clips flow seamlessly through
    // the graph. No pose is ever frozen, so all animation is continuous.
    // =========================================================================

    // =========================================================================
    // Standard live execution loop for active gameplay
    // =========================================================================
    switch (currentState) {
        case HybridState::LOCOMOTION:
            UpdateLocomotion(dt);
            break;
        case HybridState::JUMP:
            UpdateJump(dt);
            break;
        case HybridState::FALL:
            UpdateFall(dt);
            break;
        case HybridState::CROUCH:
        case HybridState::CROUCH_WALK:
            UpdateCrouch(dt);
            break;
        default:
            break;
    }

    // =========================================================================
    // FIX: Tight synchronous layout loop to destroy leg stretching.
    // The state tick above updates animation sampling (Pass 1 bone evaluation),
    // but foot IK / pelvis adjustment must run in the SAME frame against fresh
    // bone positions. Previously these ran on the next frame, using stale root
    // data — knees pulled straight and ankles stretched during pose switches.
    // =========================================================================
    if (animator && skeleton && mmActive) {
        // Character world transform: root bone's global matrix (identity-safe).
        glm::mat4 currentModelMatrix =
            animator->globalBoneMatrices.empty()
                ? glm::mat4(1.0f)
                : animator->globalBoneMatrices[0];

        motionMatcher.SetCharacterModelMatrix(currentModelMatrix);
        motionMatcher.SetFloorHeight(characterState.position.y);
        motionMatcher.ApplyFootIK(dt);
        // Re-evaluate skeleton in this frame so the visual pose matches the
        // IK solve — eliminates the 1-frame lag that caused knee jitter.
        animator->RevalidateIK();

        // ---- Structural Motion Graph: Transition clip playback ----
        // When a pre-computed transition clip is playing, the MotionMatcher
        // handles playback internally (driving the Animator from the clip).
        // No runtime stature offset or pose blend patches needed — the clip
        // already structurally blends all joints via slerp + 2D alignment.
        // Just check for completion and commit the state switch.
        if (m_transitionClipPlaying && !motionMatcher.IsPlayingTransitionClip()) {
            // Transition clip has completed — MotionMatcher has swapped to
            // the target database. Commit the state switch.
            m_transitionClipPlaying = false;
            if (isTransitioning) {
                isTransitioning = false;
                transitionProgress = 0.0f;
                previousState = transitionFrom;
                currentState = transitionTo;
                std::cout << "[HybridMMFSM] Transition complete (Atomic database hand-off): "
                          << HybridStateToString(previousState) << " -> "
                          << HybridStateToString(currentState) << "\n";

                // CRITICAL FIX: FORCE THE INSTANT RESTORATION OF TARGET ANIM
                // TO LAYER STACK. When IsPlayingTransitionClip() turns false,
                // the state machine commits the switch, but the Animator may
                // not have had its base blend weights raised above 0.0001f yet
                // for the incoming database, causing totalWeight < 0.0001f →
                // T-pose snap on the next bone buffer update. Prime the layer
                // stack immediately so the target animation is at full weight
                // the same frame the transition completes.
                auto targetAnim = motionMatcher.GetCurrentAnimation();
                if (targetAnim && animator) {
                    animator->Play(targetAnim.get());
                    animator->SetCurrentTime(motionMatcher.GetCurrentAnimationTime());
                }
            }
        }

        animator->UpdateBoneBuffer();
    }
}

void HybridMMFSM::UpdateStateMachine(float dt) {
    // Update transition progress
    if (isTransitioning) {
        // If a structural transition clip is playing, use its duration
        // (≈0.33s) instead of the default blend duration (0.1s) so the
        // state machine commits at the same time the clip completes.
        float effectiveDuration = transitionDuration;
        if (m_transitionClipPlaying) {
            effectiveDuration = (float)motionMatcher.GetTransitionClipDuration();
        }

        transitionProgress += dt / effectiveDuration;
        if (transitionProgress >= 1.0f) {
            // If a transition clip is playing, let the clip complete first.
            // The state switch is committed in Update() when
            // IsPlayingTransitionClip() returns false.
            if (m_transitionClipPlaying) {
                transitionProgress = 1.0f;
                return;  // Wait for clip completion in Update()
            }

            // Transition complete
            isTransitioning = false;
            transitionProgress = 0.0f;
            previousState = transitionFrom;
            currentState = transitionTo;
            
            std::cout << "[HybridMMFSM] Transition complete: " << HybridStateToString(previousState)
                      << " -> " << HybridStateToString(currentState) << "\n";
        }
        return;  // Don't evaluate new transitions while transitioning
    }

    // Evaluate transitions
    EvaluateTransitions();
}

void HybridMMFSM::EvaluateTransitions() {
    for (const auto& t : transitions) {
        bool conditionMet = t.condition();

        // ── Phase 3: Airborne Gating Safeties ──────────────────────────
        // Block transitions that would crossfade airborne animations into
        // grounded locomotion datasets (or vice-versa) when the motion
        // matching database has no airborne poses. Without this gate, a
        // grounded→Jump transition fires the Jump animation, but if the
        // database lacks Jump/Fall clips the matcher can't represent the
        // arc — the character snaps back to a grounded pose mid-air. Similarly,
        // an airborne→grounded transition without landing detection causes
        // the grounded clip to play while the feet are still in the air.
        static auto IsAirborne = [](HybridState s) {
            return s == HybridState::JUMP || s == HybridState::FALL;
        };
        if (conditionMet && mmActive && motionMatcher.GetDatabase() &&
            motionMatcher.GetDatabase()->GetPoseCount() > 0) {
            bool fromAir = IsAirborne(t.from);
            bool toAir   = IsAirborne(t.to);
            if (fromAir != toAir) {
                // Cross-domain transition (grounded ↔ airborne). Only allow
                // if the database actually has airborne poses to represent
                // the arc.
                if (!motionMatcher.GetDatabase()->HasAirbornePoses()) {
                    conditionMet = false;
                }
            }
        }

        if (debugEnabled) {
            std::cout << "[HybridMMFSM] Checking transition: " << HybridStateToString(t.from)
                      << " -> " << HybridStateToString(t.to)
                      << " current=" << HybridStateToString(currentState)
                      << " condition=" << (conditionMet ? "TRUE" : "FALSE") << "\n";
        }
        if (currentState == t.from && conditionMet) {
            StartTransition(t.to);
            return;  // Only one transition per frame
        }
    }
}

void HybridMMFSM::StartTransition(HybridState toState) {
    if (toState == currentState) return;

    // Find transition config
    const HybridTransition* transitionConfig = nullptr;
    for (const auto& t : transitions) {
        if (t.from == currentState && t.to == toState) {
            transitionConfig = &t;
            break;
        }
    }

    float duration = 0.1f;  // Default
    float inertializationDuration = 0.15f;
    bool useInertialization = true;

    if (transitionConfig) {
        duration = transitionConfig->blendDuration;
        inertializationDuration = transitionConfig->inertializationDuration;
        useInertialization = transitionConfig->useInertialization;
    }

    // Capture source state for inertialization (root position/rotation momentum
    // is still preserved — the transition clip handles bone-shape blending
    // structurally via pre-computed slerp + 2D alignment)
    inertialization.sourceRootPos = characterState.position;
    inertialization.sourceRootRot = glm::angleAxis(characterState.rotation,
                                                     glm::vec3(0, 1, 0));
    inertialization.sourceVelocity = characterState.velocity;

    transitionFrom = currentState;
    transitionTo = toState;
    transitionDuration = duration;
    transitionProgress = 0.0f;
    isTransitioning = true;

    std::cout << "[HybridMMFSM] Starting transition: " << HybridStateToString(transitionFrom)
              << " -> " << HybridStateToString(transitionTo)
              << " (blend=" << duration << "s, inertial=" << (useInertialization ? "YES" : "NO") << ")\n";

    // ---- Structural Motion Graph: Play pre-computed transition clip ----
    // Instead of instantly swapping databases (SetDatabaseExplicit), look up
    // a pre-baked transition clip from the motion transition graph. The clip
    // smoothly blends ALL joints via slerp + linear root interpolation with
    // 2D coordinate alignment — no runtime stature offset or pose blend patches
    // needed.
    if (mmActive) {
        auto transitionClip = transitionGraph.GetTransition(
            static_cast<int>(currentState), static_cast<int>(toState));

        if (transitionClip && animator) {
            // Find the target state's database and KD-tree for post-transition
            auto dbIt = hybridStateSlots.find(toState);
            const MotionDatabase* targetDB =
                (dbIt != hybridStateSlots.end()) ? dbIt->second.database.get() : nullptr;
            const MotionKDTree* targetTree =
                (dbIt != hybridStateSlots.end()) ? dbIt->second.searchTree.get() : nullptr;

            // Find the nearest pose in the target database to the transition
            // clip's ending pose, so the matcher resumes from a compatible pose
            int targetPoseIndex = -1;
            if (targetDB && targetDB->GetPoseCount() > 0) {
                // Use the first pose of the target database as a reasonable
                // starting point — the transition clip ends at a pose sampled
                // from this database, so any pose in the same clip is close.
                targetPoseIndex = 0;
            }

            motionMatcher.PlayTransitionClip(
                transitionClip,
                targetDB ? *targetDB : *motionMatcher.GetDatabase(),
                targetTree ? *targetTree : motionMatcher.GetSearchTree(),
                targetPoseIndex);

            m_transitionClipPlaying = true;
            m_transitionClipTarget = toState;
            std::cout << "[HybridMMFSM] Playing structural transition clip ("
                      << transitionClip->duration << "s)\n";
            return;
        }
    }

    // Fallback: if no transition clip exists (e.g., JUMP, VAULT states that
    // are one-shot animations, not MM databases), use the old inertialization
    // approach for root momentum preservation.
    if (useInertialization) {
        inertialization.active = true;
        inertialization.progress = 0.0f;
        inertialization.duration = inertializationDuration;
        inertialization.preservedMomentum = characterState.velocity;

        // Target state setup
        inertialization.targetRootPos = characterState.position;
        inertialization.targetRootRot = glm::angleAxis(characterState.rotation,
                                                     glm::vec3(0, 1, 0));
        inertialization.targetVelocity = glm::vec3(0.0f);
    }
}

void HybridMMFSM::UpdateLocomotion(float dt) {
    // Motion Matching handles smooth idle↔walk↔run blending.
    // Crouch ↔ Locomotion transitions are now handled structurally via
    // pre-computed transition clips (MotionTransitionGraph), so the old
    // instant SetDatabaseExplicit swap + crouch DB switch token is removed.
    if (mmActive) {
        // Crouch↔Locomotion database restoration is handled by
        // MotionMatcher::CompleteTransitionNow() which swaps the
        // database pointer when the transition clip finishes.
        // No manual ResetDatabaseExplicit needed — the transition clip
        // ends at a pose sampled from the target database, so the matcher
        // resumes seamlessly.

        CharacterState mmState;
        mmState.position = characterState.position;
        mmState.velocity = characterState.velocity;
        mmState.rotation = characterState.rotation;
        mmState.moveDirection = characterState.moveDirection;
        mmState.grounded = characterState.grounded;
        mmState.crouching = false;
        mmState.worldVelocity = characterState.velocity;  // real-world m/s for speed-smoothed IK

        CharacterInput mmInput;
        mmInput.moveDirection = characterState.moveDirection;
        mmInput.moveMagnitude = characterState.moveMagnitude;
        mmInput.grounded = characterState.grounded;

        motionMatcher.Update(dt, mmInput, mmState);
    }
}

void HybridMMFSM::UpdateJump(float dt) {
    // Jump is a one-shot animation - let it play through
    // FSM will transition back to LOCOMOTION when grounded
    
    // During inertialization, preserve upward momentum
    if (inertialization.active) {
        inertialization.targetVelocity = glm::vec3(0.0f, -9.8f * dt, 0.0f);
    }
}

void HybridMMFSM::UpdateFall(float dt) {
    // Fall is a looping animation
    // FSM will transition back to LOCOMOTION when grounded
    
    // Apply gravity during inertialization
    if (inertialization.active) {
        inertialization.preservedMomentum.y -= 9.8f * dt;
    }
}

void HybridMMFSM::UpdateCrouch(float dt) {
    // Crouch uses MM with crouch database for smooth crouch idle <-> crouch walk blending.
    // The database switch from LOCOMOTION is now handled by the structural
    // transition clip (MotionTransitionGraph), which plays a pre-computed
    // blend clip and then seamlessly swaps the database via
    // MotionMatcher::CompleteTransitionNow(). No instant SetDatabaseExplicit
    // swap — no stature offset patches — no pose snapshot blending.
    auto it = hybridStateSlots.find(HybridState::CROUCH_WALK);
    if (it != hybridStateSlots.end() && it->second.database &&
        it->second.database->GetPoseCount() > 0 && it->second.isBuilt) {
        // Use crouch-specific motion matching. The crouch database is
        // already active (set by CompleteTransitionNow when the clip finished).
        CharacterState crouchState;
        crouchState.position = characterState.position;
        crouchState.velocity = characterState.velocity;
        crouchState.rotation = characterState.rotation;
        crouchState.moveDirection = characterState.moveDirection;
        crouchState.grounded = characterState.grounded;
        crouchState.crouching = true;
        crouchState.worldVelocity = characterState.velocity;

        CharacterInput crouchInput;
        crouchInput.moveDirection = characterState.moveDirection;
        crouchInput.moveMagnitude = characterState.moveMagnitude;
        crouchInput.grounded = characterState.grounded;

        motionMatcher.Update(dt, crouchInput, crouchState);
    } else {
        // Fallback: use locomotion MM with crouching flag
        if (mmActive) {
            CharacterState crouchState;
            crouchState.position = characterState.position;
            crouchState.velocity = characterState.velocity;
            crouchState.rotation = characterState.rotation;
            crouchState.moveDirection = characterState.moveDirection;
            crouchState.grounded = characterState.grounded;
            crouchState.crouching = true;
            crouchState.worldVelocity = characterState.velocity;

            CharacterInput crouchInput;
            crouchInput.moveDirection = characterState.moveDirection;
            crouchInput.moveMagnitude = characterState.moveMagnitude;
            crouchInput.grounded = characterState.grounded;

            motionMatcher.Update(dt, crouchInput, crouchState);
        }
    }
}

void HybridMMFSM::UpdateInertialization(float dt) {
    if (!inertialization.active) return;

    // Update progress
    inertialization.progress += dt / inertialization.duration;
    
    if (inertialization.progress >= 1.0f) {
        // Inertialization complete
        inertialization.active = false;
        inertialization.progress = 1.0f;
        // NOTE: Stature offset / pose snapshot cleanup removed —
        // these fields no longer exist (replaced by transition clips).
        std::cout << "[HybridMMFSM] Inertialization complete\n";
        return;
    }

    // Apply inertialization blending
    ApplyInertializationBlending(dt);
}

void HybridMMFSM::ApplyInertializationBlending(float dt) {
    if (!inertialization.active || !animator) return;

    // Quintic smoothstep blend weight: 6t^5 - 15t^4 + 10t^3
    // Gives zero first AND second derivatives at both t=0 and t=1,
    // eliminating the velocity/acceleration "pop" at transition start/end
    // that the old exponential (1 - e^{-5t}) produced (non-zero derivative
    // at t=1 → knee jerk on state switch).
    float blendWeight = CalculateInertializationWeight(inertialization.progress);
    const float t = inertialization.progress;
    const float quintic = t * t * (3.0f - 2.0f * t);  // cubic smoothstep
    // Refine to quintic for zero 2nd derivative at endpoints
    const float quinticDecay = quintic * quintic * (3.0f - 2.0f * quintic);

    // ---- Note: Stature-offset blending removed ----
    // Previously decayed statureOffset here to hold the pelvis up during
    // walk→crouch database swaps. Replaced by structural transition clips
    // (MotionTransitionGraph) that pre-compute all-joint slerp + 2D alignment.

    // FIX (refreshed todo Fix 1): Synchronize inertialization with the
    // multi-database root path. When the instant 0.0f database switch
    // transitions into a Crouch/CrouchWalk pose domain while the character is
    // practically stationary, the motion matcher immediately picks crouch
    // frames (ankle close to pelvis) while inertialization still carries
    // forward locomotion momentum. The two fight each other, the posture
    // relaxer fires, and the residual ankleOffset stretches the leg.
    // Snap the root to the target and zero momentum so the new database's
    // rest pose aligns with the already-planted feet.
    bool enteringCrouch = (transitionTo == HybridState::CROUCH ||
                           transitionTo == HybridState::CROUCH_WALK);
    bool isStationary = (glm::length(characterState.velocity) < 0.05f);

    // Position: blend between (source + momentum carry) and target root
    glm::vec3 blendedPos;
    if (enteringCrouch && isStationary) {
        blendedPos = inertialization.targetRootPos;
        inertialization.preservedMomentum = glm::vec3(0.0f);
    } else {
        blendedPos = glm::mix(
            inertialization.sourceRootPos + inertialization.preservedMomentum * inertialization.progress,
            inertialization.targetRootPos,
            blendWeight
        );

        // Apply quintic decay to preserved momentum to prevent structural sliding.
        // Uses the editor-configurable driftRecoveryRate for live tuning.
        inertialization.preservedMomentum = glm::mix(
            inertialization.preservedMomentum,
            glm::vec3(0.0f),
            dt * Config::getInertializationConfig().driftRecoveryRate);
    }

    // Rotation: track the source→target facing delta and apply it as a
    // root-rotation offset, weighted by the quintic decay. This smoothly
    // aligns the new pose's heading with the old pose's momentum, rather
    // than snapping at transition start. Rotation is still applied during
    // the stationary-crouch snap (only the positional drift is suppressed).
    if (skeleton && skeleton->rootBoneIndex >= 0) {
        // Positional offset (carried by AddIKOffset on the root bone) —
        // suppressed when snapping into a stationary crouch to avoid the
        // root drift that stretches the legs against planted feet.
        if (!(enteringCrouch && isStationary)) {
            glm::vec3 rootOffset = blendedPos - inertialization.sourceRootPos;
            animator->AddIKOffset(skeleton->rootBoneIndex, rootOffset, blendWeight);
        }

        // Rotational offset: slerp from source→target facing over quintic decay
        glm::quat rotDelta = glm::normalize(inertialization.targetRootRot *
                                            glm::conjugate(inertialization.sourceRootRot));
        // Only apply if there's a meaningful rotation (avoid no-op slerp)
        if (glm::angle(rotDelta) > 0.001f) {
            glm::quat applyRot = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                                            rotDelta, quinticDecay);
            animator->AddRootRotationOffset(applyRot);
        }
    } else {
        // Fallback: try to find root bone by name
        int rootBoneIdx = skeleton ? skeleton->GetBoneIndex("Hips") : -1;
        if (rootBoneIdx < 0) {
            rootBoneIdx = skeleton ? skeleton->GetBoneIndex("Root") : -1;
        }
        if (rootBoneIdx >= 0) {
            if (!(enteringCrouch && isStationary)) {
                glm::vec3 rootOffset = blendedPos - inertialization.sourceRootPos;
                animator->AddIKOffset(rootBoneIdx, rootOffset, blendWeight);
            }

            glm::quat rotDelta = glm::normalize(inertialization.targetRootRot *
                                                glm::conjugate(inertialization.sourceRootRot));
            if (glm::angle(rotDelta) > 0.001f) {
                glm::quat applyRot = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                                                rotDelta, quinticDecay);
                animator->AddRootRotationOffset(applyRot);
            }
        }
    }

    // Update source position for next frame
    inertialization.sourceRootPos = blendedPos;

    // NOTE: Stature offset decay removed — replaced by structural transition
    // clips. The transition clip already blends all joints via slerp + 2D
    // alignment, so no runtime pelvis-hold offset is needed.

    if (debugEnabled) {
        glm::vec3 debugOffset = blendedPos - inertialization.sourceRootPos;
        std::cout << "[Inertialization] Applied root offset: ("
                  << debugOffset.x << ", " << debugOffset.y << ", " << debugOffset.z << ")\n";
    }
}

float HybridMMFSM::CalculateInertializationWeight(float progress) const {
    // Quintic smoothstep: 6t^5 - 15t^4 + 10t^3
    // Zero 1st & 2nd derivatives at both endpoints — no popping.
    // Replaces the exponential decay (1 - e^{-5t}) which had a non-zero
    // derivative at t=1 (velocity discontinuity → "knee snap").
    if (progress <= 0.0f) return 0.0f;
    if (progress >= 1.0f) return 1.0f;
    return progress * progress * progress *
           (progress * (progress * 6.0f - 15.0f) + 10.0f);
}

std::string HybridMMFSM::GetDebugInfo() const {
    std::string info = "Hybrid MM+FSM System (Fixed)\n";
    info += "========================\n";
    info += "Current State: " + HybridStateToString(currentState) + "\n";
    info += "Previous State: " + HybridStateToString(previousState) + "\n";
    info += "Transitioning: " + std::string(isTransitioning ? "YES" : "NO") + "\n";
    info += "Inertialization Active: " + std::string(inertialization.active ? "YES" : "NO") + "\n";
    if (isTransitioning || inertialization.active) {
        float progress = inertialization.active ? inertialization.progress : transitionProgress;
        info += "Progress: " + std::to_string((int)(progress * 100)) + "%\n";
    }
    info += "MM Active: " + std::string(mmActive ? "YES" : "NO") + "\n";
    info += "State Databases: " + std::to_string(hybridStateSlots.size()) + "\n";
    
    // List state databases
    for (const auto& [state, slot] : hybridStateSlots) {
        if (slot.database) {
            info += "  " + HybridStateToString(state) + ": " + 
                    std::to_string(slot.database->GetPoseCount()) + " poses\n";
        }
    }
    
    return info;
}

const MotionDatabase* HybridMMFSM::GetStateDatabase(HybridState state) const {
    auto it = hybridStateSlots.find(state);
    if (it != hybridStateSlots.end() && it->second.database) {
        return it->second.database.get();
    }
    return nullptr;
}

bool HybridMMFSM::HasStateDatabase(HybridState state) const {
    auto it = hybridStateSlots.find(state);
    return (it != hybridStateSlots.end() && it->second.database &&
            it->second.database->GetPoseCount() > 0);
}

std::string HybridMMFSM::GetMotionMatcherDebug() const {
    if (!mmActive) return "Motion Matcher: INACTIVE\n";
    
    const auto& debug = motionMatcher.GetDebugInfo();
    std::string info = "Motion Matcher Debug\n";
    info += "======================\n";
    info += "Current Animation: " + (debug.currentAnimationName.empty() ? "Unknown" : debug.currentAnimationName) + "\n";
    info += "Animation Time: " + std::to_string(debug.currentAnimationTime) + "\n";
    info += "Poses Searched: " + std::to_string(debug.posesSearched) + "\n";
    info += "Search Time: " + std::to_string(debug.searchTimeMs) + " ms\n";
    info += "Best Score: " + std::to_string(debug.currentResult.score) + "\n";
    info += "Left Foot Planted: " + std::string(debug.leftFootPlanted ? "YES" : "NO") + "\n";
    info += "Right Foot Planted: " + std::string(debug.rightFootPlanted ? "YES" : "NO") + "\n";
    return info;
}

size_t HybridMMFSM::GetMotionMatcherDatabaseSize() const {
    if (!mmActive) return 0;
    return motionMatcher.GetDatabase()->GetPoseCount();
}
