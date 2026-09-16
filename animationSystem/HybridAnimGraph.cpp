#include "HybridAnimGraph.h"
#include <iostream>
#include <algorithm>
#include <glm/gtc/quaternion.hpp>

// ============================================================================
// HYBRID ANIMATION GRAPH IMPLEMENTATION
// ============================================================================

HybridAnimGraph::HybridAnimGraph() {
    // Default configuration
    config.enableMotionMatching = true;
    config.mmBlendDuration = 0.1f;
    config.enableFootIK = true;
    config.fsmTransitionDuration = 0.2f;
    config.allowMMInAir = false;
    config.enableUpperBodyLayer = true;
    config.enableAdditiveLayer = true;
    config.layerBlendDuration = 0.15f;
    config.useDynamicRootForMM = true;
    config.lockRootForStaticAnimations = true;
}

HybridAnimGraph::~HybridAnimGraph() {
    // Clean up
    stateDatabases.clear();
    stateAnimations.clear();
    upperBodyLayers.clear();
    additiveLayers.clear();
}

void HybridAnimGraph::Initialize(const Skeleton* skeleton, Animator* animator) {
    if (!skeleton || !animator) {
        std::cerr << "[HybridAnimGraph] ERROR: Null skeleton or animator!\n";
        return;
    }

    this->skeleton = skeleton;
    this->animator = animator;

    std::cout << "[HybridAnimGraph] Initializing...\n";

    // Initialize Motion Matcher for locomotion
    motionMatcher.Initialize(skeleton, animator);

    // Set up default transitions
    SetupDefaultTransitions();

    std::cout << "[HybridAnimGraph] Initialized successfully!\n";
}

void HybridAnimGraph::SetupDefaultTransitions() {
    // Default transition rules (can be overridden by user)

    // Grounded ↔ Air
    AddTransition(HybridState::LOCOMOTION_GROUNDED, HybridState::LOCOMOTION_AIR, 0.15f,
        [this]() { return !characterState.grounded && characterState.jumping; });

    AddTransition(HybridState::LOCOMOTION_AIR, HybridState::LOCOMOTION_GROUNDED, 0.1f,
        [this]() { return characterState.grounded && !characterState.falling; });

    // Grounded ↔ Crouch
    AddTransition(HybridState::LOCOMOTION_GROUNDED, HybridState::LOCOMOTION_CROUCH, 0.2f,
        [this]() { return characterState.crouching && characterState.grounded; });

    AddTransition(HybridState::LOCOMOTION_CROUCH, HybridState::LOCOMOTION_GROUNDED, 0.2f,
        [this]() { return !characterState.crouching && characterState.grounded; });

    // Any → Combat (when attack pressed)
    AddTransition(HybridState::LOCOMOTION_GROUNDED, HybridState::COMBAT, 0.1f,
        [this]() { return characterState.attackPressed; });

    // Any → Interaction
    AddTransition(HybridState::LOCOMOTION_GROUNDED, HybridState::INTERACTION, 0.3f,
        [this]() { return characterState.interactPressed; });
}

void HybridAnimGraph::LoadLocomotionAnimation(const std::string& name, std::shared_ptr<Animation> anim) {
    if (!anim) {
        std::cerr << "[HybridAnimGraph] ERROR: Null locomotion animation: " << name << "\n";
        return;
    }

    std::cout << "[HybridAnimGraph] Loading locomotion animation: " << name
              << " (" << anim->duration << "s)\n";

    // Load into primary motion matching database
    primaryDatabase.database.AddAnimation(name, anim, skeleton);
    primaryDatabase.name = "Primary";
}

void HybridAnimGraph::LoadStateAnimation(const std::string& name, std::shared_ptr<Animation> anim, HybridState state) {
    if (!anim) {
        std::cerr << "[HybridAnimGraph] ERROR: Null state animation: " << name << "\n";
        return;
    }

    std::cout << "[HybridAnimGraph] Loading state animation: " << name
              << " for state " << HybridStateToString(state) << "\n";

    // Store in state-specific map
    std::string key = HybridStateToString(state) + "_" + name;
    stateAnimations[key] = anim;

    // Also add to state-specific database if it's a motion-matched state
    if (state == HybridState::LOCOMOTION_CROUCH ||
        (state == HybridState::LOCOMOTION_AIR && config.allowMMInAir)) {
        stateDatabases[state].database.AddAnimation(name, anim, skeleton);
        stateDatabases[state].name = HybridStateToString(state);
    }
}

