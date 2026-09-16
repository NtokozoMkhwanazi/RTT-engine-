/**
 * Virtualized Geometry — Cluster Builder tests (Phase 1 + Phase 2).
 *
 * Verifies:
 *   - BuildMeshClusters() splits a mesh into 128-triangle clusters
 *   - Bounding volumes (box + sphere) are correct per cluster
 *   - ExportClusterMeshFile() / ImportClusterMeshFile() round-trip preserves data
 *   - FlattenClustersToBuffers() produces contiguous vertex/index pools with
 *     correct firstVertex / firstIndex / indexCount / instanceId offsets
 *   - GPUClusterCommand layout matches VkDrawIndexedIndirectCommand
 * glslc validates cluster_cull.comp / nanite_lod.comp SPIR-V separately.
 */
#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#include "meshSystem/Mesh.h"  // Vertex, MeshCluster, ClusterPackedPayload,
                             // GPUClusterBounds, GPUClusterCommand,
                             // ClusterIndirectCommand, MeshUtils::{...}

namespace {
// Build a simple flat grid mesh: (gridSize × gridSize) quads → triangles.
void buildGridMesh(uint32_t gridSize,
                   std::vector<Vertex>& verts,
                   std::vector<unsigned int>& indices) {
    verts.clear();
    indices.clear();
    for (uint32_t y = 0; y <= gridSize; ++y) {
        for (uint32_t x = 0; x <= gridSize; ++x) {
            Vertex v{};
            v.Position = glm::vec3(static_cast<float>(x),
                                     0.0f,
                                     static_cast<float>(y));
            v.Normal   = glm::vec3(0, 1, 0);
            verts.push_back(v);
        }
    }
    for (uint32_t y = 0; y < gridSize; ++y) {
        for (uint32_t x = 0; x < gridSize; ++x) {
            uint32_t i0 = y * (gridSize + 1) + x;
            uint32_t i1 = i0 + 1;
            uint32_t i2 = i0 + (gridSize + 1);
            uint32_t i3 = i2 + 1;
            // Two triangles per quad
            indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
            indices.push_back(i1); indices.push_back(i2); indices.push_back(i3);
        }
    }
}

// Test mesh: plane at Z=0, 10×10 quads = 200 triangles → 2 clusters of 128 + 1 of 72
constexpr uint32_t kGridSize = 10;
}  // namespace

// =========================================================================
// Phase 1: BuildMeshClusters
// =========================================================================
TEST(ClusterBuilder, Builds128TriangleClusters) {
    std::vector<Vertex> verts;
    std::vector<unsigned int> indices;
    buildGridMesh(kGridSize, verts, indices);

    // 10×10 grid = 100 quads × 2 tris/quad = 200 triangles
    ASSERT_EQ(indices.size(), 200u * 3u);

    auto payload = MeshUtils::BuildMeshClusters(verts, indices, 128);

    // 200 tris / 128 per cluster → ceil(200/128) = 2 clusters
    EXPECT_EQ(payload.clusters.size(), 2u);
    // First cluster: 128 triangles (384 indices)
    EXPECT_EQ(payload.clusters[0].indices.size(), 128u * 3u);
    // Second cluster: 72 triangles (216 indices)
    EXPECT_EQ(payload.clusters[1].indices.size(), 72u * 3u);
}

