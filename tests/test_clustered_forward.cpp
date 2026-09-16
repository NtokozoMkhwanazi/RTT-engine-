/**
 * Clustered/Forward+ binning tests (suggestions.txt #2) — CPU core only.
 *
 * The GPU tile that consumes the per-cluster light lists is intentionally
 * NOT asserted here (no render test in this repo). These tests pin the
 * deterministic C++ binning layer: grid construction, plane math, single-light
 * placement, frustum rejection, multi-cluster overlap, and the cap.
 */
#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cmath>
#include <vector>

#include "lighting/ClusteredForward.h"

using namespace Clustered;

namespace {
// Canonical test camera: looks down -Z, symmetric frustum (aspect handled),
// identity-ish view (eye at origin, looks toward -Z). buildGrid is tested with
// this view so worldToView == identity for points at z=0 plane... we use pure
// view-space inputs to keep math analytic.
glm::mat4 identityView() { return glm::mat4(1.0f); }

Light pointLight(const glm::vec3& p, float radius = 1.0f) {
    // radius is encoded via LINEAR attenuation so that lightRadius(threshold=0.01)
    // returns exactly `radius`: 1/(0 + l*d) = 0.01 -> d = 100/l => l = 100/radius.
    Light l(LightType::POINT, p, glm::vec3(0, -1, 0), glm::vec3(1), 1.0f);
    l.linear  = 100.0f / radius;
    l.quadratic = 0.0f;
    l.constant = 0.0f;
    return l;
}

ClusterGrid grid4x4x4(float nearZ, float farZ) {
    LightBinning b;
    return b.buildGrid(4, 4, 4, nearZ, farZ, glm::radians(60.0f), 1.0f,
                       identityView());
}
} // namespace

// ---- grid construction -----------------------------------------------------

TEST(Clustered, GridDimensionsAndPlaneCount) {
    LightBinning b;
    ClusterGrid g = b.buildGrid(4, 4, 4, 1.0f, 1024.0f,
                                glm::radians(90.0f), 1.0f, identityView());
    EXPECT_TRUE(g.built);
    EXPECT_EQ(g.clusterCount(), 64);
    EXPECT_EQ(g.planeCount(), 6 * 64);
    EXPECT_EQ(g.zRange.size(), 64u);
}

TEST(Clustered, LogDepthSlicesAreMonotonicNonOverlapping) {
    LightBinning b;
    const float nearZ = 1.0f, farZ = 1024.0f;
    ClusterGrid g = b.buildGrid(8, 8, 8, nearZ, farZ,
                                glm::radians(60.0f), 16.0f/9.0f, identityView());
    // zRange[x] = (-farK, -nearK). Successive layers must be monotonic &
    // non-overlapping: farK of layer k == nearK of layer k+1.
    for (int layer = 0; layer < g.layers; ++layer) {
        for (int y = 0; y < g.rows; ++y) {
            for (int x = 0; x < g.columns; ++x) {
                const int idx = g.index(x, y, layer);
                const float nearK = -g.zRange[idx].y; // -(-nearK)
                const float farK  = -g.zRange[idx].x; // -(-farK)
                EXPECT_GT(farK, nearK);
                if (layer + 1 < g.layers) {
                    const int nxt = g.index(x, y, layer + 1);
                    // layer k's far boundary must equal layer k+1's near boundary
                    // (slices abut, no gap/overlap).
                    EXPECT_LE(std::abs(farK - (-g.zRange[nxt].y)), 1e-4f)
                        << "slices must abut (layer " << layer << ")";
                }
            }
        }
    }
}

TEST(Clustered, NearAndFarZPlanesFaceInward) {
    // 1x1x1 grid: single cluster's near plane at nearZ=1, far plane at farZ=64.
    LightBinning b;
    ClusterGrid g = b.buildGrid(1, 1, 1, 1.0f, 64.0f,
                                glm::radians(60.0f), 1.0f, identityView());
    EXPECT_EQ(g.clusterCount(), 1);
    // Near plane (index 4): inward normal -Z, constant = -nearK = -1.
    const Plane& nearP = g.planes[4];
    EXPECT_NEAR(nearP.n.z, -1.0f, 1e-5f);
    EXPECT_NEAR(nearP.n.w, -1.0f, 1e-4f); // nearK=1 -> w=-1
    // Far plane (index 5): inward normal +Z, constant = +farK = 64.
    const Plane& farP = g.planes[5];
    EXPECT_NEAR(farP.n.z,  1.0f, 1e-5f);
    EXPECT_NEAR(farP.n.w, 64.0f, 1e-3f); // farK=64
}

// ---- single-light placement ------------------------------------------------

