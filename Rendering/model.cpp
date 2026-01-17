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
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>



// ----------------- Utility Functions -----------------
std::unordered_map<std::string, int> BuildNormalizedBoneMap(const Skeleton& skeleton) {
    std::unordered_map<std::string, int> result;
    for (const auto& [boneName, index] : skeleton.boneMapping) {
        result[NormalizeBoneName(boneName)] = index;
    }
    return result;
}

glm::mat4 Model::aiMat4ToGlm(const aiMatrix4x4& m) {
    glm::mat4 result;
    result[0][0] = m.a1; result[1][0] = m.a2; result[2][0] = m.a3; result[3][0] = m.a4;
    result[0][1] = m.b1; result[1][1] = m.b2; result[2][1] = m.b3; result[3][1] = m.b4;
    result[0][2] = m.c1; result[1][2] = m.c2; result[2][2] = m.c3; result[3][2] = m.c4;
    result[0][3] = m.d1; result[1][3] = m.d2; result[2][3] = m.d3; result[3][3] = m.d4;
    return result;
}

unsigned int TextureFromFile(const char* path, const std::string& directory) {
    std::string filename = directory + "/" + path;
    unsigned int textureID;
    glGenTextures(1, &textureID);

    int w, h, comp;
    unsigned char* data = stbi_load(filename.c_str(), &w, &h, &comp, 0);
    if(!data) { std::cerr<<"Failed to load texture: "<<filename<<"\n"; return 0; }

    GLenum format = (comp==1)? GL_RED : (comp==3)? GL_RGB : GL_RGBA;
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

// ----------------- Skeleton Debug / Lines -----------------
void BuildSkeletonLines(
    const AssimpNodeData& node,
    const glm::mat4& parentTransform,
    const Skeleton& skeleton,
    const std::vector<glm::mat4>& finalBones,
    std::vector<DebugLine>& outLines)
{
    glm::mat4 global = parentTransform * node.transform;
    if(node.boneIndex != -1){
        glm::vec3 parentPos = glm::vec3(parentTransform[3]);
        glm::vec3 nodePos   = glm::vec3(global[3]);
        outLines.push_back({parentPos, nodePos});
    }
    for(const auto& child : node.children)
        BuildSkeletonLines(child, global, skeleton, finalBones, outLines);
}

// ----------------- Hierarchy -----------------
void Model::ReadHierarchy(AssimpNodeData& dest, const aiNode* src) {
    dest.name = src->mName.C_Str();
    dest.transform = aiMat4ToGlm(src->mTransformation);
    dest.children.clear();

    for(unsigned int i = 0; i < src->mNumChildren; ++i) {
        AssimpNodeData child;
        ReadHierarchy(child, src->mChildren[i]);
        dest.children.push_back(child);
    }
}

// ----------------- Find Root Bone (Future-Proof) -----------------
int FindRootBoneIndex(const Skeleton& skeleton) {
    // Root is the first bone not found as a child anywhere
    std::unordered_map<int,bool> isChild;
    std::function<void(const AssimpNodeData&)> markChildren = [&](const AssimpNodeData& node){
        for(auto& child : node.children){
            if(child.boneIndex != -1) isChild[child.boneIndex] = true;
            markChildren(child);
        }
    };
    markChildren(skeleton.rootNode);

    for(auto& [name,index] : skeleton.boneMapping){
        if(isChild.find(index) == isChild.end()) return index;
    }
    // fallback
    return 0;
}

// ----------------- Constructor -----------------
Model::Model(const std::string& path) {
    loadModel(path);

    // Debug: count vertices influenced by bones
    int influenced = 0;
    for(auto& mesh : meshes)
        for(auto& v : mesh.vertices)
            if (v.Weights.x + v.Weights.y + v.Weights.z + v.Weights.w > 0.0f)
                influenced++;

    std::cout << "Vertices influenced by bones: "
              << influenced << " / total\n";

    // Detect root bone dynamically
    int rootIndex = FindRootBoneIndex(m_Skeleton);
    std::cout << "Root bone index: " << rootIndex << " | Name: ";
    for(auto& [name,index] : m_Skeleton.boneMapping)
        if(index == rootIndex){ std::cout << name << "\n"; break; }
}

// ----------------- Draw -----------------
void Model::Draw(Shader& shader) {
    for(auto& mesh : meshes) mesh.Draw(shader);
}

// ----------------- Animation -----------------
Animation* Model::GetAnimation(size_t index) {
    if(index >= m_Animations.size()) return nullptr;
    return m_Animations[index].get();
}

// ----------------- Load Model -----------------
void Model::loadModel(const std::string& path) {
    scene = importer.ReadFile(path,
        aiProcess_Triangulate |
        aiProcess_GenSmoothNormals |
        aiProcess_FlipUVs |
        aiProcess_CalcTangentSpace
    );

    if(!scene || !scene->mRootNode) {
        std::cerr<<"ASSIMP ERROR: "<<importer.GetErrorString()<<"\n";
        return;
    }

    directory = path.substr(0,path.find_last_of('/'));

    // ---- Build Skeleton ----
    ReadHierarchy(m_Skeleton.rootNode, scene->mRootNode);

    m_Skeleton.globalInverseTransform = glm::inverse(m_Skeleton.rootNode.transform);

    // ---- Process Nodes ----
    processNode(scene->mRootNode, scene);

    // ---- Build Node-Bone map ----
    auto normMap = BuildNormalizedBoneMap(m_Skeleton);
    BuildNodeBoneMap(m_Skeleton.rootNode, normMap);

    // ---- Count bone nodes ----
    int boneNodes = 0;
    CountBoneNodes(m_Skeleton.rootNode, boneNodes);
    std::cout<<"Bone nodes linked: "<<boneNodes<<std::endl;

    // ---- Load Animations ----
    for(unsigned int i=0;i<scene->mNumAnimations;++i){
        aiAnimation* a = scene->mAnimations[i];
        m_Animations.push_back(std::make_unique<Animation>(
            a->mName.C_Str(),
            static_cast<float>(a->mDuration),
            static_cast<float>((a->mTicksPerSecond>0)?a->mTicksPerSecond:25.0f)
        ));
    }

    std::cout<<"Model loaded: "<<path<<" | Bones: "<<m_Skeleton.bones.size()<<"\n";
}

// ----------------- Node Processing -----------------
void Model::processNode(aiNode* node,const aiScene* scene){
    for(unsigned int i=0;i<node->mNumMeshes;++i){
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        meshes.push_back(processMesh(mesh, scene));
    }
    for(unsigned int i=0;i<node->mNumChildren;++i)
        processNode(node->mChildren[i], scene);
}

// ----------------- Mesh Processing -----------------
Mesh Model::processMesh(aiMesh* mesh,const aiScene* scene){
    std::vector<Vertex> vertices(mesh->mNumVertices);
    std::vector<unsigned int> indices;
    std::vector<Texture> textures;

    for(unsigned int i=0;i<mesh->mNumVertices;++i){
        Vertex& v = vertices[i];
        v.Position = {mesh->mVertices[i].x,mesh->mVertices[i].y,mesh->mVertices[i].z};
        v.Normal = mesh->HasNormals()? glm::vec3(mesh->mNormals[i].x,mesh->mNormals[i].y,mesh->mNormals[i].z) : glm::vec3(0.0f);
        v.TexCoords = mesh->mTextureCoords[0]? glm::vec2(mesh->mTextureCoords[0][i].x,mesh->mTextureCoords[0][i].y):glm::vec2(0.0f);
        for(int j=0;j<MAX_BONE_INFLUENCE;++j){ v.BoneIDs[j]=-1; v.Weights[j]=0.0f; }
    }

    for(unsigned int i=0;i<mesh->mNumFaces;++i)
        for(unsigned int j=0;j<mesh->mFaces[i].mNumIndices;++j)
            indices.push_back(mesh->mFaces[i].mIndices[j]);

    // ---- Bones ----
    ExtractBones(mesh);
    extractBoneWeights(vertices, mesh);

    // ---- Normalize weights ----
    for(auto& v : vertices){
        float sum = 0.0f;
        for(int i=0;i<MAX_BONE_INFLUENCE;++i) sum += v.Weights[i];
        if(sum>0.0f) for(int i=0;i<MAX_BONE_INFLUENCE;++i) v.Weights[i]/=sum;
    }

    // ---- Materials ----
    if(mesh->mMaterialIndex>=0){
        aiMaterial* mat = scene->mMaterials[mesh->mMaterialIndex];
        auto albedo = loadMaterialTextures(mat, aiTextureType_DIFFUSE, "albedo");
        textures.insert(textures.end(), albedo.begin(), albedo.end());
    }

    return Mesh(vertices, indices, textures);
}

// ----------------- Extract Bones -----------------
void Model::ExtractBones(aiMesh* mesh){
    for(unsigned int i=0;i<mesh->mNumBones;++i){
        std::string rawName(mesh->mBones[i]->mName.C_Str());
        std::string cleanName = NormalizeBoneName(rawName);
        int boneIndex = 0;

        if(m_Skeleton.boneMapping.count(cleanName)==0){
            boneIndex = static_cast<int>(m_Skeleton.bones.size());
            BoneInfo bi; 
            bi.offset = aiMat4ToGlm(mesh->mBones[i]->mOffsetMatrix);
            m_Skeleton.bones.push_back(bi);
            m_Skeleton.boneMapping[cleanName] = boneIndex;
        } else {
            boneIndex = m_Skeleton.boneMapping[cleanName];
        }
    }
}

// ----------------- Extract Bone Weights -----------------
void Model::extractBoneWeights(std::vector<Vertex>& vertices, aiMesh* mesh){
    for(unsigned int i=0;i<mesh->mNumBones;++i){
        std::string cleanName = NormalizeBoneName(mesh->mBones[i]->mName.C_Str());
        int boneIndex = m_Skeleton.boneMapping[cleanName];

        for(unsigned int j=0;j<mesh->mBones[i]->mNumWeights;++j){
            unsigned int vertexID = mesh->mBones[i]->mWeights[j].mVertexId;
            float weight = mesh->mBones[i]->mWeights[j].mWeight;
            for(int k=0;k<MAX_BONE_INFLUENCE;++k){
                if(vertices[vertexID].Weights[k]==0.0f){
                    vertices[vertexID].BoneIDs[k]=boneIndex;
                    vertices[vertexID].Weights[k]=weight;
                    break;
                }
            }
        }
    }
}

// ----------------- Node-Bone Map -----------------
void Model::BuildNodeBoneMap(AssimpNodeData& node, const std::unordered_map<std::string,int>& normBoneMap){
    std::string cleanNodeName = NormalizeBoneName(node.name);
    auto it = normBoneMap.find(cleanNodeName);
    node.boneIndex = (it!=normBoneMap.end())? it->second : -1;

    for(auto& child: node.children)
        BuildNodeBoneMap(child, normBoneMap);
}

// ----------------- Count Bone Nodes -----------------
void Model::CountBoneNodes(const AssimpNodeData& node,int& count){
    if(node.boneIndex!=-1) count++;
    for(const auto& child: node.children) CountBoneNodes(child,count);
}

// ----------------- Load Textures -----------------
std::vector<Texture> Model::loadMaterialTextures(aiMaterial* mat, aiTextureType type, const std::string& typeName){
    std::vector<Texture> textures;
    for(unsigned int i=0;i<mat->GetTextureCount(type);++i){
        aiString str;
        mat->GetTexture(type,i,&str);
        bool skip=false;
        for(auto& t : loaded_textures){
            if(t.path==str.C_Str()){ textures.push_back(t); skip=true; break; }
        }
        if(!skip){
            Texture tex; tex.id = TextureFromFile(str.C_Str(), directory);
            tex.type = typeName; tex.path=str.C_Str();
            textures.push_back(tex); loaded_textures.push_back(tex);
        }
    }
    return textures;
}

