#define GLM_ENABLE_EXPERIMENTAL

#include "Model.h"
#include "boneSystem/DebugDraw.h"
#include "shaderSystem/stb_image.h"
#include "shaderSystem/load_texture_image.h"
#include "boneSystem/BoneName.h"
#include "renderer/DefaultTexture.h"
#include "renderer/Renderer.h"
#include "../editor/gl_context_lifecycle.h"
#include "TextureCompression.h"

#include <iostream>
#include <fstream>
#include <functional>
#include <algorithm>
#include <cstdio>

#include <assimp/postprocess.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/string_cast.hpp>

#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <Imath/half.h>

// =====================================================
// Utility
// =====================================================

// Decode a native linear-HDR OpenEXR map into RGBA8 pixels (row 0 = TOP,
// matching the stbi-flipped JPG loads and the RHI texture convention). Reads
// R/G/B/A (or single-channel Y) half data via the system OpenEXR library -
// stb_image cannot decode EXR. Returns false on any failure so the caller
// falls back to the referenced (JPG/PNG) file.
static bool LoadEXRPixelsRGBA8(const std::string& path, int& outW, int& outH,
                               std::vector<uint8_t>& outRGBA) {
    try {
        Imf::InputFile file(path.c_str());
        const Imath::Box2i& dw = file.header().dataWindow();
        const int w = dw.max.x - dw.min.x + 1;
        const int h = dw.max.y - dw.min.y + 1;
        if (w <= 0 || h <= 0) return false;

        // Zero-filled RGBA half buffer: channels missing from the file (e.g.
        // a single-channel roughness EXR) read as 0 / alpha defaults to 1.
        std::vector<half> pixels(static_cast<size_t>(w) * h * 4, half(0.0f));
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = half(1.0f);
        const int xStride = static_cast<int>(sizeof(half) * 4);
        const int64_t yStride = static_cast<int64_t>(xStride) * w;
        const char* base = reinterpret_cast<const char*>(pixels.data()) -
                           (dw.min.x + static_cast<int64_t>(dw.min.y) * w) * xStride;
        Imf::FrameBuffer fb;
        const Imf::ChannelList& chans = file.header().channels();
        auto addChan = [&](const char* name, int comp) {
            if (chans.findChannel(name))
                fb.insert(name, Imf::Slice(Imf::HALF,
                                           const_cast<char*>(base) + comp * sizeof(half),
                                           xStride, yStride));
        };
        addChan("R", 0); addChan("G", 1); addChan("B", 2); addChan("A", 3);
        // Single-channel EXRs (roughness stores its value in "Y") still fill
        // the red component.
        if (!chans.findChannel("R")) addChan("Y", 0);
        file.setFrameBuffer(fb);
        file.readPixels(dw.min.y, dw.max.y);

        outW = w;
        outH = h;
        outRGBA.resize(static_cast<size_t>(w) * h * 4);
        // OpenEXR's origin is bottom-left: flip vertically so row 0 is the
        // image TOP (the RHI texture convention / stbi-flipped loads).
        for (int y = 0; y < h; ++y) {
            const size_t src = static_cast<size_t>(h - 1 - y) * w * 4;
            const size_t dst = static_cast<size_t>(y) * w * 4;
            for (size_t i = 0; i < static_cast<size_t>(w) * 4; ++i) {
                outRGBA[dst + i] = static_cast<uint8_t>(
                    std::clamp(float(pixels[src + i]) * 255.0f, 0.0f, 255.0f));
            }
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Forward declaration - defined near the hierarchy builders below.
static void BuildCollapsedHierarchyStatic(AssimpNodeData &dest, const aiNode *src);

std::unordered_map<std::string, int>
BuildNormalizedBoneMap(const Skeleton &skeleton)
{
    std::unordered_map<std::string, int> result;
    for (const auto &[boneName, index] : skeleton.boneMapping)
        result[NormalizeBoneName(boneName)] = index;

    return result;
}

void Model::BuildNodeBoneMap(
    AssimpNodeData &node,
    const std::unordered_map<std::string, int> &normBoneMap)
{
    // The map is normalized ("hips") but the hierarchy stores raw names
    // ("mixamorig:Hips") - normalize the lookup or every boneIndex stays -1
    // and the animator can never write the bone matrices (frozen bind pose).
    auto it = normBoneMap.find(NormalizeBoneName(node.name));
    node.boneIndex = (it != normBoneMap.end()) ? it->second : -1;

    for (auto &child : node.children)
        BuildNodeBoneMap(child, normBoneMap);
}

static const aiNode *FindNodeHierarchy(const aiNode *node, const std::string &target)
{
    if (!node)
        return nullptr;

    std::string n = NormalizeBoneName(node->mName.C_Str());
    if (n == target)
        return node;

    for (unsigned i = 0; i < node->mNumChildren; ++i)
    {
        const aiNode *found = FindNodeHierarchy(node->mChildren[i], target);
        if (found)
            return found;
    }

    return nullptr;
}

// =====================================================
glm::mat4 Model::aiMat4ToGlm(const aiMatrix4x4 &m)
{
    glm::mat4 r;

    // Assimp row-major → GLM column-major
    r[0][0] = m.a1; r[1][0] = m.a2; r[2][0] = m.a3; r[3][0] = m.a4;
    r[0][1] = m.b1; r[1][1] = m.b2; r[2][1] = m.b3; r[3][1] = m.b4;
    r[0][2] = m.c1; r[1][2] = m.c2; r[2][2] = m.c3; r[3][2] = m.c4;
    r[0][3] = m.d1; r[1][3] = m.d2; r[2][3] = m.d3; r[3][3] = m.d4;

    return r;
}

// =====================================================
// Find bind pose transform from hierarchy
// =====================================================
static bool FindBindPoseInHierarchy(
    const AssimpNodeData &node,
    const std::string &boneName,
    glm::mat4 &out)
{
    if (node.name == boneName)
    {
        out = node.transform;
        return true;
    }

    for (const auto &child : node.children)
    {
        if (FindBindPoseInHierarchy(child, boneName, out))
            return true;
    }

    return false;
}

// =====================================================
// Root bone detection
// =====================================================
int FindRootBoneIndex(const Skeleton &skeleton)
{
    std::function<int(const AssimpNodeData &, int)> walk =
        [&](const AssimpNodeData &node, int parentBone) -> int
    {
        int thisBone = node.boneIndex;

        // If this node IS a bone and parent is NOT a bone → ROOT
        if (thisBone >= 0 && parentBone == -1)
            return thisBone;

        for (const auto &child : node.children)
        {
            int r = walk(child, thisBone >= 0 ? thisBone : parentBone);
            if (r != -1)
                return r;
        }
        return -1;
    };

    return walk(skeleton.rootNode, -1);
}

// =====================================================
// Constructor/Destructor
// =====================================================
Model::Model(const std::string &path)
{
    if (debugOutput) {
        std::cout << "[Model] Loading: " << path << std::endl;
    }

    // Only load if path is not empty
    if (!path.empty()) {
        loadModel(path);
        calculateBoundingVolumes();
    }

    if (debugOutput) {
        int influenced = 0;
        for (auto &mesh : meshes)
            for (auto &v : mesh.vertices)
                if (v.Weights.x + v.Weights.y + v.Weights.z + v.Weights.w > 0.0f)
                    influenced++;

        rootBoneIndex = FindRootBoneIndex(m_Skeleton);

        std::cout << "Vertices influenced by bones: " << influenced << "\n";
        std::cout << "Root bone index: " << rootBoneIndex << "\n";
        std::cout << "Bounding box: " << glm::to_string(boundingBox.min) 
                  << " to " << glm::to_string(boundingBox.max) << "\n";
        std::cout << "Bounding sphere: center=" << glm::to_string(boundingSphere.center) 
                  << ", radius=" << boundingSphere.radius << "\n";

        if (m_Skeleton.rootBoneIndex == -1)
        {
            std::cerr << "WARNING: No root bone found. Animations disabled.\n";
        }

        std::cout << "Model loaded: " << path
                  << " | Bones: " << m_Skeleton.bones.size()
                  << " | Meshes: " << meshes.size()
                  << " | Triangles: " << GetTotalTriangleCount()
                  << " | Vertices: " << GetVertexCount()
                  << "\n";
    }
}

Model::~Model()
{
    // OpenGL resources are cleaned up automatically when context is destroyed.
    // Guard on glctx::isAlive(): global model caches (ResourceManager) are
    // destroyed at static-destruction time, AFTER the RHI has torn down the
    // context - issuing GL calls there is a SIGSEGV.
    if (boneTexID != 0 && glctx::isAlive()) {
        glDeleteTextures(1, &boneTexID);
    }
}

// =====================================================
// Factory Methods
// =====================================================
Model* Model::CreateFromVAO(GLuint VAO, GLsizei indexCount)
{
    Model* model = new Model("");
    model->setDebugVAO(VAO);
    model->setDebugIndexCount(indexCount);
    return model;
}

// =====================================================
// Async Model Loading
// =====================================================
Model::AsyncLoadHandle Model::LoadAsync(const std::string& path)
{
    AsyncLoadHandle handle;
    handle.progress = 0.0f;
    
    auto dataPtr = std::make_shared<float>(0.0f);
    
    handle.future = std::async(std::launch::async, [path, dataPtr]() -> std::unique_ptr<AsyncModelData> {
        try {
            auto result = LoadModelData(path, dataPtr.get());
            if (!result->success && result->error.empty()) {
                result->error = "Assimp returned no scene or no root node";
            }
            return result;
        } catch (const std::exception& e) {
            auto result = std::make_unique<AsyncModelData>();
            result->success = false;
            result->error = std::string("Exception: ") + e.what();
            return result;
        } catch (...) {
            auto result = std::make_unique<AsyncModelData>();
            result->success = false;
            result->error = "Unknown exception during async loading";
            return result;
        }
    });
    
    return handle;
}

std::unique_ptr<AsyncModelData> Model::LoadModelData(const std::string& path, float* outProgress)
{
    auto data = std::make_unique<AsyncModelData>();
    data->sourcePath = path;
    
    if (outProgress) *outProgress = 0.0f;

    // Step 1: Assimp file parsing (heavy CPU work)
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(
        path,
        aiProcess_Triangulate |
            aiProcess_GenSmoothNormals |
            aiProcess_FlipUVs |
            aiProcess_CalcTangentSpace |
            aiProcess_LimitBoneWeights);

    if (!scene || !scene->mRootNode) {
        data->error = importer.GetErrorString();
        data->success = false;
        if (outProgress) *outProgress = 1.0f;
        return data;
    }

    std::string directory = path.substr(0, path.find_last_of('/'));
    data->directory = directory;

    if (outProgress) *outProgress = 0.2f;

    // Step 2: Build skeleton hierarchy (boneIndex is filled in Step 5, then
    // copied into the Skeleton so the animator sees the node->bone map).
    // Use the COLLAPSING builder (same as the sync Model() path) - the raw
    // ReadHierarchyStatic keeps $AssimpFbx$ helper nodes, which doubles every
    // animated bone position (deformed legs/hands on the play character).
    BuildCollapsedHierarchyStatic(data->rootNode, scene->mRootNode);

    if (outProgress) *outProgress = 0.3f;

    // Step 3: Process meshes (vertices, indices, bones, materials)
    std::vector<AsyncModelData::RawMeshData> rawMeshes;
    std::vector<PBRMaterial> materials;
    
    auto processNodeAsync = [&](auto&& self, aiNode* node, const aiScene* scene,
                                const std::string& dir, std::vector<AsyncModelData::RawMeshData>& outMeshes,
                                std::vector<PBRMaterial>& outMaterials) -> void {
        for (unsigned i = 0; i < node->mNumMeshes; ++i) {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
            
            AsyncModelData::RawMeshData rawMesh;
            rawMesh.directory = dir;
            
            // Extract vertices
            rawMesh.vertices.resize(mesh->mNumVertices);
            for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
                Vertex& vert = rawMesh.vertices[v];
                vert.Position = {mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z};
                vert.Normal = mesh->HasNormals()
                    ? glm::vec3(mesh->mNormals[v].x, mesh->mNormals[v].y, mesh->mNormals[v].z)
                    : glm::vec3(0.0f, 0.0f, 1.0f);
                vert.TexCoords = mesh->mTextureCoords[0]
                    ? glm::vec2(mesh->mTextureCoords[0][v].x, mesh->mTextureCoords[0][v].y)
                    : glm::vec2(0.0f, 0.0f);
                
                if (mesh->HasTangentsAndBitangents()) {
                    vert.Tangent = glm::vec3(mesh->mTangents[v].x, mesh->mTangents[v].y, mesh->mTangents[v].z);
                    vert.Bitangent = glm::vec3(mesh->mBitangents[v].x, mesh->mBitangents[v].y, mesh->mBitangents[v].z);
                } else {
                    vert.Tangent = glm::vec3(1.0f, 0.0f, 0.0f);
                    vert.Bitangent = glm::vec3(0.0f, 1.0f, 0.0f);
                }
                
                for (int j = 0; j < MAX_BONE_INFLUENCE; ++j) {
                    vert.BoneIDs[j] = -1;
                    vert.Weights[j] = 0.0f;
                }
            }
            
            // Extract indices
            rawMesh.indices.reserve(mesh->mNumFaces * 3);
            for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
                for (unsigned j = 0; j < mesh->mFaces[f].mNumIndices; ++j) {
                    rawMesh.indices.push_back(mesh->mFaces[f].mIndices[j]);
                }
            }
            
            // Extract bone references FIRST (populates skeleton.boneMapping) so
            // extractBoneWeightsStatic can look up bone indices. This matches the
            // sync path (processMesh: ExtractBones before extractBoneWeights).
            // The previous order (weights before bones) left boneMapping empty
            // for the first mesh, forcing every vertex BoneID to -1 → no skinning
            // applied → vertices stuck in bind-pose T-pose.
            ExtractBonesStatic(mesh, data->skeleton);

            // Extract bone weights (now boneMapping is populated)
            extractBoneWeightsStatic(rawMesh.vertices, mesh, data->skeleton);
            
            // Process material
            PBRMaterial pbrMat;
            if (mesh->mMaterialIndex != static_cast<unsigned int>(-1)) {
                aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
                
                // Albedo
                aiColor3D diffuse(0.0f, 0.0f, 0.0f);
                if (material->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS) {
                    pbrMat.albedo = glm::vec3(diffuse.r, diffuse.g, diffuse.b);
                }
                
                // Collect texture paths (not loading them yet). Assimp uses
                // "*N" for embedded textures (GLB / embedded GLTF) - do NOT
                // prepend the directory to those, or the embedded-texture
                // lookup in Step 4 (texPath[0] == '*') will never match and
                // every texture falls back to grey.
                auto addTexturePath = [&](aiTextureType type, const std::string& typeName) {
                    if (material->GetTextureCount(type) > 0) {
                        aiString str;
                        material->GetTexture(type, 0, &str);
                        std::string texPath = str.C_Str();
                        std::replace(texPath.begin(), texPath.end(), '\\', '/');
                        size_t fbmPos = texPath.find(".fbm/");
                        if (fbmPos != std::string::npos) texPath = texPath.substr(fbmPos + 5);
                        // Embedded texture? Keep the raw "*N" path so Step 4
                        // can index scene->mTextures. External file? Resolve
                        // relative to the model directory.
                        if (!texPath.empty() && texPath[0] == '*') {
                            rawMesh.texturePaths.emplace_back(texPath, typeName);
                        } else {
                            std::string fullPath = dir + "/" + texPath;
                            rawMesh.texturePaths.emplace_back(fullPath, typeName);
                        }
                    }
                };
                
                addTexturePath(aiTextureType_DIFFUSE, "diffuse");
                addTexturePath(aiTextureType_NORMALS, "normal");
                addTexturePath(aiTextureType_METALNESS, "metallic");
                addTexturePath(aiTextureType_DIFFUSE_ROUGHNESS, "roughness");
                addTexturePath(aiTextureType_AMBIENT_OCCLUSION, "ao");
                addTexturePath(aiTextureType_EMISSIVE, "emissive");
                
                // Material properties
                float roughness = 0.5f;
                if (material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS) {
                    pbrMat.roughness = roughness;
                }
                float metallic = 0.0f;
                if (material->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS) {
                    pbrMat.metallic = metallic;
                }
                aiColor3D emissive(0.0f, 0.0f, 0.0f);
                if (material->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
                    pbrMat.emissive = glm::vec3(emissive.r, emissive.g, emissive.b);
                }
            }
            
            outMaterials.push_back(pbrMat);
            outMeshes.push_back(std::move(rawMesh));
        }
        
        for (unsigned i = 0; i < node->mNumChildren; ++i) {
            self(self, node->mChildren[i], scene, dir, outMeshes, outMaterials);
        }
    };
    
    processNodeAsync(processNodeAsync, scene->mRootNode, scene, directory, rawMeshes, materials);
    data->meshes = std::move(rawMeshes);
    data->meshMaterials = std::move(materials);

    if (outProgress) *outProgress = 0.5f;

    // Step 4: Load textures on background thread (file I/O, no GL)
    {
        // Collect unique texture paths
        std::unordered_map<std::string, int> texturePathToIndex;
        std::vector<std::pair<std::string, std::string>> allTexturePaths;
        
        for (const auto& mesh : data->meshes) {
            for (const auto& [texPath, type] : mesh.texturePaths) {
                if (texturePathToIndex.find(texPath) == texturePathToIndex.end()) {
                    texturePathToIndex[texPath] = data->textures.size();
                    allTexturePaths.emplace_back(texPath, type);
                    
                    TexturePixelData texData;
                    texData.path = texPath;
                    texData.type = type;
                    texData.isNormalMap = (type == "normal");
                    
                    // --- Embedded texture support ---
                    // Mixamo/FBX models embed textures (referenced by "*N"
                    // index or a ".fbm/" path that doesn't exist on disk).
                    // stbi_load only handles real files on disk, so we must
                    // also consult scene->mTextures - exactly as the sync path
                    // does via loadEmbeddedTexture / findEmbeddedTexture.
                    if (!texPath.empty() && texPath[0] == '*') {
                        // "*N" → direct index into the embedded texture array
                        try {
                            const size_t idx = std::stoul(texPath.substr(1));
                            if (idx < scene->mNumTextures && scene->mTextures[idx]) {
                                const aiTexture* at = scene->mTextures[idx];
                                if (at->mHeight == 0 && at->pcData) {
                                    // Compressed (PNG/JPEG): decode in memory
                                    int w2 = 0, h2 = 0, c2 = 0;
                                    stbi_set_flip_vertically_on_load(true);
                                    if (unsigned char* d = stbi_load_from_memory(
                                            reinterpret_cast<const unsigned char*>(at->pcData),
                                            static_cast<int>(at->mWidth), &w2, &h2, &c2, 0)) {
                                        texData.width = w2; texData.height = h2; texData.channels = c2;
                                        texData.data.assign(d, d + (size_t)w2 * h2 * c2);
                                        stbi_image_free(d);
                                    } else {
                                        // EXR/HDR embedded texture: use float loader
                                        // (stbi_load_from_memory doesn't support EXR)
                                        int fw=0,fh=0,fc=0;
                                        stbi_set_flip_vertically_on_load(true);
                                        if (float* fd = stbi_loadf_from_memory(
                                                reinterpret_cast<const unsigned char*>(at->pcData),
                                                static_cast<int>(at->mWidth), &fw, &fh, &fc, 4)) {
                                            texData.width = fw; texData.height = fh; texData.channels = 4;
                                            texData.data.resize((size_t)fw * fh * 4);
                                            for (size_t i = 0; (int)i < fw * fh * 4; ++i)
                                                texData.data[i] = (uint8_t)std::clamp(fd[i] * 255.0f, 0.0f, 255.0f);
                                            stbi_image_free(fd);
                                        }
                                    }
                                } else if (at->mHeight > 0 && at->pcData) {
                                    // Uncompressed raw RGBA
                                    texData.width = (int)at->mWidth;
                                    texData.height = (int)at->mHeight;
                                    texData.channels = 4;
                                    texData.data.assign(
                                        reinterpret_cast<const uint8_t*>(at->pcData),
                                        reinterpret_cast<const uint8_t*>(at->pcData) + (size_t)at->mWidth * at->mHeight * 4);
                                }
                            }
                        } catch (...) {}
                    }
                    
                    // If not embedded or not yet decoded, try disk. Prefer the
                    // native EXR source when a sibling exists: the shipped props
                    // carry 8-bit JPG copies (what the glTF/FBX references) PLUS
                    // the original linear-HDR EXR maps (same basename, .exr
                    // extension). The EXR is the "actual" texture - decode it
                    // with the OpenEXR loader so the engine consumes the real
                    // source pixels instead of the compressed JPG stand-in, and
                    // fall back to the referenced path when no EXR is present
                    // (albedo maps, for example, are JPG-only).
                    if (texData.data.empty()) {
                        std::string exrPath;
                        const size_t dot = texPath.find_last_of('.');
                        if (dot != std::string::npos &&
                            texPath.compare(dot, 5, ".exr") != 0) {
                            exrPath = texPath.substr(0, dot) + ".exr";
                        }
                        int w = 0, h = 0, c = 0;
                        if (!exrPath.empty() &&
                            LoadEXRPixelsRGBA8(exrPath, w, h, texData.data)) {
                            texData.width = w;
                            texData.height = h;
                            texData.channels = 4;
                        } else {
                            stbi_set_flip_vertically_on_load(true);
                            if (unsigned char* imgData = stbi_load(texPath.c_str(), &w, &h, &c, 0)) {
                                texData.width = w;
                                texData.height = h;
                                texData.channels = c;
                                texData.data.assign(imgData, imgData + (size_t)w * h * c);
                                stbi_image_free(imgData);
                            } else {
                                // HDR on disk (Radiance .hdr): stbi_load doesn't
                                // support it, so try the float loader and
                                // convert to uint8 RGBA.
                                int fw=0, fh=0, fc=0;
                                if (float* fd = stbi_loadf(texPath.c_str(), &fw, &fh, &fc, 4)) {
                                    texData.width = fw; texData.height = fh; texData.channels = 4;
                                    texData.data.resize((size_t)fw * fh * 4);
                                    for (size_t i = 0; (int)i < fw * fh * 4; ++i)
                                        texData.data[i] = (uint8_t)std::clamp(fd[i] * 255.0f, 0.0f, 255.0f);
                                    stbi_image_free(fd);
                                }
                            }
                        }
                    }
                    
                    // If disk load failed, try matching by basename against
                    // embedded textures (same logic as findEmbeddedTexture).
                    if (texData.data.empty()) {
                        auto basenameLower = [](std::string s) {
                            const size_t slash = s.find_last_of("/\\");
                            if (slash != std::string::npos) s = s.substr(slash + 1);
                            for (char& c : s) c = (char)std::tolower((unsigned char)c);
                            return s;
                        };
                        const std::string want = basenameLower(texPath);
                        if (!want.empty()) {
                            for (unsigned i = 0; i < scene->mNumTextures; ++i) {
                                const aiTexture* t = scene->mTextures[i];
                                if (!t) continue;
                                if (t->mFilename.length > 0 && basenameLower(t->mFilename.C_Str()) == want) {
                                    if (t->mHeight == 0 && t->pcData) {
                                        int w2 = 0, h2 = 0, c2 = 0;
                                        stbi_set_flip_vertically_on_load(true);
                                        if (unsigned char* d = stbi_load_from_memory(
                                                reinterpret_cast<const unsigned char*>(t->pcData),
                                                static_cast<int>(t->mWidth), &w2, &h2, &c2, 0)) {
                                            texData.width = w2; texData.height = h2; texData.channels = c2;
                                            texData.data.assign(d, d + (size_t)w2 * h2 * c2);
                                            stbi_image_free(d);
                                        } else {
                                            // EXR/HDR embedded: float loader + convert
                                            int fw=0,fh=0,fc=0;
                                            stbi_set_flip_vertically_on_load(true);
                                            if (float* fd = stbi_loadf_from_memory(
                                                    reinterpret_cast<const unsigned char*>(t->pcData),
                                                    static_cast<int>(t->mWidth), &fw, &fh, &fc, 4)) {
                                                texData.width = fw; texData.height = fh; texData.channels = 4;
                                                texData.data.resize((size_t)fw * fh * 4);
                                                for (size_t i = 0; (int)i < fw * fh * 4; ++i)
                                                    texData.data[i] = (uint8_t)std::clamp(fd[i] * 255.0f, 0.0f, 255.0f);
                                                stbi_image_free(fd);
                                            }
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    
                    data->textures.push_back(std::move(texData));
                }
            }
        }
    }

    if (outProgress) *outProgress = 0.7f;

    // Step 5: Build bone maps
    auto normMap = BuildNormalizedBoneMap(data->skeleton);
    BuildNodeBoneMapStatic(data->rootNode, normMap);
    // Copy the hierarchy (with boneIndex filled in) into the skeleton - this
    // must happen AFTER BuildNodeBoneMapStatic or the animator sees -1 on
    // every node and never writes the bone matrices (frozen bind pose).
    data->skeleton.rootNode = data->rootNode;
    data->skeleton.rootBoneIndex = FindRootBoneIndex(data->skeleton);
    data->rootBoneIndex = data->skeleton.rootBoneIndex;
    
    glm::mat4 rootTransform = aiMat4ToGlm(scene->mRootNode->mTransformation);
    data->globalInverseTransform = glm::inverse(rootTransform);
    data->skeleton.globalInverseTransform = data->globalInverseTransform;

    if (outProgress) *outProgress = 0.8f;

    // Step 6: Load animation metadata
    for (unsigned i = 0; i < scene->mNumAnimations; ++i) {
        aiAnimation* a = scene->mAnimations[i];
        data->animations.push_back(std::make_unique<Animation>(
            a->mName.C_Str(),
            float(a->mDuration),
            float(a->mTicksPerSecond > 0 ? a->mTicksPerSecond : 25.0f)));
    }

    // Step 7: Optimize meshes (CPU only)
    for (auto& mesh : data->meshes) {
        // Validate indices before optimization
        bool indicesValid = true;
        for (unsigned int idx : mesh.indices) {
            if (idx >= mesh.vertices.size()) {
                indicesValid = false;
                break;
            }
        }
        if (indicesValid && mesh.indices.size() >= 3 && mesh.vertices.size() > 0) {
            MeshUtils::OptimizeTriangleOrderingForsyth(mesh.indices, mesh.vertices.size());
        }
    }

    if (outProgress) *outProgress = 0.9f;

    // Step 8: Calculate bounding volumes
    BoundingBox bbox;
    int totalVerts = 0;
    int totalTris = 0;
    
    for (const auto& mesh : data->meshes) {
        for (const auto& v : mesh.vertices) {
            bbox.Extend(v.Position);
        }
        totalVerts += mesh.vertices.size();
        totalTris += mesh.indices.size() / 3;
    }
    
    data->boundingBox = bbox;
    data->boundingSphere = BoundingSphere::FromBoundingBox(bbox);
    data->totalVertices = totalVerts;
    data->totalTriangles = totalTris;
    data->success = true;

    if (outProgress) *outProgress = 1.0f;
    return data;
}

Model* Model::CreateFromAsync(AsyncLoadHandle& handle, std::string* outError)
{
    if (!handle.isValid()) {
        if (outError) *outError = "Invalid async handle";
        return nullptr;
    }
    
    // Block until loading is complete
    auto data = handle.future.get();
    
    if (!data) {
        if (outError) *outError = "Async loading returned null data";
        return nullptr;
    }
    
    if (!data->success) {
        if (outError) *outError = "Loading failed: " + data->error;
        return nullptr;
    }
    
    // Create model on main thread (GL resources)
    try {
        Model* model = new Model("");
        std::string setupError;
        model->setupFromAsyncData(std::move(data), setupError);
        if (!setupError.empty()) {
            if (outError) *outError = setupError;
            delete model;
            return nullptr;
        }
        return model;
    } catch (const std::exception& e) {
        if (outError) *outError = std::string("Exception during GL setup: ") + e.what();
        return nullptr;
    } catch (...) {
        if (outError) *outError = "Unknown exception during GL setup";
        return nullptr;
    }
}

void Model::setupFromAsyncData(std::unique_ptr<AsyncModelData> data, std::string& outError)
{
    if (!data) { outError = "null data"; return; }
    std::cout << "[setupFromAsyncData] meshes=" << data->meshes.size() << " textures=" << data->textures.size() << " materials=" << data->meshMaterials.size() << "\n";
    // This MUST be called on the main thread (GL context required)
    meshes.reserve(data->meshes.size());
    meshMaterials = std::move(data->meshMaterials);
    if (meshMaterials.size() < data->meshes.size()) {
        meshMaterials.resize(data->meshes.size());
    }
    
    // Create meshes with GL resources
    for (size_t i = 0; i < data->meshes.size(); ++i) {
        auto& rawMesh = data->meshes[i];
        std::cout << "[setupFromAsyncData] Creating mesh " << i << " v=" << rawMesh.vertices.size() << " idx=" << rawMesh.indices.size() << "\n";
        
        // Validate mesh data
        if (rawMesh.vertices.empty()) {
            std::cout << "[setupFromAsyncData] Skipping mesh " << i << ": no vertices\n";
            continue;
        }
        if (rawMesh.indices.empty()) {
            std::cout << "[setupFromAsyncData] Skipping mesh " << i << ": no indices\n";
            continue;
        }
        
        try {
            Mesh mesh(rawMesh.vertices, rawMesh.indices);
            mesh.CalculateBoundingVolumes();
            mesh.stats.Calculate(mesh.vertices, mesh.indices, mesh.textures);
            meshes.push_back(std::move(mesh));
        } catch (const std::exception& e) {
            std::cerr << "[setupFromAsyncData] Exception creating mesh " << i << ": " << e.what() << "\n";
            outError = std::string("Failed to create mesh ") + std::to_string(i) + ": " + e.what();
            return;
        }
    }
    
    // Upload textures to GL
    std::unordered_map<std::string, unsigned int> loadedTextures;
    
    for (size_t meshIdx = 0; meshIdx < data->meshes.size(); ++meshIdx) {
        auto& rawMesh = data->meshes[meshIdx];
        
        // Apply material textures
        auto& mat = meshMaterials[meshIdx];
        
        for (const auto& [texPath, type] : rawMesh.texturePaths) {
            if (loadedTextures.find(texPath) == loadedTextures.end()) {
                // Find the pixel data for this texture
                for (const auto& texData : data->textures) {
                    if (texData.path == texPath && !texData.data.empty()) {
                        unsigned int texID = uploadTextureFromPixels(texData);
                        loadedTextures[texPath] = texID;
                        
                        // Set material texture ID
                        if (type == "diffuse") {
                            mat.albedoMap = texID;
                            mat.hasAlbedoMap = true;
                        } else if (type == "normal") {
                            mat.normalMap = texID;
                            mat.hasNormalMap = true;
                        } else if (type == "metallic") {
                            mat.metallicMap = texID;
                            mat.hasMetallicMap = true;
                        } else if (type == "roughness") {
                            mat.roughnessMap = texID;
                            mat.hasRoughnessMap = true;
                        } else if (type == "ao") {
                            mat.aoMap = texID;
                            mat.hasAOMap = true;
                        } else if (type == "emissive") {
                            mat.emissiveMap = texID;
                            mat.hasEmissiveMap = true;
                        }
                        break;
                    }
                }
            } else {
                unsigned int texID = loadedTextures[texPath];
                if (type == "diffuse") { mat.albedoMap = texID; mat.hasAlbedoMap = true; }
                else if (type == "normal") { mat.normalMap = texID; mat.hasNormalMap = true; }
                else if (type == "metallic") { mat.metallicMap = texID; mat.hasMetallicMap = true; }
                else if (type == "roughness") { mat.roughnessMap = texID; mat.hasRoughnessMap = true; }
                else if (type == "ao") { mat.aoMap = texID; mat.hasAOMap = true; }
                else if (type == "emissive") { mat.emissiveMap = texID; mat.hasEmissiveMap = true; }
            }
        }
        
        // Set default textures if needed
        if (!mat.hasAlbedoMap) { mat.albedoMap = DefaultTexture::GetGreyTexture(); mat.hasAlbedoMap = true; }
        if (!mat.hasNormalMap) { mat.normalMap = DefaultTexture::GetFlatNormalTexture(); mat.hasNormalMap = true; }
        if (!mat.hasMetallicMap) { mat.metallicMap = DefaultTexture::GetGreyTexture(); mat.hasMetallicMap = true; }
        if (!mat.hasRoughnessMap) { mat.roughnessMap = DefaultTexture::GetGreyTexture(); mat.hasRoughnessMap = true; }
        if (!mat.hasAOMap) { mat.aoMap = DefaultTexture::GetGreyTexture(); mat.hasAOMap = true; }
    }

    // CRITICAL FIX: push the FULL PBR texture set into each mesh's texture list,
    // matching the sync path in processMesh (pushMap). Mesh::Draw binds
    // mesh.textures as texture_<semantic>N samplers, so if normal/metallic/
    // roughness/ao are missing from this list the shader samples whatever was
    // stale on those texture units — cross-contaminated maps from previously-
    // drawn meshes, which is why "some parts aren't properly textured" (flat
    // Minecraft shading, wrong normals, incorrect metal/roughness). Each
    // semantic is guaranteed present via the per-mesh pushMap with safe
    // fallbacks, so no shader ever reads an unbound or stale sampler.
    for (size_t meshIdx = 0; meshIdx < meshes.size() && meshIdx < meshMaterials.size(); ++meshIdx) {
        const auto& mat = meshMaterials[meshIdx];
        auto pushMap = [&](unsigned int id, unsigned int fallback,
                           const char* semantic, Texture::Type texType) {
            unsigned int texId = (id != 0) ? id : fallback;
            if (texId != 0) {
                Texture tex;
                tex.id = texId;
                tex.type = semantic;
                tex.textureType = texType;
                meshes[meshIdx].textures.push_back(tex);
            }
        };
        pushMap(mat.albedoMap,     DefaultTexture::GetGreyTexture(),          "diffuse",  Texture::Type::DIFFUSE);
        pushMap(mat.normalMap,     DefaultTexture::GetFlatNormalTexture(),    "normal",   Texture::Type::NORMAL);
        pushMap(mat.metallicMap,   DefaultTexture::GetBlackTexture(),         "metallic", Texture::Type::METALLIC);
        pushMap(mat.roughnessMap,  DefaultTexture::GetGreyTexture(),          "roughness",Texture::Type::ROUGHNESS);
        pushMap(mat.aoMap,         DefaultTexture::GetWhiteTexture(),          "ao",       Texture::Type::AO);
    }
    
    // Copy skeleton and animation data
    m_Skeleton = std::move(data->skeleton);
    m_Skeleton.rootNode = data->rootNode;
    rootBoneIndex = data->rootBoneIndex;
    m_Animations = std::move(data->animations);
    
    // Copy bounding volumes
    boundingBox = data->boundingBox;
    boundingSphere = data->boundingSphere;
    
    directory = data->directory;
    
    if (debugOutput) {
        std::cout << "[Model] Async loaded: " << data->sourcePath
                  << " | Bones: " << m_Skeleton.bones.size()
                  << " | Meshes: " << meshes.size()
                  << " | Triangles: " << data->totalTriangles
                  << " | Vertices: " << data->totalVertices
                  << "\n";
    }
}

unsigned int Model::uploadTextureFromPixels(const TexturePixelData& texData)
{
    if (texData.data.empty()) {
        return 0;
    }
    
    unsigned int texID;
    // Direct State Access (glCreateTextures / glTextureStorage2D, OpenGL 4.5+
    // core / ARB_direct_state_access, loaded by the GLAD 4.6 loader): allocates
    // immutable storage without binding. The bind below is kept only to
    // preserve the prior observable bind state for callers; all setup is DSA.
    glCreateTextures(GL_TEXTURE_2D, 1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);  // preserve prior bind semantics

    bool isNormalMap = texData.isNormalMap;
    GLenum pixelFormat;          // source transfer format (unsized OK here)
    GLenum internalFormat;       // SIZED format - required by glTextureStorage2D

    if (texData.channels == 1) {
        pixelFormat = GL_RED;
        internalFormat = GL_R8;
    } else if (texData.channels == 2) {
        pixelFormat = GL_RG;
        internalFormat = GL_RG8;
    } else if (texData.channels == 3) {
        pixelFormat = GL_RGB;
        internalFormat = isNormalMap ? GL_RGB8 : GL_SRGB8;
    } else {
        pixelFormat = GL_RGBA;
        internalFormat = isNormalMap ? GL_RGBA8 : GL_SRGB8_ALPHA8;
    }

    // Full mip chain (glGenerateTextureMipmap below).
    GLsizei levels = 1;
    for (int d = (texData.width > texData.height ? texData.width : texData.height);
         d >>= 1; ++levels) {
    }

    glTextureStorage2D(texID, levels, internalFormat, texData.width, texData.height);
    glTextureSubImage2D(texID, 0, 0, 0, texData.width, texData.height,
                        pixelFormat, GL_UNSIGNED_BYTE, texData.data.data());
    glGenerateTextureMipmap(texID);

    glTextureParameteri(texID, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(texID, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameteri(texID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(texID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    if (isNormalMap) {
        glTextureParameteri(texID, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(texID, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    }

    // Anisotropic filtering via DSA. ARB_texture_filter_anisotropic is core since
    // OpenGL 4.6 (GLAD 4.6 loader): samples a wider footprint at grazing angles,
    // removing shimmer on ground decals, terrain and the character albedo /
    // normal / metallic / roughness maps at no extra draw cost. Cap at the
    // hardware max (typically 16x); on a <4.6 fallback the GLAD flag is false.
    if (GLAD_GL_ARB_texture_filter_anisotropic) {
        GLfloat maxAniso = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
        if (maxAniso > 1.0f)
            glTextureParameterf(texID, GL_TEXTURE_MAX_ANISOTROPY, maxAniso);
    }

    return texID;
}

// Static versions of helper functions for async loading (no 'this' pointer)
void Model::ReadHierarchyStatic(AssimpNodeData& dest, const aiNode* src)
{
    dest.name = src->mName.C_Str();
    dest.transform = aiMat4ToGlm(src->mTransformation);
    dest.children.resize(src->mNumChildren);
    
    for (size_t i = 0; i < src->mNumChildren; ++i) {
        ReadHierarchyStatic(dest.children[i], src->mChildren[i]);
    }
}

void Model::BuildNodeBoneMapStatic(AssimpNodeData& node, const std::unordered_map<std::string, int>& normBoneMap)
{
    // Same normalized-lookup fix as BuildNodeBoneMap (see above).
    auto it = normBoneMap.find(NormalizeBoneName(node.name));
    node.boneIndex = (it != normBoneMap.end()) ? it->second : -1;
    
    for (auto& child : node.children) {
        BuildNodeBoneMapStatic(child, normBoneMap);
    }
}

void Model::ExtractBonesStatic(aiMesh* mesh, Skeleton& skeleton)
{
    for (unsigned i = 0; i < mesh->mNumBones; ++i) {
        aiBone* bone = mesh->mBones[i];
        std::string boneName(bone->mName.C_Str());
        
        if (skeleton.boneMapping.find(boneName) == skeleton.boneMapping.end()) {
            BoneInfo newBone;
            newBone.id = skeleton.bones.size();
            newBone.offset = aiMat4ToGlm(bone->mOffsetMatrix);
            skeleton.boneMapping[boneName] = newBone.id;
            skeleton.bones.push_back(newBone);
        }
    }
}

void Model::extractBoneWeightsStatic(std::vector<Vertex>& vertices, aiMesh* mesh, Skeleton& skeleton)
{
    for (unsigned b = 0; b < mesh->mNumBones; ++b) {
        aiBone* bone = mesh->mBones[b];
        for (unsigned w = 0; w < bone->mNumWeights; ++w) {
            int vertexID = bone->mWeights[w].mVertexId;
            float weight = bone->mWeights[w].mWeight;
            
            if (vertexID >= 0 && vertexID < static_cast<int>(vertices.size())) {
                Vertex& v = vertices[vertexID];
                
                for (int j = 0; j < MAX_BONE_INFLUENCE; ++j) {
                    if (v.Weights[j] == 0.0f) {
                        int boneIndex = -1;
                        auto it = skeleton.boneMapping.find(bone->mName.C_Str());
                        if (it != skeleton.boneMapping.end()) {
                            boneIndex = it->second;
                        }
                        
                        v.BoneIDs[j] = boneIndex;
                        v.Weights[j] = weight;
                        break;
                    }
                }
            }
        }
    }
}

// =====================================================
// Draw Functions
// =====================================================
void Model::Draw(Shader &shader, Animator &animator)
{
    const auto &finalBones = animator.GetFinalBoneMatrices();
    
    // Debug output for first draw
    static bool firstDraw = true;
    if (firstDraw && debugOutput) {
        std::cout << "[Model::Draw] Starting draw call\n";
        std::cout << "[Model::Draw] finalBones.size()=" << finalBones.size() << "\n";
        std::cout << "[Model::Draw] Number of meshes: " << meshes.size() << "\n";
        if (!finalBones.empty()) {
            std::cout << "[Model::Draw] First bone matrix[3]: " << glm::to_string(glm::vec3(finalBones[0][3])) << "\n";
        }
    }

    // Use SSBO/UBO for bone matrices (fast path)
    if (!finalBones.empty()) {
        if (!boneBuffer.IsInitialized()) {
            boneBuffer.Initialize(finalBones.size()); // UBO-friendly capacity (<=256)
        }
        // forceUpdate=true: the bone matrices CHANGE every frame for an
        // animated character, and BoneMatrixBuffer::Update() short-circuits
        // after the first upload (needsUpdate->false). Without the force the
        // GPU mesh renders frame-1's pose (Idle) forever while the CPU animator
        // advances - the "skeleton animates but the model is stuck" bug.
        boneBuffer.Update(finalBones, /*forceUpdate=*/true);
        boneBuffer.Bind(BONE_BUFFER_BINDING); // Bind to shader layout binding point 3
        shader.setInt("uBoneBufferBinding", BONE_BUFFER_BINDING);
        shader.setInt("uBoneBufferEnabled", 1);
    } else {
        shader.setInt("uBoneBufferEnabled", 0);
    }

    // Draw all meshes
    for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex)
    {
        auto &mesh = meshes[meshIndex];
        mesh.Draw(shader);
    }
    
    if (boneBuffer.IsInitialized()) {
        boneBuffer.Unbind();
    }
    
    if (firstDraw && debugOutput) {
        std::cout << "[Model::Draw] Finished drawing all meshes\n";
    }
    firstDraw = false;
}

void Model::DrawStatic(Shader &shader)
{
    // Disable skinning for static models
    shader.setInt("uBoneBufferEnabled", 0);
    shader.setInt("uDisableSkinning", 1);
    
    // Draw all meshes
    for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex)
    {
        auto &mesh = meshes[meshIndex];
        mesh.Draw(shader);
    }
}

// =====================================================
// LOD Drawing
// =====================================================
void Model::DrawLOD(Shader &shader, Animator &animator, const glm::vec3& cameraPos, float lodBias)
{
    const auto &finalBones = animator.GetFinalBoneMatrices();

    // Use SSBO/UBO for bone matrices (fast path)
    if (!finalBones.empty()) {
        if (!boneBuffer.IsInitialized()) {
            boneBuffer.Initialize(finalBones.size()); // UBO-friendly capacity (<=256)
        }
        // forceUpdate=true - bone matrices change every frame (see Draw()).
        boneBuffer.Update(finalBones, /*forceUpdate=*/true);
        boneBuffer.Bind(BONE_BUFFER_BINDING);
        shader.setInt("uBoneBufferBinding", BONE_BUFFER_BINDING);
        shader.setInt("uBoneBufferEnabled", 1);
    } else {
        shader.setInt("uBoneBufferEnabled", 0);
    }

    // Calculate distance to model center
    float distance = glm::length(cameraPos - boundingSphere.center);
    distance *= lodBias; // Apply LOD bias

    // Draw appropriate LOD level for each mesh
    for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex)
    {
        const auto& lodList = lodLevels[meshIndex];

        // Find appropriate LOD level
        Mesh* meshToDraw = const_cast<Mesh*>(&meshes[meshIndex]); // Default to highest quality

        for (const auto& lod : lodList)
        {
            if (distance >= lod.distanceThreshold)
            {
                meshToDraw = const_cast<Mesh*>(&lod.mesh);
                break;
            }
        }

        meshToDraw->Draw(shader);
    }
    
    if (boneBuffer.IsInitialized()) {
        boneBuffer.Unbind();
    }
}

// =====================================================
// Instanced Drawing
// =====================================================
void Model::DrawInstanced(Shader& shader, Animator& animator, const std::vector<ModelInstance>& instances)
{
    if (instances.empty()) return;
    
    const auto &finalBones = animator.GetFinalBoneMatrices();
    
    // Use SSBO/UBO for bone matrices (fast path)
    if (!finalBones.empty()) {
        if (!boneBuffer.IsInitialized()) {
            boneBuffer.Initialize(finalBones.size()); // UBO-friendly capacity (<=256)
        }
        // forceUpdate=true - bone matrices change every frame (see Draw()).
        boneBuffer.Update(finalBones, /*forceUpdate=*/true);
        boneBuffer.Bind(BONE_BUFFER_BINDING);
        shader.setInt("uBoneBufferBinding", BONE_BUFFER_BINDING);
        shader.setInt("uBoneBufferEnabled", 1);
    } else {
        shader.setInt("uBoneBufferEnabled", 0);
    }

    // Enable instancing
    shader.setInt("uInstancingEnabled", 1);
    
    // For now, just draw first instance with its transform
    // Full instancing would require vertex shader changes
    if (!instances.empty() && instances[0].enabled)
    {
        glm::mat4 modelMat = instances[0].modelMatrix;
        shader.setMat4("model", modelMat);
        
        for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex)
        {
            meshes[meshIndex].Draw(shader);
        }
    }
    
    shader.setInt("uInstancingEnabled", 0);
    
    if (boneBuffer.IsInitialized()) {
        boneBuffer.Unbind();
    }
}

// =====================================================
Animation* Model::GetAnimation(size_t index)
{
    if (index >= m_Animations.size())
        return nullptr;
    return m_Animations[index].get();
}

// =====================================================
const Animation* Model::GetAnimation(size_t index) const
{
    if (index >= m_Animations.size())
        return nullptr;
    return m_Animations[index].get();
}

// =====================================================
void Model::loadModel(const std::string &path)
{
    scene = importer.ReadFile(
        path,
        aiProcess_Triangulate |
            aiProcess_GenSmoothNormals |
            aiProcess_FlipUVs |
            aiProcess_CalcTangentSpace |
            aiProcess_LimitBoneWeights);
    // REMOVED: aiProcess_PreTransformVertices - this destroys skinning!

    if (!scene || !scene->mRootNode)
    {
        std::cerr << "ASSIMP ERROR: " << importer.GetErrorString() << "\n";
        return;
    }

    directory = path.substr(0, path.find_last_of('/'));

    // Build skeleton node hierarchy first
    ReadHierarchy(m_Skeleton.rootNode, scene->mRootNode);

    // Load meshes + extract bones/weights
    processNode(scene->mRootNode, scene);

    // Build node->boneIndex map
    auto normMap = BuildNormalizedBoneMap(m_Skeleton);
    BuildNodeBoneMap(m_Skeleton.rootNode, normMap);

    // Detect root bone
    m_Skeleton.rootBoneIndex = FindRootBoneIndex(m_Skeleton);

    if (debugOutput) {
        std::cout << "Skeleton root bone index: " << m_Skeleton.rootBoneIndex << "\n";
    }

    // CRITICAL FIX: globalInverseTransform MUST NOT be identity.
    glm::mat4 rootTransform = aiMat4ToGlm(scene->mRootNode->mTransformation);
    m_Skeleton.globalInverseTransform = glm::inverse(rootTransform);

    // Load animations
    for (unsigned i = 0; i < scene->mNumAnimations; ++i)
    {
        aiAnimation *a = scene->mAnimations[i];
        m_Animations.push_back(std::make_unique<Animation>(
            a->mName.C_Str(),
            float(a->mDuration),
            float(a->mTicksPerSecond > 0 ? a->mTicksPerSecond : 25.0f)));
    }

    // Generate LOD levels (skip optimization to avoid crashes with complex FBX models)
    // generateLODLevels();

    // Optimize all meshes for rendering (skip to avoid crashes with complex FBX models)
    // for (auto& mesh : meshes)
    // {
    //     mesh.Optimize();
    // }
}

// =====================================================
void Model::processNode(aiNode *node, const aiScene *scene)
{
    for (unsigned i = 0; i < node->mNumMeshes; ++i) {
        meshes.push_back(processMesh(scene->mMeshes[node->mMeshes[i]], scene));
    }

    for (unsigned i = 0; i < node->mNumChildren; ++i)
        processNode(node->mChildren[i], scene);
}

// =====================================================
Mesh Model::processMesh(aiMesh *mesh, const aiScene *scene)
{
    std::vector<Vertex> vertices(mesh->mNumVertices);
    std::vector<unsigned int> indices;
    std::vector<Texture> textures;

    for (unsigned i = 0; i < mesh->mNumVertices; ++i)
    {
        Vertex &v = vertices[i];

        v.Position = {mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z};

        v.Normal = mesh->HasNormals()
                       ? glm::vec3(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z)
                       : glm::vec3(0.0f, 0.0f, 1.0f);

        v.TexCoords = mesh->mTextureCoords[0]
                          ? glm::vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y)
                          : glm::vec2(0.0f, 0.0f);

        // CRITICAL: Load tangents and bitangents
        if (mesh->HasTangentsAndBitangents())
        {
            v.Tangent = glm::vec3(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z);
            v.Bitangent = glm::vec3(mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z);
        }
        else
        {
            v.Tangent = glm::vec3(1.0f, 0.0f, 0.0f);
            v.Bitangent = glm::vec3(0.0f, 1.0f, 0.0f);
        }

        for (int j = 0; j < MAX_BONE_INFLUENCE; ++j)
        {
            v.BoneIDs[j] = -1;
            v.Weights[j] = 0.0f;
        }
    }

    for (unsigned i = 0; i < mesh->mNumFaces; ++i)
        for (unsigned j = 0; j < mesh->mFaces[i].mNumIndices; ++j)
            indices.push_back(mesh->mFaces[i].mIndices[j]);

    // Extract bones/weights
    ExtractBones(mesh, m_Skeleton.rootNode);
    extractBoneWeights(vertices, mesh);

    // Process PBR materials. processMaterial already substitutes safe defaults
    // for any map that failed to load (flat normal, black metallic, grey
    // roughness, white AO), so the values below are always valid GL texture
    // objects. The simple (VS/FS) and PBR (pbrVS/pbrFS) shaders both sample
    // texture_<semantic>1, so we bind the full map set through the mesh's
    // texture list. (Previously only albedo was pushed here, leaving the normal/
    // roughness/ao samplers UNBOUND - flat "Minecraft" shading in BOTH paths.)
    PBRMaterial pbrMat;
    if (mesh->mMaterialIndex != static_cast<unsigned int>(-1))
    {
        aiMaterial *material = scene->mMaterials[mesh->mMaterialIndex];
        pbrMat = processMaterial(material, directory, scene);
    }
    else
    {
        // Default material: grey albedo, flat normal, black metallic, grey
        // roughness, white AO -> renders lit but neutral (no regression).
        pbrMat.albedoMap     = DefaultTexture::GetGreyTexture();
        pbrMat.normalMap     = DefaultTexture::GetFlatNormalTexture();
        pbrMat.metallicMap   = DefaultTexture::GetBlackTexture();
        pbrMat.roughnessMap  = DefaultTexture::GetGreyTexture();
        pbrMat.aoMap         = DefaultTexture::GetWhiteTexture();
        pbrMat.hasAlbedoMap    = true;
        pbrMat.hasNormalMap    = true;
        pbrMat.hasMetallicMap  = true;
        pbrMat.hasRoughnessMap = true;
        pbrMat.hasAOMap        = true;
    }
    meshMaterials.push_back(pbrMat);

    // Bind the full PBR texture set to the mesh. mesh.cpp names each sampler
    // texture_<semantic><N> (per-type index) and binds it to a compact texture
    // unit, which is exactly what pbrFS.glsl/FS.glsl sample
    // (texture_diffuse1, texture_normal1, texture_metallic1,
    //  texture_roughness1, texture_ao1). Every semantic is guaranteed bound
    // (defaults substituted) so no shader ever reads an unbound sampler.
    auto pushMap = [&](unsigned int id, unsigned int fallback, const char* semantic) {
        unsigned int texId = (id != 0) ? id : fallback;
        if (texId != 0) {
            Texture tex;
            tex.id = texId;
            tex.type = semantic;
            textures.push_back(tex);
        }
    };
    pushMap(pbrMat.albedoMap,    DefaultTexture::GetGreyTexture(),         "diffuse");
    pushMap(pbrMat.normalMap,    DefaultTexture::GetFlatNormalTexture(),    "normal");
    pushMap(pbrMat.metallicMap,  DefaultTexture::GetBlackTexture(),         "metallic");
    pushMap(pbrMat.roughnessMap, DefaultTexture::GetGreyTexture(),          "roughness");
    pushMap(pbrMat.aoMap,        DefaultTexture::GetWhiteTexture(),          "ao");

    return Mesh(vertices, indices, textures);
}

// =====================================================
// PBR Material Processing
// =====================================================
PBRMaterial Model::processMaterial(aiMaterial* mat, const std::string& directory, const aiScene* scene)
{
    PBRMaterial material;

    // Resolve a material map, preferring EMBEDDED textures: FBX files
    // (Mixamo characters in particular) store the pixels inside the file and
    // reference them by their original export path - usually an absolute
    // temp ".fbm" path that does not exist on disk. Loading from that path
    // silently falls back to flat grey, so try the embedded copy first and
    // only then the legacy disk-path load.
    auto loadMap = [&](aiTextureType mapType, aiTextureType glType) -> unsigned int {
        aiString str;
        if (mat->GetTexture(mapType, 0, &str) != AI_SUCCESS) return 0;
        return loadEmbeddedTexture(scene, str.C_Str(), glType);
    };

    // Albedo (diffuse)
    aiColor3D diffuse(0.0f, 0.0f, 0.0f);
    if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS) {
        material.albedo = glm::vec3(diffuse.r, diffuse.g, diffuse.b);
    }

    // Check for albedo texture
    if (mat->GetTextureCount(aiTextureType_DIFFUSE) > 0) {
        aiString str;
        mat->GetTexture(aiTextureType_DIFFUSE, 0, &str);
        
        // FIX: Handle texture paths from FBX (may have .fbm subfolder or backslashes)
        std::string texturePath = str.C_Str();
        
        // Replace backslashes with forward slashes
        std::replace(texturePath.begin(), texturePath.end(), '\\', '/');
        
        // Remove .fbm/ or .fbm subfolder if present
        size_t fbmPos = texturePath.find(".fbm/");
        if (fbmPos != std::string::npos) {
            texturePath = texturePath.substr(fbmPos + 5);  // Skip ".fbm/"
        }
        
        // Extract just the filename for fallback
        size_t lastSlash = texturePath.find_last_of('/');
        std::string filename = (lastSlash != std::string::npos) ? texturePath.substr(lastSlash + 1) : texturePath;
        
        // Embedded texture first (see loadMap above), then the legacy disk
        // loads below.
        material.albedoMap = loadMap(aiTextureType_DIFFUSE, aiTextureType_DIFFUSE);
        if (material.albedoMap == 0) {
            // Try loading from same directory as model
            std::string path = directory + "/" + texturePath;
            material.albedoMap = loadTexture(path, aiTextureType_DIFFUSE);
            
            // If texture failed to load, try with just filename in same directory
            if (material.albedoMap == 0) {
                std::string fallbackPath = directory + "/" + filename;
                material.albedoMap = loadTexture(fallbackPath, aiTextureType_DIFFUSE);
            }
        }
        
        // If still failed, use default grey texture
        if (material.albedoMap == 0) {
            material.albedoMap = DefaultTexture::GetGreyTexture();
            std::cout << "[Material] Using default grey texture for: " << filename << "\n";
        }
        
        material.hasAlbedoMap = true;  // Always true now (either loaded or default)
    } else {
        // No texture specified - use default grey
        material.albedoMap = DefaultTexture::GetGreyTexture();
        material.hasAlbedoMap = true;
    }
    
    // Check for metallic texture
    if (mat->GetTextureCount(aiTextureType_METALNESS) > 0) {
        material.metallicMap = loadMap(aiTextureType_METALNESS, aiTextureType_METALNESS);
        if (material.metallicMap == 0) {
            aiString str;
            mat->GetTexture(aiTextureType_METALNESS, 0, &str);
            std::string texturePath = str.C_Str();
            std::replace(texturePath.begin(), texturePath.end(), '\\', '/');
            size_t fbmPos = texturePath.find(".fbm/");
            if (fbmPos != std::string::npos) texturePath = texturePath.substr(fbmPos + 5);
            std::string path = directory + "/" + texturePath;
            material.metallicMap = loadTexture(path, aiTextureType_METALNESS);
        }
        if (material.metallicMap == 0) {
            material.metallicMap = DefaultTexture::GetBlackTexture();  // Dielectric default
        }
        material.hasMetallicMap = true;
    }

    // Roughness
    float roughness = 0.5f;
    if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS) {
        material.roughness = roughness;
    }

    // Check for roughness texture
    if (mat->GetTextureCount(aiTextureType_DIFFUSE_ROUGHNESS) > 0) {
        material.roughnessMap = loadMap(aiTextureType_DIFFUSE_ROUGHNESS, aiTextureType_DIFFUSE_ROUGHNESS);
        if (material.roughnessMap == 0) {
            aiString str;
            mat->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &str);
            std::string texturePath = str.C_Str();
            std::replace(texturePath.begin(), texturePath.end(), '\\', '/');
            size_t fbmPos = texturePath.find(".fbm/");
            if (fbmPos != std::string::npos) texturePath = texturePath.substr(fbmPos + 5);
            std::string path = directory + "/" + texturePath;
            material.roughnessMap = loadTexture(path, aiTextureType_DIFFUSE_ROUGHNESS);
        }
        if (material.roughnessMap == 0) {
            material.roughnessMap = DefaultTexture::GetGreyTexture();
        }
        material.hasRoughnessMap = true;
    }

    // Normal map
    if (mat->GetTextureCount(aiTextureType_NORMALS) > 0) {
        material.normalMap = loadMap(aiTextureType_NORMALS, aiTextureType_NORMALS);
        if (material.normalMap == 0) {
            aiString str;
            mat->GetTexture(aiTextureType_NORMALS, 0, &str);
            std::string texturePath = str.C_Str();
            std::replace(texturePath.begin(), texturePath.end(), '\\', '/');
            size_t fbmPos = texturePath.find(".fbm/");
            if (fbmPos != std::string::npos) texturePath = texturePath.substr(fbmPos + 5);
            std::string path = directory + "/" + texturePath;
            material.normalMap = loadTexture(path, aiTextureType_NORMALS);
        }
        if (material.normalMap == 0) {
            material.normalMap = DefaultTexture::GetFlatNormalTexture();  // up = (0,0,1); grey would be degenerate
        }
        material.hasNormalMap = (material.normalMap > 0);
    }

    // AO (Ambient Occlusion)
    if (mat->GetTextureCount(aiTextureType_AMBIENT_OCCLUSION) > 0) {
        material.aoMap = loadMap(aiTextureType_AMBIENT_OCCLUSION, aiTextureType_AMBIENT_OCCLUSION);
        if (material.aoMap == 0) {
            aiString str;
            mat->GetTexture(aiTextureType_AMBIENT_OCCLUSION, 0, &str);
            std::string texturePath = str.C_Str();
            std::replace(texturePath.begin(), texturePath.end(), '\\', '/');
            size_t fbmPos = texturePath.find(".fbm/");
            if (fbmPos != std::string::npos) texturePath = texturePath.substr(fbmPos + 5);
            std::string path = directory + "/" + texturePath;
            material.aoMap = loadTexture(path, aiTextureType_AMBIENT_OCCLUSION);
        }
        if (material.aoMap == 0) {
            material.aoMap = DefaultTexture::GetWhiteTexture();  // white = 1.0, no occlusion darkening
        }
        material.hasAOMap = true;
    }
    
    // Emissive
    aiColor3D emissive(0.0f, 0.0f, 0.0f);
    if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
        material.emissive = glm::vec3(emissive.r, emissive.g, emissive.b);
    }
    
    float emissiveStrength = 1.0f;
    if (mat->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissiveStrength) == AI_SUCCESS) {
        material.emissiveStrength = emissiveStrength;
    }
    
    // Check for emissive texture
    if (mat->GetTextureCount(aiTextureType_EMISSIVE) > 0) {
        material.emissiveMap = loadMap(aiTextureType_EMISSIVE, aiTextureType_EMISSIVE);
        if (material.emissiveMap == 0) {
            aiString str;
            mat->GetTexture(aiTextureType_EMISSIVE, 0, &str);
            std::string texturePath = str.C_Str();
            // FIX: same .fbm/ stripping + backslash normalization as the
            // other texture types above (normal, metallic, etc.). Without this
            // the emissive path retains the absolute temp .fbm path and can
            // never resolve on disk.
            std::replace(texturePath.begin(), texturePath.end(), '\\', '/');
            size_t fbmPos = texturePath.find(".fbm/");
            if (fbmPos != std::string::npos) texturePath = texturePath.substr(fbmPos + 5);
            std::string path = directory + "/" + texturePath;
            material.emissiveMap = loadTexture(path, aiTextureType_EMISSIVE);
        }
        material.hasEmissiveMap = true;
    }
    
    // Alpha/Transparency
    float opacity = 1.0f;
    if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
        material.alpha = opacity;
    }
    
    // Double-sided
    bool twoSided = false;
    if (mat->Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS) {
        material.doubleSided = twoSided;
    }
    
    return material;
}

// =====================================================
unsigned int Model::loadTexture(const std::string& path, aiTextureType type)
{
    // Per-model cache: glTF/FBX assets commonly share one texture file across
    // many meshes. Decoding + CPU-compressing a 4K image per mesh is wasteful
    // (minutes for a single plant model), so return the already-uploaded id.
    // Failed paths are cached as 0 to avoid re-trying them per mesh too.
    // Keyed by path + type because the normal-map upload differs (NEAREST
    // filter, GL_RG/BC5 packing) from diffuse/metallic/ao - reusing the wrong
    // type's GL object would silently apply the wrong sampling state.
    const std::string key = path + "\n" + std::to_string((int)type);
    auto cacheIt = m_textureCache.find(key);
    if (cacheIt != m_textureCache.end()) {
        return cacheIt->second;
    }

    // Check if OpenGL context is available
    if (!glGenTextures) {
        std::cerr << "[Model] WARNING: No OpenGL context available, skipping texture: " << path << "\n";
        return 0;
    }
    
    unsigned int textureID = 0;
    
    // === PATH 1: Try DDS file (pre-compressed BC7/BC5) ===
    std::string ddsPath = path;
    size_t dotPos = ddsPath.rfind('.');
    if (dotPos != std::string::npos) {
        ddsPath.replace(dotPos, std::string::npos, ".dds");
    } else {
        ddsPath += ".dds";
    }
    
    FILE* ddsTest = fopen(ddsPath.c_str(), "rb");
    if (ddsTest) {
        fclose(ddsTest);
        textureID = LoadDDS(ddsPath);
        if (textureID) {
            return textureID;
        }
    }
    
    // === PATH 1.5: Try native EXR counterpart (higher quality than JPG/PNG) ===
    // The shipped assets carry a compressed JPG/PNG stand-in alongside a
    // linear-HDR EXR source (same basename, .exr extension). The async path
    // in LoadModelData() already prefers EXR, but the SYNC Model path
    // (used by SimpleWorldRenderer::loadModel → new Model(path)) skips EXR
    // entirely and falls straight to stbi_load, which cannot decode EXR.
    // We replicate the async path's check here so world objects get the real
    // high-precision normal/roughness/metallic data.
    {
        std::string exrPath = path;
        size_t dotPos2 = exrPath.rfind('.');
        if (dotPos2 != std::string::npos &&
            exrPath.compare(dotPos2, 4, ".exr") != 0) {
            exrPath.replace(dotPos2, std::string::npos, ".exr");
        } else {
            exrPath.clear();
        }
        if (!exrPath.empty()) {
            int exrW = 0, exrH = 0;
            std::vector<uint8_t> exrRGBA;
            if (LoadEXRPixelsRGBA8(exrPath, exrW, exrH, exrRGBA)) {
                std::cout << "[Model] EXR texture loaded: " << exrPath
                          << " (" << exrW << "x" << exrH << ")\n";
                textureID = uploadImageData(exrRGBA.data(), exrW, exrH, 4, type, /*ownedByStbi=*/false);
                m_textureCache[key] = textureID;
                return textureID;
            }
        }
    }
    
    // === PATH 2: Decode the image and upload (BC1/BC3-compressed) ===
    int width, height, nrComponents;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &nrComponents, 0);

    if (data)
    {
        textureID = uploadImageData(data, width, height, nrComponents, type, /*ownedByStbi=*/true);
    }
    else
    {
        std::cerr << "Texture failed to load at path: " << path << std::endl;
    }

    m_textureCache[key] = textureID;
    return textureID;
}

// =====================================================
// Upload decoded pixels (downscale + BC1/BC3 compression
// + GL upload). Shared by loadTexture (disk images) and
// loadEmbeddedTexture (FBX/glTF textures stored inside
// the model file) so both get identical sampling state.
// =====================================================
unsigned int Model::uploadImageData(unsigned char* data, int width, int height,
                                    int nrComponents, aiTextureType type, bool ownedByStbi)
{
    // No GL context (headless model loads / tests): skip the upload instead of
    // calling a NULL glad function pointer (SIGSEGV). Mirrors the guard in
    // loadTexture(). The model still loads its meshes/skeleton - only the GPU
    // texture is missing, which is correct for CPU-only consumers.
    if (!glGenTextures) {
        std::cerr << "[Model] WARNING: No OpenGL context available, skipping texture upload\n";
        return 0;
    }
    unsigned int textureID = 0;
    // DSA object creation: glCreateTextures gives us a texture name with no
    // storage and no binding-point side effects. (Note: the `if (!glGenTextures)`
    // above is a *context-availability* check, NOT a first-use guard -- glGenTextures
    // is a GLAD function-pointer macro that is NULL only when the loader failed /
    // no GL context exists, so it must remain as-is to keep headless/CPU-only
    // loads safe.)
    glCreateTextures(GL_TEXTURE_2D, 1, &textureID);

    glTextureParameteri(textureID, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(textureID, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameteri(textureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(textureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Cap huge (4K+) textures to 1024 before CPU BC1 compression. Decoding
    // + box-filtering a 16M-pixel RGBA buffer is microseconds; compressing
    // it to BC1 is the expensive part (seconds per texture), and 4K maps
    // on small plant meshes gain nothing visually.
    // Cache-friendly single-pass downsampler: reads and writes memory in
    // strict contiguous row-major lines, avoiding the pointer-jumping
    // (non-unit stride) that stalled L1/L2 on the old integer-step loop.
    const int kMaxTexSize = 1024;
    if (width > kMaxTexSize || height > kMaxTexSize) {
        const int newW = std::min(width, kMaxTexSize);
        const int newH = std::min(height, kMaxTexSize);
        const int c = nrComponents > 0 ? nrComponents : 4;

        // Fractional steps in fixed-point space — prevents floating-point
        // accumulation drift and ensures every source pixel contributes.
        const float stepX = static_cast<float>(width) / static_cast<float>(newW);
        const float stepY = static_cast<float>(height) / static_cast<float>(newH);

        m_resizedTexBuffer.assign(static_cast<size_t>(newW) * newH * c, 0);
        unsigned char* dPtr = m_resizedTexBuffer.data();

        // Cache-friendly row-major streaming loop — each inner kernel
        // iteration walks a tightly localised, contiguous row window.
        for (int y = 0; y < newH; ++y) {
            const int srcYStart = static_cast<int>(y * stepY);
            const int srcYEnd   = std::min(height, static_cast<int>((y + 1) * stepY));
            const int filterHeight = std::max(1, srcYEnd - srcYStart);

            for (int x = 0; x < newW; ++x) {
                const int srcXStart = static_cast<int>(x * stepX);
                const int srcXEnd   = std::min(width, static_cast<int>((x + 1) * stepX));
                const int filterWidth  = std::max(1, srcXEnd - srcXStart);

                int r = 0, g = 0, b = 0, a = 0;

                // Kernel aggregation — operates on a tightly localised,
                // contiguous row window (rowPtr walks unit-stride).
                for (int sy = srcYStart; sy < srcYEnd; ++sy) {
                    const unsigned char* rowPtr = data + (static_cast<size_t>(sy) * width + srcXStart) * c;

                    for (int sx = srcXStart; sx < srcXEnd; ++sx) {
                        r += rowPtr[0];
                        if (c > 1) g += rowPtr[1];
                        if (c > 2) b += rowPtr[2];
                        if (c > 3) a += rowPtr[3];
                        rowPtr += c; // Safe, linear stride stepping
                    }
                }

                // Compute box-average division over the accumulated kernel footprint
                const int divisor = filterWidth * filterHeight;
                dPtr[0] = static_cast<unsigned char>(r / divisor);
                if (c > 1) dPtr[1] = static_cast<unsigned char>(g / divisor);
                if (c > 2) dPtr[2] = static_cast<unsigned char>(b / divisor);
                if (c > 3) dPtr[3] = static_cast<unsigned char>(a / divisor);

                dPtr += c; // Stream straight to the next output memory address
            }
        }

        if (ownedByStbi) {
            stbi_image_free(data);  // Safely clean up original STB allocation
        }
        data = m_resizedTexBuffer.data();
        ownedByStbi = false;
        width = newW;
        height = newH;
    }

    bool isNormalMap = (type == aiTextureType_NORMALS);
    bool useCompression = (width >= 4 && height >= 4); // Skip compression for tiny textures

    // Immutable storage must commit to every mip level up front. Compute the
    // full level count on the FINAL (post-downscale) dimensions.
    int levels = 1;
    for (int s = std::max(width, height); s > 1; s >>= 1) ++levels;

    if (useCompression && nrComponents <= 3 && !isNormalMap && nrComponents == 3) {
        // === COMPRESSED: BC1/DXT1 for RGB (6:1 ratio) ===
        std::vector<uint8_t> compressed = CompressBC1(data, width, height);
        int blockW = (width + 3) / 4;
        int blockH = (height + 3) / 4;
        int imageSize = blockW * blockH * 8;
        glTextureStorage2D(textureID, levels, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, width, height);
        glCompressedTextureSubImage2D(textureID, 0, 0, 0, width, height,
                                      GL_COMPRESSED_RGB_S3TC_DXT1_EXT, imageSize, compressed.data());
        glGenerateTextureMipmap(textureID);
    } else if (useCompression && nrComponents == 4) {
        // === COMPRESSED: BC3/DXT5 for RGBA (4:1 ratio) ===
        std::vector<uint8_t> compressed = CompressBC3(data, width, height);
        int blockW = (width + 3) / 4;
        int blockH = (height + 3) / 4;
        int imageSize = blockW * blockH * 16;
        glTextureStorage2D(textureID, levels, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, width, height);
        glCompressedTextureSubImage2D(textureID, 0, 0, 0, width, height,
                                      GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, imageSize, compressed.data());
        glGenerateTextureMipmap(textureID);
    } else if (nrComponents == 1) {
        // === 1-channel: uncompressed (GL_R8) ===
        glTextureStorage2D(textureID, levels, GL_R8, width, height);
        glTextureSubImage2D(textureID, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, data);
        glGenerateTextureMipmap(textureID);
    } else if (isNormalMap && nrComponents == 3) {
        // === Normal map RGB8 ===
        // Use GL_RGB8 + GL_RGB (not GL_RG8 + GL_RG) — the source data from
        // stbi / embedded textures is 3-bytes-per-pixel RGB, and packing it
        // into a 2-component GL_RG upload causes a row-stride mismatch (OpenGL
        // expects W*2 bytes/row for RG but the data is W*3 bytes/row),
        // producing garbled normals and "some parts not properly textured."
        // The async path (uploadTextureFromPixels) already does this correctly.
        glTextureStorage2D(textureID, levels, GL_RGB8, width, height);
        glTextureSubImage2D(textureID, 0, 0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, data);
        glGenerateTextureMipmap(textureID);
    } else {
        // === UNCOMPRESSED FALLBACK (tiny textures or unusual formats) ===
        GLenum format;
        if (nrComponents == 1)
            format = GL_RED;
        else if (nrComponents == 3)
            format = GL_RGB;
        else if (nrComponents == 4)
            format = GL_RGBA;
        else
            format = GL_RGB;

        // glTextureStorage2D requires a *sized* internal format; map the
        // unsized pixel format to its sized storage equivalent.
        GLenum internalFormat = (format == GL_RED)  ? GL_R8
                              : (format == GL_RGB)  ? GL_RGB8
                              : (format == GL_RGBA) ? GL_RGBA8
                              :                       GL_RGB8;
        glTextureStorage2D(textureID, levels, internalFormat, width, height);
        glTextureSubImage2D(textureID, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, data);
        glGenerateTextureMipmap(textureID);
    }

    // ── Phase 2 (Texture): Unify Anisotropic Filtering ────────────────
    // The async path (uploadTextureFromPixels) already sets max anisotropy,
    // but the SYNC Model path (this function) was missing it. Textures loaded
    // on the main thread (environment objects, editor UI assets, etc.) get
    // default 1× filtering, causing grazing-angle shimmer on floors/walls/boots
    // that disrupts FSR3 upscaling. Copy the DSA block so both paths agree.
    if (GLAD_GL_ARB_texture_filter_anisotropic && textureID != 0) {
        GLfloat maxAniso = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
        if (maxAniso > 1.0f) {
            glTextureParameterf(textureID, GL_TEXTURE_MAX_ANISOTROPY, maxAniso);
        }
    }

    if (ownedByStbi) {
        stbi_image_free(data);
    }
    return textureID;
}

// =====================================================
// Embedded textures (FBX Video nodes / glTF bufferViews)
// =====================================================
const aiTexture* Model::findEmbeddedTexture(const aiScene* scene, const std::string& path) const
{
    if (!scene || path.empty()) return nullptr;

    // Assimp convention: a material path of "*<N>" directly indexes the
    // embedded texture array.
    if (path[0] == '*') {
        try {
            const size_t idx = std::stoul(path.substr(1));
            if (idx < scene->mNumTextures) return scene->mTextures[idx];
        } catch (...) { /* not an index - fall through */ }
        return nullptr;
    }

    // Otherwise match by basename. FBX Video nodes record the texture's
    // ORIGINAL export path (often an absolute temp path like
    // ".../skins_xxx.fbm/Vampire_diffuse.png"); the material references the
    // same path, but directories may differ after re-export, so compare only
    // the file name, case-insensitively.
    auto basenameLower = [](std::string s) {
        const size_t slash = s.find_last_of("/\\");
        if (slash != std::string::npos) s = s.substr(slash + 1);
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    const std::string want = basenameLower(path);
    if (want.empty()) return nullptr;
    for (unsigned i = 0; i < scene->mNumTextures; ++i) {
        const aiTexture* t = scene->mTextures[i];
        if (!t) continue;
        if (t->mFilename.length > 0 && basenameLower(t->mFilename.C_Str()) == want) {
            return t;
        }
    }
    return nullptr;
}

unsigned int Model::loadEmbeddedTexture(const aiScene* scene, const std::string& path, aiTextureType type)
{
    // Cache by the material's referenced path so a texture shared across
    // meshes uploads once (mirrors loadTexture's cache).
    const std::string key = "embedded:" + path + "\n" + std::to_string((int)type);
    auto cacheIt = m_textureCache.find(key);
    if (cacheIt != m_textureCache.end()) {
        return cacheIt->second;
    }
    unsigned int id = 0;
    if (const aiTexture* tex = findEmbeddedTexture(scene, path)) {
        // Compressed image (PNG/JPEG/TGA...): pcData holds mWidth bytes.
        // Uncompressed: pcData holds mWidth*mHeight raw RGBA8888 texels.
        if (tex->mHeight == 0 && tex->pcData) {
            int w = 0, h = 0, c = 0;
            stbi_set_flip_vertically_on_load(true);
            unsigned char* data = stbi_load_from_memory(
                reinterpret_cast<const unsigned char*>(tex->pcData),
                static_cast<int>(tex->mWidth), &w, &h, &c, 0);
            if (data) {
                id = uploadImageData(data, w, h, c, type, /*ownedByStbi=*/true);
            }
        } else if (tex->mHeight > 0 && tex->pcData) {
            id = uploadImageData(reinterpret_cast<unsigned char*>(tex->pcData),
                                 static_cast<int>(tex->mWidth), static_cast<int>(tex->mHeight),
                                 4, type, /*ownedByStbi=*/false);
        }
    }
    m_textureCache[key] = id;
    return id;
}

// =====================================================
// Hierarchy
// =====================================================

// Helper: Check if a node is an Assimp helper node
// Assimp creates helper nodes with names like "$AssimpFbx$_Translation", "$AssimpFbx$_PreRotation", etc.
static bool IsAssimpHelperNode(const std::string& name)
{
    // Check for $AssimpFbx$_ (case-insensitive)
    std::string lowerName = name;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
    return lowerName.find("$assimpfbx$_") != std::string::npos || 
           lowerName.find("$assimpfbx$") != std::string::npos;
}

// Helper: Convert aiMatrix4x4 to glm::mat4 (static version for use in helper functions)
static glm::mat4 AiMat4ToGlm(const aiMatrix4x4& m)
{
    glm::mat4 r;
    r[0][0] = m.a1; r[1][0] = m.a2; r[2][0] = m.a3; r[3][0] = m.a4;
    r[0][1] = m.b1; r[1][1] = m.b2; r[2][1] = m.b3; r[3][1] = m.b4;
    r[0][2] = m.c1; r[1][2] = m.c2; r[2][2] = m.c3; r[3][2] = m.c4;
    r[0][3] = m.d1; r[1][3] = m.d2; r[2][3] = m.d3; r[3][3] = m.d4;
    return r;
}

// Recursive function to build collapsed hierarchy - processes a single node
static void ProcessNodeRecursive(
    const aiNode* src,
    const glm::mat4& accumulatedTransform,
    std::vector<AssimpNodeData>& outChildren)
{
    std::string srcName = src->mName.C_Str();
    bool isHelper = IsAssimpHelperNode(srcName);
    
    // Calculate this node's accumulated transform
    glm::mat4 srcTransform = AiMat4ToGlm(src->mTransformation);
    glm::mat4 newAccumulated = accumulatedTransform * srcTransform;
    
    if (isHelper) {
        // This is a helper node - don't create a node for it,
        // but process its children with accumulated transform
        for (unsigned i = 0; i < src->mNumChildren; ++i) {
            ProcessNodeRecursive(src->mChildren[i], newAccumulated, outChildren);
        }
    } else {
        // This is a real node - create it and add to children
        AssimpNodeData newNode;
        newNode.name = NormalizeBoneName(srcName);
        newNode.transform = newAccumulated;
        newNode.boneIndex = -1;
        newNode.children.clear();
        
        // Process this node's children
        std::vector<AssimpNodeData> childNodes;
        for (unsigned i = 0; i < src->mNumChildren; ++i) {
            ProcessNodeRecursive(src->mChildren[i], glm::mat4(1.0f), childNodes);
        }
        newNode.children = std::move(childNodes);
        
        outChildren.push_back(std::move(newNode));
    }
}

// Static collapsing-hierarchy builder for the async loading path.
// Mirrors ReadHierarchyRecursive(dest, src, identity): collapses
// $AssimpFbx$ helper nodes and accumulates their transforms so the skeleton
// hierarchy matches the sync Model() path. WITHOUT this the async skeleton
// (used by AnimatedCharacter) keeps the raw FBX helper nodes, and every
// animated bone position is computed ~2x too large - legs/hands visibly
// deformed (the "idle pose blown up" bug).
static void BuildCollapsedHierarchyStatic(AssimpNodeData &dest, const aiNode *src)
{
    std::string srcName = src->mName.C_Str();
    bool isHelper = IsAssimpHelperNode(srcName);

    glm::mat4 srcTransform = AiMat4ToGlm(src->mTransformation);
    glm::mat4 newAccumulated = srcTransform;

    if (isHelper) {
        std::vector<AssimpNodeData> rootChildren;
        for (unsigned i = 0; i < src->mNumChildren; ++i) {
            ProcessNodeRecursive(src->mChildren[i], newAccumulated, rootChildren);
        }
        if (rootChildren.size() == 1) {
            dest = std::move(rootChildren[0]);
        } else if (rootChildren.size() > 1) {
            dest.name = "root";
            dest.transform = glm::mat4(1.0f);
            dest.boneIndex = -1;
            dest.children = std::move(rootChildren);
        } else {
            dest.name = "root";
            dest.transform = glm::mat4(1.0f);
            dest.boneIndex = -1;
            dest.children.clear();
        }
    } else {
        dest.name = NormalizeBoneName(srcName);
        dest.transform = newAccumulated;
        dest.boneIndex = -1;
        dest.children.clear();
        for (unsigned i = 0; i < src->mNumChildren; ++i) {
            AssimpNodeData child;
            std::vector<AssimpNodeData> childNodes;
            ProcessNodeRecursive(src->mChildren[i], glm::mat4(1.0f), childNodes);
            if (childNodes.size() == 1) {
                child = std::move(childNodes[0]);
            } else if (childNodes.size() > 1) {
                child.name = NormalizeBoneName(src->mChildren[i]->mName.C_Str());
                child.transform = AiMat4ToGlm(src->mChildren[i]->mTransformation);
                child.boneIndex = -1;
                child.children = std::move(childNodes);
            }
            dest.children.push_back(std::move(child));
        }
    }
}

void Model::ReadHierarchyRecursive(AssimpNodeData &dest, const aiNode *src, const glm::mat4 &accumulatedTransform)
{
    // Process the root - it might be a helper or a real node
    std::string srcName = src->mName.C_Str();
    bool isHelper = IsAssimpHelperNode(srcName);
    
    glm::mat4 srcTransform = aiMat4ToGlm(src->mTransformation);
    glm::mat4 newAccumulated = accumulatedTransform * srcTransform;
    
    if (isHelper) {
        // Root is a helper - process its children directly into dest's children
        std::vector<AssimpNodeData> rootChildren;
        for (unsigned i = 0; i < src->mNumChildren; ++i) {
            ProcessNodeRecursive(src->mChildren[i], newAccumulated, rootChildren);
        }
        
        // If there's exactly one child, make it the root
        if (rootChildren.size() == 1) {
            dest = std::move(rootChildren[0]);
        } else if (rootChildren.size() > 1) {
            // Multiple children - create a synthetic root
            dest.name = "root";
            dest.transform = glm::mat4(1.0f);
            dest.boneIndex = -1;
            dest.children = std::move(rootChildren);
        } else {
            // No children - empty node
            dest.name = "root";
            dest.transform = glm::mat4(1.0f);
            dest.boneIndex = -1;
            dest.children.clear();
        }
    } else {
        // Root is a real node
        dest.name = NormalizeBoneName(srcName);
        dest.transform = newAccumulated;
        dest.boneIndex = -1;
        dest.children.clear();
        
        // Process children
        for (unsigned i = 0; i < src->mNumChildren; ++i) {
            AssimpNodeData child;
            std::vector<AssimpNodeData> childNodes;
            ProcessNodeRecursive(src->mChildren[i], glm::mat4(1.0f), childNodes);
            
            if (childNodes.size() == 1) {
                child = std::move(childNodes[0]);
            } else if (childNodes.size() > 1) {
                // This shouldn't happen for direct children, but handle it
                child.name = NormalizeBoneName(src->mChildren[i]->mName.C_Str());
                child.transform = aiMat4ToGlm(src->mChildren[i]->mTransformation);
                child.boneIndex = -1;
                child.children = std::move(childNodes);
            }
            dest.children.push_back(std::move(child));
        }
    }
}

// Overload for backward compatibility
void Model::ReadHierarchyRecursive(AssimpNodeData &dest, const aiNode *src)
{
    ReadHierarchyRecursive(dest, src, glm::mat4(1.0f));
}

void Model::ReadHierarchy(AssimpNodeData &dest, const aiNode *sceneRoot)
{
    if (debugOutput) {
        std::cout << "[ReadHierarchy] Using scene root as skeleton root.\n";
        std::cout << "[ReadHierarchy] Collapsing Assimp helper nodes ($AssimpFbx$) for correct animation.\n";
    }
    ReadHierarchyRecursive(dest, sceneRoot);
}

// =====================================================
// ExtractBones
// =====================================================
void Model::ExtractBones(aiMesh *mesh, const AssimpNodeData &rootNode)
{
    for (unsigned i = 0; i < mesh->mNumBones; ++i)
    {
        aiBone *bone = mesh->mBones[i];
        std::string name = NormalizeBoneName(bone->mName.C_Str());

        if (m_Skeleton.boneMapping.count(name) == 0)
        {
            int id = (int)m_Skeleton.bones.size();
            m_Skeleton.boneMapping[name] = id;

            glm::mat4 offsetMat = aiMat4ToGlm(bone->mOffsetMatrix);

            glm::mat4 bindLocal(1.0f);
            bool found = FindBindPoseInHierarchy(rootNode, name, bindLocal);

            if (!found)
            {
                bindLocal = glm::mat4(1.0f);
            }

            BoneInfo info;
            info.id = id;
            info.offset = offsetMat;
            info.bindTransform = bindLocal;

            m_Skeleton.bones.push_back(info);

            if (debugOutput) {
                std::cout << "[EXTRACT_BONE] '" << name << "' -> globalID=" << id
                          << " foundInHierarchy=" << (found ? "YES" : "NO") << "\n";
            }
        }
    }
}

// =====================================================
// extractBoneWeights
// =====================================================
void Model::extractBoneWeights(
    std::vector<Vertex> &vertices,
    aiMesh *mesh)
{
    std::cout << "[extractBoneWeights] Processing " << mesh->mNumBones << " bones, " << vertices.size() << " vertices\n";
    
    // Count vertices per bone for debugging
    std::map<int, int> boneVertexCount;
    
    for (unsigned i = 0; i < mesh->mNumBones; ++i)
    {
        aiBone *bone = mesh->mBones[i];
        std::string name = NormalizeBoneName(bone->mName.C_Str());

        auto it = m_Skeleton.boneMapping.find(name);
        if (it == m_Skeleton.boneMapping.end()) {
            std::cout << "  [WARN] Bone '" << bone->mName.C_Str() << "' not found in skeleton mapping!\n";
            continue;
        }

        int boneID = it->second;
        int assignedCount = 0;

        for (unsigned j = 0; j < bone->mNumWeights; ++j)
        {
            auto &w = bone->mWeights[j];
            Vertex &v = vertices[w.mVertexId];

            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k)
            {
                if (v.Weights[k] == 0.0f)
                {
                    v.BoneIDs[k] = boneID;
                    v.Weights[k] = w.mWeight;
                    assignedCount++;
                    boneVertexCount[boneID]++;
                    break;
                }
            }
        }
        
        // Print for leg bones
        if (boneID >= 55 && boneID <= 62) {
            std::cout << "  Bone " << boneID << " (" << name << "): assigned " << assignedCount << " vertices\n";
        }
    }
    
    // Print summary for key bones
    std::cout << "[extractBoneWeights] Vertex distribution for key bones:\n";
    for (auto& [boneID, count] : boneVertexCount) {
        if (boneID == 0 || boneID == 55 || boneID == 56 || boneID == 60 || boneID == 61 || boneID == 62) {
            std::cout << "  Bone " << boneID << ": " << count << " vertices\n";
        }
    }
}

// =====================================================
void Model::UploadBoneTexture(Shader &shader, const std::vector<glm::mat4> &mats)
{
    if (mats.empty())
        return;
    
    // Check if OpenGL context is available
    if (!glGenTextures) {
        return;  // Silently skip - no GL context (e.g., in unit tests)
    }

    // Create once with fixed size
    if (boneTexID == 0)
    {
        glCreateTextures(GL_TEXTURE_2D, 1, &boneTexID);

        // Allocate once with maximum reasonable size
        int maxWidth = 256 * 4; // Support up to 256 bones
        glTextureStorage2D(boneTexID, 1, GL_RGBA32F, maxWidth, 1);   // immutable, 1 level (no mipmaps)

        glTextureParameteri(boneTexID, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(boneTexID, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(boneTexID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(boneTexID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // Layout: 1 row, width = mats.size()*4 pixels
    int width = (int)mats.size() * 4;

    std::vector<glm::vec4> pixels;
    pixels.resize(width);

    for (size_t i = 0; i < mats.size(); i++)
    {
        const glm::mat4 &m = mats[i];

        // Store columns (matches shader sampling)
        pixels[i * 4 + 0] = m[0];
        pixels[i * 4 + 1] = m[1];
        pixels[i * 4 + 2] = m[2];
        pixels[i * 4 + 3] = m[3];
    }

    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, boneTexID);

    // DSA sub-image update into the immutable storage allocated above (no
    // reallocation, no bind dependency for the upload itself).
    glTextureSubImage2D(
        boneTexID,
        0,
        0, 0,
        width,
        1,
        GL_RGBA,
        GL_FLOAT,
        pixels.data());

    shader.setInt("boneTex", 10);
    shader.setInt("uPaletteSize", (int)mats.size());
}

// =====================================================
glm::vec3 Model::GetSize() const
{
    return boundingBox.Size();
}

// =====================================================
// Bounding Volume Calculation
// =====================================================
void Model::calculateBoundingVolumes()
{
    boundingBox = BoundingBox();
    
    // Calculate bounding box from all meshes
    for (const auto &mesh : meshes) {
        for (const auto &v : mesh.vertices) {
            boundingBox.Extend(v.Position);
        }
    }
    
    // Calculate bounding sphere
    if (boundingBox.IsValid()) {
        boundingSphere.center = boundingBox.Center();
        boundingSphere.radius = boundingBox.Radius();
    }
}

// =====================================================
// LOD Generation
// =====================================================
void Model::generateLODLevels()
{
    lodLevels.resize(meshes.size());
    
    for (size_t i = 0; i < meshes.size(); ++i)
    {
        // LOD 0: Original mesh with Forsyth triangle reordering only
        {
            Mesh lodMesh(meshes[i].vertices, meshes[i].indices, meshes[i].textures);
            MeshUtils::MeshOptimizationConfig config;
            config.reorderTriangles = true;
            config.clusterVertices = false;
            config.targetCacheSize = 24;
            MeshUtils::OptimizeMeshForRendering(lodMesh, config);
            lodLevels[i].emplace_back(std::move(lodMesh), 0.0f);
        }
        
        // LOD 1: Medium quality (light vertex clustering)
        {
            Mesh lodMesh(meshes[i].vertices, meshes[i].indices, meshes[i].textures);
            float cellSize = glm::max(0.05f, meshes[i].boundingBox.Radius() * 0.05f);
            MeshUtils::MeshOptimizationConfig config;
            config.reorderTriangles = true;
            config.clusterVertices = true;
            config.clusterCellSize = cellSize;
            config.targetCacheSize = 24;
            MeshUtils::OptimizeMeshForRendering(lodMesh, config);
            lodMesh.RecalculateNormals();
            lodLevels[i].emplace_back(std::move(lodMesh), 10.0f);
        }
        
        // LOD 2: Low quality (moderate vertex clustering)
        {
            Mesh lodMesh(meshes[i].vertices, meshes[i].indices, meshes[i].textures);
            float cellSize = glm::max(0.1f, meshes[i].boundingBox.Radius() * 0.1f);
            MeshUtils::MeshOptimizationConfig config;
            config.reorderTriangles = true;
            config.clusterVertices = true;
            config.clusterCellSize = cellSize;
            config.targetCacheSize = 24;
            MeshUtils::OptimizeMeshForRendering(lodMesh, config);
            lodMesh.RecalculateNormals();
            lodLevels[i].emplace_back(std::move(lodMesh), 25.0f);
        }
        
        // LOD 3: Lowest quality (aggressive vertex clustering)
        {
            Mesh lodMesh(meshes[i].vertices, meshes[i].indices, meshes[i].textures);
            float cellSize = glm::max(0.2f, meshes[i].boundingBox.Radius() * 0.2f);
            MeshUtils::MeshOptimizationConfig config;
            config.reorderTriangles = true;
            config.clusterVertices = true;
            config.clusterCellSize = cellSize;
            config.targetCacheSize = 24;
            MeshUtils::OptimizeMeshForRendering(lodMesh, config);
            lodMesh.RecalculateNormals();
            lodLevels[i].emplace_back(std::move(lodMesh), 50.0f);
        }
    }
    
    if (debugOutput) {
        std::cout << "[LOD] Generated " << lodLevels.size() << " LOD level sets with Forsyth optimization\n";
        for (size_t i = 0; i < lodLevels.size(); ++i) {
            std::cout << "  Mesh " << i << ": " << lodLevels[i].size() << " LOD levels\n";
            for (size_t j = 0; j < lodLevels[i].size(); ++j) {
                std::cout << "    LOD" << j << ": " << lodLevels[i][j].mesh.vertices.size() 
                          << " vertices, " << lodLevels[i][j].mesh.indices.size() / 3 
                          << " triangles (threshold: " << lodLevels[i][j].distanceThreshold << ")\n";
            }
        }
    }
}

// =====================================================
void Model::AddLODLevel(const std::string& lodModelPath, float distanceThreshold)
{
    // Load additional LOD model
    Assimp::Importer importer;
    const aiScene* lodScene = importer.ReadFile(
        lodModelPath,
        aiProcess_Triangulate | aiProcess_GenSmoothNormals);
    
    if (!lodScene) {
        std::cerr << "Failed to load LOD model: " << lodModelPath << std::endl;
        return;
    }
    
    // Process first mesh as LOD level
    if (lodScene->mNumMeshes > 0 && lodLevels.size() > 0) {
        Mesh lodMesh = processMesh(lodScene->mMeshes[0], lodScene);
        lodLevels[0].emplace_back(std::move(lodMesh), distanceThreshold);
        
        // Sort LOD levels by distance threshold
        std::sort(lodLevels[0].begin(), lodLevels[0].end(),
            [](const LODLevel& a, const LODLevel& b) {
                return a.distanceThreshold < b.distanceThreshold;
            });
    }
}

// =====================================================
// Material Management
// =====================================================
void Model::SetMeshMaterial(size_t meshIndex, const PBRMaterial& material)
{
    if (meshIndex < meshMaterials.size()) {
        meshMaterials[meshIndex] = material;
    }
}

const PBRMaterial& Model::GetMeshMaterial(size_t meshIndex) const
{
    static PBRMaterial defaultMat;
    if (meshIndex < meshMaterials.size()) {
        return meshMaterials[meshIndex];
    }
    return defaultMat;
}

// =====================================================
// Statistics
// =====================================================
int Model::GetTotalTriangleCount() const
{
    int total = 0;
    for (const auto& mesh : meshes) {
        total += static_cast<int>(mesh.indices.size()) / 3;
    }
    return total;
}

int Model::GetVertexCount() const
{
    int total = 0;
    for (const auto& mesh : meshes) {
        total += static_cast<int>(mesh.vertices.size());
    }
    return total;
}

// =====================================================
// Texture Loading (legacy)
// =====================================================
std::vector<Texture> Model::loadMaterialTextures(
    aiMaterial *mat,
    aiTextureType type,
    const std::string &typeName)
{
    std::vector<Texture> textures;

    for (unsigned i = 0; i < mat->GetTextureCount(type); ++i)
    {
        aiString str;
        mat->GetTexture(type, i, &str);

        Texture tex;
        tex.path = str.C_Str();
        tex.type = std::string("texture_diffuse") + std::to_string(textures.size() + 1);

        try
        {
            load_texture_image loader(tex.path.c_str());
            tex.id = loader.texture;
            loader.freeBuffer();
        }
        catch (...)
        {
            tex.id = 0;
        }

        textures.push_back(tex);
    }

    return textures;
}
