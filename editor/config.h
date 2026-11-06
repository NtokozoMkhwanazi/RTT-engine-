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
    
    // verify.txt: artist-authored terrain material texture set for one splat layer.
    // Artists point these at the texture files from any Poly Haven / Quixel /
    // custom asset folder they have imported. Leave a path empty to fall back
    // to the engine's built-in default for that channel.
    struct TerrainMaterialLayer {
        std::string albedo;  // sRGB albedo/diffuse JPG
        std::string normal;  // EXR or JPG normal map (GL convention: green=+Y)
        std::string rough;   // EXR (RGBA16F) or JPG roughness (R8)
        std::string ao;      // AO JPG (optional; empty = proxy from rough's R channel)
    };

    // Terrain configuration
    struct TerrainConfig {
        float chunkSize = 80.0f;
        int chunkResolution = 32;
        int viewDistance = 2;
        float lodDistance = 60.0f;
        int heightmapSize = 1024;
        float heightScale = 50.0f;
        // Optional real heightmap asset (.exr or .jpg/.png L8). When set, the
        // terrain loads authored elevation instead of the procedural Perlin
        // arena. Defaults to a shipped 256x256 authored heightmap so a fresh
        // open gets real terrain geometry instead of the flat Perlin arena.
        std::string heightmapPath = "assets/terrain/default_heightmap.png";

        // verify.txt: artist-authored terrain material texture sets (defaults
        // match the engine's built-in rock/gravel set). When the artist sets
        // a terrain material, these path strings override the compiled-in
        // texture constants, so the surface material (albedo/normal/roughness/ao)
        // follows the heightmap. No procedural fallbacks: if a path is empty or
        // the file is missing, the artist is alerted.
        TerrainMaterialLayer materialLayers[4];
        std::string desertAlbedoPath  = "assets/rocky_trail/textures_1k/rocky_trail_02_diff_4k.jpg";
        std::string desertNormalPath  = "assets/rocky_trail/textures_1k/rocky_trail_02_nor_gl_4k.jpg";
        std::string desertRoughPath   = "assets/rocky_trail/textures_1k/rocky_trail_02_arm_4k.jpg";

        // Performance optimizations
        int maxActiveChunks = 8;
        float chunkLoadDistance = 80.0f;
        float chunkUnloadDistance = 120.0f;
    };
    
    // Vegetation configuration. Driven into VegetationSystem, then transferred
    // to WorldObjectManager which actually renders the instances.
    // Chunk is 80x80m = 6400 m2. With the values below:
    //   trees   ~0.004 * 6400 = ~26/chunk (capped 32)  -> fuller forest
    //   grass   ~0.04  * 6400 = ~256/chunk (clamped)   -> a tufted coastal field
    //   rocks   ~0.003 * 6400 = ~24/chunk (capped 24)  -> denser outcrops
    //   pebbles ~0.01  * 6400 = ~64/chunk (capped 60)  -> ground scatter of
    //                    tiny stones (the "tiny rocks" of a rocky coast)
    // across the 5x5-chunk view (~25 chunks) that gives a genuinely "whole
    // world" feel instead of a sparse scatter. These were previously ~0,
    // which silently produced an empty world.
    struct VegetationConfig {
        float treeDensity = 0.01f;
        float grassDensity = 0.04f;
        float rockDensity = 0.006f;
        int maxTreesPerChunk = 64;
        int maxRocksPerChunk = 32;
        int maxPebblesPerChunk = 60;
        float pebbleDensity = 0.01f;
        float minTreeHeight = 1.0f;
        float maxTreeHeight = 3.0f;
        
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

    // NPC spawn configuration. Each NPC is a skinned, animated 3D model (the
    // same kind of rig as the player character — ModelComponent + Animator)
    // with its own model file, animation clips and motion context. Populated by
    // Config::loadConfig() from the "npcs" array in engine_config.json; the
    // editor's NPC inspector reads getNpcConfig() to instantiate/preview them.
    // (Rendering skinned NPCs is enabled once RenderSystem/ModelRenderSystem
    // are driven by the render pipeline — see the primitive-render plan.)
    struct AnimationClipSet {
        std::string idle            = "Idle";
        std::string walk            = "Walk";
        std::string run             = "Run";
        std::string jump            = "Jump";
        std::string leftStrafe      = "LeftStrafe";
        std::string leftStrafeWalk  = "LeftStrafeWalk";
        std::string rightStrafe     = "RightStrafe";
        std::string rightStrafeWalk = "RightStrafeWalk";
        std::string leftTurn90      = "LeftTurn90";
        std::string rightTurn90     = "RightTurn90";
    };
    struct NpcEntry {
        std::string name;               // display + spawn name
        std::string modelPath;          // e.g. "assets/NPCs/guard.fbx"
        std::string clipsDir;           // asset dir the clips live in (e.g. "assets")
        std::string motionContext;      // "Locomotion" | "Crouch" | "Combat" | "Capoeira" | "Dance"
        AnimationClipSet animSet;
    };
    struct NpcConfig {
        std::vector<NpcEntry> entries;
    };
    inline NpcConfig& getNpcConfig() {
        static NpcConfig instance;
        return instance;
    }

    // Play-character configuration (engine_config.json "character"). Drives the
    // boot play model + clip set from config instead of a hardcoded bot.fbx
    // stand-in - forUI.txt: "engine as a tool — no pre-baked model paths;
    // the user's config drives loading."
    struct CharacterConfig {
        std::string modelPath;   // e.g. "assets/bot321.fbx" (artist-authored)
        std::string clipsDir;    // e.g. "assets" (dir holding the clip set)
        AnimationClipSet clips;  // named clip -> source file (motion context)
    };
    inline CharacterConfig& getCharacterConfig() {
        static CharacterConfig instance;
        return instance;
    }

    // World-object prop configuration (engine_config.json "world_objects").
    // Artists author prop modelPaths per WorldObjectType; WorldManager overlays
    // them over the hardcoded stand-ins via WorldObjectManager::loadObject, so
    // no prop path is baked at the tool layer - everything is dynamic.
    struct WorldObjectConfigEntry {
        std::string type;       // WorldObjectType name, e.g. "TREE_PINE"
        std::string modelPath;  // e.g. "assets/pine_tree/fir_tree_01_lt.gltf"
        float minScale = 0.8f;
        float maxScale = 1.2f;
        float windSway = 0.0f;  // vertex-sway amplitude (0 = static, forUI.txt)
    };
    struct WorldObjectConfig {
        std::vector<WorldObjectConfigEntry> entries;
    };
    inline WorldObjectConfig& getWorldObjectConfig() {
        static WorldObjectConfig instance;
        return instance;
    }

    // forUI.txt Part 2: live terrain-splat PBR parameters exposed by the
    // World Settings -> Camera Settings / Landscape Splat panel. Persisted to
    // engine_config.json under "terrain_splat_pbr" by Config::saveConfig.
    struct TerrainSplatUiState {
        glm::vec4 layerHeights = glm::vec4(2.0f, 16.0f, 38.0f, 62.0f);
        glm::vec4 blendWidths  = glm::vec4(6.0f, 9.0f, 9.0f, 8.0f);
        glm::vec4 slopeBands   = glm::vec4(0.0f, 0.15f, 0.60f, 0.85f);
        glm::vec3 grassTint    = glm::vec3(1.0f);
        glm::vec3 rockTint     = glm::vec3(1.0f);
        glm::vec3 stoneTint    = glm::vec3(1.0f);
        glm::vec3 snowTint     = glm::vec3(0.88f, 0.92f, 1.0f);
        float globalTextureScale = 8.0f;  // matches runtime g_terrainTexScale (TerrainChunk.cpp)
        bool  enableSplatShader  = true;

        // forUI.txt: live POM / normal-strength / tessellation sliders, wired
        // through Terrain::renderSplat -> TerrainSplatParams -> terrain_splat
        // shaders (uParallaxScale / uNormalStrength / uTessFactorOuter/Inner).
        // 0 disables the effect (vertex displacement + tessellation-off).
        float parallaxScale   = 0.04f;   // POM strength (0 = vertex displacement only)
        float normalStrength  = 1.0f;    // detail normal-map strength
        float tessFactorOuter = 4.0f;   // outer tessellation level (0 = tess off)
        float tessFactorInner = 4.0f;   // inner tessellation level (0 = tess off)
    };
    inline TerrainSplatUiState& getTerrainSplatConfig() {
        static TerrainSplatUiState instance;
        return instance;
    }

    // forUI.txt: skybox + analytic physical sky authoring state (exposed in
    // RenderWorldSettingsPanel, persisted to engine_config.json
    // "atmosphere_pipeline" via Save/LoadAtmosphereSettings).
    struct SkyboxUiState {
        float blendFactor = 0.0f;        // 0.0 = Pure Day, 1.0 = Pure Night
        bool  autoCycleTime = false;     // auto-increment blend via dt
        float timeCycleSpeed = 0.02f;   // increment step multiplier
        bool  debugSolidMagenta = false; // fragment shader debug fill
    };
    struct PhysicalSkyUiState {
        float turbidity = 2.0f;                  // 1.0 (clear) .. 64.0 (hazy)
        float sunIntensity = 1.0f;             // brightness scale
        bool  useProceduralOverCubemap = false; // pipeline routing switch
    };

    // forUI.txt Step 2/4: persist / reload the "atmosphere_pipeline" block
    // (cubemap day/night + analytic physical sky params). Read-modify-writes
    // engine_config.json so existing keys survive. (jsoncpp, NOT nlohmann.)
    bool SaveAtmosphereSettings(const std::string& filepath,
                                const SkyboxUiState& skyboxConfig,
                                const PhysicalSkyUiState& physicalSkyConfig);
    bool LoadAtmosphereSettings(const std::string& filepath,
                                SkyboxUiState& outSkyboxConfig,
                                PhysicalSkyUiState& outPhysicalSkyConfig);

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
