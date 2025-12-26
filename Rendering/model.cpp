#include "Model.h"
#include "DebugDraw.h"
#include "stb_image.h"
#include <iostream>

#include <assimp/postprocess.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

std::string NormalizeNodeName(const std::string& name)
{
    // Remove everything before last | or :
    size_t pos = name.find_last_of("|:");
    if (pos != std::string::npos)
        return name.substr(pos + 1);
    return name;
}

std::unordered_map<std::string, int> BuildNormalizedBoneMap(const Skeleton& skeleton)
{
    std::unordered_map<std::string, int> result;

    for (const auto& [boneName, index] : skeleton.boneMapping) {
        std::string clean = NormalizeNodeName(boneName);
        result[clean] = index;
    }

    return result;
}


// ---------------- Texture Loader ----------------
unsigned int TextureFromFile(const char* path, const std::string& directory) {
    std::string filename = directory + "/" + path;
    unsigned int textureID;
    glGenTextures(1, &textureID);

    int w,h,comp;
    unsigned char* data = stbi_load(filename.c_str(), &w, &h, &comp, 0);
    if (!data) {
        std::cerr << "Failed to load texture: " << filename << std::endl;
        return 0;
    }

    GLenum format = (comp == 1) ? GL_RED : (comp == 3 ? GL_RGB : GL_RGBA);
    glBindTexture(GL_TEXTURE_2D, textureID);
    glTexImage2D(GL_TEXTURE_2D,0,format,w,h,0,format,GL_UNSIGNED_BYTE,data);
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);

    stbi_image_free(data);
    return textureID;
}
void BuildSkeletonLines(
    const AssimpNodeData& node,
    const glm::mat4& parentTransform,
    const Skeleton& skeleton,
    const std::vector<glm::mat4>& finalBones,
    std::vector<DebugLine>& outLines)
{
    glm::mat4 global = parentTransform * node.transform;

    auto it = skeleton.boneMapping.find(node.name);
    if (it != skeleton.boneMapping.end()) {
        int boneIndex = it->second;

        glm::vec3 parentPos = glm::vec3(parentTransform[3]);
        glm::vec3 nodePos   = glm::vec3(global[3]);

        outLines.push_back({ parentPos, nodePos });
    }

    for (const auto& child : node.children) {
        BuildSkeletonLines(child, global, skeleton, finalBones, outLines);
    }
}

inline void ValidateMeshWeights(const std::vector<Vertex>& vertices) {
    for (size_t i = 0; i < vertices.size(); ++i) {
        float sum = 0.0f;
        for (int j = 0; j < 4; ++j) {
            if (vertices[i].BoneIDs[j] < 0 || vertices[i].BoneIDs[j] >= MAX_BONES) {
                std::cerr << "[ERROR] Invalid BoneID at vertex " << i << "\n";
            }
            sum += vertices[i].Weights[j];
        }
        if (fabs(sum - 1.0f) > 0.01f) {
            std::cerr << "[WARN] Weight sum != 1 at vertex " << i
                      << " (sum=" << sum << ")\n";
        }
    }
}

// ---------------- Read Node Hierarchy ----------------
void Model::ReadHierarchy(AssimpNodeData& dest, const aiNode* src) {
    dest.name = src->mName.C_Str();
    dest.transform = glm::transpose(glm::make_mat4(&src->mTransformation.a1));
    dest.children.clear();

    for (unsigned int i = 0; i < src->mNumChildren; i++) {
        AssimpNodeData child;
        ReadHierarchy(child, src->mChildren[i]);
        dest.children.push_back(child);
    }
}

// ---------------- Model ----------------
Model::Model(const std::string& path) {
    loadModel(path);
}


Animation* Model::GetAnimation(size_t index) {
    if (index >= m_Animations.size()) return nullptr;
    return m_Animations[index].get();
}

void Model::Draw(Shader& shader) {
    for (auto& mesh : meshes)
        mesh.Draw(shader);
}

// ---------------- Loading ----------------
void Model::loadModel(const std::string& path) {
    scene = importer.ReadFile(path,
        aiProcess_Triangulate |
        aiProcess_GenSmoothNormals |
        aiProcess_FlipUVs |
        aiProcess_CalcTangentSpace
    );

    if (!scene || !scene->mRootNode) {
        std::cerr << "ASSIMP ERROR: " << importer.GetErrorString() << std::endl;
        return;
    }

    directory = path.substr(0, path.find_last_of('/'));

    processNode(scene->mRootNode, scene);
    ReadHierarchy(m_Skeleton.rootNode, scene->mRootNode);

    auto normBoneMap = BuildNormalizedBoneMap(m_Skeleton);
    BuildNodeBoneMap(m_Skeleton.rootNode, normBoneMap);


    int boneNodes=0;
    CountBoneNodes(m_Skeleton.rootNode, boneNodes);
    std::cout<<"Bone nodes linked:"<<boneNodes <<std::endl;
    

    for (unsigned int i = 0; i < scene->mNumAnimations; ++i) {
        aiAnimation* a = scene->mAnimations[i];
        m_Animations.push_back(std::make_unique<Animation>(
            a->mName.C_Str(),
            (float)a->mDuration,
            (float)a->mTicksPerSecond
        ));
    }
}
void Model::BuildNodeBoneMap(AssimpNodeData& node,
                      const std::unordered_map<std::string, int>& normBoneMap)
{
    std::string cleanNodeName = NormalizeNodeName(node.name);

    auto it = normBoneMap.find(cleanNodeName);
    node.boneIndex = (it != normBoneMap.end()) ? it->second : -1;

    for (auto& child : node.children)
        BuildNodeBoneMap(child, normBoneMap);
}

