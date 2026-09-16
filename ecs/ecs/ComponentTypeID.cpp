#include "ecs/Entity.h"
#include "ecs/components/Components.h"
#include "ecs/components/GeospatialComponent.h"
#include "ecs/components/PredictionComponent.h"
#include <unordered_map>
#include <mutex>
#include <typeinfo>

namespace ecs {

struct TypeIDRegistry {
    ComponentTypeID nextID = 0;
    std::unordered_map<size_t, ComponentTypeID> typeToID;
    std::mutex mutex;

    static TypeIDRegistry& instance() {
        static TypeIDRegistry reg;
        return reg;
    }

    ComponentTypeID getOrAssign(size_t hash) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = typeToID.find(hash);
        if (it == typeToID.end()) {
            ComponentTypeID id = nextID++;
            typeToID[hash] = id;
            return id;
        }
        return it->second;
    }
};

template<typename T>
ComponentTypeID getComponentTypeID() {
    return TypeIDRegistry::instance().getOrAssign(typeid(T).hash_code());
}

template<typename T>
ComponentTypeID registerComponentTypeID() {
    return TypeIDRegistry::instance().getOrAssign(typeid(T).hash_code());
}

// Explicit template instantiations for all component types
#define INSTANTIATE_TYPE(T) \
    template ComponentTypeID getComponentTypeID<T>(); \
    template ComponentTypeID registerComponentTypeID<T>();

INSTANTIATE_TYPE(TransformComponent)
INSTANTIATE_TYPE(MeshComponent)
INSTANTIATE_TYPE(ModelComponent)
INSTANTIATE_TYPE(CameraComponent)
INSTANTIATE_TYPE(CameraControllerComponent)
INSTANTIATE_TYPE(LightComponent)
INSTANTIATE_TYPE(AmbientLightComponent)
INSTANTIATE_TYPE(SkeletonComponent)
INSTANTIATE_TYPE(AnimatorComponent)
INSTANTIATE_TYPE(AnimationStateComponent)
INSTANTIATE_TYPE(ModelAnimatorComponent)
INSTANTIATE_TYPE(NameComponent)
INSTANTIATE_TYPE(TagComponent)
INSTANTIATE_TYPE(TagsComponent)
INSTANTIATE_TYPE(ParentComponent)
INSTANTIATE_TYPE(ChildrenComponent)
INSTANTIATE_TYPE(LifetimeComponent)
INSTANTIATE_TYPE(ActiveComponent)
INSTANTIATE_TYPE(RigidBodyComponent)
INSTANTIATE_TYPE(CharacterControllerComponent)
INSTANTIATE_TYPE(TerrainChunkComponent)
INSTANTIATE_TYPE(TerrainComponent)
INSTANTIATE_TYPE(WorldObjectComponent)
INSTANTIATE_TYPE(VegetationComponent)
INSTANTIATE_TYPE(GrassComponent)
INSTANTIATE_TYPE(SkinnedMeshComponent)
INSTANTIATE_TYPE(InstanceDataComponent)
INSTANTIATE_TYPE(LODComponent)
INSTANTIATE_TYPE(MotionMatchingComponent)
INSTANTIATE_TYPE(MotionDatabaseComponent)
INSTANTIATE_TYPE(GeospatialComponent)
INSTANTIATE_TYPE(PredictionComponent)

} // namespace ecs
