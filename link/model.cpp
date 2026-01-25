#define GLM_ENABLE_EXPERIMENTAL

#include "Model.h"
#include "DebugDraw.h"
#include "stb_image.h"
#include "BoneName.h"

#include <iostream>
#include <unordered_map>
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
    const std::unordered_map<std::string, int>& normBoneMap
)
{
    auto it = normBoneMap.find(node.name);
    if (it != normBoneMap.end())
    {
        node.boneIndex = it->second;
    }
    else
    {
        node.boneIndex = -1;
    }

    for (auto& child : node.children)
    {
        BuildNodeBoneMap(child, normBoneMap);
    }
}


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
// Texture loading
// =====================================================
unsigned int TextureFromFile(const char* path, const std::string& directory)
{
    std::string filename = directory + "/" + path;
    unsigned int textureID;
    glGenTextures(1, &textureID);

    int w, h, comp;
    unsigned char* data = stbi_load(filename.c_str(), &w, &h, &comp, 0);
    if (!data)
    {
        std::cerr << "Failed to load texture: " << filename << "\n";
        return 0;
    }

    GLenum format = (comp == 1) ? GL_RED : (comp == 3) ? GL_RGB : GL_RGBA;
    glBindTexture(GL_TEXTURE_2D, textureID);
    glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, format,
                 GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    stbi_image_free(data);
    return textureID;
}

// =====================================================
// Skeleton Debug
// =====================================================
void BuildSkeletonLines(
    const AssimpNodeData& node,
    const glm::mat4& parentTransform,
    const Skeleton&,
    const std::vector<glm::mat4>&,
    std::vector<DebugLine>& outLines)
{
    glm::mat4 global = parentTransform * node.transform;

    if (node.boneIndex != -1)
    {
        outLines.push_back({
            glm::vec3(parentTransform[3]),
            glm::vec3(global[3])
        });
    }

    for (const auto& c : node.children)
        BuildSkeletonLines(c, global, {}, {}, outLines);
}

// =====================================================
// Hierarchy
// =====================================================
void Model::ReadHierarchy(AssimpNodeData& dest, const aiNode* src)
{
    dest.name = src->mName.C_Str();
    dest.transform = aiMat4ToGlm(src->mTransformation);
    dest.children.clear();

    for (unsigned i = 0; i < src->mNumChildren; ++i)
    {
        AssimpNodeData child;
        ReadHierarchy(child, src->mChildren[i]);
        dest.children.push_back(child);
    }
}

// =====================================================
// Root bone detection
// =====================================================
int FindRootBoneIndex(const Skeleton& skeleton)
{
    std::unordered_map<int, bool> isChild;

    std::function<void(const AssimpNodeData&)> walk =
        [&](const AssimpNodeData& n)
        {
            for (auto& c : n.children)
            {
                if (c.boneIndex != -1)
                    isChild[c.boneIndex] = true;
                walk(c);
            }
        };

    walk(skeleton.rootNode);

    for (auto& [_, idx] : skeleton.boneMapping)
        if (!isChild[idx]) return idx;

    return 0;
}

// =====================================================
// Constructor
// =====================================================
Model::Model(const std::string& path)
{
    loadModel(path);

    int influenced = 0;
    for (auto& mesh : meshes)
        for (auto& v : mesh.vertices)
            if (v.Weights.x + v.Weights.y + v.Weights.z + v.Weights.w > 0.0f)
                influenced++;

    std::cout << "Vertices influenced by bones: "
              << influenced << "\n";

    int rootIndex = FindRootBoneIndex(m_Skeleton);
    std::cout << "Root bone index: " << rootIndex << "\n";
}

// =====================================================
// Draw
// =====================================================
void Model::Draw(Shader& shader)
{
    for (auto& mesh : meshes)
        mesh.Draw(shader);
}

// =====================================================
// Animation
// =====================================================
Animation* Model::GetAnimation(size_t index)
{
    if (index >= m_Animations.size()) return nullptr;
    return m_Animations[index].get();
}

