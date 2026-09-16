#pragma once

#include "../ECS.h"
#include "../DynamicPool.h"
#include "../../modelSystem/ModelHandle.h"
#include "../../animationSystem/Animator.h"
#include "AnimatorComponent.h"
#include <string>
#include <memory>
#include <iostream>

namespace ecs {

constexpr size_t MAX_MODEL_PATH_LENGTH = 256;

struct ModelComponent : public Component {
    ModelSystem::ModelHandle modelHandle;
    bool visible = true;
    bool castShadow = true;
    bool receiveShadow = true;

    bool useMaterialOverrides = false;
    glm::vec3 albedoOverride{1.0f};
    float metallicOverride = 0.0f;
    float roughnessOverride = 0.5f;

    bool useLOD = true;
    float lodBias = 1.0f;

    char modelPath[MAX_MODEL_PATH_LENGTH];

    ModelComponent() { modelPath[0] = '\0'; }
    
    void setModelPath(const std::string& path) {
        std::strncpy(modelPath, path.c_str(), MAX_MODEL_PATH_LENGTH - 1);
        modelPath[MAX_MODEL_PATH_LENGTH - 1] = '\0';
    }
    
    const char* getModelPath() const { return modelPath; }
    bool hasModelPath() const { return modelPath[0] != '\0'; }

    bool isValid() const { 
        return modelHandle.isValid() && ModelSystem::ModelRegistry::getInstance().get(modelHandle) != nullptr;
    }
    
    Model* getModel() const {
        return isValid() ? ModelSystem::ModelRegistry::getInstance().get(modelHandle) : nullptr;
    }

    size_t getMeshCount() const {
        Model* m = getModel();
        return m ? m->GetMeshCount() : 0;
    }

    BoundingBox getBoundingBox() const {
        Model* m = getModel();
        return m ? m->GetBoundingBox() : BoundingBox{};
    }
};

struct ModelAnimatorComponent : public Component {
    std::unique_ptr<Animator> animator;
    SkeletonComponent skeleton;
    bool animated = false;

    int activeAnimation = 0;
    bool isPlaying = true;
    float playbackSpeed = 1.0f;
    bool loop = true;

    int nextAnimation = -1;
    float blendDuration = 0.3f;
    float blendProgress = 0.0f;

    float currentTime = 0.0f;

    ModelAnimatorComponent() = default;
    
    void setPool(DynamicPool* pool) { skeleton.setPool(pool); }

    void initialize(const Skeleton* skel) {
        if (skel && !skel->bones.empty()) {
            animator = std::make_unique<Animator>(skel);
            skeleton.bones.resize(skel->bones.size());
            for (size_t i = 0; i < skel->bones.size(); ++i) {
                skeleton.bones[i].parentIndex = -1;
                skeleton.bones[i].inverseBindMatrix = skel->bones[i].offset;
            }
            skeleton.rootBoneIndex = skel->rootBoneIndex;
            animated = true;
        }
    }

    bool isValid() const { return animator != nullptr && animated; }

    void playAnimation(int animIndex, bool loopAnim = true) {
        if (!isValid()) return;
        
        activeAnimation = animIndex;
        isPlaying = true;
        loop = loopAnim;
    }

    void stopAnimation() {
        isPlaying = false;
    }

    Animator* getAnimator() { return animator.get(); }
    const Animator* getAnimator() const { return animator.get(); }
};

} // namespace ecs
