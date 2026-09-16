/**
 * Vulkan CPU-skinning regression tests.
 *
 * Historical context: the Vulkan viewport (src/editor_main.cpp) rendered the
 * animated character by CPU-skinning every frame. A previous version skinned
 * IN PLACE - reading the very vertex buffer it was overwriting - so frame N
 * skinned frame N-1's output and the bone transforms compounded frame over
 * frame (p_n = T_n * ... * T_1 * p_bind), deforming the mesh into a mess.
 * The fix keeps a pristine bind-pose copy and re-skins from it every frame.
 *
 * The old VulkanSkinning.* tests below (which included editor_main.cpp and
 * exercised BuildSkinnedCharacter / UpdateSkinnedCharacter) were removed when
 * editor_main.cpp was refactored to a 92-line GLFW bootstrap with no skinning
 * code. The SkinningHelper.* tests here exercise the SAME skinning math
 * (Skinning::SkinVertices) directly with synthetic data, and are the durable
 * regression guard for the "compounding transform" bug.
 */

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "animationSystem/Skinning.h"

namespace {

// ---------------------------------------------------------------------------
// Direct unit tests of the shared Skinning::SkinVertices helper (synthetic
// two-bone data - no FBX, no editor code).
// ---------------------------------------------------------------------------

TEST(SkinningHelper, TwoBoneBlendAveragesTransforms) {
    // One vertex at the origin bound 50/50 to bone 0 (translates +Y 2) and
    // bone 1 (translates +X 2): the blended skin must land at (1, 1, 0).
    std::vector<float> bind = {0, 0, 0,   0, 1, 0,   0, 0};   // pos, normal, uv
    std::vector<glm::ivec4> ids = {glm::ivec4(0, 1, -1, -1)};
    std::vector<glm::vec4> wts = {glm::vec4(0.5f, 0.5f, 0.0f, 0.0f)};
    std::vector<glm::mat4> mats(2, glm::mat4(1.0f));
    mats[0] = glm::translate(mats[0], glm::vec3(0.0f, 2.0f, 0.0f));
    mats[1] = glm::translate(mats[1], glm::vec3(2.0f, 0.0f, 0.0f));

    std::vector<float> out;
    Skinning::SkinVertices(bind, ids, wts, mats, out);
    ASSERT_EQ(out.size(), bind.size());
    EXPECT_NEAR(out[0], 1.0f, 1e-4f);
    EXPECT_NEAR(out[1], 1.0f, 1e-4f);
    EXPECT_NEAR(out[2], 0.0f, 1e-4f);
    // Normal: (0,1,0) weighted half rotation (identity for translations) stays.
    EXPECT_NEAR(out[3], 0.0f, 1e-4f);
    EXPECT_NEAR(out[4], 1.0f, 1e-4f);
    EXPECT_NEAR(out[5], 0.0f, 1e-4f);
    // UV passes through.
    EXPECT_NEAR(out[6], 0.0f, 1e-4f);
    EXPECT_NEAR(out[7], 0.0f, 1e-4f);
}

TEST(SkinningHelper, UnnormalizedWeightsAreNormalizedLikeGlShader) {
    // Author the weights as 1.0/1.0 (sum 2) - the GL shader normalizes them
    // to 0.5/0.5, so the result must match the blend in TwoBoneBlend.
    std::vector<float> bind = {0, 0, 0,   0, 1, 0,   0, 0};
    std::vector<glm::ivec4> ids = {glm::ivec4(0, 1, -1, -1)};
    std::vector<glm::vec4> wts = {glm::vec4(1.0f, 1.0f, 0.0f, 0.0f)};
    std::vector<glm::mat4> mats(2, glm::mat4(1.0f));
    mats[0] = glm::translate(mats[0], glm::vec3(0.0f, 2.0f, 0.0f));
    mats[1] = glm::translate(mats[1], glm::vec3(2.0f, 0.0f, 0.0f));

    std::vector<float> out;
    Skinning::SkinVertices(bind, ids, wts, mats, out);
    EXPECT_NEAR(out[0], 1.0f, 1e-4f);
    EXPECT_NEAR(out[1], 1.0f, 1e-4f);
}

TEST(SkinningHelper, StaticVertexKeepsBindPose) {
    std::vector<float> bind = {5, 6, 7,   0, 0, 1,   0.5f, 0.25f};
    std::vector<glm::ivec4> ids = {glm::ivec4(0, -1, -1, -1)};
    std::vector<glm::vec4> wts = {glm::vec4(0.0f, 0.0f, 0.0f, 0.0f)};
    std::vector<glm::mat4> mats(1, glm::translate(glm::mat4(1.0f), glm::vec3(100.0f)));

    std::vector<float> out;
    Skinning::SkinVertices(bind, ids, wts, mats, out);
    for (int k = 0; k < 8; ++k) EXPECT_NEAR(out[k], bind[k], 1e-6f);
}

TEST(SkinningHelper, OutOfRangeBoneIsIgnored) {
    // Only a valid bone contributes; the invalid id (99) must not index OOB.
    std::vector<float> bind = {0, 0, 0,   0, 1, 0,   0, 0};
    std::vector<glm::ivec4> ids = {glm::ivec4(99, -1, -1, -1)};
    std::vector<glm::vec4> wts = {glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)};
    std::vector<glm::mat4> mats(1, glm::mat4(1.0f));

    std::vector<float> out;
    Skinning::SkinVertices(bind, ids, wts, mats, out);
    ASSERT_EQ(out.size(), bind.size());
    for (int k = 0; k < 8; ++k) EXPECT_NEAR(out[k], bind[k], 1e-6f);  // falls back to bind
}

} // namespace
