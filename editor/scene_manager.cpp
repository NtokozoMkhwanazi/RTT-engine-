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
    std::cout << "[SceneManager] Scene loading with models - use Asset Browser\n";
    std::cout << "[SceneManager] Old scene files contain primitives\n";
    std::cout << "[SceneManager] Re-create scene using loaded models\n";
    (void)filename; (void)world;
    return false;
}

const std::string& GetCurrentSceneFile() {
    return g_currentSceneFile;
}

void SetCurrentSceneFile(const std::string& filename) {
    g_currentSceneFile = filename;
}

} // namespace SceneManager