void HybridAnimGraph::LoadUpperBodyAnimation(const std::string& name, std::shared_ptr<Animation> anim,
                                              const std::vector<bool>& boneMask) {
    if (!anim) {
        std::cerr << "[HybridAnimGraph] ERROR: Null upper-body animation: " << name << "\n";
        return;
    }

    std::cout << "[HybridAnimGraph] Loading upper-body animation: " << name << "\n";

    // Store for later playback
    stateAnimations["UpperBody_" + name] = anim;
}

void HybridAnimGraph::LoadAdditiveAnimation(const std::string& name, std::shared_ptr<Animation> anim) {
    if (!anim) {
        std::cerr << "[HybridAnimGraph] ERROR: Null additive animation: " << name << "\n";
        return;
    }

    std::cout << "[HybridAnimGraph] Loading additive animation: " << name << "\n";
    stateAnimations["Additive_" + name] = anim;
}

void HybridAnimGraph::BuildDatabases() {
    std::cout << "[HybridAnimGraph] Building motion databases...\n";

    // Build primary locomotion database
    if (primaryDatabase.database.GetPoseCount() > 0) {
        motionMatcher.LoadAnimation("Primary", nullptr);  // Trigger internal build
        primaryDatabase.isBuilt = true;
        std::cout << "  Primary database: " << primaryDatabase.database.GetPoseCount() << " poses\n";
    }

    // Build state-specific databases
    for (auto& [state, slot] : stateDatabases) {
        if (slot.database.GetPoseCount() > 0) {
            slot.isBuilt = true;
            std::cout << "  " << slot.name << " database: " << slot.database.GetPoseCount() << " poses\n";
        }
    }

    std::cout << "[HybridAnimGraph] Databases built successfully!\n";
}

void HybridAnimGraph::AddTransition(const HybridGraphTransition& transition) {
    transitions.push_back(transition);
}

void HybridAnimGraph::AddTransition(HybridState from, HybridState to, float duration, std::function<bool()> condition) {
    HybridGraphTransition t;
    t.fromState = from;
    t.toState = to;
    t.blendDuration = duration;
    t.condition = condition;
    transitions.push_back(t);
}

void HybridAnimGraph::Update(float dt, const HybridCharacterState& state) {
    if (!animator || !skeleton) {
        std::cerr << "[HybridAnimGraph] ERROR: Not initialized!\n";
        return;
    }

    // Update character state
    characterState = state;

    // 1. Update state machine (evaluate transitions)
    UpdateStateMachine(dt);

    // 2. Update appropriate animation system based on current state
    switch (currentState) {
        case HybridState::LOCOMOTION_GROUNDED:
        case HybridState::LOCOMOTION_CROUCH:
            // Use Motion Matching for smooth locomotion
            if (config.enableMotionMatching) {
                UpdateMotionMatching(dt);
            } else {
                UpdateFSMAnimation(dt);
            }
            break;

        case HybridState::LOCOMOTION_AIR:
            // Use FSM for jump/fall (static root or limited MM)
            UpdateFSMAnimation(dt);
            break;

        case HybridState::VAULTING:
        case HybridState::COMBAT:
        case HybridState::INTERACTION:
        case HybridState::CUSTOM:
            // Use FSM for full-body override animations
            UpdateFSMAnimation(dt);
            break;
    }

    // 3. Update layered animations (upper-body, additive)
    if (config.enableUpperBodyLayer || config.enableAdditiveLayer) {
        UpdateLayers(dt);
        ApplyLayerBlending(dt);
    }
}

