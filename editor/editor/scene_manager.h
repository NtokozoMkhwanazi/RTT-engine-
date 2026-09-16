#ifndef EDITOR_SCENE_MANAGER_H
#define EDITOR_SCENE_MANAGER_H

#include <string>
#include "ecs/ECS.h"

// ============================================================================
// Scene Serialization (Save/Load)
// ============================================================================
namespace SceneManager {

// Save current scene to file
bool SaveScene(const std::string& filename, ecs::World& world);

// Load scene from file
bool LoadScene(const std::string& filename, ecs::World& world);

// Get current scene file path
const std::string& GetCurrentSceneFile();

// Set current scene file path
void SetCurrentSceneFile(const std::string& filename);

} // namespace SceneManager

#endif // EDITOR_SCENE_MANAGER_H
