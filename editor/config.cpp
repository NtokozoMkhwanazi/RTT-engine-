// Config::loadConfig / Config::saveConfig
//
// Implements the (previously only-declared) engine configuration loader so that
// engine_config.json is actually consumed at boot, and adds the "npcs" schema:
// each NPC is a skinned animated model (model + clips + motion context) that the
// editor's NPC inspector can instantiate — exactly like the player character,
// but authored from config rather than being a geo-twin primitive cube.
//
// Parse is defensive: missing keys/sections leave the defaults from config.h in
// place (isMember guards), and a malformed file never throws — it logs and
// returns false so boot always proceeds on safe defaults.

#include "config.h"
#include <json/json.h>
#include <fstream>
#include <sstream>
#include <iostream>

namespace Config {

// ---------------------------------------------------------------------------
// NPC helpers
// ---------------------------------------------------------------------------
static void parseAnimationClipSet(NpcEntry& npc, const Json::Value& anim) {
    struct Field { const char* key; std::string& dst; };
    Field fields[] = {
        { "idle",            npc.animSet.idle },
        { "walk",            npc.animSet.walk },
        { "run",             npc.animSet.run },
        { "jump",            npc.animSet.jump },
        { "leftStrafe",      npc.animSet.leftStrafe },
        { "leftStrafeWalk",  npc.animSet.leftStrafeWalk },
        { "rightStrafe",     npc.animSet.rightStrafe },
        { "rightStrafeWalk", npc.animSet.rightStrafeWalk },
        { "leftTurn90",      npc.animSet.leftTurn90 },
        { "rightTurn90",     npc.animSet.rightTurn90 }
    };
    for (const auto& f : fields) {
        if (anim.isMember(f.key)) f.dst = anim[f.key].asString();
    }
}

static void parseNpc(NpcEntry& npc, const Json::Value& j) {
    if (j.isMember("name"))        npc.name        = j["name"].asString();
    if (j.isMember("modelPath"))   npc.modelPath   = j["modelPath"].asString();
    if (j.isMember("clipsDir"))    npc.clipsDir    = j["clipsDir"].asString();
    if (j.isMember("motionContext")) npc.motionContext = j["motionContext"].asString();
    if (j.isMember("animSet") && j["animSet"].isObject())
        parseAnimationClipSet(npc, j["animSet"]);
}

static Json::Value animSetToJson(const AnimationClipSet& a) {
    Json::Value j;
    j["idle"]            = a.idle;
    j["walk"]            = a.walk;
    j["run"]             = a.run;
    j["jump"]            = a.jump;
    j["leftStrafe"]      = a.leftStrafe;
    j["leftStrafeWalk"]  = a.leftStrafeWalk;
    j["rightStrafe"]     = a.rightStrafe;
    j["rightStrafeWalk"] = a.rightStrafeWalk;
    j["leftTurn90"]      = a.leftTurn90;
    j["rightTurn90"]     = a.rightTurn90;
    return j;
}

static Json::Value npcToJson(const NpcEntry& npc) {
    Json::Value j;
    j["name"]          = npc.name;
    j["modelPath"]     = npc.modelPath;
    j["clipsDir"]      = npc.clipsDir;
    j["motionContext"] = npc.motionContext;
    j["animSet"]       = animSetToJson(npc.animSet);
    return j;
}

// ---------------------------------------------------------------------------
// Section loaders
// ---------------------------------------------------------------------------
static void loadPaths(EnginePaths& p, const Json::Value& root) {
    const Json::Value& v = root["paths"];
    if (!v.isObject()) return;
    if (v.isMember("assetRoot"))    p.assetRoot    = v["assetRoot"].asString();
    if (v.isMember("configPath"))   p.configPath   = v["configPath"].asString();
    if (v.isMember("defaultScene")) p.defaultScene = v["defaultScene"].asString();
    if (v.isMember("fontPath"))     p.fontPath     = v["fontPath"].asString();
    if (v.isMember("modelPath"))    p.modelPath    = v["modelPath"].asString();
    if (v.isMember("savePath"))     p.savePath     = v["savePath"].asString();
    if (v.isMember("shaderPath"))   p.shaderPath   = v["shaderPath"].asString();
    if (v.isMember("skyboxPath"))   p.skyboxPath   = v["skyboxPath"].asString();
    if (v.isMember("texturePath"))  p.texturePath  = v["texturePath"].asString();
}

static void loadRender(RenderConfig& r, const Json::Value& root) {
    const Json::Value& v = root["render"];
    if (!v.isObject()) return;
    if (v.isMember("fov"))          r.defaultFOV         = v["fov"].asFloat();
    if (v.isMember("nearPlane"))    r.nearPlane          = v["nearPlane"].asFloat();
    if (v.isMember("farPlane"))     r.farPlane           = v["farPlane"].asFloat();
    if (v.isMember("vsync"))        r.vsync              = v["vsync"].asBool();
    if (v.isMember("windowWidth"))  r.defaultWindowWidth = v["windowWidth"].asInt();
    if (v.isMember("windowHeight")) r.defaultWindowHeight= v["windowHeight"].asInt();
    if (v.isMember("wireframe"))    r.wireframe          = v["wireframe"].asBool();
}

static void loadTerrain(TerrainConfig& t, const Json::Value& root) {
    const Json::Value& v = root["terrain"];
    if (!v.isObject()) return;
    if (v.isMember("chunkSize"))        t.chunkSize        = v["chunkSize"].asFloat();
    if (v.isMember("chunkResolution"))  t.chunkResolution  = v["chunkResolution"].asInt();
    if (v.isMember("viewDistance"))     t.viewDistance     = v["viewDistance"].asInt();
    // lodDistance was never parsed, so a config that raises chunkSize could not
    // raise the LOD band with it (Terrain::initialize now derives the band from
    // chunkSize as a floor, this makes it tunable from the config too).
    if (v.isMember("lodDistance"))      t.lodDistance      = v["lodDistance"].asFloat();
    if (v.isMember("heightScale"))      t.heightScale      = v["heightScale"].asFloat();
    if (v.isMember("maxActiveChunks"))  t.maxActiveChunks  = v["maxActiveChunks"].asInt();
    if (v.isMember("heightmapPath"))    t.heightmapPath    = v["heightmapPath"].asString();

    // verify.txt: artist-authored terrain material texture sets.
    // The "terrain_material" block in engine_config.json lets artists specify
    // the albedo/normal/roughness/ao paths for the 4 splat layers + the desert
    // fallback textures. These override the hardcoded paths so any imported
    // terrain asset (marble_cliff, desert_scrub, etc.) can be the surface
    // material without recompiling.
    const Json::Value& tm = root["terrain_material"];
    if (!tm.isMember("layers") || !tm["layers"].isArray()) return;
    for (int i = 0; i < 4 && i < (int)tm["layers"].size(); ++i) {
        const Json::Value& layer = tm["layers"][i];
        if (layer.isMember("albedo"))  t.materialLayers[i].albedo  = layer["albedo"].asString();
        if (layer.isMember("normal"))  t.materialLayers[i].normal  = layer["normal"].asString();
        if (layer.isMember("rough"))   t.materialLayers[i].rough   = layer["rough"].asString();
        if (layer.isMember("ao"))      t.materialLayers[i].ao      = layer["ao"].asString();
    }
    if (tm.isMember("desertAlbedo"))  t.desertAlbedoPath  = tm["desertAlbedo"].asString();
    if (tm.isMember("desertNormal"))  t.desertNormalPath  = tm["desertNormal"].asString();
    if (tm.isMember("desertRough"))   t.desertRoughPath   = tm["desertRough"].asString();
}

static void loadVegetation(VegetationConfig& vg, const Json::Value& root) {
    const Json::Value& v = root["vegetation"];
    if (!v.isObject()) return;
    if (v.isMember("treeDensity"))    vg.treeDensity    = v["treeDensity"].asFloat();
    if (v.isMember("grassDensity"))   vg.grassDensity   = v["grassDensity"].asFloat();
    if (v.isMember("rockDensity"))    vg.rockDensity    = v["rockDensity"].asFloat();
    if (v.isMember("pebbleDensity"))  vg.pebbleDensity  = v["pebbleDensity"].asFloat();
}

static void loadPhysics(PhysicsConfig& p, const Json::Value& root) {
    const Json::Value& v = root["physics"];
    if (!v.isObject()) return;
    if (v.isMember("enableCCD")) p.enableCCD = v["enableCCD"].asBool();
    if (v.isMember("gravity") && v["gravity"].isArray() &&
        v["gravity"].size() >= 3) {
        p.gravity = glm::vec3(
            v["gravity"][0].asFloat(),
            v["gravity"][1].asFloat(),
            v["gravity"][2].asFloat());
    }
}

// ---------------------------------------------------------------------------
// Character + world-object configuration
// ---------------------------------------------------------------------------
static void loadCharacter(CharacterConfig& c, const Json::Value& root) {
    const Json::Value& v = root["character"];
    if (!v.isObject()) return;
    if (v.isMember("modelPath")) c.modelPath = v["modelPath"].asString();
    if (v.isMember("clipsDir"))  c.clipsDir  = v["clipsDir"].asString();
    if (v.isMember("clips") && v["clips"].isObject()) {
        const Json::Value& clips = v["clips"];
        auto setClip = [&](const char* key, std::string& dst) {
            if (clips.isMember(key)) dst = clips[key].asString();
        };
        setClip("Idle",            c.clips.idle);
        setClip("Walk",            c.clips.walk);
        setClip("Run",             c.clips.run);
        setClip("Jump",            c.clips.jump);
        setClip("LeftStrafe",      c.clips.leftStrafe);
        setClip("LeftStrafeWalk",  c.clips.leftStrafeWalk);
        setClip("LeftTurn90",      c.clips.leftTurn90);
        setClip("RightStrafe",     c.clips.rightStrafe);
        setClip("RightStrafeWalk", c.clips.rightStrafeWalk);
        setClip("RightTurn90",     c.clips.rightTurn90);
    }
    std::cout << "[Config] Play character: model=" << c.modelPath
              << " clipsDir=" << c.clipsDir << "\n";
}

// forUI.txt Part 2: JSON helper for glm::vec3 (splat tint colors).
static Json::Value vec3ToJson(const glm::vec3& v) {
    Json::Value a(Json::arrayValue);
    a.append(v.x); a.append(v.y); a.append(v.z);
    return a;
}

static void loadWorldObjects(WorldObjectConfig& woc, const Json::Value& root) {
    const Json::Value& a = root["world_objects"];
    if (!a.isArray()) return;
    woc.entries.clear();
    woc.entries.reserve(a.size());
    for (const Json::Value& e : a) {
        if (!e.isObject()) continue;
        WorldObjectConfigEntry entry;
        if (e.isMember("type"))      entry.type      = e["type"].asString();
        if (e.isMember("modelPath")) entry.modelPath = e["modelPath"].asString();
        if (e.isMember("minScale"))  entry.minScale  = (float)e["minScale"].asFloat();
        if (e.isMember("maxScale"))  entry.maxScale  = (float)e["maxScale"].asFloat();
        if (entry.type.empty() || entry.modelPath.empty()) continue;
        woc.entries.push_back(std::move(entry));
    }
    std::cout << "[Config] Loaded " << woc.entries.size()
              << " world object(s) from engine_config.json\n";
}

// forUI.txt Part 2: hydrate the live terrain-splat PBR state from the
// "terrain_splat_pbr" block (absent -> struct defaults; gate-safe).
static void loadTerrainSplat(TerrainSplatUiState& t, const Json::Value& root) {
    const Json::Value& ts = root["terrain_splat_pbr"];
    if (!ts.isObject()) return;
    if (ts.isMember("layerHeights")  && ts["layerHeights"].size()  == 4) {
        const auto& a = ts["layerHeights"];
        t.layerHeights = glm::vec4((float)a[0].asFloat(), (float)a[1].asFloat(),
                                   (float)a[2].asFloat(), (float)a[3].asFloat());
    }
    if (ts.isMember("blendWidths")   && ts["blendWidths"].size()   == 4) {
        const auto& a = ts["blendWidths"];
        t.blendWidths = glm::vec4((float)a[0].asFloat(), (float)a[1].asFloat(),
                                  (float)a[2].asFloat(), (float)a[3].asFloat());
    }
    if (ts.isMember("slopeBands")    && ts["slopeBands"].size()    == 4) {
        const auto& a = ts["slopeBands"];
        t.slopeBands = glm::vec4((float)a[0].asFloat(), (float)a[1].asFloat(),
                                 (float)a[2].asFloat(), (float)a[3].asFloat());
    }
    const char* tintKeys[4] = {"grass", "rock", "stone", "snow"};
    glm::vec3* tintPtrs[4]  = { &t.grassTint, &t.rockTint, &t.stoneTint, &t.snowTint };
    const Json::Value& tints = ts["tints"];
    if (tints.isObject()) {
        for (int i = 0; i < 4; ++i) {
            const Json::Value& c = tints[tintKeys[i]];
            if (c.size() == 3) *tintPtrs[i] = glm::vec3((float)c[0].asFloat(), (float)c[1].asFloat(), (float)c[2].asFloat());
        }
    }
    if (ts.isMember("globalTextureScale")) t.globalTextureScale = (float)ts["globalTextureScale"].asFloat();
    if (ts.isMember("enableSplatShader"))  t.enableSplatShader  = ts["enableSplatShader"].asBool();
    if (ts.isMember("parallaxScale"))      t.parallaxScale      = (float)ts["parallaxScale"].asFloat();
    if (ts.isMember("normalStrength"))     t.normalStrength     = (float)ts["normalStrength"].asFloat();
    if (ts.isMember("tessFactorOuter"))    t.tessFactorOuter    = (float)ts["tessFactorOuter"].asFloat();
    if (ts.isMember("tessFactorInner"))    t.tessFactorInner    = (float)ts["tessFactorInner"].asFloat();
    std::cout << "[Config] Loaded terrain_splat_pbr (splatShader=" << t.enableSplatShader
              << ", texScale=" << t.globalTextureScale << ")\n";
}

// ---------------------------------------------------------------------------
// Public load / save
// ---------------------------------------------------------------------------
bool loadConfig(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.good()) {
        std::cerr << "[Config] could not open " << filename
                  << " — keeping default values.\n";
        return false;
    }

