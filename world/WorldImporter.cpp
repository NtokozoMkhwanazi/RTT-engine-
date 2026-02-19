#include "WorldImporter.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>

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
    
    // Import with optimization flags
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate |
        aiProcess_GenNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_LimitBoneWeights |
        aiProcess_RemoveRedundantMaterials |
        aiProcess_FindDegenerates |
        aiProcess_SortByPType |
        aiProcess_OptimizeGraph |
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
    
    // Process all meshes
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
        
        // For now, add as single object at origin
        // In a full implementation, you'd extract node transforms
        obj.position = glm::vec3(0);
        obj.rotation = glm::vec3(0);
        obj.scale = glm::vec3(1);
        obj.meshPath = path;
        
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