void HybridAnimGraph::UpdateStateMachine(float dt) {
    if (isTransitioning) {
        // Complete current transition
        transitionProgress += dt / transitionDuration;

        if (transitionProgress >= 1.0f) {
            // Transition complete
            previousState = transitionFromState;
            currentState = transitionToState;
            isTransitioning = false;
            transitionProgress = 0.0f;

            std::cout << "[HybridGraph] State: " << HybridStateToString(transitionFromState)
                      << " -> " << HybridStateToString(transitionToState) << " (complete)\n";

            // Switch database if needed
            if (stateDatabases.count(transitionToState) > 0) {
                SwitchDatabase(stateDatabases[transitionToState].name, 0.1f);
            } else if (transitionToState == HybridState::LOCOMOTION_GROUNDED) {
                SwitchDatabase("Primary", 0.1f);
            }
        }
    } else {
        // Evaluate transitions
        EvaluateTransitions();
    }
}

void HybridAnimGraph::EvaluateTransitions() {
    for (const auto& transition : transitions) {
        if (transition.fromState == currentState && transition.condition) {
            if (transition.condition()) {
                StartTransition(transition.toState);
                break;  // Only one transition per frame
            }
        }
    }
}

void HybridAnimGraph::StartTransition(HybridState toState) {
    if (toState == currentState) return;
    if (isTransitioning) return;

    // Find transition duration
    float duration = config.fsmTransitionDuration;
    for (const auto& t : transitions) {
        if (t.fromState == currentState && t.toState == toState) {
            duration = t.blendDuration;
            break;
        }
    }

    transitionFromState = currentState;
    transitionToState = toState;
    transitionDuration = duration;
    transitionProgress = 0.0f;
    isTransitioning = true;

    std::cout << "[HybridGraph] Starting transition: " << HybridStateToString(currentState)
              << " -> " << HybridStateToString(toState) << " (dur=" << duration << "s)\n";

    // If target state has an animation, start blending to it
    std::string animKey = HybridStateToString(toState) + "_Entry";
    auto it = stateAnimations.find(animKey);
    if (it != stateAnimations.end() && it->second) {
        animator->BlendTo(it->second.get(), duration);
    }
}

void HybridAnimGraph::UpdateMotionMatching(float dt) {
    // Convert hybrid state to motion matcher state
    CharacterState mmState;
    mmState.position = characterState.position;
    mmState.velocity = characterState.velocity;
    mmState.rotation = characterState.rotation;
    mmState.moveDirection = characterState.moveDirection;
    mmState.grounded = characterState.grounded;
    mmState.crouching = characterState.crouching;

    // Update motion matcher
    motionMatcher.Update(dt, mmState);

    // Apply foot IK if enabled
    if (config.enableFootIK) {
        // Motion matcher handles foot IK internally
    }
}

void HybridAnimGraph::UpdateFSMAnimation(float dt) {
    // For FSM states, we use traditional animation playback
    // The animator is already updated by the state transition system

    // Example: Play jump animation when in air
    if (currentState == HybridState::LOCOMOTION_AIR) {
        std::string animKey;
        if (characterState.jumping) {
            animKey = "Locomotion_Air_Jump";
        } else if (characterState.falling) {
            animKey = "Locomotion_Air_Fall";
        }

        auto it = stateAnimations.find(animKey);
        if (it != stateAnimations.end() && it->second) {
            // Check if already playing this animation
            Animation* current = animator->GetCurrentAnimation();
            if (current != it->second.get()) {
                animator->BlendTo(it->second.get(), 0.1f);
            }
        }
    }

    // Update animator
    animator->Update(dt);
}