void Model::CountBoneNodes(const AssimpNodeData& node, int& count){
    if(node.boneIndex != -1)
        count++;
        std::cout<<"count:"<<count<<std::endl;
    
    for(const auto& child : node.children)
        CountBoneNodes(child,count);
}


void Model::processNode(aiNode* node, const aiScene* scene) {
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        aiMesh* meshTest = scene->mMeshes[i];
        std::cout << "Mesh " << i << " has bones: "<< meshTest->HasBones() << std::endl;
        meshes.push_back(processMesh(mesh, scene));
    }

    for (unsigned int i = 0; i < node->mNumChildren; ++i)
        processNode(node->mChildren[i], scene);
}

Mesh Model::processMesh(aiMesh* mesh, const aiScene* scene) {
    std::vector<Vertex> vertices(mesh->mNumVertices);
    std::vector<unsigned int> indices;
    std::vector<Texture> textures;

    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        Vertex& v = vertices[i];
        v.Position = {mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z};
        v.Normal = mesh->HasNormals() ? glm::vec3(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z) : glm::vec3(0.0f);
        v.TexCoords = mesh->mTextureCoords[0] ? glm::vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y) : glm::vec2(0.0f);
        for (int j = 0; j < MAX_BONE_INFLUENCE; ++j) { v.BoneIDs[j] = -1; v.Weights[j] = 0.0f; }
    }

    for (unsigned int i = 0; i < mesh->mNumFaces; ++i)
        for (unsigned int j = 0; j < mesh->mFaces[i].mNumIndices; ++j)
            indices.push_back(mesh->mFaces[i].mIndices[j]);

    ExtractBones(mesh);
    extractBoneWeights(vertices, mesh);

    for (auto& v : vertices)
{
    float sum = 0.0f;
    for (int i = 0; i < MAX_BONES_PER_VERTEX; i++)
        sum += v.Weights[i];

    if (sum > 0.0f)
    {
        for (int i = 0; i < MAX_BONES_PER_VERTEX; i++)
            v.Weights[i] /= sum;
    }
}


    if (mesh->mMaterialIndex >= 0) {
        aiMaterial* mat = scene->mMaterials[mesh->mMaterialIndex];
        auto albedo = loadMaterialTextures(mat, aiTextureType_DIFFUSE, "albedo");
        textures.insert(textures.end(), albedo.begin(), albedo.end());
    }

    return Mesh(vertices, indices, textures);
}

void Model::ExtractBones(aiMesh* mesh) {
    for (unsigned int i = 0; i < mesh->mNumBones; ++i) {
        std::string boneName = mesh->mBones[i]->mName.C_Str();
        int boneIndex = 0;

        if (m_Skeleton.boneMapping.count(boneName) == 0) {
            boneIndex = static_cast<int>(m_Skeleton.bones.size());
            m_Skeleton.boneMapping[boneName] = boneIndex;

            BoneInfo bi;
            bi.offset = glm::transpose(glm::make_mat4(&mesh->mBones[i]->mOffsetMatrix.a1));
            m_Skeleton.bones.push_back(bi);
        } else {
            boneIndex = m_Skeleton.boneMapping[boneName];
        }
    }
}

void Model::extractBoneWeights(std::vector<Vertex>& vertices, aiMesh* mesh) {
    for (unsigned int i = 0; i < mesh->mNumBones; ++i) {
        std::string boneName = mesh->mBones[i]->mName.C_Str();
        int boneIndex = m_Skeleton.boneMapping[boneName];

        for (unsigned int j = 0; j < mesh->mBones[i]->mNumWeights; ++j) {
            unsigned int vertexID = mesh->mBones[i]->mWeights[j].mVertexId;
            float weight = mesh->mBones[i]->mWeights[j].mWeight;

            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
                if (vertices[vertexID].Weights[k] == 0.0f) {
                    vertices[vertexID].BoneIDs[k] = boneIndex;
                    vertices[vertexID].Weights[k] = weight;
                    break;
                }
            }
        }
    }
}

std::vector<Texture> Model::loadMaterialTextures(aiMaterial* mat, aiTextureType type, const std::string& typeName) {
    std::vector<Texture> textures;
    for (unsigned int i = 0; i < mat->GetTextureCount(type); ++i) {
        aiString str;
        mat->GetTexture(type, i, &str);

        bool skip = false;
        for (auto& loadedTex : loaded_textures) {
            if (loadedTex.path == str.C_Str()) {
                textures.push_back(loadedTex);
                skip = true;
                break;
            }
        }

        if (!skip) {
            Texture tex;
            tex.id = TextureFromFile(str.C_Str(), directory);
            tex.type = typeName;
            tex.path = str.C_Str();
            textures.push_back(tex);
            loaded_textures.push_back(tex); // cache
        }
    }
    return textures;
}

glm::mat4 Model::aiMat4ToGlm(const aiMatrix4x4& m) {
    return glm::transpose(glm::make_mat4(&m.a1));
}