    Json::Value root;
    std::string errors;
    Json::CharReaderBuilder builder;
    if (!Json::parseFromStream(builder, file, &root, &errors)) {
        std::cerr << "[Config] parse error in " << filename << ": " << errors
                  << " — keeping default values.\n";
        return false;
    }
    if (!root.isObject()) return false;

    loadPaths   (getPaths(),       root);
    loadRender  (getRenderConfig(),root);
    loadTerrain (getTerrainConfig(),root);
    loadVegetation(getVegetationConfig(), root);
    loadPhysics (getPhysicsConfig(),root);
    loadCharacter(getCharacterConfig(), root);      // engine_config.json "character"
    loadWorldObjects(getWorldObjectConfig(), root); // engine_config.json "world_objects"
    loadTerrainSplat(getTerrainSplatConfig(), root); // engine_config.json "terrain_splat_pbr"

    // NPCs — skinned animated models (one per entry), exactly like the player
    // character but authored from config.
    NpcConfig& npcCfg = getNpcConfig();
    npcCfg.entries.clear();
    const Json::Value& n = root["npcs"];
    if (n.isArray()) {
        npcCfg.entries.reserve(n.size());
        for (const Json::Value& e : n) {
            if (!e.isObject()) continue;
            NpcEntry entry;
            parseNpc(entry, e);
            if (entry.name.empty()) entry.name = "npc_" + std::to_string(npcCfg.entries.size());
            if (entry.modelPath.empty()) continue; // skip malformed entries
            npcCfg.entries.push_back(std::move(entry));
        }
    }

