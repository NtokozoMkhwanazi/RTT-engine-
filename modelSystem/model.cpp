#define GLM_ENABLE_EXPERIMENTAL

#include "Model.h"
#include "boneSystem/DebugDraw.h"
#include "shaderSystem/stb_image.h"
#include "shaderSystem/load_texture_image.h"
#include "boneSystem/BoneName.h"
#include "renderer/DefaultTexture.h"

#include <iostream>
#include <functional>
#include <algorithm>

#include <assimp/postprocess.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/string_cast.hpp>

// =====================================================
// Utility
// =====================================================

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
    auto it = normBoneMap.find(node.name);
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
    // OpenGL resources are cleaned up automatically when context is destroyed
    // Bone texture will be cleaned up when OpenGL context is destroyed
    if (boneTexID != 0) {
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

    // Upload bone palette
    std::vector<glm::mat4> paletteMats = finalBones;
    UploadBoneTexture(shader, paletteMats);
    shader.setInt("uPaletteSize", (int)paletteMats.size());

    // Draw all meshes
    for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex)
    {
        auto &mesh = meshes[meshIndex];
        mesh.Draw(shader);
    }
    
    if (firstDraw && debugOutput) {
        std::cout << "[Model::Draw] Finished drawing all meshes\n";
    }
    firstDraw = false;
}

// =====================================================
// LOD Drawing
// =====================================================
void Model::DrawLOD(Shader &shader, Animator &animator, const glm::vec3& cameraPos, float lodBias)
{
    const auto &finalBones = animator.GetFinalBoneMatrices();

    // Upload bone palette
    std::vector<glm::mat4> paletteMats = finalBones;
    UploadBoneTexture(shader, paletteMats);
    shader.setInt("uPaletteSize", (int)paletteMats.size());

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
}

// =====================================================
// Instanced Drawing
// =====================================================
void Model::DrawInstanced(Shader& shader, Animator& animator, const std::vector<ModelInstance>& instances)
{
    if (instances.empty()) return;
    
    const auto &finalBones = animator.GetFinalBoneMatrices();
    
    // Upload bone palette (shared across all instances)
    std::vector<glm::mat4> paletteMats = finalBones;
    UploadBoneTexture(shader, paletteMats);
    shader.setInt("uPaletteSize", (int)paletteMats.size());

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
}

// =====================================================
Animation* Model::GetAnimation(size_t index)
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

    // Generate LOD levels
    generateLODLevels();
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

    // Process PBR materials
    if (mesh->mMaterialIndex != static_cast<unsigned int>(-1))
    {
        aiMaterial *material = scene->mMaterials[mesh->mMaterialIndex];
        PBRMaterial pbrMat = processMaterial(material, directory);
        meshMaterials.push_back(pbrMat);
    }
    else
    {
        // Default material
        meshMaterials.push_back(PBRMaterial());
    }

    return Mesh(vertices, indices, textures);
}