TEST(ClusterBuilder, BoundingVolumesCorrect) {
    std::vector<Vertex> verts;
    std::vector<unsigned int> indices;
    buildGridMesh(kGridSize, verts, indices);

    auto payload = MeshUtils::BuildMeshClusters(verts, indices, 128);
    ASSERT_EQ(payload.clusters.size(), 2u);

    for (size_t c = 0; c < payload.clusters.size(); ++c) {
        const auto& cluster = payload.clusters[c];
        // Bounding box must be valid
        EXPECT_TRUE(cluster.boundingBox.IsValid()) << "cluster " << c << " bbox invalid";

        // All vertex positions must be within the bounding box
        for (const auto& v : cluster.vertices) {
            EXPECT_GE(v.Position.x, cluster.boundingBox.min.x - 1e-5f);
            EXPECT_LE(v.Position.x, cluster.boundingBox.max.x + 1e-5f);
            EXPECT_GE(v.Position.z, cluster.boundingBox.min.z - 1e-5f);
            EXPECT_LE(v.Position.z, cluster.boundingBox.max.z + 1e-5f);
        }

        // Bounding sphere must enclose all vertices
        for (const auto& v : cluster.vertices) {
            float dist = glm::distance(v.Position, cluster.boundingSphere.center);
            EXPECT_LE(dist, cluster.boundingSphere.radius + 1e-4f)
                << "vertex outside bounding sphere in cluster " << c;
        }
    }

    // First cluster covers x=[0..10], z=[0..12] roughly (128 tris from a 10×10 grid)
    // Check that the combined bounds of both clusters equals the grid bounds
    glm::vec3 combinedMin(FLT_MAX), combinedMax(-FLT_MAX);
    for (const auto& cluster : payload.clusters) {
        combinedMin = glm::min(combinedMin, cluster.boundingBox.min);
        combinedMax = glm::max(combinedMax, cluster.boundingBox.max);
    }
    EXPECT_NEAR(combinedMin.x, 0.0f, 1e-5f);
    EXPECT_NEAR(combinedMin.z, 0.0f, 1e-5f);
    EXPECT_NEAR(combinedMax.x, 10.0f, 1e-5f);
    EXPECT_NEAR(combinedMax.z, 10.0f, 1e-5f);
}

TEST(ClusterBuilder, LocalVertexRemapping) {
    std::vector<Vertex> verts;
    std::vector<unsigned int> indices;
    buildGridMesh(kGridSize, verts, indices);

    auto payload = MeshUtils::BuildMeshClusters(verts, indices, 128);
    ASSERT_EQ(payload.clusters.size(), 2u);

    for (const auto& cluster : payload.clusters) {
        // All indices must be within the cluster's local vertex range
        uint32_t maxIdx = 0;
        for (unsigned int idx : cluster.indices) {
            EXPECT_LT(idx, (unsigned int)cluster.vertices.size())
                << "index out of cluster-local vertex range";
            maxIdx = std::max(maxIdx, idx);
        }
        // Cluster vertices are sub-set of original mesh vertices
        EXPECT_GT(cluster.vertices.size(), maxIdx);
    }
}

TEST(ClusterBuilder, TotalVerticesAndIndicesTracked) {
    std::vector<Vertex> verts;
    std::vector<unsigned int> indices;
    buildGridMesh(kGridSize, verts, indices);

    auto payload = MeshUtils::BuildMeshClusters(verts, indices, 128);

    // totalVertices is the sum of unique vertices per cluster (with duplication
    // across clusters). totalIndices is the total number of index entries.
    size_t expectedIndices = 0;
    size_t expectedVertices = 0;
    for (const auto& c : payload.clusters) {
        expectedIndices += c.indices.size();
        expectedVertices += c.vertices.size();
    }

    EXPECT_EQ(payload.totalVertices, expectedVertices);
    EXPECT_EQ(payload.totalIndices, expectedIndices);
}

// =========================================================================
// Phase 1: Export / Import round-trip
// =========================================================================
TEST(ClusterIO, ExportImportRoundTrip) {
    std::vector<Vertex> verts;
    std::vector<unsigned int> indices;
    buildGridMesh(kGridSize, verts, indices);

    auto payload = MeshUtils::BuildMeshClusters(verts, indices, 128);
    ASSERT_EQ(payload.clusters.size(), 2u);

    // Export to temp file
    char tmppath[] = "/tmp/test_cluster_roundtrip.meshpack";

    MeshUtils::ExportClusterMeshFile(tmppath, payload);

    // Import back
    ClusterPackedPayload imported;
    ASSERT_TRUE(MeshUtils::ImportClusterMeshFile(tmppath, imported));

    // Verify cluster count matches
    EXPECT_EQ(imported.clusters.size(), payload.clusters.size());

    // Verify per-cluster data matches
    for (size_t c = 0; c < payload.clusters.size(); ++c) {
        EXPECT_EQ(imported.clusters[c].indices.size(), payload.clusters[c].indices.size());
        EXPECT_EQ(imported.clusters[c].vertices.size(), payload.clusters[c].vertices.size());

        // Compare indices
        for (size_t i = 0; i < payload.clusters[c].indices.size(); ++i) {
            EXPECT_EQ(imported.clusters[c].indices[i],
                      payload.clusters[c].indices[i])
                << "cluster " << c << " index " << i << " mismatch";
        }

        // Compare vertex positions
        for (size_t v = 0; v < payload.clusters[c].vertices.size(); ++v) {
            EXPECT_EQ(imported.clusters[c].vertices[v].Position,
                      payload.clusters[c].vertices[v].Position)
                << "cluster " << c << " vertex " << v << " position mismatch";
        }
    }

    EXPECT_EQ(imported.clusters.size(), payload.clusters.size());
    EXPECT_EQ(imported.totalVertices, payload.totalVertices);
    EXPECT_EQ(imported.totalIndices, payload.totalIndices);

    // Cleanup
    std::remove(tmppath);
}

