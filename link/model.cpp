#define GLM_ENABLE_EXPERIMENTAL

#include "Model.h"
#include "DebugDraw.h"
#include "stb_image.h"
#include "load_texture_image.h"
#include "BoneName.h"

#include <iostream>
#include <functional>

#include <assimp/postprocess.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/string_cast.hpp>

// =====================================================
// Utility
// =====================================================

std::unordered_map<std::string, int>
BuildNormalizedBoneMap(const Skeleton& skeleton)
{
    std::unordered_map<std::string, int> result;
    for (const auto& [boneName, index] : skeleton.boneMapping)
        result[NormalizeBoneName(boneName)] = index;

    return result;
}

void Model::BuildNodeBoneMap(
    AssimpNodeData& node,
    const std::unordered_map<std::string, int>& normBoneMap)
{
    auto it = normBoneMap.find(node.name);
    node.boneIndex = (it != normBoneMap.end()) ? it->second : -1;
    
    for (auto& child : node.children)
        BuildNodeBoneMap(child, normBoneMap);
}

// =====================================================
glm::mat4 Model::aiMat4ToGlm(const aiMatrix4x4& m)
{
    glm::mat4 r;
    // Assimp stores matrices row-major (a1..a4 is row 0). glm::mat4 is column-major.
    // Map rows->columns: r[col][row] = m(row,col)
    r[0][0] = m.a1; r[1][0] = m.a2; r[2][0] = m.a3; r[3][0] = m.a4;
    r[0][1] = m.b1; r[1][1] = m.b2; r[2][1] = m.b3; r[3][1] = m.b4;
    r[0][2] = m.c1; r[1][2] = m.c2; r[2][2] = m.c3; r[3][2] = m.c4;
    r[0][3] = m.d1; r[1][3] = m.d2; r[2][3] = m.d3; r[3][3] = m.d4;

    return r;
}

// Find bind pose transform from hierarchy
static glm::mat4 FindBindPoseInHierarchy(
    const AssimpNodeData& node,
    const std::string& boneName)
{
    if (NormalizeBoneName(node.name) == boneName)
        return node.transform;

    for (const auto& child : node.children)
    {
        glm::mat4 result = FindBindPoseInHierarchy(child, boneName);
        if (result != glm::mat4(0.0f))  // return non-zero result
            return result;
    }

    return glm::mat4(0.0f);  // not found
}

// =====================================================
static void AddBoneData(Vertex& v, int boneID, float weight)
{
    for (int i = 0; i < MAX_BONE_INFLUENCE; ++i)
    {
        if (v.Weights[i] == 0.0f)
        {
            v.BoneIDs[i] = boneID;
            v.Weights[i] = weight;
            return;
        }
    }
}

