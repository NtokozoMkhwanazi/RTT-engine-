#include "HybridMMFSM.h"
#include <iostream>
#include <algorithm>
#include <glm/gtc/quaternion.hpp>

// ============================================================================
// HYBRID MM + FSM IMPLEMENTATION (FIXED - Proper shared_ptr ownership)
// ============================================================================

HybridMMFSM::HybridMMFSM() {
    std::cout << "[HybridMMFSM] Constructor called - this=" << this << "\n";
}

HybridMMFSM::~HybridMMFSM() {
    std::cout << "[HybridMMFSM] Destructor called - this=" << this << "\n";
    // Clear all databases (shared_ptr will properly clean up animations)
    std::cout << "[HybridMMFSM] Clearing " << stateDatabases.size() << " state databases\n";
    stateDatabases.clear();
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

    // Pre-create databases for each state using unique_ptr
    stateDatabases[HybridState::LOCOMOTION] = std::make_unique<MotionDatabase>();
    stateDatabases[HybridState::CROUCH_WALK] = std::make_unique<MotionDatabase>();
    stateDatabases[HybridState::COMBAT] = std::make_unique<MotionDatabase>();

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
    if (stateDatabases.find(state) == stateDatabases.end()) {
        stateDatabases[state] = std::make_unique<MotionDatabase>();
        std::cout << "[HybridMMFSM] Created database for state: " << HybridStateToString(state) << "\n";
    }

    // Add animation to state's database - database takes ownership via shared_ptr
    stateDatabases[state]->AddAnimation(name, anim, skeleton);
    
    std::cout << "[HybridMMFSM] Loaded state animation: " << HybridStateToString(state) 
              << " - " << name << " (duration=" << anim->duration << "s)\n";
}

