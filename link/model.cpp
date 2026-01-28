#define GLM_ENABLE_EXPERIMENTAL

#include "Model.h"
#include "DebugDraw.h"
#include "stb_image.h"
#include "BoneName.h"

#include <iostream>
#include <functional>

#include <assimp/postprocess.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

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
    r[0][0] = m.a1; r[1][0] = m.a2; r[2][0] = m.a3; r[3][0] = m.a4;
    r[0][1] = m.b1; r[1][1] = m.b2; r[2][1] = m.b3; r[3][1] = m.b4;
    r[0][2] = m.c1; r[1][2] = m.c2; r[2][2] = m.c3; r[3][2] = m.c4;
    r[0][3] = m.d1; r[1][3] = m.d2; r[2][3] = m.d3; r[3][3] = m.d4;
    return r;
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

    if (m_Skeleton.rootBoneIndex == -1)
{
    std::cerr << "WARNING: No root bone found. Animations disabled.\n";
}

}

// =====================================================
void Model::Draw(Shader& shader)
{
    for (auto& mesh : meshes)
        mesh.Draw(shader);
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
    m_Skeleton.globalInverseTransform =
    glm::inverse(aiMat4ToGlm(scene->mRootNode->mTransformation));

    processNode(scene->mRootNode, scene);

 auto normMap = BuildNormalizedBoneMap(m_Skeleton);
BuildNodeBoneMap(m_Skeleton.rootNode, normMap);

m_Skeleton.rootBoneIndex = FindRootBoneIndex(m_Skeleton);
std::cout << "Skeleton root bone index: "
          << m_Skeleton.rootBoneIndex << "\n";


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

    ExtractBones(mesh);
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
    dest.name = NormalizeBone(src->mName.C_Str());
    dest.transform = aiMat4ToGlm(src->mTransformation);
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
// ------------------------------------------------------------
void Model::ExtractBones(aiMesh* mesh)
{
    for (unsigned i = 0; i < mesh->mNumBones; ++i)
    {
        aiBone* bone = mesh->mBones[i];
        std::string name = NormalizeBone(bone->mName.C_Str());

        if (m_Skeleton.boneMapping.count(name) == 0)
        {
            int id = (int)m_Skeleton.bones.size();
            m_Skeleton.boneMapping[name] = id;

            BoneInfo info;
            info.id = id;
            info.offset = aiMat4ToGlm(bone->mOffsetMatrix);
            m_Skeleton.bones.push_back(info);
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
        std::string name = NormalizeBone(bone->mName.C_Str());

        auto it = m_Skeleton.boneMapping.find(name);
        if (it == m_Skeleton.boneMapping.end())
            continue;

        int boneID = it->second;

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
tex.type = typeName;
textures.push_back(tex);

    }
    return textures;
}