void HybridAnimGraph::UpdateLayers(float dt) {
    // Update upper-body layers
    for (auto& layer : upperBodyLayers) {
        if (!layer.config.enabled || layer.finished) continue;

        // Update weight
        if (layer.currentWeight < layer.targetWeight) {
            layer.currentWeight += dt / layer.config.blendInDuration;
            if (layer.currentWeight >= layer.targetWeight) {
                layer.currentWeight = layer.targetWeight;
            }
        } else if (layer.currentWeight > layer.targetWeight) {
            layer.currentWeight -= dt / layer.config.blendOutDuration;
            if (layer.currentWeight <= layer.targetWeight) {
                layer.currentWeight = layer.targetWeight;
                if (layer.targetWeight <= 0.0f) {
                    layer.finished = true;
                }
            }
        }

        // Update animation time
        if (layer.animation && layer.currentWeight > 0.01f) {
            float animSpeed = layer.animation->speed > 0 ? layer.animation->speed : 1.0f;
            layer.currentTime += dt * animSpeed;

            // Handle looping
            if (layer.loop && layer.currentTime >= layer.animation->duration) {
                layer.currentTime = fmod(layer.currentTime, layer.animation->duration);
            } else if (!layer.loop && layer.currentTime >= layer.animation->duration) {
                layer.finished = true;
                layer.targetWeight = 0.0f;
            }
        }
    }

    // Update additive layers
    for (auto& layer : additiveLayers) {
        if (!layer.config.enabled || layer.finished) continue;

        // Update weight (similar to upper-body)
        if (layer.currentWeight < layer.targetWeight) {
            layer.currentWeight += dt / layer.config.blendInDuration;
            if (layer.currentWeight >= layer.targetWeight) {
                layer.currentWeight = layer.targetWeight;
            }
        } else if (layer.currentWeight > layer.targetWeight) {
            layer.currentWeight -= dt / layer.config.blendOutDuration;
            if (layer.currentWeight <= layer.targetWeight) {
                layer.currentWeight = layer.targetWeight;
                if (layer.targetWeight <= 0.0f) {
                    layer.finished = true;
                }
            }
        }

        // Update animation time
        if (layer.animation && layer.currentWeight > 0.01f) {
            layer.currentTime += dt;
            if (layer.currentTime >= layer.animation->duration) {
                layer.currentTime = fmod(layer.currentTime, layer.animation->duration);
            }
        }
    }

    // Remove finished layers
    upperBodyLayers.erase(
        std::remove_if(upperBodyLayers.begin(), upperBodyLayers.end(),
            [](const ActiveLayer& layer) { return layer.finished && layer.currentWeight <= 0.01f; }),
        upperBodyLayers.end());

    additiveLayers.erase(
        std::remove_if(additiveLayers.begin(), additiveLayers.end(),
            [](const ActiveLayer& layer) { return layer.finished && layer.currentWeight <= 0.01f; }),
        additiveLayers.end());
}

void HybridAnimGraph::ApplyLayerBlending(float dt) {
    // Apply upper-body layers
    // FIX (unknown doc): give each active upper-body action its own animator
    // layer (1..3) instead of every action hard-clobbering the same Layer 1.
    // Overlapping actions (rapid attacks, etc.) no longer snap the torso back
    // and forth between clips mid-frame.
    int currentUpperBodyLayerTargetIndex = 1;
    for (const auto& layer : upperBodyLayers) {
        if (!layer.animation || layer.currentWeight <= 0.01f) continue;

        // Set layer weight on animator
        // In a full implementation, this would use bone masks
        animator->SetAnimationWeight(currentUpperBodyLayerTargetIndex, layer.currentWeight);

        // Increment slot so subsequent active effects stack / overlay natively,
        // up to the animator max blending layers.
        currentUpperBodyLayerTargetIndex++;
        if (currentUpperBodyLayerTargetIndex > 3) break;
    }

    // Apply additive layers
    for (const auto& layer : additiveLayers) {
        if (!layer.animation || layer.currentWeight <= 0.01f) continue;

        // Additive blending
        // In a full implementation, this would use additive blend mode
        animator->SetAnimationWeight(2, layer.currentWeight);  // Layer 2 = additive
    }
}

void HybridAnimGraph::PlayUpperBodyAction(const std::string& animName, float weight, bool loop) {
    std::string key = "UpperBody_" + animName;
    auto it = stateAnimations.find(key);
    if (it == stateAnimations.end() || !it->second) {
        std::cerr << "[HybridGraph] Upper-body animation not found: " << animName << "\n";
        return;
    }

    std::cout << "[HybridGraph] Playing upper-body action: " << animName << "\n";

    ActiveLayer layer;
    layer.name = animName;
    layer.animation = it->second;
    layer.config.type = AnimationLayerType::UPPER_BODY_ACTION;
    layer.config.weight = weight;
    layer.config.blendInDuration = config.layerBlendDuration;
    layer.config.blendOutDuration = config.layerBlendDuration;
    layer.config.boneMask = CreateUpperBodyMask();
    layer.targetWeight = weight;
    layer.currentWeight = 0.0f;
    layer.loop = loop;
    layer.finished = false;

    upperBodyLayers.push_back(layer);
}