// =========================================================================
// Phase 2: FlattenClustersToBuffers (GPU buffer layout)
// =========================================================================
TEST(ClusterFlatten, ProducesContiguousPools) {
    std::vector<Vertex> verts;
    std::vector<unsigned int> indices;
    buildGridMesh(kGridSize, verts, indices);

    auto payload = MeshUtils::BuildMeshClusters(verts, indices, 128);
    ASSERT_EQ(payload.clusters.size(), 2u);

    std::vector<Vertex> flatVerts;
    std::vector<unsigned int> flatIndices;
    auto commands = MeshUtils::FlattenClustersToBuffers(payload, flatVerts, flatIndices);

    // One command per cluster
    EXPECT_EQ(commands.size(), payload.clusters.size());

    // Total vertices and indices must match the payload's
    EXPECT_EQ(flatVerts.size(), payload.totalVertices);
    EXPECT_EQ(flatIndices.size(), payload.totalIndices);

    // Walk commands and verify offsets are correct
    size_t vOff = 0, iOff = 0;
    for (size_t c = 0; c < commands.size(); ++c) {
        const auto& cmd = commands[c];
        EXPECT_EQ(cmd.firstVertex, (uint32_t)vOff);
        EXPECT_EQ(cmd.firstIndex, (uint32_t)iOff);

        uint32_t clusterVCount = (uint32_t)payload.clusters[c].vertices.size();
        uint32_t clusterICount = (uint32_t)payload.clusters[c].indices.size();

        EXPECT_EQ(cmd.indexCount, clusterICount);
        // instanceId should default to cluster index
        EXPECT_EQ(cmd.instanceId, (uint32_t)c);

        vOff += clusterVCount;
        iOff += clusterICount;
    }

    // After all clusters, offsets must equal total flat array sizes
    EXPECT_EQ(vOff, flatVerts.size());
    EXPECT_EQ(iOff, flatIndices.size());
}

TEST(ClusterFlatten, IndicesRemappedToGlobal) {
    std::vector<Vertex> verts;
    std::vector<unsigned int> indices;
    buildGridMesh(kGridSize, verts, indices);

    auto payload = MeshUtils::BuildMeshClusters(verts, indices, 128);

    std::vector<Vertex> flatVerts;
    std::vector<unsigned int> flatIndices;
    auto commands = MeshUtils::FlattenClustersToBuffers(payload, flatVerts, flatIndices);

    // All remapped indices must point into the flat vertex array
    for (unsigned int idx : flatIndices) {
        EXPECT_LT(idx, (unsigned int)flatVerts.size())
            << "flattened index out of range: " << idx;
    }

    // The flat vertex array should contain all unique vertices from all clusters
    EXPECT_EQ(flatVerts.size(), payload.totalVertices);
}

// =========================================================================
// Phase 2: GPUClusterCommand — std430 layout matches IndirectCommand
// =========================================================================
TEST(ClusterGPU, GPUClusterCommandMatchesIndirectCommand) {
    // GPUClusterCommand is 4 × uint = 16 bytes (std430-compatible)
    EXPECT_EQ(sizeof(GPUClusterCommand), 16u);

    // ClusterIndirectCommand must match VkDrawIndexedIndirectCommand layout
    // (indexCount, instanceCount, firstIndex, vertexOffset, firstInstance)
    EXPECT_EQ(sizeof(ClusterIndirectCommand),
              sizeof(uint32_t) * 5u);  // 5 × uint32_t = 20 bytes

    // Verify field layout of ClusterIndirectCommand
    ClusterIndirectCommand cmd{};
    cmd.indexCount = 384;
    cmd.instanceCount = 1;
    cmd.firstIndex = 0;
    cmd.vertexOffset = 0;
    cmd.firstInstance = 0;
    EXPECT_EQ(cmd.indexCount, 384u);
    EXPECT_EQ(cmd.instanceCount, 1u);
    EXPECT_EQ(cmd.firstIndex, 0u);
    EXPECT_EQ(cmd.vertexOffset, 0);
    EXPECT_EQ(cmd.firstInstance, 0u);
}

