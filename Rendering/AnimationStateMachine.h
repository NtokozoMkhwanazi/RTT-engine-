#pragma once
#include "Animator.h"
#include "Animation.h"

class AnimationStateMachine {
public:
    AnimationStateMachine(Animator* animator) : animator(animator) {}

    void SetAnimations(Animation* idle, Animation* walk, Animation* run) {
        idleAnim = idle;
        walkAnim = walk;
        runAnim = run;

        current = idleAnim;
        animator->Play(current);
    }

    void Update(float dt, float speed) {
        // Only change animation if not forced
        if (!forced) {
            Animation* next = idleAnim;
            if (speed < 0.1f) next = idleAnim;
            else if (speed < walkSpeedThreshold) next = walkAnim;
            else next = runAnim;

            if (next != current) {
                animator->BlendTo(next, blendTime);
                current = next;
            }
        }

        animator->Update(dt);
    }

    // ---- Force a specific animation regardless of speed ----
    void ForceState(Animation* anim) {
        if (anim && anim != current) {
            animator->Play(anim);
            current = anim;
        }
        forced = true;
    }

    // ---- Reset to automatic speed-based animation ----
    void ResetForce() {
        forced = false;
    }

private:
    Animator* animator = nullptr;

    Animation* idleAnim = nullptr;
    Animation* walkAnim = nullptr;
    Animation* runAnim  = nullptr;
    Animation* current   = nullptr;

    float blendTime = 0.2f;
    float walkSpeedThreshold = 5.0f;
    bool forced = false;
};