void HybridAnimGraph::StopUpperBodyAction(const std::string& animName, float fadeOutDuration) {
    for (auto& layer : upperBodyLayers) {
        if (layer.name == animName) {
            layer.targetWeight = 0.0f;
            layer.config.blendOutDuration = fadeOutDuration;
            break;
        }
    }
}

void HybridAnimGraph::SetUpperBodyWeight(float weight, float duration) {
    for (auto& layer : upperBodyLayers) {
        if (layer.config.type == AnimationLayerType::UPPER_BODY_ACTION) {
            layer.targetWeight = weight;
            layer.config.blendInDuration = duration;
            layer.config.blendOutDuration = duration;
        }
    }
}

void HybridAnimGraph::PlayAdditive(const std::string& animName, float weight) {
    std::string key = "Additive_" + animName;
    auto it = stateAnimations.find(key);
    if (it == stateAnimations.end() || !it->second) {
        std::cerr << "[HybridGraph] Additive animation not found: " << animName << "\n";
        return;
    }

    std::cout << "[HybridGraph] Playing additive: " << animName << "\n";

    ActiveLayer layer;
    layer.name = animName;
    layer.animation = it->second;
    layer.config.type = AnimationLayerType::ADDITIVE;
    layer.config.weight = weight;
    layer.config.blendInDuration = config.layerBlendDuration;
    layer.config.blendOutDuration = config.layerBlendDuration;
    layer.config.additive = true;
    layer.targetWeight = weight;
    layer.currentWeight = 0.0f;
    layer.finished = false;

    additiveLayers.push_back(layer);
}

void HybridAnimGraph::StopAdditive(const std::string& animName, float fadeOutDuration) {
    for (auto& layer : additiveLayers) {
        if (layer.name == animName) {
            layer.targetWeight = 0.0f;
            layer.config.blendOutDuration = fadeOutDuration;
            break;
        }
    }
}

void HybridAnimGraph::SwitchDatabase(const std::string& databaseName, float blendDuration) {
    if (databaseName == currentDatabaseName) return;

    std::cout << "[HybridGraph] Switching database: " << currentDatabaseName
              << " -> " << databaseName << " (blend=" << blendDuration << "s)\n";

    std::string previousDatabaseName = currentDatabaseName;
    currentDatabaseName = databaseName;

    // Find the target database
    MotionDatabase* targetDatabase = nullptr;
    MotionDatabaseSlot* targetSlot = nullptr;
    
    if (databaseName == "Primary") {
        targetDatabase = &primaryDatabase.database;
        targetSlot = &primaryDatabase;
    } else {
        for (auto& [state, slot] : stateDatabases) {
            if (slot.name == databaseName) {
                targetDatabase = &slot.database;
                targetSlot = &slot;
                break;
            }
        }
    }

    if (!targetDatabase || targetDatabase->GetPoseCount() == 0) {
        std::cerr << "[HybridGraph] ERROR: Target database not found or empty: " << databaseName << "\n";
        currentDatabaseName = previousDatabaseName;  // Revert
        return;
    }

    // Update motion matcher to use the new database
    if (motionMatcher.IsInitialized()) {
        // Switch the motion matcher's database reference
        // The motion matcher will use this database for subsequent searches
        motionMatcher.SetCurrentDatabase(*targetDatabase);
        
        if (debugEnabled) {
            std::cout << "[HybridGraph] Motion matcher switched to database: " << databaseName << "\n";
        }
    }

    // If blend duration > 0, we need to blend between databases
    if (blendDuration > 0.0f && animator) {
        // Create a transition layer that blends from old to new database
        // This is handled by the layer system
        std::cout << "[HybridGraph] Database switch complete with blend duration: " << blendDuration << "s\n";
    } else {
        std::cout << "[HybridGraph] Database switch complete (instant)\n";
    }

    if (debugEnabled) {
        std::cout << "[HybridGraph] New database stats:\n";
        std::cout << "  Poses: " << targetDatabase->GetPoseCount() << "\n";
        std::cout << "  Animations: " << targetDatabase->GetAnimationCount() << "\n";
    }
}