    std::cout << "[Config] Loaded " << filename
              << " (" << npcCfg.entries.size() << " NPCs).\n";
    return true;
}

bool saveConfig(const std::string& filename) {
    Json::Value root;

    Json::Value p;
    p["assetRoot"]    = getPaths().assetRoot;
    p["configPath"]   = getPaths().configPath;
    p["defaultScene"] = getPaths().defaultScene;
    p["fontPath"]     = getPaths().fontPath;
    p["modelPath"]    = getPaths().modelPath;
    p["savePath"]     = getPaths().savePath;
    p["shaderPath"]   = getPaths().shaderPath;
    p["skyboxPath"]   = getPaths().skyboxPath;
    p["texturePath"]  = getPaths().texturePath;
    root["paths"] = p;

    Json::Value r;
    r["fov"]          = getRenderConfig().defaultFOV;
    r["nearPlane"]    = getRenderConfig().nearPlane;
    r["farPlane"]     = getRenderConfig().farPlane;
    r["vsync"]        = getRenderConfig().vsync;
    r["windowWidth"]  = getRenderConfig().defaultWindowWidth;
    r["windowHeight"] = getRenderConfig().defaultWindowHeight;
    r["wireframe"]    = getRenderConfig().wireframe;
    root["render"] = r;

    Json::Value t;
    t["chunkSize"]       = getTerrainConfig().chunkSize;
    t["chunkResolution"] = getTerrainConfig().chunkResolution;
    t["viewDistance"]    = getTerrainConfig().viewDistance;
    t["heightScale"]     = getTerrainConfig().heightScale;
    t["maxActiveChunks"] = getTerrainConfig().maxActiveChunks;
    t["heightmapPath"]   = getTerrainConfig().heightmapPath;
    root["terrain"] = t;

    // verify.txt: persist artist-authored terrain material texture paths.
    Json::Value tm;
    tm["desertAlbedo"] = getTerrainConfig().desertAlbedoPath;
    tm["desertNormal"] = getTerrainConfig().desertNormalPath;
    tm["desertRough"]  = getTerrainConfig().desertRoughPath;
    Json::Value tmLayers = Json::arrayValue;
    for (int i = 0; i < 4; ++i) {
        Json::Value layer;
        layer["albedo"] = getTerrainConfig().materialLayers[i].albedo;
        layer["normal"] = getTerrainConfig().materialLayers[i].normal;
        layer["rough"]  = getTerrainConfig().materialLayers[i].rough;
        layer["ao"]     = getTerrainConfig().materialLayers[i].ao;
        tmLayers.append(layer);
    }
    tm["layers"] = tmLayers;
    root["terrain_material"] = tm;

    Json::Value vg;
    vg["treeDensity"]   = getVegetationConfig().treeDensity;
    vg["grassDensity"]  = getVegetationConfig().grassDensity;
    vg["rockDensity"]   = getVegetationConfig().rockDensity;
    vg["pebbleDensity"] = getVegetationConfig().pebbleDensity;
    root["vegetation"] = vg;

    Json::Value ph;
    ph["enableCCD"] = getPhysicsConfig().enableCCD;
    Json::Value g;
    g.append(getPhysicsConfig().gravity.x);
    g.append(getPhysicsConfig().gravity.y);
    g.append(getPhysicsConfig().gravity.z);
    ph["gravity"] = g;
    root["physics"] = ph;

    Json::Value npcs(Json::arrayValue);
    for (const auto& npc : getNpcConfig().entries) npcs.append(npcToJson(npc));
    root["npcs"] = npcs;

    Json::Value ch(Json::objectValue);
    ch["modelPath"] = getCharacterConfig().modelPath;
    ch["clipsDir"]  = getCharacterConfig().clipsDir;
    {
        Json::Value cl(Json::objectValue);
        cl["Idle"]            = getCharacterConfig().clips.idle;
        cl["Walk"]            = getCharacterConfig().clips.walk;
        cl["Run"]             = getCharacterConfig().clips.run;
        cl["Jump"]            = getCharacterConfig().clips.jump;
        cl["LeftStrafe"]      = getCharacterConfig().clips.leftStrafe;
        cl["LeftStrafeWalk"]  = getCharacterConfig().clips.leftStrafeWalk;
        cl["LeftTurn90"]      = getCharacterConfig().clips.leftTurn90;
        cl["RightStrafe"]     = getCharacterConfig().clips.rightStrafe;
        cl["RightStrafeWalk"] = getCharacterConfig().clips.rightStrafeWalk;
        cl["RightTurn90"]     = getCharacterConfig().clips.rightTurn90;
        ch["clips"] = cl;
    }
    root["character"] = ch;

    Json::Value wos(Json::arrayValue);
    for (const auto& e : getWorldObjectConfig().entries) {
        Json::Value j;
        j["type"]      = e.type;
        j["modelPath"] = e.modelPath;
        j["minScale"]  = e.minScale;
        j["maxScale"]  = e.maxScale;
        wos.append(j);
    }
    root["world_objects"] = wos;

    // forUI.txt Part 2: persist the live terrain-splat PBR state.
    Json::Value tsd(Json::objectValue);
    tsd["enableSplatShader"]  = getTerrainSplatConfig().enableSplatShader;
    tsd["globalTextureScale"] = getTerrainSplatConfig().globalTextureScale;
    Json::Value lh(Json::arrayValue); for (int i = 0; i < 4; ++i) lh.append(getTerrainSplatConfig().layerHeights[i]);
    Json::Value bw(Json::arrayValue); for (int i = 0; i < 4; ++i) bw.append(getTerrainSplatConfig().blendWidths[i]);
    Json::Value sb(Json::arrayValue); for (int i = 0; i < 4; ++i) sb.append(getTerrainSplatConfig().slopeBands[i]);
    tsd["layerHeights"] = lh; tsd["blendWidths"] = bw; tsd["slopeBands"] = sb;
    Json::Value tt(Json::objectValue);
    tt["grass"] = vec3ToJson(getTerrainSplatConfig().grassTint);
    tt["rock"]  = vec3ToJson(getTerrainSplatConfig().rockTint);
    tt["stone"] = vec3ToJson(getTerrainSplatConfig().stoneTint);
    tt["snow"]  = vec3ToJson(getTerrainSplatConfig().snowTint);
    tsd["tints"] = tt;
    tsd["parallaxScale"]   = getTerrainSplatConfig().parallaxScale;
    tsd["normalStrength"]  = getTerrainSplatConfig().normalStrength;
    tsd["tessFactorOuter"] = getTerrainSplatConfig().tessFactorOuter;
    tsd["tessFactorInner"] = getTerrainSplatConfig().tessFactorInner;
    root["terrain_splat_pbr"] = tsd;

    Json::StreamWriterBuilder wbuilder;
    std::string out = Json::writeString(wbuilder, root);

    std::ofstream ofs(filename);
    if (!ofs) {
        std::cerr << "[Config] could not write " << filename << "\n";
        return false;
    }
    ofs << out;
    return true;
}

