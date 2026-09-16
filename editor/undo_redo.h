#pragma once
#include "ecs/ECS.h"
#include "ecs/components/Components.h"
#include <glm/glm.hpp>
#include <jsoncpp/json/json.h>
#include <chrono>
#include <string>
#include <vector>

// ============================================================================
// Undo/Redo - snapshot-based editor history.
//
// Commands store a JSON snapshot of the affected entity (transform + mesh +
// name) and reference the entity by NAME, so a command survives a scene
// save/load round-trip even though entity ids change (SceneManager remaps by
// name automatically by simply re-resolving the name on every apply).
// ============================================================================
namespace UndoRedo {

// Drag coalesce window: a transform drag pushed within this window after the
// previous drag on the SAME entity merges into the previous command.
inline const std::chrono::milliseconds kCoalesceWindow{1500};

// Clear both stacks (undo + redo).
void Clear();

// Stack queries
bool CanUndo();
bool CanRedo();

// Recording helpers
void RecordCreate(ecs::EntityID id);
void RecordCreateMany(const std::vector<ecs::EntityID>& ids);
void RecordDelete(ecs::EntityID id);

// Gizmo drag recording: start captures the pre-drag transform, end captures
// the post-drag transform. An end without a matching start is a no-op; a drag
// that changed nothing pushes nothing.
void RecordTransformStart(ecs::EntityID id, const ecs::TransformComponent& before);
void RecordTransformEnd(ecs::EntityID id, const ecs::TransformComponent& after);

// Apply
void Undo();
void Redo();

// Scene persistence (called by SceneManager::SaveScene/LoadScene). Returns the
// "history" JSON node, or clears both stacks when the node is absent/empty.
Json::Value SerializeHistory();
void DeserializeHistory(const Json::Value& json);

} // namespace UndoRedo
