#include "AnimationStateMachine.h"
#include <iostream>
#include <algorithm>
#include <glm/gtc/quaternion.hpp>

// ========================================
// AnimationStateMachine Implementation
// ========================================
// 
// UNREAL-STYLE STATE MACHINE DESIGN:
// 1. States have explicit ENTRY and EXIT conditions
// 2. One-shot animations (jump) lock state until complete
// 3. Movement uses blend space concept (idle/walk/run blended)
// 4. Crouch is explicit toggle, not hold-based
// 5. Transitions only happen when rules allow

AnimationStateMachine::AnimationStateMachine(Animator* animator)
    : animator(animator),
      currentState(AnimationState::NONE),
      previousState(AnimationState::NONE),
      movementSpeed(0.0f),
      smoothedSpeed(0.0f),
      isGrounded(true),
      verticalVelocity(0.0f),
      prevMoving(false),
      prevSprinting(false),
      prevJump(false),
      prevCrouch(false),
      jumpAnimationPlaying(false),
      jumpAnimationStartTime(0.0f),
      useBlendSpace(true),
      idleToWalkThreshold(0.3f),
      walkToRunThreshold(0.6f),
      blendWeight(0.0f),
      targetBlendWeight(0.0f),
      blendSmoothRate(10.0f),
      maxWalkSpeed(2.0f),
      maxRunSpeed(6.0f),
      isTransitioningState(false),
      transitionProgress(0.0f),
      transitionDuration(0.2f),
      transitionFromState(AnimationState::NONE),
      transitionToState(AnimationState::NONE),
      defaultBlendDuration(0.15f),
      currentInput()
{
    if (!animator) {
        std::cerr << "[AnimationStateMachine] ERROR: Null animator!\n";
    }
}

void AnimationStateMachine::registerAnimation(AnimationState state, Animation* anim, float speed, bool loop)
{
    if (!anim) {
        std::cerr << "[AnimationStateMachine] WARNING: Trying to register null animation for state "
                  << AnimationStateToString(state) << "\n";
        return;
    }
    
    AnimationStateData data;
    data.animation = anim;
    data.speed = speed;
    data.loop = loop;
    data.additive = false;
    data.name = AnimationStateToString(state);
    
    stateAnimations[state] = data;
    std::cout << "[AnimationStateMachine] Registered animation: " << data.name 
              << " (" << anim->name << ")\n";
}

void AnimationStateMachine::registerAnimations(
    Animation* idle, Animation* walk, Animation* run,
    Animation* jump, Animation* fall,
    Animation* crouch, Animation* crouchWalk)
{
    if (idle) registerAnimation(AnimationState::IDLE, idle, 1.0f, true);
    if (walk) registerAnimation(AnimationState::WALK, walk, 1.0f, true);
    if (run) registerAnimation(AnimationState::RUN, run, 1.5f, true);
    if (jump) registerAnimation(AnimationState::JUMP, jump, 1.0f, false);
    if (fall) registerAnimation(AnimationState::FALL, fall, 1.0f, true);
    if (crouch) registerAnimation(AnimationState::CROUCH, crouch, 1.0f, true);
    if (crouchWalk) registerAnimation(AnimationState::CROUCH_WALK, crouchWalk, 1.0f, true);
}

void AnimationStateMachine::addTransition(const AnimationTransition& transition)
{
    transitions.push_back(transition);
}

void AnimationStateMachine::addTransition(AnimationState from, AnimationState to, float duration, std::function<bool()> condition)
{
    AnimationTransition t;
    t.fromState = from;
    t.toState = to;
    t.blendDuration = duration;
    t.condition = condition;
    transitions.push_back(t);
}

void AnimationStateMachine::clearTransitions()
{
    transitions.clear();
}

void AnimationStateMachine::initialize()
{
    // Start with IDLE animation
    if (stateAnimations.count(AnimationState::IDLE) > 0) {
        auto& idleData = stateAnimations[AnimationState::IDLE];
        
        if (idleData.animation) {
            currentState = AnimationState::IDLE;
            if (animator) {
                animator->Play(idleData.animation);
            }
            std::cout << "[Anim] Initialized: IDLE\n";
            return;
        }
    }
    
    // Fallback to first available animation
    if (!stateAnimations.empty()) {
        auto it = stateAnimations.begin();
        currentState = it->first;
        if (animator && it->second.animation) {
            animator->Play(it->second.animation);
        }
        std::cout << "[Anim] Initialized: " << AnimationStateToString(currentState) << "\n";
    }
}

