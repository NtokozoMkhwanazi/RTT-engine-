# Physics System

Collision detection and physics simulation with GJK/EPA algorithms, constraints, and character controller.

## 📁 Files

```
physicsSystem/
├── Physics.h                 # Physics world header
├── physics.cpp               # Physics world implementation
├── GJK.h/.cpp                # GJK collision detection + EPA penetration depth (Expanding Polytope)
├── Constraint.h/.cpp         # Constraint base class + implementation
├── AdvancedConstraints.h/.cpp # Advanced constraints (hinge, slider, 6DOF, point)
├── RigidBody.h               # Rigid body definition (mass, inertia)
├── Floor.h                   # Static floor/ground plane
├── VelocityConstraints.h     # Velocity-level constraint solver
├── StaticConstraintPipeline.h # Batched static constraint solve
├── TOI.h                     # Time-of-impact estimation
└── README.md                 # This file
```

## 🎯 Features

### Collision Detection
- **GJK Algorithm** - Gilbert-Johnson-Keerthi for convex collision
- **EPA Algorithm** - Expanding Polytope for penetration depth
- **Support Functions** - Minkowski sum support mapping
- **Broad Phase** - Spatial partitioning for pair culling
- **Narrow Phase** - Precise collision detection

### Physics Simulation
- **Rigid Bodies** - Dynamic and static bodies
- **Mass Properties** - Mass, inertia tensor calculation
- **Integration** - Velocity and position integration
- **Gravity** - Global gravity vector
- **Damping** - Linear and angular damping

### Constraints
- **Point Constraint** - Pin objects together
- **Hinge Constraint** - Rotational joints
- **Slider Constraint** - Linear sliding joints
- **6DOF Constraint** - Full 6 degrees of freedom
- **Character Constraint** - Character-specific constraints
- **Velocity-Level Solver** - Velocity constraint resolution (`VelocityConstraints.h`)
- **Static Pipeline** - Batched static constraint solve (`StaticConstraintPipeline.h`)
- **Time of Impact** - Continuous collision estimation (`TOI.h`)

### Character Controller
- **Capsule Collision** - Capsule-shaped character
- **Ground Detection** - Grounded state tracking
- **Slope Handling** - Walkable slope limits
- **Step Offset** - Automatic step climbing
- **Collision Response** - Smooth collision resolution

## 📖 Usage

### Physics World Setup

```cpp
PhysicsWorld world;
world.setGravity(glm::vec3(0, -9.81f, 0));
world.initialize();

// In update loop
world.step(deltaTime);
```

### Creating Rigid Bodies

```cpp
RigidBody body;
body.position = glm::vec3(0, 5, 0);
body.mass = 1.0f;
body.collider = BoxCollider(glm::vec3(1, 1, 1));

world.addRigidBody(body);
```

### Collision Detection

```cpp
// GJK for collision test
GJK_Result result = GJK_TestCollision(shapeA, shapeB);

if (result.intersecting) {
    // EPA for penetration depth
    EPA_Result epa = EPA_ComputePenetration(shapeA, shapeB);
    
    // Contact information
    glm::vec3 contactPoint = epa.contactPoint;
    glm::vec3 contactNormal = epa.contactNormal;
    float penetrationDepth = epa.depth;
}
```

### Character Controller

```cpp
CharacterController character;
character.position = glm::vec3(0, 0, 0);
character.height = 1.8f;
character.radius = 0.5f;
character.maxSlopeAngle = 45.0f;

world.addCharacter(character);

// Update character
character.move(inputVelocity, deltaTime);
character.update(deltaTime, world);
```

## 🔧 Configuration

### Physics Settings
```cpp
PhysicsConfig config;
config.gravity = glm::vec3(0, -9.81f, 0);
config.solverIterations = 10;
config.maxSubSteps = 4;
config.fixedTimeStep = 1.0f / 60.0f;
```

### Character Controller Settings
```cpp
CharacterConfig charConfig;
charConfig.height = 1.8f;
charConfig.radius = 0.5f;
charConfig.stepOffset = 0.5f;
charConfig.slopeLimit = 45.0f;
charConfig.jumpForce = 5.0f;
```

## 📊 Performance

| Algorithm | Complexity | Typical Time |
|-----------|------------|--------------|
| **GJK** | O(log n) | ~0.01ms |
| **EPA** | O(n) | ~0.05ms |
| **Constraint Solver** | O(n * iterations) | ~0.2ms |
| **Character Update** | O(colliders) | ~0.1ms |

## 🛠️ Build & Test

The physics system compiles as part of the main engine build. From the repository root:

```bash
make            # build bin/test_runner + bin/engine (debug)
make test       # run the full unit-test suite (731 tests / 99 suites)
make test-physics  # GJK/EPA, gravity, restitution, friction, raycasts, constraints
make run        # self-check tests, then boot the engine
make run-headless  # bounded headless engine run (CI-friendly)
make test-list  # list every test
```

Collision detection (GJK/EPA), gravity, restitution, friction, raycasts, rigid-body
integration and the constraint solver are covered by `make test-physics` (the
`PhysicsTest`, `ConstraintSolver`, `HingeConstraint`, `HingeMotor`,
`DistanceConstraint`, `PlaneConstraint` and `Static5DOFHingeBlock` suites), plus
`make test` with `--gtest_filter=Physics*|Constraint*|Hinge*|Distance*`.
See the [root README](../README.md#test-suites) for the full target list and prerequisites.

## 🐛 Debugging

See the engine documentation under `docs/`:
- [Troubleshooting](../docs/TROUBLESHOOTING.md) — build/test failures and physics debug
  rendering.
- [Architecture](../docs/ARCHITECTURE.md) — physics/systems data flow.

The editor's **Debug Rendering** (physics shape overlay, `DebugRenderer`) is togglable
from the ImGui viewport, and the **GPU Profiler** can isolate constraint-solver cost.

---

**Status:** ✅ Production Ready
**Last Updated:** September 2026
