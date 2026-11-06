#pragma once
// terrain_bridge.h — Phase B: world/Terrain -> OffscreenScene mesh bridge.
//
// Pure-CPU (NO OpenGL context required). bakeTerrainChunk turns a chunk's
// heightfield (sampled via caller-supplied getHeight/getNormal callbacks that
// take WORLD coordinates) into a GENERIC OffscreenMesh whose vertices are
// world-space and where height is BAKED into pos.y.
//
// This is necessary because the offscreen generic PBR (GL pbrFsSrc in
// renderOffsceneScene, Vulkan pbr_mesh in renderFrameSceneScene) has NO heightmap
// sampler — only shaderSystem/terrain_splat.* displaces in-shader. Baking the
// height into the vertex buffer makes terrain visible on rhivulkan (which
// otherwise receives flat y=0 verts -> invisible) and lets BOTH backends render
// the identical generic mesh -> GL<->Vulkan pixel parity without a dedicated GL
// terrain-splat shader. The bytes (albedo via stbi, vertices, indices) are
// GL-context-free -> bit-identical on both backends.

#include <functional>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>
#include "rhi/RHI.h"   // OffscreenMesh
#include "world/VegetationSystem.h" // VegetationSystem::Tree (foliage bridge)

namespace Render {

// Bake a (bakeRes+1)^2 pos/normal/uv grid over the chunk footprint
// [chunkX*chunkSize, (chunkX+1)*chunkSize) x [chunkY*chunkSize, (chunkY+1)*chunkSize).
// For each vertex: pos = (worldX, getHeight(world)*heightScale, worldZ),
// normal = getNormal(world), uv = (x/bakeRes, z/bakeRes). Indices are a standard
// CCW grid (2 triangles per cell). albedoPixels/albedoW/albedoH become the mesh's
// texturePixels (sampled as the albedo by the generic PBR on both backends).
RHI::OffscreenMesh bakeTerrainChunk(int chunkX, int chunkY, int bakeRes,
    float chunkSize, float heightScale,
    const std::function<float(float, float)>& getHeight,
    const std::function<glm::vec3(float, float)>& getNormal,
    const std::vector<uint8_t>& albedoPixels, int albedoW, int albedoH);

// Bridge procedural vegetation (VegetationSystem::Tree list) into the offscreen
// scene as a single generic OffscreenMesh: a procedural cone per tree placed via
// per-tree instance transforms (base on the ground, scaled to Tree.radius /
// Tree.height). Pure CPU (MeshBuilder::CreateConeVerts) -> identical bytes on GL
// and Vulkan. Height here is instance placement, not a heightfield.
RHI::OffscreenMesh buildFoliageMesh(const std::vector<VegetationSystem::Tree>& trees,
    const std::vector<uint8_t>& albedoPixels, int albedoW, int albedoH);

} // namespace Render
