// ============================================================================
// Clustered/Forward+ light binning — CPU implementation (suggestions.txt #2).
// Pure C++ (no GL/Vulkan). See tests/test_clustered_forward.cpp.
// ============================================================================
#include "lighting/ClusteredForward.h"

#include <algorithm>
#include <cmath>

namespace Clustered {

float LightBinning::lightRadius(const Light& l, float threshold) const {
    // Solve 1/(c + l*d + q*d^2) = threshold  ->  q*d^2 + l*d + (c - 1/q... ) = 0
    // i.e. q*d^2 + l*d + (c - 1/threshold) = 0.
    const float c = l.constant;
    const float lin = l.linear;
    const float q = l.quadratic;
    const float rhs = 1.0f / threshold;
    // Directional lights have no meaningful radius (bin by direction, separately).
    if (q == 0.0f && lin == 0.0f) {
        return (c > 0.0f) ? 1e6f : 1e6f; // effectively "everywhere" along its dir
    }
    if (q == 0.0f) {
        // linear only: l*d = rhs - c
        if (lin > 0.0f) return (rhs - c) / lin;
        return 1e6f;
    }
    // quadratic: q*d^2 + lin*d + (c - rhs) = 0
    const float disc = lin * lin - 4.0f * q * (c - rhs);
    if (disc < 0.0f) return 1e6f;
    const float d = (-lin + std::sqrt(disc)) / (2.0f * q);
    return (d > 0.0f) ? d : 1e6f;
}

glm::vec3 LightBinning::worldToView(const glm::mat4& view, const glm::vec3& p) const {
    const glm::vec4 v = view * glm::vec4(p, 1.0f);
    return glm::vec3(v.x, v.y, v.z);
}

static inline float sliceZ(int layer, int layers, float nearZ, float farZ) {
    // Logarithmic depth slice boundary (geometric).
    const float ratio = farZ / nearZ;
    return nearZ * std::pow(ratio, float(layer) / float(layers));
}

ClusterGrid LightBinning::buildGrid(int columns, int rows, int layers,
                                    float nearZ, float farZ, float fovY, float aspect,
                                    const glm::mat4& view) const {
    ClusterGrid g;
    g.columns = columns;
    g.rows = rows;
    g.layers = layers;
    g.nearZ = nearZ;
    g.farZ = farZ;
    g.fovY = fovY;
    g.aspect = aspect;
    g.view = view;
    g.planes.assign(g.planeCount(), Plane{glm::vec4(0.0f)});
    g.zRange.assign(g.clusterCount(), glm::vec2(0.0f));
    g.built = true;

    const float tanHalfY = std::tan(0.5f * fovY);

    for (int z = 0; z < layers; ++z) {
        const float nearK = sliceZ(z, layers, nearZ, farZ);
        const float farK  = sliceZ(z + 1, layers, nearZ, farZ);
        const float wNear = 2.0f * nearK * tanHalfY * aspect; // full width at near
        const float hNear = 2.0f * nearK * tanHalfY;          // full height at near
        const float wFar  = 2.0f * farK  * tanHalfY * aspect;
        const float hFar  = 2.0f * farK  * tanHalfY;

        for (int y = 0; y < rows; ++y) {
            // tile extents at near / far (y up, row 0 = -Y/bottom)
            const float yMinN = -0.5f * hNear + (float(y) / float(rows)) * hNear;
            const float yMaxN = -0.5f * hNear + (float(y+1) / float(rows)) * hNear;
            const float yMinF = -0.5f * hFar  + (float(y) / float(rows)) * hFar;
            const float yMaxF = -0.5f * hFar  + (float(y+1) / float(rows)) * hFar;

            for (int x = 0; x < columns; ++x) {
                const float xMinN = -0.5f * wNear + (float(x) / float(columns)) * wNear;
                const float xMaxN = -0.5f * wNear + (float(x+1) / float(columns)) * wNear;
                const float xMinF = -0.5f * wFar  + (float(x) / float(columns)) * wFar;
                const float xMaxF = -0.5f * wFar  + (float(x+1) / float(columns)) * wFar;

                const int idx = g.index(x, y, z);
                Plane* p = &g.planes[idx * 6];
                // Near plane (z = -nearK), inward = toward -z (deeper).
                p[4].n = glm::vec4(0.0f, 0.0f, -1.0f, -nearK); // dist = -Pz - nearK ; inside if >=0
                // Far plane (z = -farK), inward = toward +z (toward camera).
                p[5].n = glm::vec4(0.0f, 0.0f,  1.0f,  farK); // dist =  Pz + farK  ; inside if >=0
                // LEFT plane through (xMinN,-nearK)->(xMinF,-farK), xMax edge, inward +x.
                // plane normal (unnormalized) prop to (farK-nearK, 0, xMinF-xMinN) -> +x.
                {
                    glm::vec3 a(xMinN, 0.0f, -nearK);
                    glm::vec3 b(xMinF, 0.0f, -farK);
                    glm::vec3 edge = b - a;          // (dx, 0, dz)
                    glm::vec3 n = glm::normalize(glm::vec3(-edge.z, 0.0f, edge.x)); // +x-ish
                    float w = -glm::dot(n, a);
                    p[0].n = glm::vec4(n, w);
                }
                // RIGHT plane, inward -x.
                {
                    glm::vec3 a(xMaxN, 0.0f, -nearK);
                    glm::vec3 b(xMaxF, 0.0f, -farK);
                    glm::vec3 edge = b - a;
                    glm::vec3 n = glm::normalize(glm::vec3(edge.z, 0.0f, -edge.x)); // -x-ish
                    float w = -glm::dot(n, a);
                    p[1].n = glm::vec4(n, w);
                }
                // BOTTOM plane, inward +y.
                {
                    glm::vec3 a(0.0f, yMinN, -nearK);
                    glm::vec3 b(0.0f, yMinF, -farK);
                    glm::vec3 edge = b - a;
                    glm::vec3 n = glm::normalize(glm::vec3(0.0f, -edge.z, edge.y)); // +y-ish
                    float w = -glm::dot(n, a);
                    p[2].n = glm::vec4(n, w);
                }
                // TOP plane, inward -y.
                {
                    glm::vec3 a(0.0f, yMaxN, -nearK);
                    glm::vec3 b(0.0f, yMaxF, -farK);
                    glm::vec3 edge = b - a;
                    glm::vec3 n = glm::normalize(glm::vec3(0.0f, edge.z, -edge.y)); // -y-ish
                    float w = -glm::dot(n, a);
                    p[3].n = glm::vec4(n, w);
                }
                g.zRange[idx] = glm::vec2(-farK, -nearK);
            }
        }
    }
    return g;
}

float LightBinning::signedDist(const ClusterGrid& grid, int clusterIdx, int planeIdx,
                               glm::vec3 p) const {
    const Plane& pl = grid.planes[clusterIdx * 6 + planeIdx];
    return glm::dot(glm::vec3(pl.n), p) + pl.n.w;
}

int LightBinning::clusterOf(const ClusterGrid& grid, glm::vec3 pview) const {
    for (int zx = 0; zx < grid.layers; ++zx) {
        for (int zy = 0; zy < grid.rows; ++zy) {
            for (int zxx = 0; zxx < grid.columns; ++zxx) {
                const int idx = grid.index(zxx, zy, zx);
                const glm::vec2& zr = grid.zRange[idx];
                if (pview.z < zr.x || pview.z > zr.y) continue; // -farK <= Pz <= -nearK
                bool inside = true;
                for (int pl = 0; pl < 6 && inside; ++pl)
                    if (signedDist(grid, idx, pl, pview) < 0.0f) inside = false;
                if (inside) return idx;
            }
        }
    }
    return -1;
}

BinResult LightBinning::binLights(const ClusterGrid& grid,
                                  const std::vector<Light>& lights,
                                  uint16_t maxPerCluster) const {
    BinResult R;
    const int count = grid.clusterCount();
    R.offsets.assign(count, 0);
    R.counts.assign(count, 0);

    // First pass: count per cluster (capped). Two-pass keeps the flat list
    // tightly packed so a GPU can consume offsets/counts/indices directly.
    std::vector<uint32_t> rawCounts(count, 0);
    for (size_t i = 0; i < lights.size(); ++i) {
        const Light& l = lights[i];
        const glm::vec3 pview = worldToView(grid.view, l.position);
        const float radius = lightRadius(l);
        for (int c = 0; c < count; ++c) {
            bool inside = true;
            for (int pl = 0; pl < 6 && inside; ++pl)
                if (signedDist(grid, c, pl, pview) < -radius) inside = false;
            if (inside) ++rawCounts[c];
        }
    }
    uint32_t totalClipped = 0;
    for (int c = 0; c < count; ++c) {
        R.counts[c] = std::min<uint32_t>(rawCounts[c], maxPerCluster);
        if (rawCounts[c] > maxPerCluster) totalClipped += (rawCounts[c] - maxPerCluster);
    }
    R.clipped = totalClipped;

    // Offsets.
    uint32_t off = 0;
    for (int c = 0; c < count; ++c) {
        R.offsets[c] = off;
        off += R.counts[c];
    }
    R.indices.assign(off, 0);

    // Second pass: emit (append until cap).
    std::vector<uint16_t> emitted(count, 0);
    for (size_t i = 0; i < lights.size(); ++i) {
        const Light& l = lights[i];
        const glm::vec3 pview = worldToView(grid.view, l.position);
        const float radius = lightRadius(l);
        for (int c = 0; c < count; ++c) {
            if (emitted[c] >= R.counts[c]) continue;
            bool inside = true;
            for (int pl = 0; pl < 6 && inside; ++pl)
                if (signedDist(grid, c, pl, pview) < -radius) inside = false;
            if (inside) {
                R.indices[R.offsets[c] + emitted[c]] = uint16_t(i);
                ++emitted[c];
            }
        }
    }

    uint32_t totalLists = 0;
    for (int c = 0; c < count; ++c)
        if (R.counts[c] > 0) ++totalLists;
    R.totalLists = totalLists;
    return R;
}

} // namespace Clustered
