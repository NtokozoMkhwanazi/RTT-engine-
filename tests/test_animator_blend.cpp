/**
 * Animator Crossfade Deformation Regression Tests
 *
 * The multi-layer (crossfade) path in Animator::Update() used to:
 *   1. start from the bind pose and apply sequential mix() calls, so the bind
 *      pose (T-pose) leaked into every crossfade (e.g. a 50/50 Idle<->Walk
 *      blend produced 25% bind + 25% Idle + 50% Walk), and
 *   2. treat the blended LOCAL-space TRS as GLOBAL transforms - the compose
 *      step skipped the parent chain entirely, so every bone was placed
 *      relative to the origin instead of its parent.
 *
 * Both bugs visibly deformed the skeleton while switching animations
 * (Idle<->Walk, Idle<->Jump). These tests drive the real Animator::Update()
 * multi-layer path with a tiny synthetic 2-bone skeleton and assert the blend
 * is a clean weighted average composed through the hierarchy.
 */

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <cmath>
#include <vector>

#include "../animationSystem/Animator.h"
#include "../animationSystem/Animation.h"
#include "../boneSystem/Skeleton.h"

namespace {

// Build a 2-bone skeleton:
//   Root  (index 0): bind local = translate(0,0,2) * rotateZ(60 deg)
//   Child (index 1): bind local = translate(0,10,0)
// The bind pose deliberately differs from both clips so any bind-pose leak
// in the blend is caught. Identity bind offsets keep final == global for
// easy assertions.
Skeleton MakeTestSkeleton() {
    Skeleton skel;
    skel.bones.resize(2);
    skel.bones[0].offset = glm::mat4(1.0f);
    skel.bones[1].offset = glm::mat4(1.0f);
    skel.rootBoneIndex = 0;
    skel.boneMapping["root"] = 0;
    skel.boneMapping["child"] = 1;

    skel.rootNode.name = "Root";
    skel.rootNode.boneIndex = 0;
    skel.rootNode.transform =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 2.0f)) *
        glm::mat4_cast(glm::angleAxis(glm::radians(60.0f), glm::vec3(0.0f, 0.0f, 1.0f)));

    AssimpNodeData child;
    child.name = "Child";
    child.boneIndex = 1;
    child.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 10.0f, 0.0f));
    skel.rootNode.children.push_back(child);
    return skel;
}

BoneAnimation MakeConstantRot(const std::string& bone, const glm::quat& q) {
    BoneAnimation ba;
    ba.boneName = bone;
    ba.rotationTimes = {0.0, 1.0};
    ba.rotationValues = {q, q};
    return ba;
}

BoneAnimation MakeConstantPos(const std::string& bone, const glm::vec3& p) {
    BoneAnimation ba;
    ba.boneName = bone;
    ba.positionTimes = {0.0, 1.0};
    ba.positionValues = {p, p};
    return ba;
}

bool MatrixFinite(const glm::mat4& m) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!std::isfinite(m[c][r])) return false;
    return true;
}

}  // namespace

