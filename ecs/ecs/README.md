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
├── JobSystem.h               # Multi-threading (work-stealing)
├── Blueprint.h               # Entity blueprints
├── Serialization.h           # Save/load systems
├── EnhancedWorld.h           # Enhanced world features
├── ComponentSerializers.h    # Component serialization
├── RelationshipManager.h     # Entity relationships
├── BoneSoA.h                 # Structure-of-arrays bone cache
├── DynamicPool.h             # Dynamic component pool
├── DynamicTypes.h            # Dynamic type registration
├── ComponentRegistration.cpp # Component type IDs
├── ComponentTypeID.cpp       # Type ID assignment
├── README.md                 # This file
├── ENHANCED_ECS_README.md    # Enhanced features guide
│
├── components/               # Component definitions
│   ├── Components.h          # All components include
│   ├── TransformComponent.h  # Position, rotation, scale
│   ├── MeshComponent.h       # Mesh rendering
│   ├── CameraComponent.h     # Camera properties
│   ├── LightComponent.h      # Light properties
│   ├── RigidBodyComponent.h  # Physics body
│   ├── AnimatorComponent.h   # Animation component
│   ├── MotionMatchingComponent.h
│   ├── ModelComponent.h      # Model references
│   ├── GeospatialComponent.h # GPS/WGS84 component
│   ├── PredictionComponent.h # Kalman/TFLite prediction
│   ├── TerrainComponent.h    # Terrain/vegetation
│   ├── WorldObjectComponent.h
│   └── TagComponent.h        # Entity tags
│
└── systems/                  # System implementations
    ├── Systems.h             # All systems include
    ├── RenderSystem.h        # Rendering system
    ├── ModelRenderSystem.h   # Model/mesh rendering
    ├── PhysicsSystem.h       # Physics simulation
    ├── AnimationSystem.h     # Animation updates
    ├── CharacterControllerSystem.h
    ├── MotionMatchingSystem.h
    ├── TerrainSystem.h       # Terrain/vegetation updates
    ├── WorldObjectSystem.h   # World object placement
    ├── GeospatialSystem.h    # Geo orchestrator (legacy API)
    ├── GeoIngestionSystem.h  # Phase 1: GPS input (threaded)
    ├── GeoStorageSystem.h    # Phase 2: InfluxDB (threaded)
    ├── GeoPredictionSystem.h # Phase 3: Kalman/ML (threaded)
    ├── GeoVisualizationSystem.h # Phase 4: GL trajectory rendering
    └── GeoTerrainSystem.h    # Phase 5: terrain projection (NEW!)
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
make            # build bin/test_runner + bin/engine (debug)
make test       # run the full unit-test suite (731 tests / 99 suites)
make run        # self-check tests, then boot the engine
make run-headless  # bounded headless engine run (CI-friendly)
make test-list  # list every test
```

Archetypes, blueprints, relationships, events, job system, serialization and
entity-manager operations are covered by `make test` (the
`ArchetypeStorageTest`, `EntityManagerOps`, `EntityManagerSafety`,
`RelationshipTest`, `EventSystemTest`, `BlueprintTest`, `JobSystemTest`,
`SerializationTest`, `CacheLayoutProfileTest` and `UndoRedoTest` suites). Run a
single suite, e.g. `--gtest_filter=EntityManagerOps.*`. See the
[root README](../README.md#test-suites) for the full target list and prerequisites.

## 🐛 Known Issues

The legacy `ComponentManager`/signature storage path is retained for backward
compatibility but **archetype storage is the recommended path** for hot systems
(see the performance table above). Full integration status of the render/editor
viewport wiring is tracked in [Architecture](../docs/ARCHITECTURE.md) and
[Troubleshooting](../docs/TROUBLESHOOTING.md).

---

**Status:** ✅ Production Ready
**Last Updated:** September 2026