// forUI.txt Step 2/4: persist / reload the "atmosphere_pipeline" block.
// Read-modify-writes engine_config.json so existing keys (paths/render/
// terrain/...) survive. Adapted from the nlohmann snippet to jsoncpp.
bool SaveAtmosphereSettings(const std::string& filepath,
                            const SkyboxUiState& skyboxConfig,
                            const PhysicalSkyUiState& physicalSkyConfig) {
    Json::Value root;
    std::ifstream inFile(filepath);
    if (inFile.is_open()) {
        std::string errors;
        Json::CharReaderBuilder builder;
        if (!Json::parseFromStream(builder, inFile, &root, &errors)) {
            std::cerr << "[Atmosphere Serialization] Read parse error: " << errors << "\n";
        }
    }
    inFile.close();
    if (!root.isObject()) root = Json::Value(Json::objectValue);

    Json::Value atmos(Json::objectValue);
    Json::Value cube(Json::objectValue);
    cube["blendFactor"]       = skyboxConfig.blendFactor;
    cube["autoCycleTime"]     = skyboxConfig.autoCycleTime;
    cube["timeCycleSpeed"]    = skyboxConfig.timeCycleSpeed;
    cube["debugSolidMagenta"] = skyboxConfig.debugSolidMagenta;
    atmos["cubemap"] = cube;

    Json::Value proc(Json::objectValue);
    proc["turbidity"]                = physicalSkyConfig.turbidity;
    proc["sunIntensity"]             = physicalSkyConfig.sunIntensity;
    proc["useProceduralOverCubemap"] = physicalSkyConfig.useProceduralOverCubemap;
    atmos["procedural"] = proc;

    root["atmosphere_pipeline"] = atmos;

    Json::StreamWriterBuilder wbuilder;
    wbuilder["indentation"] = "  ";
    std::ofstream outFile(filepath);
    if (!outFile) {
        std::cerr << "[Atmosphere Serialization] Failed to open write stream: " << filepath << "\n";
        return false;
    }
    outFile << Json::writeString(wbuilder, root);
    return true;
}

