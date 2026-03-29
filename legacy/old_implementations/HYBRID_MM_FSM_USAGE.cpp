/**
 * Hybrid MM+FSM Usage Example
 * 
 * This shows the CORRECT architecture:
 * - Motion Matching for smooth locomotion (idle↔walk↔run)
 * - FSM for discrete states (jump, fall, crouch, combat)
 */

#include "animationSystem/HybridMMFSM.h"
#include <iostream>

// Example usage in your game loop:
/*
// 1. Initialize
HybridMMFSM hybridSystem;
hybridSystem.Initialize(skeleton, animator);

// 2. Load locomotion animations (for MM)
auto idleAnim = std::make_shared<Animation>("Idle", ...);
auto walkAnim = std::make_shared<Animation>("Walk", ...);
auto runAnim = std::make_shared<Animation>("Run", ...);

hybridSystem.LoadLocomotionAnimation("Idle", idleAnim);
hybridSystem.LoadLocomotionAnimation("Walk", walkAnim);
hybridSystem.LoadLocomotionAnimation("Run", runAnim);

// 3. Load state animations (for FSM states)
auto jumpAnim = std::make_shared<Animation>("Jump", ...);
auto fallAnim = std::make_shared<Animation>("Fall", ...);

hybridSystem.LoadStateAnimation(HybridState::JUMP, "Jump", jumpAnim);
hybridSystem.LoadStateAnimation(HybridState::FALL, "Fall", fallAnim);

// 4. Build databases
hybridSystem.BuildDatabases();

// 5. Add transitions
hybridSystem.AddTransition(
    HybridState::LOCOMOTION, 
    HybridState::JUMP,
    0.1f,  // Blend duration
    [&]() { return characterState.jump && characterState.grounded; }  // Condition
);

hybridSystem.AddTransition(
    HybridState::JUMP,
    HybridState::FALL,
    0.1f,
    [&]() { return !characterState.grounded && characterState.velocity.y < 0; }
);

hybridSystem.AddTransition(
    HybridState::FALL,
    HybridState::LOCOMOTION,
    0.1f,
    [&]() { return characterState.grounded; }
);

// 6. Game loop - update every frame
HybridCharacterState hybridState;
hybridState.position = characterPos;
hybridState.velocity = characterVelocity;
hybridState.moveDirection = moveDir;
hybridState.moveMagnitude = glm::length(moveDir);
hybridState.grounded = isGrounded;
hybridState.jump = jumpPressed;
hybridState.crouch = crouchPressed;

hybridSystem.Update(dt, hybridState);

// 7. Debug info
std::cout << hybridSystem.GetDebugInfo() << std::endl;
*/

// Key benefits of this architecture:
// 1. MM provides SMOOTH idle↔walk↔run transitions (no popping)
// 2. FSM manages DISCRETE states (jump, fall, crouch)
// 3. Each state can have its own MM database (crouch walk, combat movement)
// 4. MM handles root motion extraction and foot IK automatically
// 5. KD-Tree search is FAST (O(log n) vs O(n))

int main() {
    std::cout << "Hybrid MM+FSM Usage Example\n";
    std::cout << "===========================\n\n";
    
    std::cout << "Architecture:\n";
    std::cout << "  FSM manages: Jump, Fall, Crouch, Combat, Vault\n";
    std::cout << "  MM handles:  Idle↔Walk↔Run smooth blending\n\n";
    
    std::cout << "Benefits:\n";
    std::cout << "  ✓ Smooth locomotion (no popping)\n";
    std::cout << "  ✓ Fast KD-Tree search (O(log n))\n";
    std::cout << "  ✓ Root motion extraction\n";
    std::cout << "  ✓ Foot IK prevents footskating\n";
    std::cout << "  ✓ State-specific databases (crouch, combat)\n\n";
    
    std::cout << "See HybridMMFSM.h for full API documentation.\n";
    
    return 0;
}
