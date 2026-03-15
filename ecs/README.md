# ECS (Entity Component System) Framework

A modern, type-safe ECS implementation for the 3D Game Engine.

## Overview

This ECS framework provides:
- **Entity Management**: Create, destroy, and query entities
- **Component Storage**: Efficient, cache-friendly component arrays
- **System Execution**: Organized update/render loops with filtering
- **Type Safety**: Compile-time component type checking

## Architecture

```
ecs/
├── ECS.h                 # Main include (re-exports all)
├── Entity.h              # Entity ID and types
├── Component.h           # Component array storage
├── ComponentManager.h    # Manages all component types
├── EntityManager.h       # Creates/destroys entities
├── System.h              # Base system class with filtering
├── World.h               # Main ECS container
├── components/           # Built-in components
│   ├── Components.h
│   ├── TransformComponent.h
│   ├── MeshComponent.h
│   ├── CameraComponent.h
│   ├── RigidBodyComponent.h
│   ├── AnimatorComponent.h
│   ├── LightComponent.h
│   └── TagComponent.h
└── systems/              # Built-in systems
    ├── Systems.h
    ├── RenderSystem.h
    ├── PhysicsSystem.h
    └── AnimationSystem.h
```

## Quick Start

### 1. Include the ECS

```cpp
#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include "ecs/systems/Systems.h"
```

### 2. Create and Initialize a World

```cpp
ecs::World world;
world.init();
```

### 3. Add Systems

```cpp
auto& renderSystem = world.addSystem<ecs::RenderSystem>();
auto& physicsSystem = world.addSystem<ecs::PhysicsSystem>();
auto& cameraSystem = world.addSystem<ecs::CameraSystem>();
```

### 4. Create Entities with Components

```cpp
// Create an entity
auto entity = world.createEntity();

// Add components
auto& transform = world.addComponent<ecs::TransformComponent>(
    entity, 
    glm::vec3(0.0f, 0.0f, 0.0f)
);

auto& mesh = world.addComponent<ecs::MeshComponent>(entity);
mesh.meshID = 0;
mesh.color = glm::vec3(1.0f, 0.0f, 0.0f);

auto& rigidbody = world.addComponent<ecs::RigidBodyComponent>(entity);
rigidbody.bodyType = ecs::RigidBodyType::DYNAMIC;
rigidbody.colliderType = ecs::ColliderType::BOX;
rigidbody.initialize();

// Add a name for debugging
world.addComponent<ecs::NameComponent>(entity, "MyEntity");
```

### 5. Update and Render

```cpp
// In your game loop
world.update(deltaTime);
world.render();
```

### 6. Query Entities

```cpp
// Get a system
auto* physicsSystem = world.getSystem<ecs::PhysicsSystem>();

// Get components from an entity
auto* transform = world.getComponent<ecs::TransformComponent>(entity);
auto* mesh = world.getComponent<ecs::MeshComponent>(entity);

// Iterate over entities with specific components
physicsSystem->forEach(
    world.getEntityManager(),
    world.getComponentManager(),
    [](ecs::EntityID id, ecs::TransformComponent& t, ecs::RigidBodyComponent& rb) {
        // Process each matching entity
        t.position += rb.linearVelocity * deltaTime;
    }
);
```

## Components

### TransformComponent
Position, rotation, and scale in 3D space.

```cpp
auto& transform = world.addComponent<ecs::TransformComponent>(entity);
transform.position = glm::vec3(1.0f, 2.0f, 3.0f);
transform.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
transform.scale = glm::vec3(2.0f);

glm::mat4 modelMatrix = transform.getModelMatrix();
glm::vec3 forward = transform.getForward();
```

### MeshComponent
Static mesh rendering.

```cpp
auto& mesh = world.addComponent<ecs::MeshComponent>(entity);
mesh.meshID = modelManager->loadMesh("box.obj");
mesh.visible = true;
mesh.castShadow = true;
mesh.color = glm::vec3(1.0f, 0.5f, 0.0f);
```

