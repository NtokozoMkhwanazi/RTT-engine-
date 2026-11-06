#ifndef EDITOR_SCENE_MANAGER_H
#define EDITOR_SCENE_MANAGER_H

#include <string>
#include "ecs/ECS.h"

namespace Editor { class Editor; }   // forward; full type needed in the .cpp

// ============================================================================
// Scene Serialization (Save/Load)
// ============================================================================
namespace SceneManager {

// Save current scene to file
bool SaveScene(const std::string& filename, ecs::World& world);

// Load scene from file. When Editor::Editor::uiState.sceneLoadAsync.active is
// set, model loads are deferred (CPU parse on the JobSystem pool + main-thread
// GL commit driven by CommitSceneModelLoads) instead of running synchronously
// on the main thread. Non-model entity state (transforms/lights/meshes) is
// always restored inline.
bool LoadScene(const std::string& filename, ecs::World& world);

// Pump deferred scene model loads (GL upload + ModelHandle attach) one step on
// the MAIN (GL) thread, time-sliced to keep the render thread >=30 FPS. Call
// every frame while uiState.sceneLoadAsync.isLoading is true.
void CommitSceneModelLoads(Editor::Editor& editor);

// Get current scene file path
const std::string& GetCurrentSceneFile();

// Set current scene file path
void SetCurrentSceneFile(const std::string& filename);

} // namespace SceneManager

#endif // EDITOR_SCENE_MANAGER_H
