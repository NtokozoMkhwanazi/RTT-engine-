#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>

// World import system for loading levels from various formats
class WorldImporter {
public:
    // Supported world formats
    enum class WorldFormat {
        UNKNOWN,
        UNITY_PREFAB,
        UNREAL_FBX,
        OBJ_WORLD,
        GLTF_SCENE,
        BLEND_FILE
    };

    // Imported world object
    struct WorldObject {
        std::string meshPath;
        glm::vec3 position;
        glm::vec3 rotation;
        glm::vec3 scale;
        std::string type;  // "tree", "rock", "building", etc.
        std::string name;
    };

    // Imported world data
    struct ImportedWorld {
        std::string name;
        std::string sourceFile;
        glm::vec3 worldSize;  // Dimensions in meters
        std::vector<WorldObject> objects;
        std::string terrainHeightmap;
        std::string waterPlane;
    };

    WorldImporter();
    ~WorldImporter();

    // Import world from file
    ImportedWorld importWorld(const std::string& filePath);
    
    // Get supported formats
    static std::vector<std::string> getSupportedFormats();
    
    // Detect format from file extension
    static WorldFormat detectFormat(const std::string& filePath);
    
    // Convert world to engine format
    void convertToEngineFormat(const ImportedWorld& imported, 
                               const std::string& outputDir);

private:
    // Importers for different formats
    ImportedWorld importFromFBX(const std::string& path);
    ImportedWorld importFromOBJ(const std::string& path);
    ImportedWorld importFromGLTF(const std::string& path);
    ImportedWorld importFromUnity(const std::string& path);
    
    // Helper functions
    std::string getExtension(const std::string& path);
    bool fileExists(const std::string& path);
};