// =====================================================
// Root bone detection
// =====================================================
int FindRootBoneIndex(const Skeleton& skeleton)
{
    std::function<int(const AssimpNodeData&, int)> walk =
        [&](const AssimpNodeData& node, int parentBone) -> int
    {
        int thisBone = node.boneIndex;

        // If this node IS a bone and parent is NOT a bone → ROOT
        if (thisBone >= 0 && parentBone == -1)
            return thisBone;

        for (const auto& child : node.children)
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
Model::Model(const std::string& path)
{
    loadModel(path);

    int influenced = 0;
    for (auto& mesh : meshes)
        for (auto& v : mesh.vertices)
            if (v.Weights.x + v.Weights.y + v.Weights.z + v.Weights.w > 0.0f)
                influenced++;

    rootBoneIndex = FindRootBoneIndex(m_Skeleton);

    std::cout << "Vertices influenced by bones: " << influenced << "\n";
    std::cout << "Root bone index: " << rootBoneIndex << "\n";
    
    // Find root bone name from boneMapping
    std::string rootBoneName = "";
    for (const auto& [name, idx] : m_Skeleton.boneMapping) {
        if (idx == m_Skeleton.rootBoneIndex) {
            rootBoneName = name;
            break;
        }
    }
    std::cout << "Root bone name: " << rootBoneName << "\n";

    // DEBUG: Print bind pose for first few bones
    std::cout << "\n[BIND POSE DEBUG]\n";
    for (int i = 0; i < std::min(5, (int)m_Skeleton.bones.size()); ++i) {
        glm::vec3 pos = glm::vec3(m_Skeleton.bones[i].offset[3]);
        
        // Extract scale from offset matrix
        float sx = glm::length(glm::vec3(m_Skeleton.bones[i].offset[0]));
        float sy = glm::length(glm::vec3(m_Skeleton.bones[i].offset[1]));
        float sz = glm::length(glm::vec3(m_Skeleton.bones[i].offset[2]));
        
        std::cout << "  B" << i << " offset_pos=" << glm::to_string(pos)
                  << " scale=(" << sx << ", " << sy << ", " << sz << ")\n";
    }
    std::cout << "\n";
    
    // DEBUG: Also print leg bone offsets
    if (m_Skeleton.rootBoneIndex >= 0) {
        std::cout << "[LEG BONE OFFSETS]\n";
        for (int i = 0; i < (int)m_Skeleton.bones.size(); ++i) {
            std::string boneName = "";
            for (const auto& [name, idx] : m_Skeleton.boneMapping) {
                if (idx == i) {
                    boneName = name;
                    break;
                }
            }
            
            if (boneName.find("leg") != std::string::npos ||
                boneName.find("foot") != std::string::npos ||
                boneName.find("toe") != std::string::npos) {
                glm::vec3 pos = glm::vec3(m_Skeleton.bones[i].offset[3]);
                std::cout << "  B" << i << " (" << boneName << ") offset_pos=" 
                          << glm::to_string(pos) << "\n";
            }
        }
        std::cout << "\n";
    }

    if (m_Skeleton.rootBoneIndex == -1)
    {
        std::cerr << "WARNING: No root bone found. Animations disabled.\n";
    }

}

// =====================================================
void Model::Draw(Shader& shader, Animator& animator)
{
    const auto& finalBones = animator.GetFinalBoneMatrices();

    static bool firstDraw = true;
    if (firstDraw) {
        std::cout << "[Model::Draw] FIRST DRAW: finalBones.size()=" << finalBones.size() << "\n";
        for (int i = 0; i < std::min(3, (int)finalBones.size()); i++) {
            std::cout << "  B" << i << " pos=" << glm::to_string(glm::vec3(finalBones[i][3])) << "\n";
        }
    }

    for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex)
    {
        auto& mesh = meshes[meshIndex];
        
        if (firstDraw) {
            std::cout << "[Model::Draw] Mesh " << meshIndex << ": vertices=" << mesh.vertices.size() 
                      << ", sample vertex [0] boneIDs: " << mesh.vertices[0].BoneIDs[0] << " "
                      << mesh.vertices[0].BoneIDs[1] << " " << mesh.vertices[0].BoneIDs[2] << " "
                      << mesh.vertices[0].BoneIDs[3] << "\n";
        }

        // ✅ CRITICAL FIX: Always upload the FULL global bone array (size 65).
        // Do not use per-mesh palette compaction. Vertices store global bone IDs.
        
        std::vector<glm::mat4> paletteMats = finalBones; // copy full array

        // Upload full palette to boneTex
        UploadBoneTexture(shader, paletteMats);

        // IMPORTANT: Tell shader we have the full skeleton
        shader.setInt("uPaletteSize", (int)paletteMats.size());
        
        if (firstDraw) {
            std::cout << "[Model::Draw] UploadBoneTexture done, uPaletteSize=" << (int)paletteMats.size() << "\n";
        }

        // Draw mesh
        mesh.Draw(shader);
        
        if (firstDraw) {
            std::cout << "[Model::Draw] mesh.Draw() called for mesh " << meshIndex << "\n";
        }
    }
    
    firstDraw = false;
}



// =====================================================
Animation* Model::GetAnimation(size_t index)
{
    if (index >= m_Animations.size()) return nullptr;
    return m_Animations[index].get();
}


// =====================================================
void Model::loadModel(const std::string& path)
{
    scene = importer.ReadFile(
        path,
        aiProcess_Triangulate |
        aiProcess_GenSmoothNormals |
        aiProcess_FlipUVs |
        aiProcess_CalcTangentSpace
    );

    if (!scene || !scene->mRootNode)
    {
        std::cerr << "ASSIMP ERROR: " << importer.GetErrorString() << "\n";
        return;
    }

    directory = path.substr(0, path.find_last_of('/'));

    ReadHierarchy(m_Skeleton.rootNode, scene->mRootNode);

    processNode(scene->mRootNode, scene);

    auto normMap = BuildNormalizedBoneMap(m_Skeleton);
    BuildNodeBoneMap(m_Skeleton.rootNode, normMap);

    m_Skeleton.rootBoneIndex = FindRootBoneIndex(m_Skeleton);
    std::cout << "Skeleton root bone index: "
              << m_Skeleton.rootBoneIndex << "\n";

    // globalInverseTransform converts scene space to bind pose space
    // STEP 1: Set to identity (fast fix for visibility)
    m_Skeleton.globalInverseTransform = glm::mat4(1.0f);


    for (unsigned i = 0; i < scene->mNumAnimations; ++i)
    {
        aiAnimation* a = scene->mAnimations[i];
        m_Animations.push_back(std::make_unique<Animation>(
            a->mName.C_Str(),
            float(a->mDuration),
            float(a->mTicksPerSecond > 0 ? a->mTicksPerSecond : 25.0f)
        ));
    }

    std::cout << "Model loaded: " << path
              << " | Bones: " << m_Skeleton.bones.size() << "\n";
}

// =====================================================
void Model::processNode(aiNode* node, const aiScene* scene)
{
    for (unsigned i = 0; i < node->mNumMeshes; ++i)
        meshes.push_back(processMesh(scene->mMeshes[node->mMeshes[i]], scene));

    for (unsigned i = 0; i < node->mNumChildren; ++i)
        processNode(node->mChildren[i], scene);
}

// =====================================================
Mesh Model::processMesh(aiMesh* mesh, const aiScene* scene)
{
    std::vector<Vertex> vertices(mesh->mNumVertices);
    std::vector<unsigned int> indices;
    std::vector<Texture> textures;

    for (unsigned i = 0; i < mesh->mNumVertices; ++i)
    {
        Vertex& v = vertices[i];
        v.Position = { mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z };
        v.Normal = mesh->HasNormals()
            ? glm::vec3(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z)
            : glm::vec3(0.0f);

        v.TexCoords = mesh->mTextureCoords[0]
            ? glm::vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y)
            : glm::vec2(0.0f);

        for (int j = 0; j < MAX_BONE_INFLUENCE; ++j)
        {
            v.BoneIDs[j] = -1;
            v.Weights[j] = 0.0f;
        }
    }

    for (unsigned i = 0; i < mesh->mNumFaces; ++i)
        for (unsigned j = 0; j < mesh->mFaces[i].mNumIndices; ++j)
            indices.push_back(mesh->mFaces[i].mIndices[j]);

    ExtractBones(mesh, m_Skeleton.rootNode);
    extractBoneWeights(vertices, mesh);

    if (mesh->mMaterialIndex >= 0)
    {
        aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
        auto diffuseMaps = loadMaterialTextures(material, aiTextureType_DIFFUSE, "albedo");
        textures.insert(textures.end(), diffuseMaps.begin(), diffuseMaps.end());
    }

    return Mesh(vertices, indices, textures);
}