### SkinnedMeshComponent
Animated mesh with skeletal skinning.

```cpp
auto& skinnedMesh = world.addComponent<ecs::SkinnedMeshComponent>(entity);
skinnedMesh.meshID = modelManager->loadMesh("character.fbx");
skinnedMesh.skeletonID = skeletonID;
```

### CameraComponent
Camera for viewing the scene.

```cpp
auto& camera = world.addComponent<ecs::CameraComponent>(entity);
camera.fov = 45.0f;
camera.nearPlane = 0.1f;
camera.farPlane = 1000.0f;
camera.isActive = true;

glm::mat4 projection = camera.getProjectionMatrix();
```

### RigidBodyComponent
Physics simulation.

```cpp
auto& rigidbody = world.addComponent<ecs::RigidBodyComponent>(entity);
rigidbody.bodyType = ecs::RigidBodyType::DYNAMIC;
rigidbody.colliderType = ecs::ColliderType::SPHERE;
rigidbody.sphereRadius = 0.5f;
rigidbody.mass = 1.0f;
rigidbody.restitution = 0.5f;
rigidbody.initialize();

rigidbody.applyForce(glm::vec3(0.0f, 10.0f, 0.0f));
rigidbody.applyImpulse(glm::vec3(1.0f, 0.0f, 0.0f));
```

### CharacterControllerComponent
Player character movement.

```cpp
auto& controller = world.addComponent<ecs::CharacterControllerComponent>(entity);
controller.height = 1.8f;
controller.radius = 0.4f;
controller.moveSpeed = 5.0f;
controller.jumpForce = 5.0f;
```

### AnimatorComponent
Animation playback control.

```cpp
auto& animator = world.addComponent<ecs::AnimatorComponent>(entity);
animator.play(0, true);  // Play animation 0, loop
animator.playbackSpeed = 1.0f;
animator.useRootMotion = true;
```

### SkeletonComponent
Bone hierarchy for animation.

```cpp
auto& skeleton = world.addComponent<ecs::SkeletonComponent>(entity);
// Bones are populated by animation loader
int boneIndex = skeleton.findBoneIndex("Hips");
```

### LightComponent
Light sources.

```cpp
auto& light = world.addComponent<ecs::LightComponent>(entity);
light.setPoint(glm::vec3(1.0f, 0.8f, 0.5f), 2.0f, 10.0f);
light.castShadows = true;
```

### Tag Components
Entity labeling and hierarchy.

```cpp
world.addComponent<ecs::NameComponent>(entity, "Player");
world.addComponent<ecs::TagComponent>(entity, "Enemy");

auto& tags = world.addComponent<ecs::TagsComponent>(entity);
tags.addTag("Interactive");
tags.addTag("Pickup");
```

## Systems

### RenderSystem
Renders all entities with `TransformComponent` + `MeshComponent`.

```cpp
auto& renderSystem = world.addSystem<ecs::RenderSystem>();
renderSystem.setModelManager(modelManager);
renderSystem.setTerrain(terrain);
```

### PhysicsSystem
Simulates rigid body physics.

```cpp
auto& physicsSystem = world.addSystem<ecs::PhysicsSystem>();
physicsSystem.setPhysicsWorld(physicsWorld);
physicsSystem.setGravity(glm::vec3(0.0f, -9.81f, 0.0f));

// Apply forces
physicsSystem->applyForce(entity, glm::vec3(0.0f, 10.0f, 0.0f));
physicsSystem->applyImpulse(entity, glm::vec3(1.0f, 0.0f, 0.0f));
```

### CameraSystem
Manages the active camera.

```cpp
auto& cameraSystem = world.addSystem<ecs::CameraSystem>();

glm::mat4 view = cameraSystem->getViewMatrix();
glm::mat4 projection = cameraSystem->getProjectionMatrix();
glm::vec3 camPos = cameraSystem->getCameraPosition();
```

### AnimationSystem
Updates animation state.