void AnimationStateMachine::setState(AnimationState state)
{
    if (currentState == state) return;
    
    auto it = stateAnimations.find(state);
    if (it == stateAnimations.end()) {
        std::cerr << "[AnimationStateMachine] WARNING: No animation for state " 
                  << AnimationStateToString(state) << "\n";
        return;
    }
    
    previousState = currentState;
    currentState = state;
    isTransitioningState = false;
    transitionProgress = 0.0f;
    
    // Play the animation for this state
    if (animator && it->second.animation) {
        animator->Play(it->second.animation);
    }
    
    std::cout << "[AnimationStateMachine] State changed: " 
              << AnimationStateToString(previousState) << " -> " 
              << AnimationStateToString(currentState) << "\n";
}

void AnimationStateMachine::setBlendBands(float idleToWalk, float walkToRun)
{
    idleToWalkThreshold = idleToWalk;
    walkToRunThreshold = walkToRun;
}

void AnimationStateMachine::setWalkRunBlendThreshold(float walkThreshold, float runThreshold)
{
    maxWalkSpeed = walkThreshold;
    maxRunSpeed = runThreshold;
}

void AnimationStateMachine::update(float dt, const CharacterInput& input)
{
    currentInput = input;
    movementSpeed = input.moveMagnitude;
    isGrounded = input.grounded;
    verticalVelocity = input.verticalVelocity;
    
    update(dt, movementSpeed, isGrounded, input.jump, input.crouch, input.sprint);
}