// ------------------------------------------------------------
// Normalize
// ------------------------------------------------------------


// ------------------------------------------------------------
// Hierarchy
// ------------------------------------------------------------
void Model::ReadHierarchy(AssimpNodeData& dest, const aiNode* src)
{
    dest.name = NormalizeBoneName(src->mName.C_Str());
    glm::mat4 transform = aiMat4ToGlm(src->mTransformation);
    dest.transform = transform;
    dest.children.clear();

    for (unsigned i = 0; i < src->mNumChildren; ++i)
    {
        AssimpNodeData child;
        ReadHierarchy(child, src->mChildren[i]);
        dest.children.push_back(child);
    }
}

// ------------------------------------------------------------
// ExtractBones (CRITICAL FIX)
// STEP 2: Extract both offset matrix AND bind pose from hierarchy
// ------------------------------------------------------------
void Model::ExtractBones(aiMesh* mesh, const AssimpNodeData& rootNode)
{
    for (unsigned i = 0; i < mesh->mNumBones; ++i)
    {
        aiBone* bone = mesh->mBones[i];
        std::string name = NormalizeBoneName(bone->mName.C_Str());

        if (m_Skeleton.boneMapping.count(name) == 0)
        {
            int id = (int)m_Skeleton.bones.size();
            m_Skeleton.boneMapping[name] = id;

            glm::mat4 offsetMat = aiMat4ToGlm(bone->mOffsetMatrix);

            // STEP 2: Extract bind pose transform from hierarchy
            glm::mat4 bindLocal = FindBindPoseInHierarchy(rootNode, name);

            BoneInfo info;
            info.id = id;
            info.offset = offsetMat;
            info.bindTransform = bindLocal;
            m_Skeleton.bones.push_back(info);
            // DEBUG: report bone mapping as it's created
            std::cout << "[EXTRACT_BONE] mesh has bone '" << name << "' -> globalID=" << id << "\n";
        }
    }
}


