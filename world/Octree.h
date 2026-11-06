#pragma once

// forUI.txt Point 1: AABB Octree Spatial Partitioning System.
// Recursively divides world space into 8 sub-octants so viewport prop picking
// confines ray-box tests to nodes along the cursor's path (O(N) -> O(log N)).
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <array>
#include <cmath>
#include <algorithm>
#include <cstddef>
#include "SimpleWorldRenderer.h" // SimpleModelInstance + SimpleWorldRenderer::raycastProps

class OctreeNode {
public:
    glm::vec3 boxMin;
    glm::vec3 boxMax;
    std::vector<size_t> instanceIndices; // indices into SimpleWorldRenderer::m_instances
    std::array<std::unique_ptr<OctreeNode>, 8> children;
    bool isLeaf = true;

    OctreeNode(const glm::vec3& minBounds, const glm::vec3& maxBounds)
        : boxMin(minBounds), boxMax(maxBounds) {
        for (auto& child : children) child = nullptr;
    }

    // Slab-watertight ray/AABB intersection (Slabs method). Returns true when the
    // ray pierces this node's bounding box; tMinOut is the entry distance.
    bool intersectRay(const glm::vec3& orig, const glm::vec3& dir, float& tMinOut) const {
        float tmin = -1e9f;
        float tmax =  1e9f;
        for (int a = 0; a < 3; ++a) {
            if (std::abs(dir[a]) < 1e-8f) {
                if (orig[a] < boxMin[a] || orig[a] > boxMax[a]) return false;
            } else {
                float t1 = (boxMin[a] - orig[a]) / dir[a];
                float t2 = (boxMax[a] - orig[a]) / dir[a];
                if (t1 > t2) std::swap(t1, t2);
                tmin = std::max(tmin, t1);
                tmax = std::min(tmax, t2);
                if (tmin > tmax || tmax < 0.0f) return false;
            }
        }
        tMinOut = tmin < 0.0f ? tmax : tmin;
        return true;
    }
};

class WorldSpatialOctree {
private:
    std::unique_ptr<OctreeNode> m_root;
    const std::vector<SimpleModelInstance>* m_sourceInstances = nullptr;
    size_t m_maxObjectsPerNode = 16;
    int m_maxDepth = 6;

    void subdivide(OctreeNode* node, int currentDepth) {
        if (node->instanceIndices.size() <= m_maxObjectsPerNode || currentDepth >= m_maxDepth) {
            return;
        }

        node->isLeaf = false;
        glm::vec3 center = (node->boxMin + node->boxMax) * 0.5f;

        for (int i = 0; i < 8; ++i) {
            glm::vec3 cMin, cMax;
            cMin.x = (i & 1) ? center.x : node->boxMin.x;
            cMin.y = (i & 2) ? center.y : node->boxMin.y;
            cMin.z = (i & 4) ? center.z : node->boxMin.z;

            cMax.x = (i & 1) ? node->boxMax.x : center.x;
            cMax.y = (i & 2) ? node->boxMax.y : center.y;
            cMax.z = (i & 4) ? node->boxMax.z : center.z;

            node->children[i] = std::make_unique<OctreeNode>(cMin, cMax);

            // Sort parent elements into matching spatial sub-octants
            for (size_t idx : node->instanceIndices) {
                glm::vec3 p = glm::vec3((*m_sourceInstances)[idx].modelMatrix[3]); // col3 = position
                if (p.x >= cMin.x && p.x <= cMax.x &&
                    p.y >= cMin.y && p.y <= cMax.y &&
                    p.z >= cMin.z && p.z <= cMax.z) {
                    node->children[i]->instanceIndices.push_back(idx);
                }
            }

            subdivide(node->children[i].get(), currentDepth + 1);
        }

        node->instanceIndices.clear(); // free parent index footprints once sorted
    }

    void raycastQuery(const OctreeNode* node, const glm::vec3& orig, const glm::vec3& dir,
                      const SimpleWorldRenderer& renderer, float& closestDist,
                      size_t& bestIdx, bool& hit) const {
        float nodeT;
        if (!node->intersectRay(orig, dir, nodeT) || nodeT >= closestDist) return;

        if (node->isLeaf) {
            for (size_t idx : node->instanceIndices) {
                float outD; size_t outIdx; glm::vec3 hp, hn;
                if (renderer.raycastProps(orig, dir, closestDist, outD, outIdx, hp, hn)) {
                    if (outD < closestDist) {
                        closestDist = outD;
                        bestIdx = idx;
                        hit = true;
                    }
                }
            }
            return;
        }

        for (int i = 0; i < 8; ++i) {
            if (node->children[i]) {
                raycastQuery(node->children[i].get(), orig, dir, renderer, closestDist, bestIdx, hit);
            }
        }
    }

public:
    const OctreeNode* GetRootNode() const { return m_root.get(); }

    void RebuildIndex(const std::vector<SimpleModelInstance>& instances,
                      const glm::vec3& worldMin, const glm::vec3& worldMax) {
        m_sourceInstances = &instances;
        m_root = std::make_unique<OctreeNode>(worldMin, worldMax);
        m_root->instanceIndices.reserve(instances.size());
        for (size_t i = 0; i < instances.size(); ++i) {
            m_root->instanceIndices.push_back(i);
        }
        subdivide(m_root.get(), 0);
    }

    bool Raycast(const glm::vec3& orig, const glm::vec3& dir,
                 const SimpleWorldRenderer& renderer, float maxDist,
                 float& outDist, size_t& outIndex) const {
        if (!m_root) return false;
        bool hit = false;
        size_t bestIdx = 0;
        float closestDist = maxDist;

        raycastQuery(m_root.get(), orig, dir, renderer, closestDist, bestIdx, hit);

        if (hit) {
            outDist = closestDist;
            outIndex = bestIdx;
        }
        return hit;
    }
};
