#include "scene_manager.h"
#include "entity_manager.h"
#include "ecs/components/Components.h"
#include <fstream>
#include <sstream>
#include <iostream>

namespace SceneManager {

static std::string g_currentSceneFile;

bool SaveScene(const std::string& filename, ecs::World& world) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "ERROR: Could not open file for writing: " << filename << "\n";
        return false;
    }

    file << "{\n";
    file << "  \"version\": \"1.0\",\n";
    file << "  \"entities\": [\n";

    bool first = true;
    world.forEach<ecs::TransformComponent, ecs::NameComponent>(
        [&](ecs::EntityID id, ecs::TransformComponent& t, ecs::NameComponent& n) {
            if (!first) file << ",\n";
            first = false;

            file << "    {\n";
            file << "      \"name\": \"" << n.name << "\",\n";
            file << "      \"position\": [" << t.position.x << ", " << t.position.y << ", " << t.position.z << "],\n";
            file << "      \"rotation\": [" << t.rotation.x << ", " << t.rotation.y << ", " << t.rotation.z << ", " << t.rotation.w << "],\n";
            file << "      \"scale\": [" << t.scale.x << ", " << t.scale.y << ", " << t.scale.z << "]\n";
            file << "    }";
        });

    file << "\n  ]\n";
    file << "}\n";

    file.close();
    g_currentSceneFile = filename;
    std::cout << "Scene saved to: " << filename << "\n";
    return true;
}

bool LoadScene(const std::string& filename, ecs::World& world) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "ERROR: Could not open file for reading: " << filename << "\n";
        return false;
    }

    // Clear current scene
    world.shutdown();
    world.init();

    // Simple parsing (production would use proper JSON library)
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    // Parse entities (simplified - just looking for position patterns)
    size_t pos = 0;
    int entityCount = 0;

    while ((pos = content.find("\"position\":", pos)) != std::string::npos) {
        size_t start = content.find("[", pos);
        size_t end = content.find("]", start);
        if (start != std::string::npos && end != std::string::npos) {
            std::string posStr = content.substr(start + 1, end - start - 1);
            float x, y, z;
            if (sscanf(posStr.c_str(), "%f, %f, %f", &x, &y, &z) == 3) {
                EntityManager::CreateCube(glm::vec3(x, y, z), glm::vec3(1), glm::vec3(0.8f, 0.8f, 0.8f));
                entityCount++;
            }
        }
        pos = end;
    }

    g_currentSceneFile = filename;
    std::cout << "Loaded " << entityCount << " entities from: " << filename << "\n";
    return true;
}

const std::string& GetCurrentSceneFile() {
    return g_currentSceneFile;
}

void SetCurrentSceneFile(const std::string& filename) {
    g_currentSceneFile = filename;
}

} // namespace SceneManager