// =====================================================
// PBR Material Processing
// =====================================================
PBRMaterial Model::processMaterial(aiMaterial* mat, const std::string& directory)
{
    PBRMaterial material;

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
        
        // Try loading from same directory as model
        std::string path = directory + "/" + texturePath;
        
        // Extract just the filename for fallback
        size_t lastSlash = texturePath.find_last_of('/');
        std::string filename = (lastSlash != std::string::npos) ? texturePath.substr(lastSlash + 1) : texturePath;
        
        material.albedoMap = loadTexture(path, aiTextureType_DIFFUSE);
        
        // If texture failed to load, try with just filename in same directory
        if (material.albedoMap == 0) {
            std::string fallbackPath = directory + "/" + filename;
            material.albedoMap = loadTexture(fallbackPath, aiTextureType_DIFFUSE);
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
        aiString str;
        mat->GetTexture(aiTextureType_METALNESS, 0, &str);
        std::string texturePath = str.C_Str();
        std::replace(texturePath.begin(), texturePath.end(), '\\', '/');
        size_t fbmPos = texturePath.find(".fbm/");
        if (fbmPos != std::string::npos) texturePath = texturePath.substr(fbmPos + 5);
        std::string path = directory + "/" + texturePath;
        material.metallicMap = loadTexture(path, aiTextureType_METALNESS);
        if (material.metallicMap == 0) {
            material.metallicMap = DefaultTexture::GetGreyTexture();  // Default grey for metallic
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
        aiString str;
        mat->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &str);
        std::string texturePath = str.C_Str();
        std::replace(texturePath.begin(), texturePath.end(), '\\', '/');
        size_t fbmPos = texturePath.find(".fbm/");
        if (fbmPos != std::string::npos) texturePath = texturePath.substr(fbmPos + 5);
        std::string path = directory + "/" + texturePath;
        material.roughnessMap = loadTexture(path, aiTextureType_DIFFUSE_ROUGHNESS);
        if (material.roughnessMap == 0) {
            material.roughnessMap = DefaultTexture::GetGreyTexture();
        }
        material.hasRoughnessMap = true;
    }

    // Normal map
    if (mat->GetTextureCount(aiTextureType_NORMALS) > 0) {
        aiString str;
        mat->GetTexture(aiTextureType_NORMALS, 0, &str);
        std::string texturePath = str.C_Str();
        std::replace(texturePath.begin(), texturePath.end(), '\\', '/');
        size_t fbmPos = texturePath.find(".fbm/");
        if (fbmPos != std::string::npos) texturePath = texturePath.substr(fbmPos + 5);
        std::string path = directory + "/" + texturePath;
        material.normalMap = loadTexture(path, aiTextureType_NORMALS);
        if (material.normalMap == 0) {
            material.normalMap = DefaultTexture::GetGreyTexture();
        }
        material.hasNormalMap = (material.normalMap > 0);
    }

    // AO (Ambient Occlusion)
    if (mat->GetTextureCount(aiTextureType_AMBIENT_OCCLUSION) > 0) {
        aiString str;
        mat->GetTexture(aiTextureType_AMBIENT_OCCLUSION, 0, &str);
        std::string texturePath = str.C_Str();
        std::replace(texturePath.begin(), texturePath.end(), '\\', '/');
        size_t fbmPos = texturePath.find(".fbm/");
        if (fbmPos != std::string::npos) texturePath = texturePath.substr(fbmPos + 5);
        std::string path = directory + "/" + texturePath;
        material.aoMap = loadTexture(path, aiTextureType_AMBIENT_OCCLUSION);
        if (material.aoMap == 0) {
            material.aoMap = DefaultTexture::GetGreyTexture();
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
        aiString str;
        mat->GetTexture(aiTextureType_EMISSIVE, 0, &str);
        std::string path = directory + "/" + str.C_Str();
        material.emissiveMap = loadTexture(path, aiTextureType_EMISSIVE);
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
    // Check if OpenGL context is available
    if (!glGenTextures) {
        std::cerr << "[Model] WARNING: No OpenGL context available, skipping texture: " << path << "\n";
        return 0;  // Return 0 (invalid texture ID) instead of crashing
    }
    
    unsigned int textureID = 0;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);

    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Load texture
    int width, height, nrComponents;
    stbi_set_flip_vertically_on_load(true);
    unsigned char *data = stbi_load(path.c_str(), &width, &height, &nrComponents, 0);

    if (data)
    {
        GLenum format;
        if (nrComponents == 1)
            format = GL_RED;
        else if (nrComponents == 3)
            format = GL_RGB;
        else if (nrComponents == 4)
            format = GL_RGBA;
        else
            format = GL_RGB;

        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);

        stbi_image_free(data);
    }
    else
    {
        std::cerr << "Texture failed to load at path: " << path << std::endl;
        stbi_image_free(data);
    }

    return textureID;
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
        const glm::mat4 &m = mats[i];

        // Store columns (matches shader sampling)
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
        pixels.data());

    shader.setInt("boneTex", 10);
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
    // For now, we'll create simple LOD levels by decimating meshes
    // In a production system, you would use a proper mesh simplification algorithm
    // like quadric error metrics or vertex clustering
    
    lodLevels.resize(meshes.size());
    
    for (size_t i = 0; i < meshes.size(); ++i)
    {
        // Create LOD levels at different distances
        // Note: This is a placeholder - actual LOD would require mesh simplification
        lodLevels[i].emplace_back(meshes[i], 0.0f);      // Highest quality (original)
        // lodLevels[i].emplace_back(simplifiedMesh1, 10.0f);  // Medium quality
        // lodLevels[i].emplace_back(simplifiedMesh2, 25.0f);  // Low quality
        // lodLevels[i].emplace_back(simplifiedMesh3, 50.0f);  // Lowest quality
    }
    
    if (debugOutput) {
        std::cout << "[LOD] Generated " << lodLevels.size() << " LOD level sets\n";
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