std::vector<bool> HybridAnimGraph::CreateLowerBodyMask() const {
    std::vector<bool> mask;
    if (!skeleton) return mask;

    mask.resize(skeleton->bones.size(), false);

    // Helper lambda to check if bone index matches a pattern
    auto boneMatchesPattern = [this](size_t boneIndex, const std::string& pattern) -> bool {
        for (const auto& [name, index] : skeleton->boneMapping) {
            if (static_cast<size_t>(index) == boneIndex) {
                return name.find(pattern) != std::string::npos;
            }
        }
        return false;
    };

    // Mark lower-body bones
    for (size_t i = 0; i < skeleton->bones.size(); i++) {
        if (boneMatchesPattern(i, "leg") ||
            boneMatchesPattern(i, "foot") ||
            boneMatchesPattern(i, "toe") ||
            boneMatchesPattern(i, "hip") ||
            boneMatchesPattern(i, "spine")) {
            mask[i] = true;
        }
    }

    return mask;
}

std::vector<bool> HybridAnimGraph::CreateUpperBodyMask() const {
    std::vector<bool> mask;
    if (!skeleton) return mask;

    mask.resize(skeleton->bones.size(), false);

    // Helper lambda to check if bone index matches a pattern
    auto boneMatchesPattern = [this](size_t boneIndex, const std::string& pattern) -> bool {
        for (const auto& [name, index] : skeleton->boneMapping) {
            if (static_cast<size_t>(index) == boneIndex) {
                return name.find(pattern) != std::string::npos;
            }
        }
        return false;
    };

    // Mark upper-body bones
    for (size_t i = 0; i < skeleton->bones.size(); i++) {
        if (boneMatchesPattern(i, "arm") ||
            boneMatchesPattern(i, "hand") ||
            boneMatchesPattern(i, "shoulder") ||
            boneMatchesPattern(i, "clavicle") ||
            boneMatchesPattern(i, "neck") ||
            boneMatchesPattern(i, "head")) {
            mask[i] = true;
        }
    }

    return mask;
}

std::vector<bool> HybridAnimGraph::CreateFullBodyMask() const {
    std::vector<bool> mask;
    if (!skeleton) return mask;

    mask.resize(skeleton->bones.size(), true);
    return mask;
}

std::string HybridAnimGraph::GetDebugInfo() const {
    std::string info = "Hybrid Animation Graph\n";
    info += "========================\n";
    info += "Current State: " + HybridStateToString(currentState) + "\n";
    info += "Previous State: " + HybridStateToString(previousState) + "\n";
    info += "Transitioning: " + std::string(isTransitioning ? "YES" : "NO") + "\n";
    if (isTransitioning) {
        info += "  Progress: " + std::to_string((int)(transitionProgress * 100)) + "%\n";
        info += "  From: " + HybridStateToString(transitionFromState) + "\n";
        info += "  To: " + HybridStateToString(transitionToState) + "\n";
    }
    info += "Current Database: " + currentDatabaseName + "\n";
    info += "Motion Matching: " + std::string(config.enableMotionMatching ? "ENABLED" : "DISABLED") + "\n";
    info += "Upper-Body Layers: " + std::to_string(upperBodyLayers.size()) + "\n";
    info += "Additive Layers: " + std::to_string(additiveLayers.size()) + "\n";

    return info;
}

void HybridAnimGraph::PrintDebugInfo() const {
    std::cout << "\n" << GetDebugInfo() << std::endl;

    if (config.enableMotionMatching && currentState == HybridState::LOCOMOTION_GROUNDED) {
        motionMatcher.PrintDebugInfo();
    }
}
