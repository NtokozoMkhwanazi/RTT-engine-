#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>

namespace Config {
    // Engine paths configuration
    struct EnginePaths {
        std::string assetRoot = "./assets";
        std::string modelPath = "./models";
        std::string scenePath = "./scenes";
        std::string shaderPath = "./shaderSystem";
        std::string texturePath = "./assets/textures";
        std::string skyboxPath = "./models/skybox";
        std::string fontPath = "./fonts";
        std::string configPath = "./config";
        std::string savePath = "./saves";
        
        // Default scene file
        std::string defaultScene = "scene.json";
        
        // Font fallbacks
        std::vector<std::string> systemFontPaths = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"
        };
        
        std::string phosphorFont = "Phosphor.ttf";
    };
    
    // Render configuration
    struct RenderConfig {
        int defaultWindowWidth = 1920;
        int defaultWindowHeight = 1080;
        int defaultViewportWidth = 1280;
        int defaultViewportHeight = 720;
        
        float nearPlane = 0.1f;
        float farPlane = 2000.0f;
        float defaultFOV = 45.0f;
        
        bool vsync = false;
        bool wireframe = false;
        
        // LOD distances
        float terrainLODDistance = 60.0f;
        float vegetationLODDistance = 80.0f;
        float objectLODDistance = 100.0f;
        
        // Culling
        float frustumCullingDistance = 500.0f;
        bool enableOcclusionCulling = true;
        bool enableFrustumCulling = true;
    };
    
    // Terrain configuration
    struct TerrainConfig {
        float chunkSize = 80.0f;
        int chunkResolution = 32;
        int viewDistance = 2;
        float lodDistance = 60.0f;
        int heightmapSize = 1024;
        float heightScale = 25.0f;
        
        // Performance optimizations
        int maxActiveChunks = 25;
        float chunkLoadDistance = 150.0f;
        float chunkUnloadDistance = 200.0f;
    };
    
    // Vegetation configuration
    struct VegetationConfig {
        float treeDensity = 0.00005f;
        float grassDensity = 0.001f;
        float rockDensity = 0.00005f;
        int maxTreesPerChunk = 1;
        int maxRocksPerChunk = 1;
        float minTreeHeight = 1.0f;
        float maxTreeHeight = 2.0f;
        
        // Performance optimizations
        float vegetationDrawDistance = 300.0f;
        bool useInstancing = true;
        bool useLOD = true;
        float lodStartDistance = 50.0f;
    };
    
    // Physics configuration
    struct PhysicsConfig {
        glm::vec3 gravity = glm::vec3(0, -9.81f, 0);
        int velocityIterations = 8;
        int positionIterations = 3;
        bool enableCCD = true; // Continuous Collision Detection
    };
    
    // Get singleton instance
    inline EnginePaths& getPaths() {
        static EnginePaths instance;
        return instance;
    }
    
    inline RenderConfig& getRenderConfig() {
        static RenderConfig instance;
        return instance;
    }
    
    inline TerrainConfig& getTerrainConfig() {
        static TerrainConfig instance;
        return instance;
    }
    
    inline VegetationConfig& getVegetationConfig() {
        static VegetationConfig instance;
        return instance;
    }
    
    inline PhysicsConfig& getPhysicsConfig() {
        static PhysicsConfig instance;
        return instance;
    }
    
    // Helper functions
    inline std::string getFullPath(const std::string& relativePath) {
        return getPaths().assetRoot + "/" + relativePath;
    }
    
    inline std::string getModelPath(const std::string& modelName) {
        return getPaths().modelPath + "/" + modelName;
    }
    
    inline std::string getShaderPath(const std::string& shaderName) {
        return getPaths().shaderPath + "/" + shaderName;
    }
    
    // Load configuration from file
    bool loadConfig(const std::string& filename = "engine_config.json");
    
    // Save configuration to file
    bool saveConfig(const std::string& filename = "engine_config.json");
}
