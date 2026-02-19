#include "AnimationStateMachine.h"
#include <iostream>
#include <algorithm>
#include <glm/gtc/quaternion.hpp>

// ========================================
// AnimationStateMachine Implementation
// ========================================

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
      maxWalkSpeed(2.0f),
      maxRunSpeed(6.0f),
      isTransitioningState(false),
      transitionProgress(0.0f),
      transitionDuration(0.2f),
      defaultBlendDuration(0.15f)
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
        // After transition completes, currentState is updated - fall through to process new input
    }

    // Detect edges (key press/release events)
    bool isMoving = (speed > 0.1f);
    bool sprintPressed = sprinting && !prevSprinting;
    bool sprintReleased = !sprinting && prevSprinting;
    bool jumpPressed = jumping && !prevJump;

    prevMoving = isMoving;
    prevSprinting = sprinting;
    prevJump = jumping;

    // Determine target state based on CURRENT state and inputs
    // Priority: JUMP > CROUCH > RUN > WALK > IDLE
    // Note: FALL disabled for testing - jump loops back to idle
    AnimationState targetState = currentState;

    // JUMP: Highest priority - only trigger on jump PRESS when grounded
    if (jumpPressed && grounded) {
        targetState = AnimationState::JUMP;
    }
    // FALL: DISABLED - jump animation plays then returns to idle
    // This is for testing smooth transitions
    else if (crouching) {
        if (isMoving) {
            targetState = AnimationState::CROUCH_WALK;
        } else {
            targetState = AnimationState::CROUCH;
        }
    }
    // MOVEMENT: Respond immediately to movement input
    else if (isMoving) {
        if (sprinting) {
            targetState = AnimationState::RUN;
        } else {
            targetState = AnimationState::WALK;
        }
    }
    // IDLE: No input
    else {
        targetState = AnimationState::IDLE;
    }

    // Sprint transitions
    if (grounded && !crouching && isMoving) {
        if (sprintPressed && currentState == AnimationState::WALK) {
            targetState = AnimationState::RUN;
        } else if (sprintReleased && currentState == AnimationState::RUN) {
            targetState = AnimationState::WALK;
        }
    }

    // Debug output
    static int frameCount = 0;
    frameCount++;
    if (frameCount % 30 == 0) {
        std::cout << "[FSM] spd=" << speed << " grounded=" << grounded
                  << " jump=" << jumping << " vVel=" << verticalVelocity
                  << " sprint=" << sprinting << " crouch=" << crouching
                  << " target=" << AnimationStateToString(targetState)
                  << " current=" << AnimationStateToString(currentState)
                  << " isTrans=" << isTransitioningState << "\n";
    }

    // Transition if state changed and not already transitioning
    if (targetState != currentState && !isTransitioningState) {
        startTransition(targetState, defaultBlendDuration);
    }

    // Update animator to advance animation time
    if (animator) {
        animator->Update(dt);
    }
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
        previousState = transitionFromState;
        currentState = transitionToState;
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
