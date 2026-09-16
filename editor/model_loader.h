#pragma once

#include "ecs/ECS.h"
#include <imgui.h>

namespace UI {

/**
 * Render Model Loader Dialog
 *
 * Provides a file browser and model loading interface
 */
void RenderModelLoader(bool& showModelLoader);

/**
 * Render Model Inspector Panel
 *
 * Shows model details and animation controls for selected entity
 */
void RenderModelInspector(ecs::EntityID selectedEntity, ecs::World& world);

} // namespace UI