// =====================================================
// Bind Pose Capture (CRITICAL FIX)
// =====================================================
void CaptureBindPose(
    AssimpNodeData& node,
    const glm::mat4& parent,
    Skeleton& skeleton)
{
    glm::mat4 global = parent * node.transform;

    if (node.boneIndex != -1)
        skeleton.bones[node.boneIndex].bindTransform = global;

    for (auto& c : node.children)
        CaptureBindPose(c, global, skeleton);
}

// =====================================================
// Load Model
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
        std::cerr << "ASSIMP ERROR: "
                  << importer.GetErrorString() << "\n";
        return;
    }

    directory = path.substr(0, path.find_last_of('/'));

    // hierarchy
    ReadHierarchy(m_Skeleton.rootNode, scene->mRootNode);

    // meshes
    processNode(scene->mRootNode, scene);

    // node-bone map
    auto normMap = BuildNormalizedBoneMap(m_Skeleton);
    BuildNodeBoneMap(m_Skeleton.rootNode, normMap);

    // bind pose
    CaptureBindPose(m_Skeleton.rootNode, glm::mat4(1.0f), m_Skeleton);

    // unit scale
    float unitScale = 1.0f;
    if (scene->mMetaData)
    {
        double scale;
        if (scene->mMetaData->Get("UnitScaleFactor", scale))
            unitScale = float(scale) / 100.0f;
    }

    for (auto& b : m_Skeleton.bones)
    {
        b.bindTransform =
            glm::scale(glm::mat4(1.0f), glm::vec3(unitScale)) *
            b.bindTransform;
        b.offset = glm::inverse(b.bindTransform);
    }

    int root = FindRootBoneIndex(m_Skeleton);
    m_Skeleton.globalInverseTransform =
        glm::inverse(m_Skeleton.bones[root].bindTransform);

    // animations
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

void Model::processNode(aiNode* node, const aiScene* scene)
{
    // Process all meshes in this node
    for (unsigned int i = 0; i < node->mNumMeshes; i++)
    {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        meshes.push_back(processMesh(mesh, scene));
    }

    // Recursively process children
    for (unsigned int i = 0; i < node->mNumChildren; i++)
    {
        processNode(node->mChildren[i], scene);
    }
}

// ==========================
// BONE EXTRACTION
// ==========================
void Model::ExtractBones(aiMesh* mesh)
{
    for (unsigned int i = 0; i < mesh->mNumBones; i++)
    {
        aiBone* bone = mesh->mBones[i];
        std::string boneName = bone->mName.C_Str();

        if (m_BoneMapping.find(boneName) == m_BoneMapping.end())
        {
            BoneInfo info;
            info.id = m_BoneCounter++;
            info.offset = AssimpToGlm(bone->mOffsetMatrix);

            m_BoneMapping[boneName] = info;
        }
    }
}
// ==========================
// BONE WEIGHT EXTRACTION
// ==========================
void Model::extractBoneWeights(std::vector<Vertex>& vertices, aiMesh* mesh)
{
    for (unsigned int i = 0; i < mesh->mNumBones; i++)
    {
        aiBone* bone = mesh->mBones[i];
        int boneID = m_BoneMapping[bone->mName.C_Str()].id;

        for (unsigned int j = 0; j < bone->mNumWeights; j++)
        {
            aiVertexWeight weight = bone->mWeights[j];
            int vertexID = weight.mVertexId;
            float w = weight.mWeight;

            for (int k = 0; k < MAX_BONE_INFLUENCE; k++)
            {
                if (vertices[vertexID].weights[k] == 0.0f)
                {
                    vertices[vertexID].boneIDs[k] = boneID;
                    vertices[vertexID].weights[k] = w;
                    break;
                }
            }
        }
    }
}
// ==========================
// MATERIAL TEXTURES
// ==========================
std::vector<Texture> Model::loadMaterialTextures(
    aiMaterial* mat,
    aiTextureType type,
    const std::string& typeName)
{
    std::vector<Texture> textures;

    for (unsigned int i = 0; i < mat->GetTextureCount(type); i++)
    {
        aiString str;
        mat->GetTexture(type, i, &str);

        Texture texture;
        texture.path = str.C_Str();
        texture.type = typeName;

        textures.push_back(texture);
    }

    return textures;
}


