#pragma once

/**
 * Shared ECS<->engine animation bridge helpers.
 *
 * Factored out so multiple ECS systems (AnimationSystem, MotionMatchingSystem)
 * can build engine boneSystem::Skeleton objects from an ECS SkeletonComponent
 * without each re-implementing the bone-layout conversion. This is the same
 * conversion AnimationSystem performs in its private createEngineSkeleton();
 * it is lifted here as a free function so MotionMatchingSystem can reuse it
 * when booting a motionMatching::MotionMatcher per entity.
 *
 * (AnimationSystem is intentionally left unchanged: it keeps its own private
 * copy. A future cleanup can point it at this helper too.)
 */

#include "../components/Components.h"   // SkeletonComponent
#include "../../boneSystem/Skeleton.h"  // engine ::Skeleton
#include <glm/glm.hpp>
#include <memory>

namespace ecs {

/**
 * Build an engine boneSystem::Skeleton from an ECS SkeletonComponent.
 *
 * Mirrors AnimationSystem::createEngineSkeleton: copies root bone index + bone
 * count and, per bone, sets id / bindTransform from the ECS inverse-bind
 * matrix and derives offset = inverse(bind).
 */
inline std::unique_ptr<Skeleton> makeEngineSkeleton(const SkeletonComponent& skeleton) {
    auto engineSkeleton = std::make_unique<Skeleton>();

    engineSkeleton->bones.resize(skeleton.bones.size());
    engineSkeleton->rootBoneIndex = skeleton.rootBoneIndex;

    for (size_t i = 0; i < skeleton.bones.size(); i++) {
        // bones[i] returns a BoneRef proxy by value; bind it via auto&& so the
        // proxy (and its embedded references) live for the body of the loop.
        auto&& ecsBone = skeleton.bones[i];
        auto& engineBone = engineSkeleton->bones[i];

        engineBone.id = static_cast<int>(i);
        engineBone.bindTransform = ecsBone.inverseBindMatrix;
        engineBone.offset = glm::inverse(ecsBone.inverseBindMatrix);
    }

    return engineSkeleton;
}

} // namespace ecs
