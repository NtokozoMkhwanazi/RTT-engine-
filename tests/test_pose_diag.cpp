/**
 * Pose-space regression test for the play character:
 *
 * 1. The ASYNC skeleton path (Model::LoadModelData, used by AnimatedCharacter's
 *    animator) must produce the SAME animated pose as the SYNC skeleton path
 *    (Model ctor, used by the render meshes). They diverge when the async
 *    hierarchy keeps raw $AssimpFbx$ helper nodes - every animated bone
 *    position ends up ~2x too large and the play character's legs/hands are
 *    visibly deformed.
 * 2. The bone-ID ORDER must be identical between the two skeletons: the mesh's
 *    vertex BoneIDs (sync order) index into the animator's matrix array (async
 *    order). A different ordering skin-deforms the mesh even when both
 *    skeletons are individually correct.
 */
#include <gtest/gtest.h>
#include <iostream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../modelSystem/Model.h"
#include "../animationSystem/Animation.h"
#include "../animationSystem/Animator.h"
#include "../animationSystem/AssimpAnimationLoader.h"
#include "../boneSystem/Skeleton.h"
#include "../boneSystem/BoneName.h"

#include <assimp/postprocess.h>

namespace {

Animation* loadClip(const std::string& path) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(
        path, aiProcess_Triangulate | aiProcess_GenUVCoords | aiProcess_FlipUVs);
    if (!scene || !scene->HasAnimations() || scene->mNumAnimations == 0) return nullptr;
    Animation raw("", 0.0f, 0.0f);
    try {
        raw = AssimpAnimationLoader::LoadAnimation(scene, scene->mAnimations[0]);
    } catch (...) { return nullptr; }
    const float tps = raw.GetTicksPerSecond();
    if (tps > 0.0f) {
        raw.duration = raw.duration / tps;
        for (auto& kv : raw.boneAnimations) {
            for (auto& t : kv.second.positionTimes) t /= tps;
            for (auto& t : kv.second.rotationTimes) t /= tps;
            for (auto& t : kv.second.scaleTimes) t /= tps;
        }
    }
    return new Animation(std::move(raw));
}

void collectPose(const Skeleton& skel, Animation* clip,
                 std::map<std::string, glm::vec3>& out) {
    Animator anim(&skel);
    anim.Play(clip);
    for (int i = 0; i < 3; ++i) anim.Update(1.0f / 60.0f);
    const char* names[] = {"hips", "head", "leftfoot", "rightfoot",
                           "lefthand", "righthand", "leftleg", "rightleg"};
    for (const char* n : names) {
        int idx = skel.GetBoneIndex(n);
        if (idx < 0) continue;
        out[n] = anim.GetBoneWorldPosition(idx, glm::mat4(1.0f));
    }
}

}  // namespace

TEST(PoseDiag, AsyncSkeletonMatchesSync) {
    // Sync skeleton (Model ctor - same path as the render meshes).
    Model syncModel("assets/bot.fbx");
    const Skeleton& syncSkel = syncModel.GetSkeleton();

    // Async skeleton (LoadModelData - same path as AnimatedCharacter::load).
    auto data = Model::LoadModelData("assets/bot.fbx");
    ASSERT_TRUE(data && data->success);
    Skeleton asyncSkel = data->skeleton;
    // Replicate AnimatedCharacter's normalization.
    std::map<std::string, int> normMapping;
    for (const auto& kv : asyncSkel.boneMapping) {
        normMapping[NormalizeBoneName(kv.first)] = kv.second;
    }
    asyncSkel.boneMapping.swap(normMapping);
    if (asyncSkel.rootBoneIndex < 0) {
        auto it = asyncSkel.boneMapping.find("hips");
        if (it != asyncSkel.boneMapping.end()) asyncSkel.rootBoneIndex = it->second;
    }
    asyncSkel.rootNode = data->rootNode;

    ASSERT_EQ(syncSkel.bones.size(), asyncSkel.bones.size());

    // Bone-ID order must match: the mesh vertex BoneIDs (sync order) index
    // into the animator matrix array (async order).
    int idOrderMismatch = 0;
    for (size_t i = 0; i < syncSkel.bones.size(); ++i) {
        const glm::mat4& so = syncSkel.bones[i].offset;
        const glm::mat4& ao = asyncSkel.bones[i].offset;
        float d = 0.0f;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                d += std::abs(so[c][r] - ao[c][r]);
        if (d > 0.01f) {
            idOrderMismatch++;
            if (idOrderMismatch <= 5) {
                std::cout << "  bone[" << i << "] offset mismatch d=" << d << "\n";
            }
        }
    }
    std::cout << "  bone-ID offset mismatches: " << idOrderMismatch << " / "
              << syncSkel.bones.size() << "\n";
    EXPECT_EQ(idOrderMismatch, 0) << "bone ID order diverges between sync and "
                                     "async skeletons - mesh skinning would "
                                     "index the wrong matrices";

    std::unique_ptr<Animation> idle(loadClip("assets/Idle.fbx"));
    ASSERT_TRUE(idle != nullptr);

    std::map<std::string, glm::vec3> syncPose, asyncPose;
    collectPose(syncSkel, idle.get(), syncPose);
    collectPose(asyncSkel, idle.get(), asyncPose);

    for (const auto& [name, sp] : syncPose) {
        auto it = asyncPose.find(name);
        ASSERT_NE(it, asyncPose.end()) << "async pose missing bone " << name;
        const glm::vec3& ap = it->second;
        const float dist = glm::length(sp - ap);
        std::cout << "  " << name << ": sync=(" << (int)sp.x << "," << (int)sp.y
                  << "," << (int)sp.z << ") async=(" << (int)ap.x << ","
                  << (int)ap.y << "," << (int)ap.z << ") dist=" << dist << "\n";
        EXPECT_LT(dist, 1.0f) << "bone " << name << " pose diverges (async skeleton "
                                 "hierarchy not collapsed)";
    }
}