void AnimationStateMachine::update(float dt, float speed, bool grounded, bool jumping, bool crouching, bool sprinting)
{
    movementSpeed = speed;
    isGrounded = grounded;

    // First: complete any pending transition
    if (isTransitioningState) {
        updateTransition(dt);
    }

    // Detect edges (key press/release events)
    bool isMoving = (speed > 0.1f);
    bool movingJustStarted = isMoving && !prevMoving;
    bool movingJustStopped = !isMoving && prevMoving;
    bool sprintJustPressed = sprinting && !prevSprinting;
    bool sprintJustReleased = !sprinting && prevSprinting;
    bool jumpJustPressed = jumping && !prevJump;
    bool crouchJustPressed = crouching && !prevCrouch;
    bool crouchJustReleased = !crouching && prevCrouch;

    // Update previous state tracking
    prevMoving = isMoving;
    prevSprinting = sprinting;
    prevJump = jumping;
    prevCrouch = crouching;

    // Track jump animation playback
    if (currentState == AnimationState::JUMP) {
        jumpAnimationPlaying = true;
        jumpAnimationStartTime += dt;
    }

    // ============================================================
    // GRADIENT BAND INTERPOLATION (Blend Space) for Locomotion
    // ============================================================
    // Instead of discrete idle/walk/run states, we continuously blend
    // based on movement speed using gradient bands:
    //
    // Speed:     0.0 ---- 0.3 ---- 0.6 ---- 1.0
    //           IDLE  →  WALK  →   RUN
    // Blend:    0.0    0.25   0.5    0.75   1.0
    //
    // This eliminates popping and creates smooth transitions!
    // ============================================================

    AnimationState targetState = currentState;
    bool canExitCurrentState = true;

    // ------------------------------------------------------------
    // STEP 1: Check if we can exit current state (EXIT RULES)
    // ------------------------------------------------------------

    if (currentState == AnimationState::JUMP) {
        // JUMP is a ONE-SHOT animation - cannot exit until:
        // 1. Animation has played for minimum duration (0.4s)
        // 2. OR character is grounded (landed)
        // Both conditions must be true to exit smoothly
        float minJumpDuration = 0.4f;
        
        // Can exit if BOTH conditions are met:
        // - Played long enough
        // - Currently grounded
        if (jumpAnimationStartTime >= minJumpDuration && grounded) {
            canExitCurrentState = true;
            jumpAnimationPlaying = false;
            jumpAnimationStartTime = 0.0f;
        } else {
            canExitCurrentState = false;
        }
    }
    
    if (currentState == AnimationState::CROUCH || currentState == AnimationState::CROUCH_WALK) {
        // CROUCH states use TOGGLE behavior - only exit when:
        // 1. Crouch button is released (toggle off)
        // 2. OR crouch button is pressed again (toggle off)
        if (crouching && !crouchJustReleased && !crouchJustPressed) {
            canExitCurrentState = false;  // Still in crouch hold, don't exit
        }
    }

    // ------------------------------------------------------------
    // STEP 2: Calculate blend weights using gradient bands
    // ------------------------------------------------------------

    if (useBlendSpace && !crouching && grounded && currentState != AnimationState::JUMP) {
        // Normalize speed to blend weight (0.0 to 1.0)
        // 0.0 = idle, 0.5 = walk, 1.0 = run
        float normalizedSpeed = speed / maxRunSpeed;
        targetBlendWeight = glm::clamp(normalizedSpeed, 0.0f, 1.0f);

        // Smooth blend weight transition
        blendWeight = glm::mix(blendWeight, targetBlendWeight, blendSmoothRate * dt);

        // Determine dominant state based on blend weight
        if (blendWeight < 0.25f) {
            targetState = AnimationState::IDLE;
        } else if (blendWeight < 0.6f) {
            targetState = AnimationState::WALK;
        } else {
            targetState = AnimationState::RUN;
        }

        // CRITICAL FIX: Only apply blend space when state CHANGES
        // This prevents animation time reset and allows full animation cycles

        // Only re-blend if state changed significantly
        // CRITICAL FIX: Update blend space EVERY frame for instant WASD response
        // Don't wait for state changes - blend weight should follow input immediately
        applyBlendSpaceAnimation(dt);
    }

    // ------------------------------------------------------------
    // STEP 3: If we can exit, evaluate transition candidates
    // ------------------------------------------------------------
    
    if (canExitCurrentState && !isTransitioningState) {
        
        // PRIORITY 1: Jump (highest priority action)
        if (jumpJustPressed && grounded && !crouching) {
            targetState = AnimationState::JUMP;
            jumpAnimationPlaying = true;
            jumpAnimationStartTime = 0.0f;
            std::cout << "[FSM] JUMP triggered (edge detect)\n";
        }
        // PRIORITY 2: Crouch toggle
        else if (crouchJustPressed && grounded) {
            if (currentState == AnimationState::CROUCH || currentState == AnimationState::CROUCH_WALK) {
                // Toggle OFF: Crouch → Idle
                targetState = AnimationState::IDLE;
                std::cout << "[FSM] CROUCH OFF (toggle)\n";
            } else {
                // Toggle ON: Any → Crouch
                targetState = AnimationState::CROUCH;
                std::cout << "[FSM] CROUCH ON (toggle)\n";
            }
        }
        // PRIORITY 3: Movement states handled by blend space (if enabled)
        else if (useBlendSpace && !crouching && grounded) {
            // Blend space handles smooth idle/walk/run transitions
            // Just ensure we're transitioning to the right dominant state
            if (targetState != currentState && currentState != AnimationState::JUMP) {
                // Let blend space handle it - no explicit transition needed
                // unless state changed significantly
            }
        }
        // PRIORITY 3b: Movement states (discrete, if blend space disabled)
        else if (!useBlendSpace && !crouching && grounded) {
            if (movingJustStarted) {
                targetState = sprinting ? AnimationState::RUN : AnimationState::WALK;
                std::cout << "[FSM] MOVE START: " << (sprinting ? "RUN" : "WALK") << "\n";
            }
            else if (movingJustStopped) {
                targetState = AnimationState::IDLE;
                std::cout << "[FSM] MOVE STOP → IDLE\n";
            }
            else if (sprintJustPressed && isMoving && currentState == AnimationState::WALK) {
                targetState = AnimationState::RUN;
                std::cout << "[FSM] SPRINT PRESSED → RUN\n";
            }
            else if (sprintJustReleased && isMoving && currentState == AnimationState::RUN) {
                targetState = AnimationState::WALK;
                std::cout << "[FSM] SPRINT RELEASED → WALK\n";
            }
        }
        // PRIORITY 4: Crouch movement
        else if (crouching && grounded) {
            if (isMoving) {
                targetState = AnimationState::CROUCH_WALK;
            }
            else if (currentState != AnimationState::CROUCH) {
                targetState = AnimationState::CROUCH;
            }
        }
        // PRIORITY 5: Default to idle when no input
        else if (!isMoving && !crouching && grounded && currentState != AnimationState::IDLE) {
            targetState = AnimationState::IDLE;
            std::cout << "[FSM] NO INPUT → IDLE\n";
        }
    }

    // ------------------------------------------------------------
    // STEP 4: Execute transition if state changed
    // ------------------------------------------------------------

    if (targetState != currentState && !isTransitioningState) {
        // CRITICAL FIX: Instant transition for JUMP (one-shot action)
        // Use blend for locomotion, instant for actions
        float blendDur = (targetState == AnimationState::JUMP) ? 0.02f : defaultBlendDuration;
        startTransition(targetState, blendDur);
    }

    // Update animator to advance animation time
    if (animator) {
        animator->Update(dt);
    }

    // Debug output every 60 frames
    static int frameCount = 0;
    frameCount++;
    if (frameCount % 60 == 0) {
        std::cout << "[FSM] State=" << AnimationStateToString(currentState)
                  << " Speed=" << speed << " Grounded=" << grounded
                  << " BlendWeight=" << blendWeight << " TargetBlend=" << targetBlendWeight
                  << " canExit=" << canExitCurrentState
                  << " jumpPlaying=" << jumpAnimationPlaying
                  << " jumpTime=" << jumpAnimationStartTime << "\n";
    }
}

