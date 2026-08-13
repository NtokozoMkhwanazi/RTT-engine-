# ECS Framework

Entity-Component-System architecture with archetype-based storage for cache-coherent iteration.

## 📁 Files

```
ecs/
├── ECS.h                     # Main include file
├── World.h                   # World management
├── Entity.h                  # Entity types
├── Component.h               # Component base classes
├── System.h                  # System base classes
├── Archetype.h               # Archetype data structure
├── ArchetypeManager.h        # Archetype management
├── ComponentManager.h        # Legacy component storage
├── EntityManager.h           # Entity lifecycle
├── EventSystem.h             # Event dispatching
├── JobSystem.h               # Multi-threading
├── Blueprint.h               # Entity blueprints
├── Serialization.h           # Save/load systems
├── EnhancedWorld.h           # Enhanced world features
├── ComponentSerializers.h    # Component serialization
├── RelationshipManager.h     # Entity relationships
├── README.md                 # This file
├── ENHANCED_ECS_README.md    # Enhanced features guide
│
├── components/               # Component definitions
│   ├── Components.h          # All components include
│   ├── TransformComponent.h  # Position, rotation, scale
│   ├── MeshComponent.h       # Mesh rendering
│   ├── SkinnedMeshComponent.h# Skinned mesh
│   ├── CameraComponent.h     # Camera properties
│   ├── LightComponent.h      # Light properties
│   ├── RigidBodyComponent.h  # Physics body
│   ├── CharacterControllerComponent.h
│   ├── AnimatorComponent.h   # Animation component
│   ├── SkeletonComponent.h   # Bone hierarchy
│   ├── MotionMatchingComponent.h
│   ├── TerrainComponent.h
│   ├── WorldObjectComponent.h
│   ├── TagComponent.h        # Entity tags
│   └── NameComponent.h       # Entity names
│
└── systems/                  # System implementations
    ├── Systems.h             # All systems include
    ├── RenderSystem.h        # Rendering system
    ├── PhysicsSystem.h       # Physics simulation
    ├── AnimationSystem.h     # Animation updates
    ├── CharacterControllerSystem.h
    ├── MotionMatchingSystem.h
    ├── TerrainSystem.h
    └── WorldObjectSystem.h
```

## 🏗️ Architecture

### Core Concepts

**Entity** - Unique ID with no data
```cpp
Entity entity = world.createEntity();
```

**Component** - Pure data, no behavior
```cpp
auto& transform = world.addComponent<TransformComponent>(entity);
transform.position = glm::vec3(0, 1, 0);
```

**System** - Behavior that operates on components
```cpp
class RenderSystem : public System {
    void update(float dt) override {
        // Render all entities with Transform + Mesh
    }
};
```

### Archetype Storage

Entities with the same components are stored together in memory for cache efficiency:

```
Archetype: [Transform, Mesh, Name]
Chunk 1: [Entity1, Entity2, Entity3, ...]
Chunk 2: [Entity4, Entity5, Entity6, ...]
```

## 📖 Usage

### Creating Entities

```cpp
// Method 1: Archetype-based (recommended)
auto e = world.createEntityWithComponents<
    TransformComponent, 
    MeshComponent, 
    NameComponent
>();

// Method 2: Legacy (ComponentManager storage)
auto e = world.createEntity();
world.addComponent<TransformComponent>(e);
world.addComponent<MeshComponent>(e);
```

### Component Access

```cpp
// For archetype-stored components
auto* t = world.getComponentArchetype<TransformComponent>(e);

// For legacy components
auto* t = world.getComponent<TransformComponent>(e);
```

### System Iteration

```cpp
// Efficient archetype iteration
world.forEach<TransformComponent, MeshComponent>(
    [](EntityID id, TransformComponent& t, MeshComponent& m) {
        // Process entity
    }
);
```

## 🔧 Enhanced Features

See [ENHANCED_ECS_README.md](ENHANCED_ECS_README.md) for:
- Relationship manager (parent/child)
- Event system
- Job system (multi-threading)
- Blueprints
- Serialization

## 📊 Performance

| Operation | Legacy | Archetype | Speedup |
|-----------|--------|-----------|---------|
| **Iteration** | Random access | Sequential | 5-10x |
| **Cache Hits** | ~50% | ~95% | 2x |
| **Memory** | Fragmented | Contiguous | 30% less |

## 🛠️ Build & Test

The ECS framework compiles as part of the main engine build. From the repository root:

```bash
make            # build the test runner
make test       # run the full unit-test suite (494 tests)
make run        # self-check tests, then boot the engine
make run-headless  # bounded headless engine run (CI-friendly)
```

Archetypes, blueprints, relationships, events, jobs, serialization and entity-manager
operations are covered by `make test` (ECS + entity-manager + undo/redo suites; filter
with `--gtest_filter=ECS*:EntityManager*`). See the [root README](../README.md) for
prerequisites and engine controls.

## 🐛 Known Issues

See [VIEWPORT_STATUS.md](../VIEWPORT_STATUS.md) for current integration status.

---

**Status:** ✅ Production Ready
**Last Updated:** August 2026
