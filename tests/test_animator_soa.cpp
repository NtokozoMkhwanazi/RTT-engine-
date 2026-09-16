/**
 * Bones per-chunk SoA batch (C, item 3) - GL-free validation.
 *
 * The per-animator bone matrices are already a contiguous std::vector<glm::mat4>
 * slab (Animator::finalBoneMatrices). The bones-SoA win is collapsing N animators'
 * slabs into one staging buffer + one buffer update per frame (ComputeBatch +
 * UpdateBatched in BoneMatrixBuffer). This test pins the GL-free flattening logic
 * headless - it is what the batched skinning path in ModelRenderSystem would feed
 * to BoneMatrixBuffer::UpdateBatched every frame.
 */
#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

#include "../animationSystem/BoneMatrixBuffer.h"
#include "../ecs/BoneSoA.h"
#include "../ecs/components/AnimatorComponent.h"

// ============================================================================
// BoneSoA correctness tests (L1075, GL-free).
//
// Validates that the member-split SoA storage (ecs::BoneSoA, which
// SkeletonComponent.bones now uses) round-trips with the legacy AoS ecs::Bone,
// that the per-element BoneRef proxy forwards reads/writes into the split
// streams, and that the bulk accessors (worldMatrices()/parentIndices()...) the
// batched skinning upload relies on match per-element access.
// ============================================================================

namespace {
glm::mat4 mat4At(int seed) {
    glm::mat4 m(1.0f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) m[c][r] = float(seed * 10 + c * 4 + r);
    return m;
}
} // namespace

TEST(BoneSoaTest, ResizeAndDefaults) {
    ecs::BoneSoA soa;
    EXPECT_EQ(soa.size(), 0u);
    EXPECT_TRUE(soa.empty());

    soa.resize(8);
    EXPECT_EQ(soa.size(), 8u);
    EXPECT_FALSE(soa.empty());

    // Defaults: parent -1, identity matrices, empty name.
    for (size_t i = 0; i < soa.size(); ++i) {
        EXPECT_EQ(soa[i].parentIndex, -1);
        EXPECT_EQ(soa[i].worldTransform, glm::mat4(1.0f));
        EXPECT_EQ(soa[i].inverseBindMatrix, glm::mat4(1.0f));
        EXPECT_EQ(soa[i].localTransform, glm::mat4(1.0f));
        EXPECT_STREQ(soa[i].name, "");
    }

    // Const access path (findBoneIndex is const in SkeletonComponent).
    const ecs::BoneSoA& csoa = soa;
    for (size_t i = 0; i < csoa.size(); ++i) {
        EXPECT_EQ(csoa[i].parentIndex, -1);
        EXPECT_STREQ(csoa[i].name, "");
    }
}

TEST(BoneSoaTest, ProxyReadWriteRoundTrip) {
    ecs::BoneSoA soa;
    soa.resize(3);

    soa[0].parentIndex      = -1;
    soa[0].inverseBindMatrix = mat4At(1);
    soa[0].localTransform    = mat4At(2);
    soa[0].worldTransform    = mat4At(3);
    std::snprintf(soa[0].name, ecs::BoneSoA::BONE_NAME_LEN, "root");

    soa[1].parentIndex      = 0;
    soa[1].inverseBindMatrix = mat4At(4);
    soa[1].localTransform    = mat4At(5);
    soa[1].worldTransform    = mat4At(6);
    std::snprintf(soa[1].name, ecs::BoneSoA::BONE_NAME_LEN, "child");

    // Read back via the mutable proxy.
    EXPECT_EQ(soa[0].parentIndex, -1);
    EXPECT_EQ(soa[0].inverseBindMatrix, mat4At(1));
    EXPECT_EQ(soa[0].localTransform, mat4At(2));
    EXPECT_EQ(soa[0].worldTransform, mat4At(3));
    EXPECT_STREQ(soa[0].name, "root");
    EXPECT_EQ(soa[1].parentIndex, 0);
    EXPECT_STREQ(soa[1].name, "child");

    // findBoneIndex mirrors SkeletonComponent::findBoneIndex (const path).
    EXPECT_EQ(soa.findBoneIndex("root"), 0);
    EXPECT_EQ(soa.findBoneIndex("child"), 1);
    EXPECT_EQ(soa.findBoneIndex("missing"), -1);
    const ecs::BoneSoA& csoa = soa;
    EXPECT_EQ(csoa.findBoneIndex("child"), 1);
}

