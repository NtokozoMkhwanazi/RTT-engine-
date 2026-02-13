#pragma once
#include <assimp/scene.h>
#include "Animation.h"

class AssimpAnimationLoader {
public:
    static Animation LoadAnimation(const aiScene* scene, const aiAnimation* aiAnim);
};

