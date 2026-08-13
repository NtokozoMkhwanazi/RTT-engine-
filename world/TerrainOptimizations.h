#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <cmath>

// ============================================================================
// Terrain Optimization Utilities
// ============================================================================

namespace TerrainOptimizations {
    
    // Frustum culling for terrain chunks
    class FrustumCuller {
    public:
        struct Plane {
            glm::vec3 normal;
            float distance;
        };
        
        void update(const glm::mat4& vpMatrix) {
            // Extract frustum planes from view-projection matrix
            // Left plane
            m_planes[0].normal.x = vpMatrix[0][3] + vpMatrix[0][0];
            m_planes[0].normal.y = vpMatrix[1][3] + vpMatrix[1][0];
            m_planes[0].normal.z = vpMatrix[2][3] + vpMatrix[2][0];
            m_planes[0].distance = vpMatrix[3][3] + vpMatrix[3][0];
            
            // Right plane
            m_planes[1].normal.x = vpMatrix[0][3] - vpMatrix[0][0];
            m_planes[1].normal.y = vpMatrix[1][3] - vpMatrix[1][0];
            m_planes[1].normal.z = vpMatrix[2][3] - vpMatrix[2][0];
            m_planes[1].distance = vpMatrix[3][3] - vpMatrix[3][0];
            
            // Bottom plane
            m_planes[2].normal.x = vpMatrix[0][3] + vpMatrix[0][1];
            m_planes[2].normal.y = vpMatrix[1][3] + vpMatrix[1][1];
            m_planes[2].normal.z = vpMatrix[2][3] + vpMatrix[2][1];
            m_planes[2].distance = vpMatrix[3][3] + vpMatrix[3][1];
            
            // Top plane
            m_planes[3].normal.x = vpMatrix[0][3] - vpMatrix[0][1];
            m_planes[3].normal.y = vpMatrix[1][3] - vpMatrix[1][1];
            m_planes[3].normal.z = vpMatrix[2][3] - vpMatrix[2][1];
            m_planes[3].distance = vpMatrix[3][3] - vpMatrix[3][1];
            
            // Near plane
            m_planes[4].normal.x = vpMatrix[0][3] + vpMatrix[0][2];
            m_planes[4].normal.y = vpMatrix[1][3] + vpMatrix[1][2];
            m_planes[4].normal.z = vpMatrix[2][3] + vpMatrix[2][2];
            m_planes[4].distance = vpMatrix[3][3] + vpMatrix[3][2];
            
            // Far plane
            m_planes[5].normal.x = vpMatrix[0][3] - vpMatrix[0][2];
            m_planes[5].normal.y = vpMatrix[1][3] - vpMatrix[1][2];
            m_planes[5].normal.z = vpMatrix[2][3] - vpMatrix[2][2];
            m_planes[5].distance = vpMatrix[3][3] - vpMatrix[3][2];
            
            // Normalize planes
            for (int i = 0; i < 6; i++) {
                float length = glm::length(m_planes[i].normal);
                if (length > 0.0001f) {
                    m_planes[i].normal /= length;
                    m_planes[i].distance /= length;
                }
            }
        }
        
        bool isBoxInFrustum(const glm::vec3& center, const glm::vec3& halfExtents) const {
            for (int i = 0; i < 6; i++) {
                // Compute the projection of the half-extents onto the plane normal
                float r = halfExtents.x * std::abs(m_planes[i].normal.x) +
                         halfExtents.y * std::abs(m_planes[i].normal.y) +
                         halfExtents.z * std::abs(m_planes[i].normal.z);
                
                // Return false if the box is behind the plane
                if (glm::dot(m_planes[i].normal, center) + m_planes[i].distance + r < 0.0f) {
                    return false;
                }
            }
            return true;
        }
        
        bool isSphereInFrustum(const glm::vec3& center, float radius) const {
            for (int i = 0; i < 6; i++) {
                float distance = glm::dot(m_planes[i].normal, center) + m_planes[i].distance;
                if (distance + radius < 0.0f) {
                    return false;
                }
            }
            return true;
        }
        
    private:
        Plane m_planes[6];
    };
    
    // LOD calculation based on distance
    inline int calculateLODLevel(float distance, const std::vector<float>& lodDistances) {
        int lodLevel = 0;
        for (size_t i = 0; i < lodDistances.size(); i++) {
            if (distance > lodDistances[i]) {
                lodLevel = static_cast<int>(i) + 1;
            } else {
                break;
            }
        }
        return lodLevel;
    }
    
    // Get resolution for LOD level
    inline int getLODResolution(int baseResolution, int lodLevel) {
        return std::max(4, baseResolution >> lodLevel); // Divide resolution by 2 for each LOD level
    }
    
    // Smooth height transition between LOD levels
    inline float smoothLODTransition(float height1, float height2, float t) {
        // t is 0-1, where 0 = fully lod1, 1 = fully lod2
        float smoothT = t * t * (3.0f - 2.0f * t); // Smoothstep
        return glm::mix(height1, height2, smoothT);
    }
    
    // Occlusion culling helper
    class OcclusionCuller {
    public:
        struct OcclusionQuery {
            unsigned int queryID;
            bool visible;
            bool queryActive;
        };
        
        void initialize() {
            // Pre-allocate query pool
            m_queries.resize(256);
            for (auto& q : m_queries) {
                glGenQueries(1, &q.queryID);
                q.visible = true;
                q.queryActive = false;
            }
        }
        
        void beginQuery(unsigned int chunkIndex) {
            if (chunkIndex >= m_queries.size()) return;
            
            auto& query = m_queries[chunkIndex];
            if (!query.queryActive) {
                glBeginQuery(GL_ANY_SAMPLES_PASSED, query.queryID);
                // Draw a simple bounding box
                query.queryActive = true;
            }
        }
        
        void endQuery(unsigned int chunkIndex) {
            if (chunkIndex >= m_queries.size()) return;
            
            auto& query = m_queries[chunkIndex];
            if (query.queryActive) {
                glEndQuery(GL_ANY_SAMPLES_PASSED);
                query.queryActive = false;
            }
        }
        
        bool wasVisible(unsigned int chunkIndex) const {
            if (chunkIndex >= m_queries.size()) return true;
            
            const auto& query = m_queries[chunkIndex];
            unsigned int passed = 0;
            glGetQueryObjectuiv(query.queryID, GL_QUERY_RESULT, &passed);
            return passed != 0;
        }
        
        void cleanup() {
            for (auto& q : m_queries) {
                glDeleteQueries(1, &q.queryID);
            }
            m_queries.clear();
        }
        
    private:
        std::vector<OcclusionQuery> m_queries;
    };
    
    // Performance metrics tracker
    struct TerrainMetrics {
        float updateTime = 0.0f;
        float renderTime = 0.0f;
        size_t activeChunks = 0;
        size_t culledChunks = 0;
        size_t totalTriangles = 0;
        
        void reset() {
            updateTime = 0.0f;
            renderTime = 0.0f;
            activeChunks = 0;
            culledChunks = 0;
            totalTriangles = 0;
        }
    };
    
} // namespace TerrainOptimizations