#include "WorldImporter.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>          // std::transform (ClassifyImportedNode)
#include <cctype>             // std::tolower (ClassifyImportedNode)
#include <glad/glad.h>        // glGenTextures/etc (CompileBilinearPBRTexture)
#include "shaderSystem/stb_image.h"  // stbi_load (CompileBilinearPBRTexture)
#include "renderer/TextureProfiler.h"  // forUI.txt: VRAM footprint profiling

namespace fs = std::filesystem;

WorldImporter::WorldImporter() {
}

WorldImporter::~WorldImporter() {
}

std::vector<std::string> WorldImporter::getSupportedFormats() {
    return {
        ".fbx",    // FBX scenes (Unity, Unreal, Blender)
        ".obj",    // OBJ worlds
        ".gltf",   // glTF 2.0 scenes
        ".glb",    // glTF binary
        ".blend",  // Blender files (needs Blender installed)
        ".unity",  // Unity prefab/scenes
        ".umap"    // Unreal maps (requires conversion)
    };
}

WorldImporter::WorldFormat WorldImporter::detectFormat(const std::string& filePath) {
    std::string ext = fs::path(filePath).extension().string();
    
    if (ext == ".fbx") return WorldFormat::UNREAL_FBX;
    if (ext == ".obj") return WorldFormat::OBJ_WORLD;
    if (ext == ".gltf" || ext == ".glb") return WorldFormat::GLTF_SCENE;
    if (ext == ".blend") return WorldFormat::BLEND_FILE;
    if (ext == ".unity") return WorldFormat::UNITY_PREFAB;
    
    return WorldFormat::UNKNOWN;
}

bool WorldImporter::fileExists(const std::string& path) {
    return fs::exists(path);
}

std::string WorldImporter::getExtension(const std::string& path) {
    return fs::path(path).extension().string();
}

WorldImporter::ImportedWorld WorldImporter::importWorld(const std::string& filePath) {
    std::cout << "\n=== WORLD IMPORTER ===\n";
    std::cout << "Importing: " << filePath << "\n";
    
    if (!fileExists(filePath)) {
        std::cerr << "ERROR: File not found: " << filePath << "\n";
        return ImportedWorld();
    }
    
    WorldFormat format = detectFormat(filePath);
    
    switch (format) {
        case WorldFormat::UNREAL_FBX:
        case WorldFormat::UNITY_PREFAB:
            return importFromFBX(filePath);
        
        case WorldFormat::OBJ_WORLD:
            return importFromOBJ(filePath);
        
        case WorldFormat::GLTF_SCENE:
            return importFromGLTF(filePath);
        
        default:
            std::cerr << "ERROR: Unsupported format: " << fs::path(filePath).extension().string() << "\n";
            std::cout << "Supported formats: .fbx, .obj, .gltf, .glb\n";
            return ImportedWorld();
    }
}