void HybridMMFSM::BuildDatabases() {
    // Build MM database for locomotion
    if (mmActive) {
        motionMatcher.BuildSearchIndex();
        std::cout << "[HybridMMFSM] Locomotion MM database built\n";
    }

    // Build KD-Trees for state-specific databases
    for (auto& [state, database] : stateDatabases) {
        if (database && database->GetPoseCount() > 0) {
            std::cout << "[HybridMMFSM] State " << HybridStateToString(state) 
                      << " has " << database->GetPoseCount() << " poses\n";
        }
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

    // Update based on current state
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
}

void HybridMMFSM::UpdateStateMachine(float dt) {
    // Update transition progress
    if (isTransitioning) {
        transitionProgress += dt / transitionDuration;
        if (transitionProgress >= 1.0f) {
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

    // Capture source state for inertialization
    inertialization.sourceRootPos = characterState.position;
    inertialization.sourceRootRot = characterState.rotation;
    inertialization.sourceVelocity = characterState.velocity;

    transitionFrom = currentState;
    transitionTo = toState;
    transitionDuration = duration;
    transitionProgress = 0.0f;
    isTransitioning = true;

    std::cout << "[HybridMMFSM] Starting transition: " << HybridStateToString(transitionFrom)
              << " -> " << HybridStateToString(transitionTo) 
              << " (blend=" << duration << "s, inertial=" << (useInertialization ? "YES" : "NO") << ")\n";

    // Play animation for target state
    if (useInertialization) {
        // Start inertialization blending
        inertialization.active = true;
        inertialization.progress = 0.0f;
        inertialization.duration = inertializationDuration;
        inertialization.preservedMomentum = characterState.velocity;
        
        // Target state setup
        inertialization.targetRootPos = characterState.position;
        inertialization.targetRootRot = characterState.rotation;
        inertialization.targetVelocity = glm::vec3(0.0f);  // Will be updated
    }

    // Try to get animation from state database
    auto dbIt = stateDatabases.find(toState);
    if (dbIt != stateDatabases.end() && dbIt->second) {
        // For state-specific MM, we would switch databases here
        // For now, just log it
        std::cout << "[HybridMMFSM] Using state database for " << HybridStateToString(toState) << "\n";
    }
}

void HybridMMFSM::UpdateLocomotion(float dt) {
    // Motion Matching handles smooth idle↔walk↔run blending
    if (mmActive) {
        CharacterState mmState;
        mmState.position = characterState.position;
        mmState.velocity = characterState.velocity;
        mmState.rotation = characterState.rotation;
        mmState.moveDirection = characterState.moveDirection;
        mmState.grounded = characterState.grounded;
        mmState.crouching = false;

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
    // Crouch uses MM with crouch database for smooth crouch idle <-> crouch walk blending
    auto it = stateDatabases.find(HybridState::CROUCH_WALK);
    if (it != stateDatabases.end() && it->second && it->second->GetPoseCount() > 0) {
        // Use crouch-specific motion matching
        CharacterState crouchState;
        crouchState.position = characterState.position;
        crouchState.velocity = characterState.velocity;
        crouchState.rotation = characterState.rotation;
        crouchState.moveDirection = characterState.moveDirection;
        crouchState.grounded = characterState.grounded;
        crouchState.crouching = true;

        CharacterInput crouchInput;
        crouchInput.moveDirection = characterState.moveDirection;
        crouchInput.moveMagnitude = characterState.moveMagnitude;
        crouchInput.grounded = characterState.grounded;

        // Temporarily switch to crouch database
        motionMatcher.SetDatabase(std::move(it->second), 0.1f);
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
        
        std::cout << "[HybridMMFSM] Inertialization complete\n";
        return;
    }

    // Apply inertialization blending
    ApplyInertializationBlending(dt);
}

void HybridMMFSM::ApplyInertializationBlending(float dt) {
    if (!inertialization.active || !animator) return;

    float blendWeight = CalculateInertializationWeight(inertialization.progress);

    // Calculate blended root position
    glm::vec3 blendedPos = glm::mix(
        inertialization.sourceRootPos + inertialization.preservedMomentum * inertialization.progress,
        inertialization.targetRootPos,
        blendWeight
    );

    // Calculate root motion offset to apply
    glm::vec3 rootOffset = blendedPos - inertialization.sourceRootPos;

    // Apply root motion offset to the animator's root bone
    // The root bone is typically at index 0 or can be found via skeleton
    if (skeleton && skeleton->rootBoneIndex >= 0) {
        // Apply the offset to the root bone
        animator->AddIKOffset(skeleton->rootBoneIndex, rootOffset, blendWeight);
    } else {
        // Fallback: try to find root bone by name
        int rootBoneIdx = skeleton ? skeleton->GetBoneIndex("Hips") : -1;
        if (rootBoneIdx < 0) {
            rootBoneIdx = skeleton ? skeleton->GetBoneIndex("Root") : -1;
        }
        if (rootBoneIdx >= 0) {
            animator->AddIKOffset(rootBoneIdx, rootOffset, blendWeight);
        }
    }

    // Update source position for next frame
    inertialization.sourceRootPos = blendedPos;

    if (debugEnabled) {
        std::cout << "[Inertialization] Applied root offset: (" 
                  << rootOffset.x << ", " << rootOffset.y << ", " << rootOffset.z << ")\n";
    }
}

float HybridMMFSM::CalculateInertializationWeight(float progress) const {
    // Exponential decay function for natural momentum fade
    // w = 1 - e^(-k * progress)
    // This gives a smooth, natural-looking transition
    
    const float k = 5.0f;  // Decay rate
    return 1.0f - std::exp(-k * progress);
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
    info += "State Databases: " + std::to_string(stateDatabases.size()) + "\n";
    
    // List state databases
    for (const auto& [state, db] : stateDatabases) {
        if (db) {
            info += "  " + HybridStateToString(state) + ": " + 
                    std::to_string(db->GetPoseCount()) + " poses\n";
        }
    }
    
    return info;
}

const MotionDatabase* HybridMMFSM::GetStateDatabase(HybridState state) const {
    auto it = stateDatabases.find(state);
    if (it != stateDatabases.end() && it->second) {
        return it->second.get();  // Return raw pointer from unique_ptr
    }
    return nullptr;
}

bool HybridMMFSM::HasStateDatabase(HybridState state) const {
    auto it = stateDatabases.find(state);
    return (it != stateDatabases.end() && it->second && it->second->GetPoseCount() > 0);
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