TEST(AnimatorBlend, Crossfade_NoBindPoseLeak_HierarchyPreserved) {
    Skeleton skel = MakeTestSkeleton();

    // Idle: root identity rotation (bind was rotateZ(60 deg)), root pos (0,0,2).
    // Child not animated -> falls back to bind (0,10,0).
    // NOTE: channel names must be the NORMALIZED form (lowercase) that
    // Animator::EvaluateNodeTRS looks up (GetBoneAnimation does a raw find).
    Animation idle("Idle", 1.0f, 30.0f);
    idle.AddBoneAnimation(MakeConstantPos("root", glm::vec3(0.0f, 0.0f, 2.0f)));
    idle.AddBoneAnimation(MakeConstantRot("root", glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));

    // Walk: root rotateY(90 deg), child local pos (0,5,0).
    Animation walk("Walk", 1.0f, 30.0f);
    walk.AddBoneAnimation(MakeConstantPos("root", glm::vec3(0.0f, 0.0f, 2.0f)));
    walk.AddBoneAnimation(MakeConstantRot(
        "root", glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f))));
    walk.AddBoneAnimation(MakeConstantPos("child", glm::vec3(0.0f, 5.0f, 0.0f)));

    Animator anim(&skel);
    anim.AddAnimationLayer(&idle, 0.5f, 0.3f);
    anim.AddAnimationLayer(&walk, 0.5f, 0.3f);
    anim.Update(0.0f);

    const auto& final = anim.GetFinalBoneMatrices();
    ASSERT_EQ(final.size(), 2u);

    // No NaN/Inf anywhere (a zero-length accumulated quaternion NaNs).
    for (size_t i = 0; i < final.size(); ++i) {
        EXPECT_TRUE(MatrixFinite(final[i])) << "bone " << i << " has non-finite values";
    }

    // Root rotation must be the exact 50/50 average of identity and
    // rotateY(90 deg) -> rotateY(45 deg). If the bind pose (rotateZ(60 deg))
    // leaked into the blend, the resulting axis/angle drifts off Y.
    glm::vec3 fwd = glm::vec3(final[0] * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    EXPECT_NEAR(fwd.x, std::sin(glm::radians(45.0f)), 1e-2f) << "bind pose leaked into root rotation";
    EXPECT_NEAR(fwd.y, 0.0f, 1e-2f);
    EXPECT_NEAR(fwd.z, std::cos(glm::radians(45.0f)), 1e-2f);

    // Root translation stays (0,0,2) (both clips agree with the bind).
    glm::vec3 rootT = glm::vec3(final[0][3]);
    EXPECT_NEAR(rootT.z, 2.0f, 1e-3f);

    // Child local translation is 50/50 of (0,10,0) and (0,5,0) -> (0,7.5,0),
    // then composed through the parent: parent translate (0,0,2) applied with
    // the parent's rotateY(45). Hierarchical result is (0,7.5,2). The old bug
    // treated the local TRS as global -> (0,7.5,0), dropping the parent chain.
    glm::vec3 childT = glm::vec3(final[1][3]);
    EXPECT_NEAR(childT.x, 0.0f, 1e-3f);
    EXPECT_NEAR(childT.y, 7.5f, 1e-3f);
    EXPECT_NEAR(childT.z, 2.0f, 1e-3f)
        << "child lost its parent's transform - local TRS treated as global?";
}

TEST(AnimatorBlend, Crossfade_AsymmetricWeights) {
    Skeleton skel = MakeTestSkeleton();

    Animation idle("Idle", 1.0f, 30.0f);
    idle.AddBoneAnimation(MakeConstantPos("root", glm::vec3(0.0f, 0.0f, 2.0f)));
    idle.AddBoneAnimation(MakeConstantRot("root", glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));

    Animation walk("Walk", 1.0f, 30.0f);
    walk.AddBoneAnimation(MakeConstantPos("root", glm::vec3(0.0f, 0.0f, 2.0f)));
    walk.AddBoneAnimation(MakeConstantRot(
        "root", glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f))));
    walk.AddBoneAnimation(MakeConstantPos("child", glm::vec3(0.0f, 5.0f, 0.0f)));

    Animator anim(&skel);
    anim.AddAnimationLayer(&idle, 0.75f, 0.3f);   // old clip, fading out
    anim.AddAnimationLayer(&walk, 0.25f, 0.3f);   // new clip, fading in
    anim.Update(0.0f);

    const auto& final = anim.GetFinalBoneMatrices();
    ASSERT_EQ(final.size(), 2u);
    for (size_t i = 0; i < final.size(); ++i) {
        EXPECT_TRUE(MatrixFinite(final[i])) << "bone " << i << " has non-finite values";
    }

    // Child local translation: 0.75*(0,10,0) + 0.25*(0,5,0) = (0,8.75,0),
    // composed through the parent -> (0, 8.75, 2).
    glm::vec3 childT = glm::vec3(final[1][3]);
    EXPECT_NEAR(childT.x, 0.0f, 1e-3f);
    EXPECT_NEAR(childT.y, 8.75f, 1e-3f);
    EXPECT_NEAR(childT.z, 2.0f, 1e-3f);

    // Root rotation: nlerp of identity and rotateY(90) at t=0.25 is ~21.6 deg
    // about Y (between 15 and 30 deg). A bind-pose leak would pull it toward
    // rotateZ and away from +X.
    glm::vec3 fwd = glm::vec3(final[0] * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    EXPECT_GT(fwd.x, std::sin(glm::radians(15.0f)));
    EXPECT_LT(fwd.x, std::sin(glm::radians(30.0f)));
    EXPECT_NEAR(fwd.y, 0.0f, 1e-2f);
    EXPECT_GT(fwd.z, 0.0f);
}

