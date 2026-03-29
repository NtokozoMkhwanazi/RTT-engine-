#pragma once

/**
 * Enhanced Systems Header - Includes all updated systems with archetype support
 */

#include "RenderSystem.h"
#include "PhysicsSystem.h"
#include "AnimationSystem.h"

// New integrated systems
#include "CharacterControllerSystem.h"
#include "TerrainSystem.h"
#include "WorldObjectSystem.h"
#include "MotionMatchingSystem.h"

/**
 * Available Systems:
 *
 * Rendering:
 * - RenderSystem: Static mesh rendering with frustum culling
 * - SkinnedMeshRenderSystem: Animated character rendering
 * - CameraSystem: Active camera management
 * - LightSystem: Light collection for shaders
 *
 * Physics:
 * - PhysicsSystem: Rigid body simulation with substepping (direct component processing)
 * - CharacterControllerSystem: Player movement (direct component processing)
 *
 * Animation:
 * - AnimationSystem: Animator updates and bone transforms (direct component processing)
 * - AnimationStateSystem: State machine parameter updates
 * - MotionMatchingSystem: Advanced motion matching integration
 *
 * World:
 * - TerrainSystem: Terrain chunk streaming and LOD
 * - WorldObjectSystem: Object placement and culling
 * - VegetationSystem: Grass and tree rendering
 *
 * All systems now support:
 * - Archetype-based iteration for cache-coherent access
 * - Parallel execution via JobSystem
 * - Event publishing for component changes
 * - Direct component processing (no wrapper overhead)
 */
