/**
 * =============================================================================
 * MESH OPTIMIZATION USAGE GUIDE
 * =============================================================================
 * 
 * This file demonstrates how to use the fast triangle reordering and vertex
 * clustering algorithms for optimized mesh rendering.
 */

#include "../meshSystem/Mesh.h"
#include <iostream>

namespace MeshOptimizationExamples
{

        std::vector<Mesh> GenerateLODLevels(const Mesh& originalMesh);
    // ========================================================================
    // Example 1: Basic Triangle Reordering (Cache Optimization)
    // ========================================================================
    // Best for: When you want to keep original quality but improve GPU performance
    // Expected gain: 20-50% faster vertex processing
    void Example_TriangleReordering(Mesh& mesh)
    {
        std::cout << "Optimizing triangle order for vertex cache...\n";
        
        // Reorder triangles for a GPU cache size of 24 vertices (typical)
        MeshUtils::OptimizeTriangleOrderingForsyth(mesh.indices, mesh.vertices.size(), 24);
        
        // Update the GPU buffer
        mesh.UpdateVertexBuffer();
        
        std::cout << "Done! Vertex cache hit rate improved.\n";
    }

    // ========================================================================
    // Example 2: Vertex Clustering (Mesh Reduction)
    // ========================================================================
    // Best for: LOD generation, distant objects, performance-critical scenes
    // Expected gain: 50-90% reduction in vertices/triangles
    void Example_VertexClustering(Mesh& mesh)
    {
        std::cout << "Applying vertex clustering for mesh reduction...\n";
        
        // Cluster vertices within 0.1 world units
        // Adjust based on your mesh scale:
        // - Small objects (0.01 - 0.05)
        // - Medium objects (0.05 - 0.2)
        // - Large objects (0.2 - 1.0)
        float clusterCellSize = 0.1f;
        
        MeshUtils::VertexClustering(mesh.vertices, mesh.indices, clusterCellSize);
        
        // Recalculate normals after clustering (vertices moved!)
        mesh.RecalculateNormals();
        
        // Update GPU buffers
        mesh.UpdateVertexBuffer();
        
        std::cout << "Done! Mesh simplified.\n";
    }

    // ========================================================================
    // Example 3: Combined Optimization (Recommended)
    // ========================================================================
    // Best for: Maximum performance gain
    // Expected gain: Both quality retention AND faster rendering
    void Example_CombinedOptimization(Mesh& mesh)
    {
        MeshUtils::MeshOptimizationConfig config;
        config.reorderTriangles = true;      // Enable Forsyth reordering
        config.clusterVertices = false;      // Set true for reduction
        config.clusterCellSize = 0.1f;       // Adjust for desired quality
        config.targetCacheSize = 24;         // Typical GPU cache size
        
        MeshUtils::OptimizeMeshForRendering(mesh, config);
    }