TEST(AnimatorBlend, Crossfade_OldLayerActuallyFadesOut) {
    Skeleton skel = MakeTestSkeleton();

    Animation clipA("A", 1.0f, 30.0f);
    clipA.AddBoneAnimation(MakeConstantPos("root", glm::vec3(0.0f, 0.0f, 2.0f)));
    clipA.AddBoneAnimation(MakeConstantRot("root", glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));

    Animation clipB("B", 1.0f, 30.0f);
    clipB.AddBoneAnimation(MakeConstantPos("root", glm::vec3(0.0f, 0.0f, 2.0f)));
    clipB.AddBoneAnimation(MakeConstantRot(
        "root", glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f))));

    Animator anim(&skel);
    anim.AddAnimationLayer(&clipA, 1.0f, 0.3f);

    // Let A complete its blend so blendProgress == 1.0 - exactly the condition
    // that used to wedge old layers at full weight forever (the weight ramp in
    // UpdateAnimationBlending only runs while blendProgress < 1.0, and the old
    // fade-out paths never reset it).
    anim.Update(0.5f);
    ASSERT_EQ(anim.GetActiveAnimationLayerCount(), 1);

    // Crossfade to B.
    anim.BlendToAt(&clipB, 0.0f, 0.3f);
    anim.Update(0.5f);  // completes the crossfade window

    // The old clip must have faded to ~0 weight, NOT stuck at 1.0 (the stuck
    // layer bug stacked every played clip into a permanent equal-weight blend).
    // Fully-faded layers are only erased once the list grows past 4 layers, so
    // assert on the weights rather than the layer count.
    int count = anim.GetActiveAnimationLayerCount();
    float wA = -1.0f, wB = -1.0f;
    for (int i = 0; i < count; ++i) {
        const auto* l = anim.GetActiveLayer(i);
        if (!l) continue;
        if (l->animation == &clipA) wA = l->weight;
        if (l->animation == &clipB) wB = l->weight;
    }
    ASSERT_GE(wA, 0.0f) << "old clip layer not found";
    EXPECT_LT(wA, 0.01f) << "old clip stuck at weight " << wA;
    ASSERT_GE(wB, 0.0f) << "new clip layer not found";
    EXPECT_NEAR(wB, 1.0f, 1e-3f);

    // Visual outcome: with the fade-out broken the pose would be a permanent
    // 50/50 mix (root at 45 deg about Y). With a working crossfade it is pure
    // B (90 deg about Y).
    const auto& final = anim.GetFinalBoneMatrices();
    glm::vec3 fwd = glm::vec3(final[0] * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    EXPECT_NEAR(fwd.x, std::sin(glm::radians(90.0f)), 1e-2f);
    EXPECT_NEAR(fwd.z, std::cos(glm::radians(90.0f)), 1e-2f);
}

TEST(AnimatorBlend, Crossfade_IKOffsetPropagatesToChildren) {
    Skeleton skel = MakeTestSkeleton();

    Animation clipA("A", 1.0f, 30.0f);
    clipA.AddBoneAnimation(MakeConstantPos("root", glm::vec3(0.0f, 0.0f, 2.0f)));
    clipA.AddBoneAnimation(MakeConstantRot("root", glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
    // Child not animated -> bind (0,10,0).
    Animation clipB("B", 1.0f, 30.0f);  // only used to force size > 1

    // Single-layer reference through the EvaluateNode path.
    Animator ref(&skel);
    ref.AddAnimationLayer(&clipA, 1.0f, 0.0f);
    ref.AddIKOffset(0, glm::vec3(0.0f, -1.0f, 0.0f), 1.0f);
    ref.Update(0.0f);
    glm::vec3 refChild = glm::vec3(ref.GetFinalBoneMatrices()[1][3]);

    // Multi-layer (crossfade) path: a zero-weight second layer forces the
    // multi-layer branch. With the same IK offset on the parent bone, the
    // child must land on the SAME position - the parent's post-IK global is
    // what EvaluateNode propagates down the hierarchy.
    Animator multi(&skel);
    multi.AddAnimationLayer(&clipA, 1.0f, 0.0f);
    multi.AddAnimationLayer(&clipB, 0.0f, 0.3f);
    multi.AddIKOffset(0, glm::vec3(0.0f, -1.0f, 0.0f), 1.0f);
    multi.Update(0.0f);
    glm::vec3 multiChild = glm::vec3(multi.GetFinalBoneMatrices()[1][3]);

    EXPECT_NEAR(multiChild.x, refChild.x, 1e-3f);
    EXPECT_NEAR(multiChild.y, refChild.y, 1e-3f);
    EXPECT_NEAR(multiChild.z, refChild.z, 1e-3f);
    // Explicit check: root global = T(0,0,2) * IK T(0,-1,0) = T(0,-1,2), so
    // child = T(0,-1,2) * T(0,10,0) = T(0,9,2). Passing the pre-IK parent
    // would give T(0,10,2) instead.
    EXPECT_NEAR(multiChild.y, 9.0f, 1e-3f);
}