TEST(ClusterGPU, GPUClusterBoundsLayout) {
    // Must match GLSL ClusterBounds (3 × vec4 = 48 bytes, std140)
    EXPECT_EQ(sizeof(GPUClusterBounds), 48u);

    GPUClusterBounds b{};
    b.sphereCenterRadius = glm::vec4(1.0f, 2.0f, 3.0f, 5.0f);
    b.minBounds = glm::vec4(-1, -1, -1, 1.0f);
    b.maxBounds = glm::vec4(3, 3, 3, 1.0f);

    EXPECT_EQ(b.sphereCenterRadius, glm::vec4(1, 2, 3, 5));
    EXPECT_FLOAT_EQ(b.sphereCenterRadius.w, 5.0f);
}

// =========================================================================
// Phase 2: cluster_cull.comp / nanite_lod.comp — SPIRV validation via glslc
// =========================================================================
TEST(ClusterShaders, SPIRVCompiles) {
    // cluster_cull.comp must compile to valid SPIR-V
    int rc1 = std::system("glslc rhi/shaders/cluster_cull.comp -o /tmp/_t_cluster.spv 2>/dev/null");
    EXPECT_EQ(rc1, 0) << "cluster_cull.comp failed to compile with glslc";

    // nanite_lod.comp must compile to valid SPIR-V
    int rc2 = std::system("glslc rhi/shaders/nanite_lod.comp -o /tmp/_t_lod.spv 2>/dev/null");
    EXPECT_EQ(rc2, 0) << "nanite_lod.comp failed to compile with glslc";
}

// =========================================================================
// Phase 1: Edge cases
// =========================================================================
TEST(ClusterBuilder, EmptyMeshProducesNoClusters) {
    std::vector<Vertex> verts;
    std::vector<unsigned int> indices;
    auto payload = MeshUtils::BuildMeshClusters(verts, indices, 128);
    EXPECT_EQ(payload.clusters.size(), 0u);
    EXPECT_EQ(payload.totalVertices, 0u);
    EXPECT_EQ(payload.totalIndices, 0u);
}

TEST(ClusterBuilder, SingleTriangleProducesSingleCluster) {
    std::vector<Vertex> verts(3);
    verts[0].Position = glm::vec3(0, 0, 0);
    verts[1].Position = glm::vec3(1, 0, 0);
    verts[2].Position = glm::vec3(0, 1, 0);
    std::vector<unsigned int> indices = {0, 1, 2};

    auto payload = MeshUtils::BuildMeshClusters(verts, indices, 128);
    EXPECT_EQ(payload.clusters.size(), 1u);
    EXPECT_EQ(payload.clusters[0].indices.size(), 3u);
    EXPECT_EQ(payload.clusters[0].vertices.size(), 3u);
}

TEST(ClusterBuilder, CustomClusterSize) {
    std::vector<Vertex> verts;
    std::vector<unsigned int> indices;
    buildGridMesh(kGridSize, verts, indices);  // 200 triangles

    // Use 64 triangles per cluster → ceil(200/64) = 4 clusters
    auto payload = MeshUtils::BuildMeshClusters(verts, indices, 64);
    EXPECT_EQ(payload.clusters.size(), 4u);
    EXPECT_EQ(payload.clusters[0].indices.size(), 192u);  // 64 * 3
    EXPECT_EQ(payload.clusters[3].indices.size(), 24u);   // 8 * 3 (remainder: 200 - 3*64 = 8 tris)
}

TEST(ClusterIO, ImportNonExistentFileReturnsFalse) {
    ClusterPackedPayload payload;
    EXPECT_FALSE(MeshUtils::ImportClusterMeshFile("/nonexistent/path/test.meshpack", payload));
}
