#include "terrain_bridge.h"

#include "mesh_builder.h"   // CreateConeVerts (CPU vertex data; no GL context)
#include <cstddef>
#include <cstdint>
#include <cstring>          // std::memcpy into OffscreenInstance::model
#include <glm/gtc/matrix_transform.hpp> // glm::translate / glm::scale (foliage instances)

namespace Render {

RHI::OffscreenMesh bakeTerrainChunk(int chunkX, int chunkY, int bakeRes,
    float chunkSize, float heightScale,
    const std::function<float(float, float)>& getHeight,
    const std::function<glm::vec3(float, float)>& getNormal,
    const std::vector<uint8_t>& albedoPixels, int albedoW, int albedoH) {
    if (bakeRes < 1) bakeRes = 1;
    const int   n    = bakeRes + 1;
    const float step = chunkSize / (float)bakeRes;

    RHI::OffscreenMesh om;
    // Interleaved [pos.xyz][normal.xyz][uv] = 8 floats/vert (matches pbr_mesh.vert).
    om.vertices.resize((size_t)n * n * 8);
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const float worldX = chunkX * chunkSize + x * step;
            const float worldZ = chunkY * chunkSize + z * step;
            const float h       = getHeight(worldX, worldZ) * heightScale; // baked into pos.y
            const glm::vec3 nrm = getNormal(worldX, worldZ);
            const size_t o = ((size_t)z * n + x) * 8;
            om.vertices[o + 0] = worldX;
            om.vertices[o + 1] = h;
            om.vertices[o + 2] = worldZ;
            om.vertices[o + 3] = nrm.x;
            om.vertices[o + 4] = nrm.y;
            om.vertices[o + 5] = nrm.z;
            om.vertices[o + 6] = x / (float)bakeRes;
            om.vertices[o + 7] = z / (float)bakeRes;
        }
    }
    // Standard CCW grid: 2 triangles per cell, winding matches the generic PBR.
    om.indices.reserve((size_t)bakeRes * bakeRes * 6);
    for (int z = 0; z < bakeRes; ++z) {
        for (int x = 0; x < bakeRes; ++x) {
            const uint32_t tl = (uint32_t)(z * n + x);
            const uint32_t tr = tl + 1;
            const uint32_t bl = (uint32_t)((z + 1) * n + x);
            const uint32_t br = bl + 1;
            om.indices.push_back(tl); om.indices.push_back(bl); om.indices.push_back(tr);
            om.indices.push_back(tr);  om.indices.push_back(bl); om.indices.push_back(br);
        }
    }
    om.texturePixels.assign(albedoPixels.begin(), albedoPixels.end());
    om.textureWidth  = albedoW;
    om.textureHeight = albedoH;
    om.version = 1;
    return om;
}

RHI::OffscreenMesh buildFoliageMesh(const std::vector<VegetationSystem::Tree>& trees,
    const std::vector<uint8_t>& albedoPixels, int albedoW, int albedoH) {
    // Unit cone (apex +0.5, base -0.5) -> CPU verts, reused by every instance.
    static const MeshBuilder::Verts cone = MeshBuilder::CreateConeVerts(16, 1.0f, 1.0f);
    RHI::OffscreenMesh om;
    om.vertices.assign(cone.vertices.begin(), cone.vertices.end());
    om.indices.assign(cone.indices.begin(), cone.indices.end());
    if (!albedoPixels.empty()) {
        om.texturePixels.assign(albedoPixels.begin(), albedoPixels.end());
        om.textureWidth  = albedoW;
        om.textureHeight = albedoH;
    } else {
        static const uint8_t green[4] = {0, 128, 0, 255}; // flat tree-green fallback
        om.texturePixels.assign(green, green + 4);
        om.textureWidth  = 1;
        om.textureHeight = 1;
    }
    om.version = 1;
    om.instances.clear();
    for (const auto& t : trees) {
        RHI::OffscreenInstance inst{};
        // Scale (radius, height, radius) then translate so the cone base rests on
        // the tree's ground position (position.y).
        glm::mat4 m = glm::translate(glm::mat4(1.0f),
                                     glm::vec3(t.position.x,
                                               t.position.y + 0.5f * t.height,
                                               t.position.z))
                    * glm::scale(glm::mat4(1.0f), glm::vec3(t.radius, t.height, t.radius));
        std::memcpy(inst.model, &m[0][0], sizeof(inst.model));
        inst.color[0] = inst.color[1] = inst.color[2] = inst.color[3] = 1.0f;
        om.instances.push_back(inst);
    }
    return om;
}

} // namespace Render
