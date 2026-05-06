#pragma once

// Core ECS Components

#include "TransformComponent.h"
#include "MeshComponent.h"
#include "ModelComponent.h"
#include "CameraComponent.h"
#include "RigidBodyComponent.h"
#include "AnimatorComponent.h"
#include "LightComponent.h"
#include "TagComponent.h"

// Terrain and World
#include "TerrainComponent.h"
#include "WorldObjectComponent.h"

// Animation
#include "MotionMatchingComponent.h"

// Geospatial
#include "GeospatialComponent.h"

/**
 * ECS Components Module
 *
 * Available Components:
 * 
 * Core:
 * - TransformComponent: Position, rotation, scale
 * - MeshComponent: Static mesh rendering
 * - SkinnedMeshComponent: Animated mesh rendering
 * - CameraComponent: Camera for viewing the scene
 * - CameraControllerComponent: Camera input control
 * - RigidBodyComponent: Physics simulation
 * - CharacterControllerComponent: Player physics
 * - SkeletonComponent: Bone hierarchy
 * - AnimatorComponent: Animation playback
 * - AnimationStateComponent: Animation state machine
 * - LightComponent: Light sources
 * - AmbientLightComponent: Global ambient light
 * - NameComponent: Entity name
 * - TagComponent: Single tag
 * - TagsComponent: Multiple tags
 * - ParentComponent: Entity parent
 * - ChildrenComponent: Entity children
 * - LifetimeComponent: Auto-destruct timer
 * - ActiveComponent: Enable/disable
 * 
 * Terrain & World:
 * - TerrainComponent: Terrain root settings
 * - TerrainChunkComponent: Terrain chunk data
 * - WorldObjectComponent: World object (trees, rocks, etc.)
 * - VegetationComponent: Vegetation-specific properties
 * - GrassComponent: Grass patch properties
 * 
 * Animation:
 * - MotionMatchingComponent: Motion matching state
 * - MotionDatabaseComponent: Motion database reference
 */