WorldImporter::ImportedWorld WorldImporter::importFromFBX(const std::string& path) {
    ImportedWorld world;
    world.sourceFile = path;
    world.name = fs::path(path).stem().string();
    
    std::cout << "Detected format: FBX scene\n";
    std::cout << "Loading with Assimp...\n";
    
    Assimp::Importer importer;
    
    // Import with optimization flags — keep node hierarchy intact for
    // transform extraction (remove OptimizeGraph which flattens it).
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate |
        aiProcess_GenNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_LimitBoneWeights |
        aiProcess_RemoveRedundantMaterials |
        aiProcess_FindDegenerates |
        aiProcess_SortByPType |
        aiProcess_OptimizeMeshes
    );
    
    if (!scene || !scene->mRootNode) {
        std::cerr << "ERROR: Failed to load FBX: " << importer.GetErrorString() << "\n";
        return world;
    }
    
    std::cout << "Scene loaded successfully!\n";
    std::cout << "  Meshes: " << scene->mNumMeshes << "\n";
    std::cout << "  Materials: " << scene->mNumMaterials << "\n";
    std::cout << "  Nodes: " << scene->mRootNode->mNumChildren << "\n";
    
    // Calculate world bounds
    glm::vec3 minBound(999999), maxBound(-999999);

    // Build a mesh-index → node-transform lookup by recursively traversing
    // the aiNode hierarchy. Each mesh inherits the accumulated parent
    // transform so objects placed in Blender/Unreal arrive at the correct
    // world position, rotation, and scale.
    struct MeshTransform {
        glm::vec3 position{0.0f};
        glm::vec3 rotation{0.0f};
        glm::vec3 scale{1.0f};
    };
    std::vector<MeshTransform> meshTransforms(scene->mNumMeshes);

    std::function<void(const aiNode*, const glm::mat4&)> extractTransforms;
    extractTransforms = [&](const aiNode* node, const glm::mat4& parentXform) {
        if (!node) return;
        // aiNode transform is row-major; Assimp stores it transposed from GLM.
        const aiMatrix4x4& aiT = node->mTransformation;
        glm::mat4 localXform(
            aiT.a1, aiT.a2, aiT.a3, aiT.a4,
            aiT.b1, aiT.b2, aiT.b3, aiT.b4,
            aiT.c1, aiT.c2, aiT.c3, aiT.c4,
            aiT.d1, aiT.d2, aiT.d3, aiT.d4);
        glm::mat4 worldXform = parentXform * localXform;

        // Decompose the accumulated transform for each mesh this node owns.
        for (unsigned int m = 0; m < node->mNumMeshes; ++m) {
            unsigned int meshIdx = node->mMeshes[m];
            if (meshIdx < scene->mNumMeshes) {
                glm::vec3 skew;
                glm::vec4 persp;
                glm::quat rotQuat;
                glm::decompose(worldXform, meshTransforms[meshIdx].scale,
                               rotQuat, meshTransforms[meshIdx].position, skew, persp);
                meshTransforms[meshIdx].rotation = glm::eulerAngles(rotQuat);
            }
        }
        for (unsigned int c = 0; c < node->mNumChildren; ++c)
            extractTransforms(node->mChildren[c], worldXform);
    };
    extractTransforms(scene->mRootNode, glm::mat4(1.0f));

    // Process all meshes with their extracted world-space transforms
    for (unsigned int i = 0; i < scene->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[i];
        
        WorldObject obj;
        obj.name = std::string(mesh->mName.C_Str());
        obj.type = "mesh";
        
        // Extract position from mesh
        if (mesh->HasPositions()) {
            for (unsigned int j = 0; j < mesh->mNumVertices; j++) {
                glm::vec3 pos(mesh->mVertices[j].x, mesh->mVertices[j].y, mesh->mVertices[j].z);
                minBound = glm::min(minBound, pos);
                maxBound = glm::max(maxBound, pos);
            }
        }
        
        // Use the transform extracted from the node hierarchy
        obj.position = meshTransforms[i].position;
        obj.rotation = meshTransforms[i].rotation;
        obj.scale = meshTransforms[i].scale;
        obj.meshPath = path;

        // forUI.txt Item 2.B: resolve PBR texture paths from the mesh material
        if (mesh->mMaterialIndex < scene->mNumMaterials) {
            aiMaterial* mat = scene->mMaterials[mesh->mMaterialIndex];
            aiString texPath;
            if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
                std::string filename = std::filesystem::path(texPath.C_Str()).filename().string();
                obj.albedoTexturePath = "assets/textures/" + filename;
                obj.hasPBRMaterials = true;
            }
            if (mat->GetTexture(aiTextureType_NORMALS, 0, &texPath) == AI_SUCCESS) {
                std::string filename = std::filesystem::path(texPath.C_Str()).filename().string();
                obj.normalTexturePath = "assets/textures/" + filename;
            }
        }
        
        world.objects.push_back(obj);
    }
    
    // Calculate world size
    world.worldSize = maxBound - minBound;
    
    std::cout << "World bounds: (" << minBound.x << ", " << minBound.y << ", " << minBound.z 
              << ") to (" << maxBound.x << ", " << maxBound.y << ", " << maxBound.z << ")\n";
    std::cout << "World size: " << world.worldSize.x << " x " << world.worldSize.y 
              << " x " << world.worldSize.z << " meters\n";
    std::cout << "Objects found: " << world.objects.size() << "\n";
    
    return world;
}

WorldImporter::ImportedWorld WorldImporter::importFromOBJ(const std::string& path) {
    ImportedWorld world;
    world.sourceFile = path;
    world.name = fs::path(path).stem().string();
    
    std::cout << "Detected format: OBJ world\n";
    
    // Simple OBJ parser
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "ERROR: Cannot open OBJ file\n";
        return world;
    }
    
    std::string line;
    int vertexCount = 0;
    int faceCount = 0;
    
    while (std::getline(file, line)) {
        if (line.substr(0, 2) == "v ") vertexCount++;
        else if (line.substr(0, 2) == "f ") faceCount++;
    }
    
    std::cout << "OBJ stats:\n";
    std::cout << "  Vertices: " << vertexCount << "\n";
    std::cout << "  Faces: " << faceCount << "\n";
    
    // Create single world object for the entire OBJ
    WorldObject obj;
    obj.name = world.name;
    obj.type = "terrain";
    obj.meshPath = path;
    obj.position = glm::vec3(0);
    obj.rotation = glm::vec3(0);
    obj.scale = glm::vec3(1);
    
    world.objects.push_back(obj);
    world.worldSize = glm::vec3(100, 50, 100);  // Default size
    
    return world;
}

