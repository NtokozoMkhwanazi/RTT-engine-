#pragma once
#include "Animator.h"
#include "Animation.h"

class AnimationStateMachine {
public:
    AnimationStateMachine(Animator* animator)
        : animator(animator) {}

    void SetAnimations(Animation* idle, Animation* walk, Animation* run) {
        idleAnim = idle;
        walkAnim = walk;
        runAnim  = run;

        current = idleAnim;
        animator->Play(current);
    }

    // locomotion ∈ [0..1]
    // 0.0 = idle
    // 0.5 = walk
    // 1.0 = run
    void Update(float dt, float locomotion)
    {
        if (!forced)
        {
            Animation* next = idleAnim;

            if (locomotion < 0.1f)
                next = idleAnim;
            else if (locomotion < runThreshold)
                next = walkAnim;
            else
                next = runAnim;

            if (next && next != current)
            {
                animator->BlendTo(next, blendTime);
                current = next;
            }
        }

        animator->Update(dt);
    }

    // ---- Force a specific animation regardless of speed ----
    void ForceState(Animation* anim)
    {
        if (anim && anim != current)
        {
            animator->Play(anim);
            current = anim;
        }
        forced = true;
    }

    // ---- Reset to automatic speed-based animation ----
    void ResetForce()
    {
        forced = false;
    }

private:
    Animator* animator = nullptr;

    Animation* idleAnim = nullptr;
    Animation* walkAnim = nullptr;
    Animation* runAnim  = nullptr;
    Animation* current  = nullptr;

    float blendTime   = 0.2f;
    float runThreshold = 0.75f; // >= run
    bool forced = false;
};