bool LoadAtmosphereSettings(const std::string& filepath,
                            SkyboxUiState& outSkyboxConfig,
                            PhysicalSkyUiState& outPhysicalSkyConfig) {
    std::ifstream inFile(filepath);
    if (!inFile.is_open()) {
        std::cerr << "[Atmosphere Serialization] config asset missing: " << filepath << "\n";
        return false;
    }
    Json::Value root;
    std::string errors;
    Json::CharReaderBuilder builder;
    if (!Json::parseFromStream(builder, inFile, &root, &errors)) {
        std::cerr << "[Atmosphere Serialization] parse error: " << errors << "\n";
        return false;
    }
    inFile.close();

    if (root.isMember("atmosphere_pipeline") && root["atmosphere_pipeline"].isObject()) {
        const Json::Value& atmos = root["atmosphere_pipeline"];
        if (atmos.isMember("cubemap") && atmos["cubemap"].isObject()) {
            const Json::Value& c = atmos["cubemap"];
            if (c.isMember("blendFactor"))       outSkyboxConfig.blendFactor       = (float)c["blendFactor"].asFloat();
            if (c.isMember("autoCycleTime"))     outSkyboxConfig.autoCycleTime     = c["autoCycleTime"].asBool();
            if (c.isMember("timeCycleSpeed"))    outSkyboxConfig.timeCycleSpeed    = (float)c["timeCycleSpeed"].asFloat();
            if (c.isMember("debugSolidMagenta")) outSkyboxConfig.debugSolidMagenta = c["debugSolidMagenta"].asBool();
        }
        if (atmos.isMember("procedural") && atmos["procedural"].isObject()) {
            const Json::Value& p = atmos["procedural"];
            if (p.isMember("turbidity"))                outPhysicalSkyConfig.turbidity              = (float)p["turbidity"].asFloat();
            if (p.isMember("sunIntensity"))             outPhysicalSkyConfig.sunIntensity           = (float)p["sunIntensity"].asFloat();
            if (p.isMember("useProceduralOverCubemap")) outPhysicalSkyConfig.useProceduralOverCubemap = p["useProceduralOverCubemap"].asBool();
        }
    }
    std::cout << "[Config] Loaded atmosphere_pipeline (blend=" << outSkyboxConfig.blendFactor
              << ", turbidity=" << outPhysicalSkyConfig.turbidity << ")\n";
    return true;
}

} // namespace Config