// ============================================================
// GRADIENT BAND INTERPOLATION - Apply blended animations
// ============================================================
void AnimationStateMachine::applyBlendSpaceAnimation(float dt)
{
    if (!animator || !useBlendSpace) return;

    // Blend space uses blend weight to mix idle/walk/run
    // Weight 0.0 = 100% idle
    // Weight 0.5 = 100% walk
    // Weight 1.0 = 100% run

    Animation* idleAnim = nullptr;
    Animation* walkAnim = nullptr;
    Animation* runAnim = nullptr;

    auto it = stateAnimations.find(AnimationState::IDLE);
    if (it != stateAnimations.end()) idleAnim = it->second.animation;

    it = stateAnimations.find(AnimationState::WALK);
    if (it != stateAnimations.end()) walkAnim = it->second.animation;

    it = stateAnimations.find(AnimationState::RUN);
    if (it != stateAnimations.end()) runAnim = it->second.animation;

    // CRITICAL FIX: Don't use BlendTwoAnimations - it disrupts playback!
    // Instead, determine the DOMINANT animation and Play() it
    // The blend weight is used for smooth state determination, not layer blending
    
    Animation* dominantAnim = nullptr;
    
    if (blendWeight < 0.25f) {
        // Idle dominant
        dominantAnim = idleAnim;
    } else if (blendWeight < 0.6f) {
        // Walk dominant
        dominantAnim = walkAnim;
    } else {
        // Run dominant
        dominantAnim = runAnim;
    }
    
    // Only change animation if it's different from current
    // This allows animations to play through without interruption
    static Animation* lastPlayedAnim = nullptr;
    
    if (dominantAnim && dominantAnim != lastPlayedAnim) {
        animator->Play(dominantAnim);
        lastPlayedAnim = dominantAnim;
    }
    
    // NOTE: Don't call animator->Update(dt) here - it's called in the main FSM update
    // Calling it twice would advance animation time double speed
}

void AnimationStateMachine::evaluateTransitions()
{
    // Don't evaluate if we're in NONE state (use direct state determination instead)
    if (currentState == AnimationState::NONE) return;
    
    for (const auto& transition : transitions) {
        if (transition.fromState == currentState && transition.condition) {
            if (transition.condition()) {
                startTransition(transition.toState, transition.blendDuration);
                break;  // Only one transition per frame
            }
        }
    }
}

void AnimationStateMachine::startTransition(AnimationState toState, float duration)
{
    std::cout << ">>> TRANSITION: " << AnimationStateToString(currentState)
              << " -> " << AnimationStateToString(toState) << "\n";

    if (toState == currentState) {
        std::cout << "    SKIP: Same state\n";
        return;
    }
    if (isTransitioningState) {
        std::cout << "    SKIP: Already transitioning\n";
        return;
    }

    auto it = stateAnimations.find(toState);
    if (it == stateAnimations.end()) {
        std::cerr << "[Anim] Cannot transition - no animation for: "
                  << AnimationStateToString(toState) << "\n";
        return;
    }

    if (!it->second.animation) {
        currentState = toState;
        isTransitioningState = false;
        std::cout << "    No animation data, instant state change\n";
        return;
    }

    // Start transition
    transitionFromState = currentState;
    transitionToState = toState;
    transitionDuration = duration;
    transitionProgress = 0.0f;
    isTransitioningState = true;
    
    // CRITICAL FIX: Update currentState immediately for correct state queries
    // The blend weight controls visual blending, not the logical state
    currentState = toState;

    std::cout << "    Animation ptr=" << it->second.animation
              << " dur=" << it->second.animation->duration
              << " bones=" << it->second.animation->boneAnimations.size() << "\n";

    // Blend to new animation
    animator->BlendTo(it->second.animation, duration);

    std::cout << "[Anim] " << AnimationStateToString(transitionFromState)
              << " -> " << AnimationStateToString(transitionToState) << "\n";
}

