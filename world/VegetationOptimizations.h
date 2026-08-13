#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <algorithm>

// ============================================================================
// Vegetation Optimization Utilities
// ============================================================================

namespace VegetationOptimizations {
    
    // Distance-based culling for vegetation instances
    class VegetationCuller {
    public:
        struct CullResult {
            size_t visibleTreeStart = 0;
            size_t visibleTreeCount = 0;
            size_t visibleRockStart = 0;
            size_t visibleRockCount = 0;
            size_t culledTreeCount = 0;
            size_t culledRockCount = 0;
        };
        
        CullResult cullByDistance(
            const std::vector<glm::vec3>& treePositions,
            const std::vector<glm::vec3>& rockPositions,
            const glm::vec3& cameraPosition,
            float treeDrawDistance,
            float rockDrawDistance
        ) const {
            CullResult result;
            
            // Sort trees by distance and find cutoff
            std::vector<std::pair<float, size_t>> treeDistances;
            treeDistances.reserve(treePositions.size());
            
            for (size_t i = 0; i < treePositions.size(); i++) {
                float dist = glm::distance(cameraPosition, treePositions[i]);
                treeDistances.push_back({dist, i});
            }
            
            std::sort(treeDistances.begin(), treeDistances.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            
            // Count visible trees
            for (const auto& [dist, idx] : treeDistances) {
                if (dist <= treeDrawDistance) {
                    result.visibleTreeCount++;
                } else {
                    result.culledTreeCount++;
                }
            }
            
            // Sort rocks by distance
            std::vector<std::pair<float, size_t>> rockDistances;
            rockDistances.reserve(rockPositions.size());
            
            for (size_t i = 0; i < rockPositions.size(); i++) {
                float dist = glm::distance(cameraPosition, rockPositions[i]);
                rockDistances.push_back({dist, i});
            }
            
            std::sort(rockDistances.begin(), rockDistances.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            
            // Count visible rocks
            for (const auto& [dist, idx] : rockDistances) {
                if (dist <= rockDrawDistance) {
                    result.visibleRockCount++;
                } else {
                    result.culledRockCount++;
                }
            }
            
            return result;
        }
        
        // Frustum culling for vegetation
        bool isInFrustum(const glm::vec3& position, float radius,
                        const glm::mat4& vpMatrix, float aspectRatio) const {
            // Transform position to clip space
            glm::vec4 clipPos = vpMatrix * glm::vec4(position, 1.0f);
            
            // Check if behind near plane or beyond far plane
            if (clipPos.w < 0.0f) return false;
            
            // Check horizontal bounds
            float ndcX = clipPos.x / clipPos.w;
            if (ndcX - radius / clipPos.w > 1.0f || ndcX + radius / clipPos.w < -1.0f) {
                return false;
            }
            
            // Check vertical bounds
            float ndcY = clipPos.y / clipPos.w;
            if (ndcY - radius / (clipPos.w * aspectRatio) > 1.0f || 
                ndcY + radius / (clipPos.w * aspectRatio) < -1.0f) {
                return false;
            }
            
            return true;
        }
    };
    
    // LOD system for vegetation
    class VegetationLOD {
    public:
        struct LODConfig {
            float lod0Distance = 50.0f;   // Full detail
            float lod1Distance = 100.0f;  // Medium detail
            float lod2Distance = 200.0f;  // Low detail
            float lod3Distance = 300.0f;  // Very low detail
            
            float lod0Scale = 1.0f;
            float lod1Scale = 0.9f;
            float lod2Scale = 0.8f;
            float lod3Scale = 0.7f;
        };
        
        static int getLODLevel(float distance, const LODConfig& config = LODConfig()) {
            if (distance < config.lod0Distance) return 0;
            if (distance < config.lod1Distance) return 1;
            if (distance < config.lod2Distance) return 2;
            if (distance < config.lod3Distance) return 3;
            return 4; // Beyond draw distance
        }
        
        static float getLODScale(int lodLevel, const LODConfig& config = LODConfig()) {
            switch (lodLevel) {
                case 0: return config.lod0Scale;
                case 1: return config.lod1Scale;
                case 2: return config.lod2Scale;
                case 3: return config.lod3Scale;
                default: return 0.0f; // Don't render
            }
        }
        
        // Smooth LOD transition
        static float smoothTransition(float distance, float lod0Dist, float lod1Dist) {
            if (distance < lod0Dist - 10.0f) return 0.0f;
            if (distance > lod1Dist + 10.0f) return 1.0f;
            
            float t = (distance - (lod0Dist - 10.0f)) / ((lod1Dist + 10.0f) - (lod0Dist - 10.0f));
            return t * t * (3.0f - 2.0f * t); // Smoothstep
        }
    };
    
    // Instanced rendering optimization
    class InstancedRenderer {
    public:
        struct InstanceData {
            glm::mat4 modelMatrix;
            glm::vec3 color;
            float alpha;
        };
        
        // Batch instances by type for efficient rendering
        template<typename T>
        static std::vector<std::vector<size_t>> batchByType(
            const std::vector<T>& instances,
            const std::vector<int>& types
        ) {
            std::vector<std::vector<size_t>> batches;
            
            for (size_t i = 0; i < instances.size(); i++) {
                int type = types[i];
                if (type >= static_cast<int>(batches.size())) {
                    batches.resize(type + 1);
                }
                batches[type].push_back(i);
            }
            
            return batches;
        }
        
        // Sort instances front-to-back for better GPU cache utilization
        static std::vector<size_t> sortByDistance(
            const std::vector<glm::vec3>& positions,
            const glm::vec3& cameraPosition
        ) {
            std::vector<std::pair<float, size_t>> distances;
            distances.reserve(positions.size());
            
            for (size_t i = 0; i < positions.size(); i++) {
                float dist = glm::distance(cameraPosition, positions[i]);
                distances.push_back({dist, i});
            }
            
            std::sort(distances.begin(), distances.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            
            std::vector<size_t> sortedIndices;
            sortedIndices.reserve(positions.size());
            for (const auto& [dist, idx] : distances) {
                sortedIndices.push_back(idx);
            }
            
            return sortedIndices;
        }
    };
    
    // Performance metrics for vegetation
    struct VegetationMetrics {
        size_t totalTrees = 0;
        size_t visibleTrees = 0;
        size_t culledTrees = 0;
        size_t totalRocks = 0;
        size_t visibleRocks = 0;
        size_t culledRocks = 0;
        float cullTime = 0.0f;
        float renderTime = 0.0f;
        size_t drawCalls = 0;
        
        void reset() {
            totalTrees = 0;
            visibleTrees = 0;
            culledTrees = 0;
            totalRocks = 0;
            visibleRocks = 0;
            culledRocks = 0;
            cullTime = 0.0f;
            renderTime = 0.0f;
            drawCalls = 0;
        }
        
        float getCullEfficiency() const {
            if (totalTrees + totalRocks == 0) return 0.0f;
            size_t totalCulled = culledTrees + culledRocks;
            size_t total = totalTrees + totalRocks;
            return static_cast<float>(totalCulled) / static_cast<float>(total) * 100.0f;
        }
    };
    
} // namespace VegetationOptimizations