WorldImporter::ImportedWorld WorldImporter::importFromGLTF(const std::string& path) {
    ImportedWorld world;
    world.sourceFile = path;
    world.name = fs::path(path).stem().string();
    
    std::cout << "Detected format: glTF scene\n";
    
    // glTF loading would use Assimp or a dedicated glTF loader
    // For now, use Assimp
    return importFromFBX(path);  // Assimp handles glTF similarly
}

void WorldImporter::convertToEngineFormat(const ImportedWorld& imported, 
                                          const std::string& outputDir) {
    std::cout << "\nConverting world to engine format...\n";
    std::cout << "Output directory: " << outputDir << "\n";
    
    // Create output directory
    fs::create_directories(outputDir);
    
    // Save world configuration
    std::string configFile = outputDir + "/world_config.txt";
    std::ofstream out(configFile);
    
    if (out.is_open()) {
        out << "# World Configuration\n";
        out << "name=" << imported.name << "\n";
        out << "source=" << imported.sourceFile << "\n";
        out << "size=" << imported.worldSize.x << " " << imported.worldSize.y 
            << " " << imported.worldSize.z << "\n";
        out << "objects=" << imported.objects.size() << "\n";
        
        for (const auto& obj : imported.objects) {
            out << "object: " << obj.name << " type=" << obj.type 
                << " pos=(" << obj.position.x << "," << obj.position.y << "," << obj.position.z << ")\n";
        }
        
        out.close();
        std::cout << "Saved config: " << configFile << "\n";
    }
    
    std::cout << "Conversion complete!\n";
}

// forUI.txt Item 1.A: procedural category assignment rule filter.
WorldObjectType ClassifyImportedNode(const std::string& nodeName) {
    std::string nameLower = nodeName;
    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // 1. Evaluate Tree Classifications
    if (nameLower.find("pine") != std::string::npos || nameLower.find("fir") != std::string::npos) {
        return WorldObjectType::TREE_PINE;
    }
    if (nameLower.find("oak") != std::string::npos) {
        return WorldObjectType::TREE_OAK;
    }
    if (nameLower.find("birch") != std::string::npos) {
        return WorldObjectType::TREE_BIRCH;
    }
    if (nameLower.find("tree") != std::string::npos || nameLower.find("foliage") != std::string::npos) {
        return WorldObjectType::TREE_PINE; // Default fallback tree category
    }

    // 2. Evaluate Rock & Geological Structure Classifications
    if (nameLower.find("boulder") != std::string::npos) {
        return WorldObjectType::ROCK_BOULDER;
    }
    if (nameLower.find("cliff") != std::string::npos || nameLower.find("mountain") != std::string::npos) {
        return WorldObjectType::ROCK_CLIFF;
    }
    if (nameLower.find("stone") != std::string::npos || nameLower.find("rock") != std::string::npos) {
        return WorldObjectType::ROCK_STONE;
    }
    if (nameLower.find("pebble") != std::string::npos) {
        return WorldObjectType::ROCK_STONE; // Maps down to stone slot at micro scale
    }

    // 3. Evaluate Props, Ground Covers, & Logistics
    if (nameLower.find("log") != std::string::npos) {
        return WorldObjectType::LOG;
    }
    if (nameLower.find("stump") != std::string::npos) {
        return WorldObjectType::STUMP;
    }
    if (nameLower.find("grass") != std::string::npos) {
        return WorldObjectType::GRASS_CLUSTER;
    }
    if (nameLower.find("flower") != std::string::npos || nameLower.find("plant") != std::string::npos) {
        return WorldObjectType::FLOWER_PATCH;
    }
    if (nameLower.find("bush") != std::string::npos || nameLower.find("shrub") != std::string::npos) {
        return WorldObjectType::BUSH;
    }

    // Default structural fallback category if strings match no operational keyword patterns
    return WorldObjectType::ROCK_BOULDER;
}

// forUI.txt Item 2.C: asynchronous bilinear GPU upload completion gate
// (main-thread; called from the polling loop after the JobSystem task resolves).
unsigned int CompileBilinearPBRTexture(const std::string& texturePath) {
    if (texturePath.empty() || !std::filesystem::exists(texturePath)) return 0;

    int width = 0, height = 0, channels = 0;
    unsigned char* pixelData = stbi_load(texturePath.c_str(), &width, &height, &channels, 4);
    if (!pixelData) return 0;

    unsigned int glTextureID = 0;
    glGenTextures(1, &glTextureID);
    glBindTexture(GL_TEXTURE_2D, glTextureID);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixelData);
    glGenerateMipmap(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixelData);

    // forUI.txt: register the staged texture footprint with the VRAM profiler.
    TextureProfiler::Instance().RegisterTexture(glTextureID, texturePath, width, height, GL_RGBA8);

    return glTextureID;
}