void AnimationStateMachine::updateTransition(float dt)
{
    if (transitionDuration <= 0.0f) {
        // Instant transition
        transitionProgress = 1.0f;
    } else {
        transitionProgress += dt / transitionDuration;
    }

    if (transitionProgress >= 1.0f) {
        transitionProgress = 1.0f;
        isTransitioningState = false;
        // State was already updated in startTransition(), just clear transition state
        std::cout << "[Anim] State: " << AnimationStateToString(currentState)
                  << " (transition complete, dur=" << transitionDuration << ")\n";
    } else {
        std::cout << "[Trans] Progress: " << (int)(transitionProgress * 100)
                  << "% dur=" << transitionDuration << " dt=" << dt << "\n";
    }
}

void AnimationStateMachine::applyAnimationBlend()
{
    // Blending is handled by state transitions
}

std::string AnimationStateMachine::getDebugInfo() const
{
    std::string info = "Animation State Machine\n";
    info += "========================\n";
    info += "Current State: " + AnimationStateToString(currentState) + "\n";
    info += "Previous State: " + AnimationStateToString(previousState) + "\n";
    info += "Transitioning: " + std::string(isTransitioningState ? "YES" : "NO") + "\n";
    if (isTransitioningState) {
        info += "Transition Progress: " + std::to_string((int)(transitionProgress * 100)) + "%\n";
        info += "From: " + AnimationStateToString(transitionFromState) + "\n";
        info += "To: " + AnimationStateToString(transitionToState) + "\n";
    }
    info += "Movement Speed: " + std::to_string(movementSpeed) + "\n";
    info += "Grounded: " + std::string(isGrounded ? "YES" : "NO") + "\n";
    info += "Vertical Velocity: " + std::to_string(verticalVelocity) + "\n";
    info += "Registered States: " + std::to_string(stateAnimations.size()) + "\n";
    info += "Transitions: " + std::to_string(transitions.size()) + "\n";
    return info;
}

void AnimationStateMachine::printState() const
{
    std::cout << "\n" << getDebugInfo() << std::endl;
}

// ========================================
// AnimationGraph Implementation
// ========================================

void AnimationGraph::addBlendNode(Animation* anim, const glm::vec2& position)
{
    BlendNode node;
    node.animation = anim;
    node.blendPosition = position;
    node.weight = 0.0f;
    nodes.push_back(node);
}

void AnimationGraph::updateWeights(const glm::vec2& targetPosition)
{
    currentPosition = targetPosition;
    
    // Calculate distances to all nodes
    std::vector<std::pair<float, size_t>> distances;
    for (size_t i = 0; i < nodes.size(); i++) {
        float dist = glm::distance(targetPosition, nodes[i].blendPosition);
        distances.push_back({dist, i});
    }
    
    // Sort by distance
    std::sort(distances.begin(), distances.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    
    // Use inverse distance weighting for closest nodes
    float totalWeight = 0.0f;
    
    // Reset all weights
    for (auto& node : nodes) {
        node.weight = 0.0f;
    }
    
    // Weight the 3 closest nodes
    int numNodesToBlend = std::min(3, (int)nodes.size());
    for (int i = 0; i < numNodesToBlend; i++) {
        float dist = distances[i].first;
        size_t idx = distances[i].second;
        
        // Inverse distance weighting
        float weight = 1.0f / (dist + 0.001f);
        nodes[idx].weight = weight;
        totalWeight += weight;
    }
    
    // Normalize weights
    if (totalWeight > 0.0f) {
        for (auto& node : nodes) {
            node.weight /= totalWeight;
        }
    }
}

void AnimationGraph::applyToAnimator(Animator* animator, float dt)
{
    if (!animator) return;
    
    for (const auto& node : nodes) {
        if (node.animation && node.weight > 0.01f) {
            // animator->BlendToWithWeight(node.animation, node.weight, dt * 5.0f);
        }
    }
}