TEST(Clustered, PointLightAtClusterCenterIsBinnedAlone) {
    LightBinning b;
    // 4x4x4 grid, nearZ=1, farZ=16, 60° fov, aspect 1 (symmetric). Look down -Z.
    ClusterGrid g = b.buildGrid(4, 4, 4, 1.0f, 16.0f, glm::radians(60.0f), 1.0f,
                                identityView());
    // place a tiny light strictly INSIDE tile (2,2) of layer 1 (zRange covers
    // z=-3). Tile (2,2) center in view space ~ (0.866, 0.866, -3).
    const glm::vec3 p(0.8f, 0.8f, -3.0f);
    const int idx = b.clusterOf(g, p);
    ASSERT_GE(idx, 0) << "interior point must resolve to some cluster";
    EXPECT_EQ(idx, g.index(2, 2, 1));

    // radius-~0 light: binLights should place it in exactly ONE cluster.
    Light l = pointLight(glm::vec3(0.8f, 0.8f, -3.0f), 0.001f);
    EXPECT_NEAR(b.lightRadius(l), 0.001f, 1e-4f);
    BinResult R = b.binLights(g, {l});
    EXPECT_EQ(R.totalLists, 1u);
    // find the cluster holding light 0
    int foundC = -1;
    for (int c = 0; c < g.clusterCount(); ++c)
        for (uint32_t j = 0; j < R.counts[c]; ++j)
            if (R.indices[R.offsets[c] + j] == 0u) { foundC = c; break; }
    ASSERT_GE(foundC, 0);
    EXPECT_EQ(foundC, idx);
    EXPECT_EQ(R.counts[foundC], 1u);
}

TEST(Clustered, LightOutsideFrustumIsNotBinned) {
    LightBinning b;
    ClusterGrid g = grid4x4x4(1.0f, 16.0f);
    // Behind camera (z > -1 in view space => behind near plane) and way outside.
    Light behind(LightType::POINT, glm::vec3(0,0,0), glm::vec3(0,0,0));
    behind.linear = 1.0f; behind.quadratic = 0; behind.constant = 0;
    // position world = view (identity); put it inside near plane: z=-0.2 (in front of near=1? no)
    // Use a point clearly outside the frustum far to the side.
    glm::vec3 outside(1e4f, 0.0f, -5.0f);
    Light l = pointLight(outside, 0.5f);
    BinResult R = b.binLights(g, {l});
    EXPECT_EQ(R.totalLists, 0u);
    EXPECT_EQ(b.clusterOf(g, outside), -1);
}

// ---- multi-cluster overlap -------------------------------------------------

TEST(Clustered, LargeLightBleedsIntoAdjacentClusters) {
    LightBinning b;
    ClusterGrid g = grid4x4x4(1.0f, 16.0f);
    // A big radius light centered at the origin (0,0,-5) should overlap
    // several tiles in its layer (radius ~ 5 view units spans multiple tiles).
    Light l = pointLight(glm::vec3(0,0,0), 5.0f);
    BinResult R = b.binLights(g, {l});
    EXPECT_GE(R.totalLists, 2u) << "a radius-5 light must overlap >=2 clusters";
    // and light index 0 should appear in every cluster it was assigned.
    uint32_t apparitions = 0;
    for (int c = 0; c < g.clusterCount(); ++c)
        for (uint32_t j = 0; j < R.counts[c]; ++j)
            if (R.indices[R.offsets[c] + j] == 0u) ++apparitions;
    EXPECT_EQ(apparitions, R.totalLists);
}

// ---- cap + flat layout -----------------------------------------------------

TEST(Clustered, CapClipsLightsAndLayoutIsPacked) {
    LightBinning b;
    // 1x1x1 grid (1 cluster), cap = 2. 5 lights all inside -> 2 kept, 3 clipped.
    ClusterGrid g = b.buildGrid(1, 1, 1, 0.1f, 100.0f,
                                glm::radians(90.0f), 1.0f, identityView());
    ASSERT_EQ(g.clusterCount(), 1);
    std::vector<Light> lights;
    for (int i = 0; i < 5; ++i)
        lights.push_back(pointLight(glm::vec3(0,0,-5), 0.2f));
    BinResult R = b.binLights(g, lights, /*maxPerCluster=*/2);
    EXPECT_EQ(R.counts[0], 2u);
    EXPECT_EQ(R.offsets[0], 0u);
    // indices are the first 2 lights (0 and 1) in insertion order
    EXPECT_EQ(R.indices[0], 0u);
    EXPECT_EQ(R.indices[1], 1u);
    EXPECT_EQ(R.clipped, 3u);
}

// ---- lightRadius math ------------------------------------------------------

TEST(Clustered, LightRadiusLinearOnlyPath) {
    LightBinning b;
    Light l(LightType::POINT, glm::vec3(0), glm::vec3(0,0,0), glm::vec3(1), 1.0f);
    l.constant = 0.0f; l.linear = 0.1f; l.quadratic = 0.0f;
    // threshold 0.01: d = (1/0.01 - 0)/0.1 = 100/0.1 = 1000
    float r = b.lightRadius(l, 0.01f);
    EXPECT_GT(r, 999.0f) << "linear-only radius at thr=0.01 should be ~1000";
    EXPECT_NEAR(r, 1000.0f, 5.0f);
}
