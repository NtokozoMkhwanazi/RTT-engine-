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
        Animation* next = idleAnim;
        if (speed < 0.1f) next = idleAnim;
        else if (speed < 5.0f) next = walkAnim;
        else next = runAnim;

        if (next != current) {
            animator->BlendTo(next, 0.2f);
            current = next;
        }
        animator->Update(dt);
    }

private:
    Animator* animator;
    Animation* idleAnim = nullptr;
    Animation* walkAnim = nullptr;
    Animation* runAnim = nullptr;
    Animation* current = nullptr;
};

