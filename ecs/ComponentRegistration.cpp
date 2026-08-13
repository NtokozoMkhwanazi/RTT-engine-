// ============================================================================
// Component Registration
//
// The archetype storage system needs the sizeof() of every component type to
// lay out its chunks (ECS_REGISTER_COMPONENT). Without registration it falls
// back to a 128-byte slot, which corrupts larger components and poisons the
// archetype iterators. Registering all core components here (at static-init
// time, before any World is constructed) makes archetype storage safe.
// ============================================================================

#include "Archetype.h"
#include "components/Components.h"
#include "components/AnimatorComponent.h"
#include "components/CameraComponent.h"
#include "components/GeospatialComponent.h"
#include "components/LightComponent.h"
#include "components/MeshComponent.h"
#include "components/ModelComponent.h"
#include "components/MotionMatchingComponent.h"
#include "components/PredictionComponent.h"
#include "components/RigidBodyComponent.h"
#include "components/TagComponent.h"
#include "components/TerrainComponent.h"
#include "components/TransformComponent.h"
#include "components/WorldObjectComponent.h"

namespace {
// One TU-local static registrar per component type. Registration is idempotent
// (ComponentSizeRegistry::registerType early-outs when already registered).
ECS_REGISTER_COMPONENT(ecs::TransformComponent);
ECS_REGISTER_COMPONENT(ecs::MeshComponent);
ECS_REGISTER_COMPONENT(ecs::SkinnedMeshComponent);
ECS_REGISTER_COMPONENT(ecs::InstanceDataComponent);
ECS_REGISTER_COMPONENT(ecs::LODComponent);
ECS_REGISTER_COMPONENT(ecs::ModelComponent);
ECS_REGISTER_COMPONENT(ecs::CameraComponent);
ECS_REGISTER_COMPONENT(ecs::CameraControllerComponent);
ECS_REGISTER_COMPONENT(ecs::LightComponent);
ECS_REGISTER_COMPONENT(ecs::AmbientLightComponent);
ECS_REGISTER_COMPONENT(ecs::RigidBodyComponent);
ECS_REGISTER_COMPONENT(ecs::AnimatorComponent);
ECS_REGISTER_COMPONENT(ecs::NameComponent);
ECS_REGISTER_COMPONENT(ecs::TagComponent);
ECS_REGISTER_COMPONENT(ecs::TagsComponent);
ECS_REGISTER_COMPONENT(ecs::ParentComponent);
ECS_REGISTER_COMPONENT(ecs::ChildrenComponent);
ECS_REGISTER_COMPONENT(ecs::LifetimeComponent);
ECS_REGISTER_COMPONENT(ecs::ActiveComponent);
ECS_REGISTER_COMPONENT(ecs::TerrainComponent);
ECS_REGISTER_COMPONENT(ecs::TerrainChunkComponent);
ECS_REGISTER_COMPONENT(ecs::WorldObjectComponent);
ECS_REGISTER_COMPONENT(ecs::MotionMatchingComponent);
ECS_REGISTER_COMPONENT(ecs::GeospatialComponent);
ECS_REGISTER_COMPONENT(ecs::PredictionComponent);
} // namespace
