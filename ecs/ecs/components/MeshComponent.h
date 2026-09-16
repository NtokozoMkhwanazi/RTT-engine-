#pragma once

#include "../ECS.h"
#include <glm/glm.hpp>
#include <string>

namespace ecs {

/**
 * Mesh type enumeration for procedural meshes
 */
enum class MeshType : int {
    Cube = 0,
    Sphere = 1,
    Plane = 2,
    Cylinder = 3,
    Cone = 4,
    Torus = 5,
    Custom = 100  // Loaded from file
};

/**
 * Mesh Component - Reference to a mesh asset
 */
struct MeshComponent : public Component {
    int meshID = -1;              // Reference to loaded mesh
    MeshType meshType = MeshType::Cube;  // Type of procedural mesh
    bool visible = true;
    bool castShadow = true;
    bool receiveShadow = true;
    
    // Material properties
    glm::vec3 color{1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float alpha = 1.0f;
    
    // Rendering options
    bool useInstancing = false;
    int instanceCount = 1;
    
    MeshComponent() = default;
    MeshComponent(int id) : meshID(id) {}
    
    /**
     * Check if mesh is valid
     */
    bool isValid() const { return meshID >= 0; }
};

/**
 * Skinned Mesh Component - For animated characters
 */
struct SkinnedMeshComponent : public Component {
    int meshID = -1;
    int skeletonID = -1;          // Reference to skeleton
    bool visible = true;
    bool castShadow = true;
    bool receiveShadow = true;
    
    // Bone texture for GPU skinning
    int boneTextureWidth = 0;
    int boneTextureHeight = 0;
    unsigned int boneTextureID = 0;
    
    SkinnedMeshComponent() = default;
    SkinnedMeshComponent(int meshId, int skeletonId) 
        : meshID(meshId), skeletonID(skeletonId) {}
};

/**
 * Instance Data Component - For instanced rendering
 */
struct InstanceDataComponent : public Component {
    std::vector<glm::mat4> instanceMatrices;
    std::vector<glm::vec3> instanceColors;
    
    InstanceDataComponent() = default;
    InstanceDataComponent(size_t count) {
        instanceMatrices.resize(count, glm::mat4(1.0f));
        instanceColors.resize(count, glm::vec3(1.0f));
    }
};

/**
 * LOD Component - Level of Detail settings
 */
struct LODComponent : public Component {
    float lodDistances[4] = {0.0f, 20.0f, 50.0f, 100.0f};
    int currentLOD = 0;
    int maxLOD = 3;
    bool autoLOD = true;
    
    LODComponent() = default;
    LODComponent(const float distances[4]) {
        for (int i = 0; i < 4; ++i) {
            lodDistances[i] = distances[i];
        }
    }
};

} // namespace ecs
