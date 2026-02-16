#pragma once
#include <assimp/scene.h>
#include "Animation.h"

class AssimpAnimationLoader {
public:
    static Animation LoadAnimation(const aiScene* scene, const aiAnimation* aiAnim);
    
    // Load animation with pose correction for Mixamo rigs
    static Animation LoadAnimationWithPoseCorrection(const aiScene* scene, const aiAnimation* aiAnim);
    
    // Detect if animation is in T-pose
    static bool IsTPoseAnimation(const aiScene* scene, const aiAnimation* aiAnim);
    
    // Apply corrective transforms to fix common pose issues
    static Animation ApplyPoseCorrections(const Animation& anim);
};

