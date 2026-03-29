# 3D Game Engine - ECS Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           ECS_TEST.CPP (Main)                           │
│  - GLFW/OpenGL Initialization                                           │
│  - Main Game Loop (Update/Render)                                       │
│  - Input Handling                                                       │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                         WORLD (ecs::World)                              │
│  ┌─────────────────────┐  ┌─────────────────────┐                      │
│  │  EntityManager      │  │  ComponentManager   │                      │
│  │  - Create/Destroy   │  │  - Add/Get/Remove   │                      │
│  │  - Entity Signatures│  │  - Component Arrays │                      │
│  └─────────────────────┘  └─────────────────────┘                      │
│                                                                       │
│  Systems (Updated each frame):                                        │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐ ┌─────────────────┐  │
│  │ CameraSystem│ │PhysicsSystem│ │RenderSystem │ │AnimationSystem  │  │
│  │ (Priority:  │ │ (Priority:  │ │ (Priority:  │ │ (Priority: 0)   │  │
│  │  -100)      │ │  -50)       │ │  0)         │ │                 │  │
│  └─────────────┘ └─────────────┘ └─────────────┘ └─────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
        ┌───────────────────────────┼───────────────────────────┐
        │                           │                           │
        ▼                           ▼                           ▼
┌──────────────────┐      ┌──────────────────┐      ┌──────────────────┐
│   COMPONENTS     │      │     SYSTEMS      │      │  ENGINE MODULES  │
│──────────────────│      │──────────────────│      │──────────────────│
│ Transform        │◄────►│ RenderSystem     │─────►│ ModelManager     │
│ Mesh             │      │ SkinnedMeshRender│      │ Terrain          │
│ SkinnedMesh      │      │ CameraSystem     │─────►│ WorldObjectMgr   │
│ Camera           │      │ LightSystem      │      │                  │
│ RigidBody        │◄────►│ PhysicsSystem    │─────►│ (External)       │
│ CharacterCtrl    │      │ CharacterCtrlSys │      │                  │
│ Animator         │◄────►│ AnimationSystem  │─────►│ Animator (legacy)│
│ Skeleton         │      │ AnimationStateSys│      │ HybridMMFSM      │
│ Light            │      │ MotionMatchingSys│      │                  │
│ Name/Tag         │      │ LightSystem      │      │                  │
└──────────────────┘      └──────────────────┘      └──────────────────┘
```

## Data Flow

```
┌──────────┐     ┌──────────┐     ┌──────────┐     ┌──────────┐
│  Input   │────►│  Update  │────►│  Render  │────►│  Display │
│ (GLFW)   │     │(Systems) │     │ (OpenGL) │     │ (Screen) │
└──────────┘     └──────────┘     └──────────┘     └──────────┘
                      │                  │
                      ▼                  ▼
                ┌──────────┐       ┌──────────┐
                │ Physics  │       │  Camera  │
                │  State   │       │  Matrices│
                └──────────┘       └──────────┘
```

## Entity Composition Examples

```
┌─────────────────────────────────────────────────────────────┐
│  PLAYER ENTITY                                              │
│  ┌─────────────┬─────────────┬─────────────┬─────────────┐ │
│  │  Transform  │ Character   │  Animator   │   Name      │ │
│  │  Component  │ Controller  │  Component  │  Component  │ │
│  │             │  Component  │             │             │ │
│  │  - position │  - height   │  - current  │  - "Player" │ │
│  │  - rotation │  - radius   │    anim     │             │ │
│  │  - scale    │  - velocity │  - blending │             │ │
│  └─────────────┴─────────────┴─────────────┴─────────────┘ │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  CAMERA ENTITY                                              │
│  ┌─────────────┬─────────────┬─────────────┐               │
│  │  Transform  │   Camera    │    Name     │               │
│  │  Component  │  Component  │  Component  │               │
│  │             │             │             │               │
│  │  - position │  - fov      │  - "Camera" │               │
│  │  - rotation │  - near/far │             │               │
│  │             │  - active   │             │               │
│  └─────────────┴─────────────┴─────────────┘               │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  PHYSICS BOX ENTITY                                         │
│  ┌─────────────┬─────────────┬─────────────┬─────────────┐ │
│  │  Transform  │  RigidBody  │    Mesh     │    Name     │ │
│  │  Component  │  Component  │  Component  │  Component  │ │
│  │             │             │             │             │ │
│  │  - position │  - mass     │  - meshID   │  - "Box_1"  │ │
│  │  - scale    │  - velocity │  - visible  │             │ │
│  │             │  - collider │  - color    │             │ │
│  └─────────────┴─────────────┴─────────────┴─────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## System Update Order (by Priority)

```
Frame Start
    │
    ├─► [CameraSystem]      (Priority: -100)  ──► Update camera matrices
    │
    ├─► [PhysicsSystem]     (Priority: -50)   ──► Simulate physics
    │
    ├─► [AnimationSystem]   (Priority: -25)   ──► Update animations
    │
    ├─► [RenderSystem]      (Priority: 0)     ──► Draw all meshes
    │
    └─► [LightSystem]       (Priority: 0)     ──► Update lights
    │
Frame End (Swap Buffers)
```

## Component Relationships

```
TransformComponent
    │
    ├─► Used by: ALL systems (position/rotation/scale)
    │
    ▼
MeshComponent ──────► RenderSystem ──────► OpenGL Draw Calls
    │
    ├─► SkinnedMeshComponent ──► AnimationSystem
    │                              │
    │                              ▼
    │                         SkeletonComponent
    │
    ▼
RigidBodyComponent ──► PhysicsSystem ──────► Collision Detection
    │
    ├─► CharacterControllerComponent
    │
    ▼
CameraComponent ────► CameraSystem ───────► View/Projection Matrices
    │
    ▼
LightComponent ────► LightSystem ─────────► Shader Uniforms
```

## File Structure

```
3D GAME ENGINE/
├── ecs_test.cpp              # Main application (ECS-based)
├── ecs/                      # ECS Framework
│   ├── ECS.h                 # Main include
│   ├── Entity.h
│   ├── Component.h
│   ├── EntityManager.h
│   ├── ComponentManager.h
│   ├── System.h
│   ├── World.h
│   ├── README.md
│   ├── components/           # Component definitions
│   │   ├── TransformComponent.h
│   │   ├── MeshComponent.h
│   │   ├── CameraComponent.h
│   │   ├── RigidBodyComponent.h
│   │   ├── AnimatorComponent.h
│   │   ├── LightComponent.h
│   │   └── ...
│   └── systems/              # System implementations
│       ├── RenderSystem.h
│       ├── PhysicsSystem.h
│       ├── AnimationSystem.h
│       └── ...
├── animationSystem/          # Legacy animation (integrated)
├── physicsSystem/            # Legacy physics (integrated)
├── world/                    # World/Terrain system
├── modelSystem/              # Model loading
├── cameraSystem/             # Camera implementation
├── renderer/                 # OpenGL renderer
├── shaderSystem/             # Shaders
├── lighting/                 # Lighting
├── memory/                   # Memory management
├── motionMatching/           # Motion matching
├── playerSystem/             # Player controller
├── boneSystem/               # Bone/skeleton
├── meshSystem/               # Mesh handling
├── demo/                     # Demo recording
├── utils/                    # Utilities
├── tests/                    # Unit tests
├── Makefile
└── legacy/                   # Old files (backup)
    ├── ecs_old/
    ├── old_tests/
    └── old_implementations/
```