    // ========================================================================
    // Example 4: LOD Generation
   // ========================================================================
    // Create multiple LOD levels for a mesh
    std::vector<Mesh> GenerateLODLevels(const Mesh& originalMesh)
    {   
        std::vector<Mesh> lodLevels;
        // Reserve space to prevent reallocation overhead 
        lodLevels.reserve(4);
        
        // LOD 0: Original quality (just reordered)
        Mesh lod0(originalMesh.vertices, originalMesh.indices, originalMesh.textures);
        MeshUtils::MeshOptimizationConfig config0;
        config0.reorderTriangles = true;
        config0.clusterVertices = false;
        MeshUtils::OptimizeMeshForRendering(lod0, config0);
        lodLevels.push_back(std::move(lod0)); // Transfer ownership using std::move
        
        // LOD 1: Medium quality (25% reduction)
        Mesh lod1(originalMesh.vertices, originalMesh.indices, originalMesh.textures);
        MeshUtils::MeshOptimizationConfig config1;
        config1.reorderTriangles = true;
        config1.clusterVertices = true;
        config1.clusterCellSize = 0.15f;  // Larger cells = more reduction
        MeshUtils::OptimizeMeshForRendering(lod1, config1);
        lodLevels.push_back(std::move(lod1));
        
        // LOD 2: Low quality (50% reduction)
        Mesh lod2(originalMesh.vertices, originalMesh.indices, originalMesh.textures);
        MeshUtils::MeshOptimizationConfig config2;
        config2.reorderTriangles = true;
        config2.clusterVertices = true;
        config2.clusterCellSize = 0.25f;
        MeshUtils::OptimizeMeshForRendering(lod2, config2);
        lodLevels.push_back(std::move(lod2));
        
        // LOD 3: Lowest quality (75% reduction)
        Mesh lod3(originalMesh.vertices, originalMesh.indices, originalMesh.textures);
        MeshUtils::MeshOptimizationConfig config3;
        config3.reorderTriangles = true;
        config3.clusterVertices = true;
        config3.clusterCellSize = 0.5f;
        MeshUtils::OptimizeMeshForRendering(lod3, config3);
        lodLevels.push_back(std::move(lod3));
    
        // Print statistics
        std::cout << "\nLOD Levels Generated:\n";
        for (size_t i = 0; i < lodLevels.size(); i++)
        {
            std::cout << "  LOD" << i << ": " 
                      << lodLevels[i].vertices.size() << " vertices, "
                      << lodLevels[i].indices.size() / 3 << " triangles\n";
        }
    
        return lodLevels; // RVO (Return Value Optimisation) will move the vector out smoothly
    }

    // ========================================================================
    // Example 5: Batch Optimization for Multiple Meshes
    // ========================================================================
    void OptimizeAllMeshes(std::vector<Mesh>& meshes)
    {
        MeshUtils::MeshOptimizationConfig config;
        config.reorderTriangles = true;
        config.clusterVertices = false;  // Keep original quality
        config.targetCacheSize = 24;
        
        for (auto& mesh : meshes)
        {
            MeshUtils::OptimizeMeshForRendering(mesh, config);
        }
    }

    // ========================================================================
    // Example 6: Distance-Based LOD Selection
    // ========================================================================
    Mesh* SelectLODByDistance(const std::vector<Mesh>& lodLevels, 
                              float distanceToCamera)
    {
        if (lodLevels.empty()) return nullptr;
        
        // Distance thresholds (adjust based on your scene scale)
        if (distanceToCamera < 10.0f)
            return const_cast<Mesh*>(&lodLevels[0]);  // Highest quality
        else if (distanceToCamera < 25.0f)
            return const_cast<Mesh*>(&lodLevels[1]);  // Medium
        else if (distanceToCamera < 50.0f)
            return const_cast<Mesh*>(&lodLevels[2]);  // Low
        else
            return const_cast<Mesh*>(&lodLevels[3]);  // Lowest
    }

    // ========================================================================
    // Performance Tips
    // ========================================================================
    /*
     * 1. CACHE SIZE TUNING:
     *    - NVIDIA GPUs: 24-32 vertices
     *    - AMD GPUs: 16-24 vertices
     *    - Integrated: 8-16 vertices
     *    - Default (24) works well for most cases
     *
     * 2. CLUSTER CELL SIZE:
     *    - Measure your mesh bounding box first
     *    - Cell size should be 1-5% of mesh diameter
     *    - Too small: minimal reduction
     *    - Too large: visible artifacts
     *
     * 3. WHEN TO USE:
     *    - Triangle reordering: ALWAYS use on static meshes
     *    - Vertex clustering: Use for LOD or distant objects
     *
     * 4. RUNTIME COST:
     *    - Forsyth reordering: O(n log n) - run once at load time
     *    - Vertex clustering: O(n) - fast enough for runtime LOD
     *
     * 5. EXPECTED RESULTS:
     *    - Cache hit rate: 60-80% (vs 30-40% unoptimized)
     *    - Frame rate improvement: 10-30% for geometry-bound scenes
     *    - Memory reduction: 50-90% with clustering
     */
}