TEST(BoneSoaTest, BulkAccessorsMatchProxy) {
    // The skinning upload walks worldMatrices()/parentIndices(); these must be
    // the exact contiguous streams the per-element proxy reads/writes.
    constexpr size_t N = 128;
    ecs::BoneSoA soa;
    soa.resize(N);
    for (size_t i = 0; i < N; ++i) {
        soa[i].parentIndex      = int(i - 1);
        soa[i].inverseBindMatrix = mat4At((int)i);
        soa[i].localTransform    = mat4At((int)i + 1000);
        soa[i].worldTransform    = mat4At((int)i + 2000);
    }

    const glm::mat4*   cW = soa.worldMatrices();
    const glm::mat4*   cI = soa.inverseBindMatrices();
    const glm::mat4*   cL = soa.localMatrices();
    const int*         cP = soa.parentIndices();
    // Non-const views for address-comparison assertions below.
    glm::mat4*         wBulk = soa.worldMatrices();
    glm::mat4*         iBulk = soa.inverseBindMatrices();
    glm::mat4*         lBulk = soa.localMatrices();
    int*               pBulk = soa.parentIndices();
    ASSERT_NE(wBulk, nullptr);
    ASSERT_NE(iBulk, nullptr);
    ASSERT_NE(lBulk, nullptr);
    ASSERT_NE(pBulk, nullptr);
    (void)cW; (void)cI; (void)cL; (void)cP;

    for (size_t i = 0; i < N; ++i) {
        EXPECT_EQ(pBulk[i], int(i - 1));
        EXPECT_EQ(iBulk[i], mat4At((int)i));
        EXPECT_EQ(lBulk[i], mat4At((int)i + 1000));
        EXPECT_EQ(wBulk[i], mat4At((int)i + 2000));
        // Bulk stream element address must equal the proxy's backing store.
        EXPECT_EQ(&(soa[i].worldTransform), &wBulk[i]);
        EXPECT_EQ(&(soa[i].inverseBindMatrix), &iBulk[i]);
        EXPECT_EQ(&(soa[i].localTransform), &lBulk[i]);
        EXPECT_EQ(&soa[i].parentIndex, &pBulk[i]);
    }
}

TEST(BoneSoaTest, InteroperateWithLegacyBoneAoS) {
    // Build a legacy AoS skeleton, push into BoneSoA, and confirm the split
    // streams carry identical values (the integration contract with
    // AnimationSystem/ModelComponent which still read ecs::Bone-shaped fields).
    std::vector<ecs::Bone> aos;
    aos.push_back(ecs::Bone("root", -1));
    aos.push_back(ecs::Bone("child", 0));
    for (size_t i = 0; i < aos.size(); ++i) {
        aos[i].inverseBindMatrix = mat4At((int)i);
        aos[i].localTransform    = mat4At((int)i + 7);
        aos[i].worldTransform    = mat4At((int)i + 70);
    }

    ecs::BoneSoA soa;
    for (size_t i = 0; i < aos.size(); ++i) {
        soa.push_back(aos[i].getName(), aos[i].parentIndex,
                      aos[i].inverseBindMatrix, aos[i].localTransform,
                      aos[i].worldTransform);
    }
    EXPECT_EQ(soa.size(), aos.size());

    for (size_t i = 0; i < aos.size(); ++i) {
        EXPECT_EQ(soa[i].parentIndex,      aos[i].parentIndex);
        EXPECT_EQ(soa[i].inverseBindMatrix, aos[i].inverseBindMatrix);
        EXPECT_EQ(soa[i].localTransform,    aos[i].localTransform);
        EXPECT_EQ(soa[i].worldTransform,    aos[i].worldTransform);
        EXPECT_STREQ(soa[i].name,           aos[i].getName());
    }
    EXPECT_EQ(soa.findBoneIndex("child"), 1);
    EXPECT_EQ(ecs::BoneSoA::BONE_NAME_LEN, ecs::MAX_BONE_NAME_LENGTH);
}


TEST(AnimatorSoaBatch, ComputeBatch_EmptyBatchesYieldsEmpty) {
    std::vector<const std::vector<glm::mat4>*> batches;
    EXPECT_TRUE(BoneMatrixBuffer::ComputeBatch(batches).empty());
}

TEST(AnimatorSoaBatch, ComputeBatch_PreservesOrderAndCount) {
    const glm::mat4 a = glm::translate(glm::mat4(1.0f), glm::vec3(1, 0, 0));
    const glm::mat4 b = glm::translate(glm::mat4(1.0f), glm::vec3(0, 2, 0));
    const glm::mat4 c = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 3));
    std::vector<glm::mat4> va(3, a), vb(2, b), vc(4, c);
    std::vector<const std::vector<glm::mat4>*> batches = {&va, &vb, &vc};

    auto staged = BoneMatrixBuffer::ComputeBatch(batches);
    ASSERT_EQ(staged.size(), 9u);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(staged[i], a);
    for (int i = 0; i < 2; ++i) EXPECT_EQ(staged[3 + i], b);
    for (int i = 0; i < 4; ++i) EXPECT_EQ(staged[5 + i], c);
}

TEST(AnimatorSoaBatch, ComputeBatch_SkipsNullBatches) {
    std::vector<glm::mat4> va(2, glm::mat4(1.0f));
    std::vector<const std::vector<glm::mat4>*> batches = {&va, nullptr};
    EXPECT_EQ(BoneMatrixBuffer::ComputeBatch(batches).size(), 2u);
}

TEST(AnimatorSoaBatch, ShouldSkipUpload_BatchDecisionReused) {
    // Same force/skip/count guard reused for the batched slab:
    // skip only when NOT forced, NOT dirty, count unchanged.
    EXPECT_TRUE (BoneMatrixBuffer::ShouldSkipUpload(false, false,  9,  9));
    EXPECT_FALSE(BoneMatrixBuffer::ShouldSkipUpload(true,  false,  9,  9)); // forced
    EXPECT_FALSE(BoneMatrixBuffer::ShouldSkipUpload(false, false,  9, 10)); // count change
}