```cpp
auto& animationSystem = world.addSystem<ecs::AnimationSystem>();
animationSystem.setAnimator(animator);
animationSystem.setHybrid(hybridMMFSM);
```

## System Filters

Control which entities a system processes:

```cpp
// Require specific components
m_filter = SystemFilter::require<TransformComponent, MeshComponent>();

// Exclude entities with components
m_filter = SystemFilter::exclude<CameraComponent>();

// Require any of these components
m_filter = SystemFilter::anyOf<RigidBodyComponent, CharacterControllerComponent>();

// Combine filters
m_filter = SystemFilter::require<TransformComponent>()
    .withExcluded(SystemFilter::exclude<CameraComponent>())
    .withAny(SystemFilter::anyOf<MeshComponent, SkinnedMeshComponent>());
```

## Advanced Usage

### Custom Components

```cpp
struct HealthComponent : public ecs::Component {
    int current = 100;
    int max = 100;
    
    void takeDamage(int amount) {
        current = std::max(0, current - amount);
    }
};

struct DamageSystem : public ecs::TypedSystem<HealthComponent> {
    void update(float deltaTime) override {
        forEach(*m_entityManager, *m_componentManager,
            [](ecs::EntityID id, HealthComponent& health) {
                // Apply damage over time
                health.takeDamage(1);
            });
    }
    
    const char* getName() const override { return "DamageSystem"; }
};

// Usage
auto& damageSystem = world.addSystem<DamageSystem>();
```

### Custom Systems

```cpp
class MyCustomSystem : public ecs::System {
public:
    void init() override {
        m_filter = ecs::SystemFilter::require<ecs::TransformComponent>();
    }
    
    void update(float deltaTime) override {
        // Custom update logic
    }
    
    void render() override {
        // Custom render logic
    }
    
    const char* getName() const override { return "MyCustomSystem"; }
};
```

### Entity Queries

```cpp
// Find all entities with specific components
world.forEach<ecs::TransformComponent, ecs::MeshComponent>(
    [&viewMatrix](ecs::EntityID id, 
                  ecs::TransformComponent& t, 
                  ecs::MeshComponent& m) {
        glm::mat4 model = t.getModelMatrix();
        glm::mat4 mvp = viewMatrix * model;
        // Render...
    }
);
```

### System Priority

```cpp
// Lower priority runs first
physicsSystem.setPriority(-100);
animationSystem.setPriority(-50);
renderSystem.setPriority(0);
```

### Enable/Disable Systems

```cpp
renderSystem.setEnabled(false);  // Skip rendering
physicsSystem.enable();          // Resume physics
```

## Best Practices

1. **Component Design**: Keep components as data-only (no logic)
2. **System Design**: Systems contain logic, components contain data
3. **Entity Creation**: Batch create entities at startup when possible
4. **Component Access**: Cache component pointers when iterating
5. **System Order**: Use priorities to control update order
6. **Memory**: Components are stored in contiguous arrays (cache-friendly)

## Performance Tips

1. Use `TypedSystem` for type-safe, efficient iteration
2. Minimize component additions/removals during gameplay
3. Use system priorities to optimize cache usage
4. Batch similar operations in systems
5. Use spatial partitioning for large worlds

## Migration from Old Entity System

The old `Entity` class in `components/Entity.h` used a map-based approach:

```cpp
// Old way (still works, but not recommended)
Entity entity;
entity.addComponent<Transform>();
Transform* t = entity.getComponent<Transform>();
```

The new ECS uses a centralized world:

```cpp
// New way (recommended)
auto entity = world.createEntity();
auto& transform = world.addComponent<Transform>(entity);
Transform* t = world.getComponent<Transform>(entity);
```

## Testing

Build and run the ECS test application:

```bash
make ecs_test
./bin/ecs_test
```

## TODO

- [ ] Archetype-based storage for better cache coherency
- [ ] Multi-threaded system execution
- [ ] Entity relationship queries (parent/children)
- [ ] Event system for component changes
- [ ] Serialization/deserialization
- [ ] Blueprint/prefab system
