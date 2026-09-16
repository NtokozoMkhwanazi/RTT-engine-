#pragma once
// ============================================================================
// Clustered/Forward+ light binning (suggestions.txt #2) — CPU core.
// ============================================================================
// Builds a logarithmic-Z, uniform-XY-tiled cluster grid over the camera
// frustum and bins a flat array of `Light`s into per-cluster light-index lists
// using an inward-facing 6-plane (4 side + near + far) signed-distance test
// against each light's bounding sphere.
//
// The GPU shader tile that consumes these lists is the *untestable* part in
// this repo (no render test asserts Forward+ output), so this header exposes
// ONLY the verified C++ binning layer. `buildGrid` is deterministic,
// `LightBinning::binLights` is pure (no GL/Vulkan), and the helpers are unit
// tested in tests/test_clustered_forward.cpp.
// ============================================================================
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include "lighting/LightingSystem.h"  // Light, LightType

namespace Clustered {

// A frustum plane stored as (unit normal xyz, -dot(n, point_on_plane) = w).
// A point P is INSIDE the cluster when, for every plane, dot(n, P) + w >= 0
// (i.e. on the inward side). With a light radius r the test becomes >= -r.
struct Plane {
    glm::vec4 n; // .xyz = inward unit normal, .w = plane constant
};

struct ClusterGrid {
    int columns  = 16;
    int rows     = 16;
    int layers   = 24;
    float nearZ  = 0.1f;
    float farZ   = 1000.0f;
    float fovY   = glm::radians(60.0f);
    float aspect = 16.0f / 9.0f;
    glm::mat4 view = glm::mat4(1.0f);

    // Built by LightBinning::buildGrid(): 6 planes + (nearZ, farZ) per cluster,
    // cluster-major (index = z*columns*rows + y*columns + x).
    std::vector<Plane> planes;       // size = 6 * count
    std::vector<glm::vec2> zRange;   // (nearZ, farZ) in view space per cluster
    bool built = false;

    int clusterCount() const { return columns * rows * layers; }
    int planeCount()   const { return 6 * clusterCount(); }
    int index(int x, int y, int z) const {
        return z * columns * rows + y * columns + x;
    }
};

// Result of binning: per-cluster index lists. Flat for easy upload to a GPU
// SSBO/tile buffer (each cluster owns `counts[c]` indices starting at
// `offsets[c]`).
struct BinResult {
    std::vector<uint32_t> offsets;   // per cluster: start into `indices`
    std::vector<uint16_t> counts;    // per cluster: #lights (capped at maxPerCluster)
    std::vector<uint16_t> indices;   // flat light-index lists (capped per cluster)
    uint32_t              totalLists = 0; // clusters that received >=1 light
    uint32_t              clipped    = 0; // lights clipped by the per-cluster cap
};

class LightBinning {
public:
    // Radius at which a light's attenuation falls below `threshold`
    // (solves 1/(c + l*d + q*d^2) = threshold for d). Deterministic & testable.
    float lightRadius(const Light& l, float threshold = 0.01f) const;

    // World -> view space using the grid's view matrix.
    glm::vec3 worldToView(const glm::mat4& view, const glm::vec3& p) const;

    // Builds the frustum cluster grid (log-Z slices, uniform XY tiling).
    // Pure: no GL state. Returns the grid by value for test ergonomics.
    ClusterGrid buildGrid(int columns, int rows, int layers,
                          float nearZ, float farZ, float fovY, float aspect,
                          const glm::mat4& view) const;

    // Bin `lights` into `grid`. Each light is transformed to view space by
    // `grid.view`, culled against the camera Z range, then its bounding sphere
    // is tested against every cluster's 6 planes (>= -radius). A light may
    // legitimately appear in MULTIPLE clusters (overlap region).
    BinResult binLights(const ClusterGrid& grid,
                        const std::vector<Light>& lights,
                        uint16_t maxPerCluster = 64) const;

    // For tests: which cluster holds the (radius-0) point pview, or -1 if
    // outside the frustum. pview is already in view space.
    int clusterOf(const ClusterGrid& grid, glm::vec3 pview) const;

    // Signed distance of a view-space point to plane i of `grid`.
    float signedDist(const ClusterGrid& grid, int clusterIdx, int planeIdx,
                     glm::vec3 p) const;

private:
    // Side-plane normals: 0=-X(left)? we store inward; order:
    // 0=LEFT(+x),1=RIGHT(-x),2=BOTTOM(+y),3=TOP(-y),4=NEAR(in -z),5=FAR(in +z)
    void buildSlicePlanes(ClusterGrid& g, int layer, float nearK, float farK) const;
};

} // namespace Clustered