// ------------------------------------------------------------
// extractBoneWeights
// ------------------------------------------------------------
void Model::extractBoneWeights(
    std::vector<Vertex>& vertices,
    aiMesh* mesh)
{
    for (unsigned i = 0; i < mesh->mNumBones; ++i)
    {
        aiBone* bone = mesh->mBones[i];
        std::string name = NormalizeBoneName(bone->mName.C_Str());

        auto it = m_Skeleton.boneMapping.find(name);
        if (it == m_Skeleton.boneMapping.end())
            continue;

        int boneID = it->second;

        // DEBUG: print first few weights for this aiBone
        std::cout << "[AI_BONE_WEIGHTS] bone='" << name << "' globalID=" << boneID
                  << " weightCount=" << bone->mNumWeights << " samples:";
        for (unsigned j = 0; j < std::min<unsigned>(bone->mNumWeights, 5); ++j) {
            auto& w = bone->mWeights[j];
            std::cout << " (v=" << w.mVertexId << ",w=" << w.mWeight << ")";
        }
        std::cout << "\n";

        for (unsigned j = 0; j < bone->mNumWeights; ++j)
        {
            auto& w = bone->mWeights[j];
            Vertex& v = vertices[w.mVertexId];

            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k)
            {
                if (v.Weights[k] == 0.0f)
                {
                    v.BoneIDs[k] = boneID;
                    v.Weights[k] = w.mWeight;
                    break;
                }
            }
        }
    }
}
void Model::UploadBoneTexture(Shader& shader, const std::vector<glm::mat4>& mats)
{
    if (mats.empty())
        return;

    // Create once
    if (boneTexID == 0)
    {
        glGenTextures(1, &boneTexID);
        glBindTexture(GL_TEXTURE_2D, boneTexID);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // Layout: 1 row, width = mats.size()*4 pixels
    int width = (int)mats.size() * 4;

    std::vector<glm::vec4> pixels;
    pixels.resize(width);

    for (size_t i = 0; i < mats.size(); i++)
    {
        const glm::mat4& m = mats[i];

        // IMPORTANT: store columns (matches your shader)
        pixels[i * 4 + 0] = m[0];
        pixels[i * 4 + 1] = m[1];
        pixels[i * 4 + 2] = m[2];
        pixels[i * 4 + 3] = m[3];
    }

    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, boneTexID);

    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA32F,
        width,
        1,
        0,
        GL_RGBA,
        GL_FLOAT,
        pixels.data()
    );

    shader.setInt("boneTex", 10);
}

// =====================================================
std::vector<Texture> Model::loadMaterialTextures(
    aiMaterial* mat,
    aiTextureType type,
    const std::string& typeName)
{
    std::vector<Texture> textures;
    for (unsigned i = 0; i < mat->GetTextureCount(type); ++i)
    {
        aiString str;
        mat->GetTexture(type, i, &str);
        Texture tex;
        tex.path = str.C_Str();
        // map to conventional sampler name used in FS.glsl
        tex.type = std::string("texture_diffuse") + std::to_string(textures.size() + 1);

        // Attempt to load the texture image into GL and store id
        // Uses helper load_texture_image which wraps stb_image + glTexImage2D
        try {
            load_texture_image loader(tex.path.c_str());
            tex.id = loader.texture;
            loader.freeBuffer();
        } catch (...) {
            tex.id = 0;
        }

        textures.push_back(tex);

    }
    return textures;
}

