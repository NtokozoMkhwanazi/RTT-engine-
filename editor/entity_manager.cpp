#include "entity_manager.h"
#include "editor_state.h"
#include "console.h"
#include "undo_redo.h"
#include "ecs/components/Components.h"
#include "ecs/components/ModelComponent.h"
#include "ecs/components/GeospatialComponent.h"
#include "ecs/components/PredictionComponent.h"
#include "modelSystem/ModelManager.h"
#include "AssetKindMesh.h"   // Editor::meshPathForAssetKind for twin render mesh
#include "AssetKindScale.h"  // Editor::characterParityScale / assetKindSizeMultiplier (scale parity w/ play bot)
#include "geospatial/GeoAPI.h"   // geo::AssetKind + AssetKindToString (canonical)
#include "AnimatedCharacter.h"        // AnimatedCharacter (shared NPC properties)
#include "ecs/components/AnimatedCharacterComponent.h"
#include <iostream>
#include <filesystem>
#include <algorithm>

namespace EntityManager {

// characterParityScale / assetKindSizeMultiplier now live in AssetKindScale.h
// (shared with editor_application.cpp's live-telemetry sync) so BOTH twin
// spawn paths normalize viewport scale to the play bot's 1.26 m base.

static AnimatedCharacter::MotionContext parseMotionContext(const std::string& s) {
    if (s == "Crouch" || s == "crouch") return AnimatedCharacter::MotionContext::CROUCH;
    if (s == "Combat" || s == "combat") return AnimatedCharacter::MotionContext::COMBAT;
    if (s == "Capoeira" || s == "capoeira") return AnimatedCharacter::MotionContext::CAPOEIRA;
    if (s == "Dance" || s == "dance") return AnimatedCharacter::MotionContext::DANCE;
    return AnimatedCharacter::MotionContext::LOCOMOTION;  // default / "Locomotion"
}

// Remove primitives - use model loading instead

ecs::Entity CreateLight(const glm::vec3& pos, const glm::vec3& color, float intensity) {
    auto e = g_editor.world().createEntityWithComponents<ecs::TransformComponent,
                                                         ecs::LightComponent,
                                                         ecs::NameComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e);
    if (t) { t->position = pos; t->scale = glm::vec3(0.2f); t->rotation = glm::quat(1, 0, 0, 0); }

    auto* l = g_editor.world().getComponentArchetype<ecs::LightComponent>(e);
    if (l) { l->color = color; l->intensity = intensity; l->type = ecs::LightType::POINT; }

    // Unique name (id-suffixed) so the undo stack can locate this entity by
    // name and the outliner shows something meaningful.
    if (auto* n = g_editor.world().getComponentArchetype<ecs::NameComponent>(e)) {
        n->setName("Point Light " + std::to_string(e.id));
    }

    UndoRedo::RecordCreate(e.id);
    EditorConsole::Log("Created Light");
    return e;
}

ecs::Entity CreateCamera(const glm::vec3& pos, const glm::vec3& target) {
    auto e = g_editor.world().createEntityWithComponents<ecs::TransformComponent,
                                                         ecs::CameraComponent,
                                                         ecs::NameComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e);
    if (t) { t->position = pos; t->scale = glm::vec3(1); t->rotation = glm::quat(1, 0, 0, 0); }

    auto* c = g_editor.world().getComponentArchetype<ecs::CameraComponent>(e);
    if (c) { c->isActive = true; }

    if (auto* n = g_editor.world().getComponentArchetype<ecs::NameComponent>(e)) {
        n->setName("Camera " + std::to_string(e.id));
    }

    // Orient the camera toward the target so it spawns facing somewhere useful.
    glm::vec3 dir = target - pos;
    const float len = glm::length(dir);
    if (len > 0.0001f) {
        dir /= len;
        if (std::abs(dir.y) > 0.999f) dir.x += 0.001f;  // avoid degenerate up-alignment
        if (auto* t = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e)) {
            t->rotation = glm::quatLookAt(glm::normalize(dir), glm::vec3(0.0f, 1.0f, 0.0f));
        }
    }

    UndoRedo::RecordCreate(e.id);
    EditorConsole::Log("Created Camera");
    return e;
}

void DeleteEntity(ecs::EntityID id) {
    if (!EntityExists(id)) return;

    // The playable character is engine-owned; refuse to delete it.
    ecs::Entity ent{id};
    if (auto* n = g_editor.world().getComponentArchetype<ecs::NameComponent>(ent)) {
        if (std::string(n->getName()) == "Bot Player") return;
    }

    EditorConsole::Log("Deleted entity " + std::to_string(id));

    // Record BEFORE destroying so the snapshot can capture the live entity.
    UndoRedo::RecordDelete(id);
    g_editor.world().destroyEntity(ent);

    if (g_editor.selectedEntity() == id) {
        g_editor.setSelectedEntity(ecs::INVALID_ENTITY_ID);
    }
}

ecs::EntityID DuplicateEntity(ecs::EntityID id) {
    if (!EntityExists(id)) return ecs::INVALID_ENTITY_ID;

    ecs::World& world = g_editor.world();
    ecs::Entity entity{id};
    auto* t = world.getComponentArchetype<ecs::TransformComponent>(entity);
    auto* m = world.getComponentArchetype<ecs::MeshComponent>(entity);
    auto* n = world.getComponentArchetype<ecs::NameComponent>(entity);

    auto newEntity = world.createEntityWithComponents<ecs::TransformComponent>();
    if (!newEntity.isValid()) return ecs::INVALID_ENTITY_ID;

    if (auto* newT = world.getComponentArchetype<ecs::TransformComponent>(newEntity)) {
        if (t) {
            newT->position = t->position + glm::vec3(1, 0, 0);
            newT->rotation = t->rotation;
            newT->scale = t->scale;
        }
    }

    if (m) {
        world.addComponentArchetype<ecs::MeshComponent>(newEntity);
        if (auto* newM = world.getComponentArchetype<ecs::MeshComponent>(newEntity)) {
            newM->color = m->color;
            newM->visible = m->visible;
            newM->meshID = m->meshID;
            newM->meshType = m->meshType;
        }
    }

    if (n) {
        world.addComponentArchetype<ecs::NameComponent>(newEntity);
        if (auto* newN = world.getComponentArchetype<ecs::NameComponent>(newEntity)) {
            newN->setName(std::string(n->getName()) + "_copy");
        }
    }

    UndoRedo::RecordCreate(newEntity.id);
    EditorConsole::Log("Duplicated entity " + std::to_string(id) + " -> " + std::to_string(newEntity.id));
    return newEntity.id;
}

std::string GetEntityName(ecs::EntityID id) {
    ecs::Entity entity{id};
    auto* n = g_editor.world().getComponentArchetype<ecs::NameComponent>(entity);
    if (n && n->hasName()) return n->getName();
    return "Entity " + std::to_string(id);
}

void SetEntityName(ecs::EntityID id, const std::string& name) {
    if (!EntityExists(id)) return;

    ecs::Entity entity{id};
    auto* n = g_editor.world().getComponentArchetype<ecs::NameComponent>(entity);
    if (n) {
        n->setName(name);
    }
}

// ============================================================================
// Entity Validation Helpers
// ============================================================================

bool EntityExists(ecs::EntityID id) {
    if (!IsValidEntityID(id)) return false;
    return g_editor.world().isAlive(id);
}

ecs::EntityID ValidateOrClear(ecs::EntityID id) {
    return EntityExists(id) ? id : ecs::INVALID_ENTITY_ID;
}

std::string GetSafeEntityName(ecs::EntityID id, const char* fallback) {
    if (!EntityExists(id)) return std::string(fallback ? fallback : "Invalid Entity");
    return GetEntityName(id);
}

ecs::Entity CreateModel(const std::string& modelPath, const glm::vec3& pos, const glm::vec3& scale, const glm::vec3& rotation) {
    auto handle = ModelSystem::ModelRegistry::getInstance().load(modelPath);
    Model* model = ModelSystem::ModelRegistry::getInstance().get(handle);
    if (!model || model->GetMeshCount() == 0) {
        // Missing file OR a file that failed to parse (0 meshes) - never
        // create a broken empty entity.
        EditorConsole::Log("Failed to load model: " + modelPath, 2);
        return ecs::Entity{ecs::INVALID_ENTITY_ID};
    }

    auto e = g_editor.world().createEntityWithComponents<ecs::TransformComponent, ecs::ModelComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_editor.world().getComponentArchetype<ecs::ModelComponent>(e);

    if (t) {
        t->position = pos;
        t->scale = scale;
        t->setEulerAngles(glm::radians(rotation));
    }

    if (m) {
        m->modelHandle = handle;
        m->visible = true;
        m->setModelPath(modelPath);
    }

    UndoRedo::RecordCreate(e.id);
    EditorConsole::Log("Loaded model: " + modelPath + 
                       " | Meshes: " + std::to_string(model->GetMeshCount()) +
                       " | Triangles: " + std::to_string(model->GetTotalTriangleCount()));
    
    return e;
}

ecs::Entity CreateAnimatedModel(const std::string& modelPath, const glm::vec3& pos, const glm::vec3& scale, const glm::vec3& rotation, int animationIndex) {
    auto handle = ModelSystem::ModelRegistry::getInstance().load(modelPath);
    Model* model = ModelSystem::ModelRegistry::getInstance().get(handle);
    if (!model || model->GetMeshCount() == 0) {
        EditorConsole::Log("Failed to load animated model: " + modelPath, 2);
        return ecs::Entity{ecs::INVALID_ENTITY_ID};
    }

    if (model->GetAnimationCount() == 0) {
        EditorConsole::Log("Model has no animations, creating as static model", 1);
        return CreateModel(modelPath, pos, scale, rotation);
    }

    // Create up-front with the animator component (no addComponentArchetype
    // migration) so the returned component pointers stay valid.
    auto e = g_editor.world().createEntityWithComponents<
        ecs::TransformComponent, ecs::ModelComponent, ecs::ModelAnimatorComponent,
        ecs::LayerComponent>();
    if (!e.isValid()) return e;

    auto* t = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e);
    auto* m = g_editor.world().getComponentArchetype<ecs::ModelComponent>(e);
    auto* ac = g_editor.world().getComponentArchetype<ecs::ModelAnimatorComponent>(e);

    if (t) {
        t->position = pos;
        t->scale = Editor::characterParityScale(model, scale);  // parity with the 1.26 m play character
        t->setEulerAngles(glm::radians(rotation));
    }

    if (m) {
        m->modelHandle = handle;
        m->visible = true;
        m->setModelPath(modelPath);
    }

    // Wire the live Animator off the model's skeleton + first animation so
    // ModelRenderSystem::renderAnimated() can skin it via Model::Draw(shader,animator).
    // NOTE: foot IK is intentionally NOT driven for NPCs — animator->Update() only
    // advances bone matrices, so there is no 2nd-skeleton IK crash (IK stays
    // player-only via PlayModeController/UpdateFootIK).
    if (ac && model && model->GetAnimationCount() > 0) {
        ac->initialize(&model->GetSkeleton());
        ac->activeAnimation = (animationIndex >= 0 &&
            animationIndex < static_cast<int>(model->GetAnimationCount()))
            ? animationIndex : 0;
        ac->isPlaying = true;
        ac->loop = true;
    }
    if (auto* lc = g_editor.world().getComponentArchetype<ecs::LayerComponent>(e)) {
        lc->layer = ecs::LayerComponent::Characters;  // skinned NPCs -> Characters layer
    }

    UndoRedo::RecordCreate(e.id);
    EditorConsole::Log("Loaded model: " + modelPath +
                       " | Animations: " + std::to_string(model->GetAnimationCount()));

    return e;
}

/**
 * Spawn an NPC / character that SHARES the play character's full AnimatedCharacter
 * (locomotion FSM + motion-matching + foot IK) instead of a single static-clip
 * Animator. The AnimatedCharacterComponent entity is ticked by
 * ModelRenderSystem::update() every frame (its pose is mirrored into the
 * TransformComponent twin so the selection-driven camera / focus / gizmo can
 * target it) and rendered by ModelRenderSystem::renderAnimated() via the skin
 * shader, mirroring RenderPlayCharacter.
 */
ecs::Entity CreateAnimatedCharacterEntity(const std::string& modelPath,
                                          const std::string& clipsDir,
                                          const glm::vec3& pos,
                                          const glm::vec3& scale,
                                          const glm::vec3& rotation,
                                          const std::string& motionContext) {
    auto handle = ModelSystem::ModelRegistry::getInstance().load(modelPath);
    Model* model = ModelSystem::ModelRegistry::getInstance().get(handle);
    if (!model || model->GetMeshCount() == 0) {
        EditorConsole::Log("Failed to load animated character: " + modelPath, 2);
        return ecs::Entity{ecs::INVALID_ENTITY_ID};
    }

    auto e = g_editor.world().createEntityWithComponents<
        ecs::TransformComponent, ecs::AnimatedCharacterComponent,
        ecs::NameComponent, ecs::LayerComponent>();
    if (!e.isValid()) return e;

    auto* t  = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e);
    auto* ac = g_editor.world().getComponentArchetype<ecs::AnimatedCharacterComponent>(e);
    auto* n  = g_editor.world().getComponentArchetype<ecs::NameComponent>(e);
    auto* lc = g_editor.world().getComponentArchetype<ecs::LayerComponent>(e);

    if (t) {
        t->position = pos;
        t->scale = Editor::characterParityScale(model, scale);  // match the 1.26 m play character
        t->setEulerAngles(glm::radians(rotation));
    }
    if (ac) {
        ac->renderHandle = handle;
        ac->clipsDir = clipsDir;
        ac->motionContext = parseMotionContext(motionContext);
        ac->character = std::make_unique<AnimatedCharacter>();
        if (!ac->character->load(modelPath)) {
            EditorConsole::Log("AnimatedCharacter::load failed: " + modelPath, 2);
            g_editor.world().destroyEntity(e);
            return ecs::Entity{ecs::INVALID_ENTITY_ID};
        }
        ac->character->loadLocomotion(clipsDir);
        ac->character->setMotionContext(ac->motionContext);
        // Match the play character's visual convention (1.8 m authored -> 0.7
        // visualScale -> 1.26 m render). load() already normalized
        // `character->scale` to targetHeight_/rawSizeY_; we keep visualScale.
        ac->character->visualScale = 0.7f;
        ac->character->position = pos;
        ac->character->heading  = glm::radians(rotation.y);
    }
    if (n) n->setName(EntityManager::DeriveEntityNameFromAsset(modelPath));
    if (lc) lc->layer = ecs::LayerComponent::Characters;  // skinned chars -> Characters layer

    UndoRedo::RecordCreate(e.id);
    const size_t bones = (ac && ac->character) ? ac->character->boneCount() : 0;
    EditorConsole::Log("Spawned animated-character NPC: " + modelPath +
                       " | Skeleton bones: " + std::to_string(bones));
    return e;
}

// A (forUI.txt §3 async). Attach a worker-built AnimatedCharacter to a
// pre-registered GL Model without re-parsing. Mirrors CreateAnimatedCharacterEntity
// above; the only differences are the Model (fetched from the registered handle
// via ModelRegistry::get, NOT ModelRegistry::load) and the character (moved in
// pre-built instead of load+loadLocomotion+setMotionContext here). setMotionContext
// is still called on the main thread to match the NPC config's context exactly.
ecs::Entity CreateAnimatedCharacterEntity(const glm::vec3& pos,
                                          const glm::vec3& scale,
                                          const glm::vec3& rotation,
                                          ModelSystem::ModelHandle handle,
                                          std::unique_ptr<AnimatedCharacter> character,
                                          const std::string& modelPath,
                                          const std::string& clipsDir,
                                          const std::string& motionContext) {
    if (!handle.isValid() || !character) {
        EditorConsole::Log("Async NPC handle/character missing", 2);
        return ecs::Entity{ecs::INVALID_ENTITY_ID};
    }
    Model* model = ModelSystem::ModelRegistry::getInstance().get(handle);
    if (!model || model->GetMeshCount() == 0) {
        EditorConsole::Log("Async NPC model invalid: " + modelPath, 2);
        return ecs::Entity{ecs::INVALID_ENTITY_ID};
    }

    auto e = g_editor.world().createEntityWithComponents<
        ecs::TransformComponent, ecs::AnimatedCharacterComponent,
        ecs::NameComponent, ecs::LayerComponent>();
    if (!e.isValid()) return e;

    auto* t  = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e);
    auto* ac = g_editor.world().getComponentArchetype<ecs::AnimatedCharacterComponent>(e);
    auto* n  = g_editor.world().getComponentArchetype<ecs::NameComponent>(e);
    auto* lc = g_editor.world().getComponentArchetype<ecs::LayerComponent>(e);

    const auto ctx = parseMotionContext(motionContext);
    if (t) {
        t->position = pos;
        t->scale    = Editor::characterParityScale(model, scale);  // match 1.26 m parity
        t->setEulerAngles(glm::radians(rotation));
    }
    if (ac) {
        ac->renderHandle  = handle;                       // pre-registered GL model
        ac->clipsDir      = clipsDir;
        ac->motionContext = ctx;
        ac->character     = std::move(character);         // attach pre-built char
        ac->character->setMotionContext(ctx);             // match npc config
        ac->character->visualScale = 0.7f;                // 1.8 m authored -> 1.26 m render
        ac->character->position   = pos;
        ac->character->heading    = glm::radians(rotation.y);
    }
    if (n) n->setName(EntityManager::DeriveEntityNameFromAsset(modelPath));
    if (lc) lc->layer = ecs::LayerComponent::Characters;

    UndoRedo::RecordCreate(e.id);
    const size_t bones = (ac && ac->character) ? ac->character->boneCount() : 0;
    EditorConsole::Log("Spawned (async) NPC '" + modelPath +
                       "' | Skeleton bones: " + std::to_string(bones));
    return e;
}

/**
 * Spawn a character entity from a content-browser selection. Distinguishes
 * between skinned characters (model has a skeleton + animations → full
 * ModelAnimatorComponent + Characters layer) and static character meshes
 * (ModelComponent only, Props layer). The entity name is derived from the
 * file basename so the Outliner shows "Shaw" instead of "Entity 42".
 */
ecs::Entity CreateCharacterEntity(const std::string& modelPath,
                                   const glm::vec3& pos, const glm::vec3& scale,
                                   const glm::vec3& rotation) {
    auto handle = ModelSystem::ModelRegistry::getInstance().load(modelPath);
    Model* model = ModelSystem::ModelRegistry::getInstance().get(handle);
    if (!model || model->GetMeshCount() == 0) {
        EditorConsole::Log("Failed to load character: " + modelPath, 2);
        return ecs::Entity{ecs::INVALID_ENTITY_ID};
    }

    // Decide: skinned character vs. static character mesh.
    const bool hasSkeleton = !model->GetSkeleton().bones.empty();
    const bool hasAnimations = model->GetAnimationCount() > 0;

    ecs::Entity e;
    if (hasSkeleton && hasAnimations) {
        // Full animated character — mirror CreateAnimatedModel but with a
        // descriptive name and the Characters layer.
        e = g_editor.world().createEntityWithComponents<
            ecs::TransformComponent, ecs::ModelComponent, ecs::ModelAnimatorComponent,
            ecs::NameComponent, ecs::LayerComponent>();
        if (!e.isValid()) {
            EditorConsole::Log("Failed to create character entity (no slots)", 2);
            return e;
        }
        auto* t  = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e);
        auto* m  = g_editor.world().getComponentArchetype<ecs::ModelComponent>(e);
        auto* ac = g_editor.world().getComponentArchetype<ecs::ModelAnimatorComponent>(e);
        auto* n  = g_editor.world().getComponentArchetype<ecs::NameComponent>(e);
        auto* lc = g_editor.world().getComponentArchetype<ecs::LayerComponent>(e);

        if (t) { t->position = pos; t->scale = scale; t->setEulerAngles(glm::radians(rotation)); }
        if (m) { m->modelHandle = handle; m->visible = true; m->setModelPath(modelPath); }
        if (ac && model) ac->initialize(&model->GetSkeleton());
        if (n) n->setName(EntityManager::DeriveEntityNameFromAsset(modelPath));
        if (lc) lc->layer = ecs::LayerComponent::Characters;
    } else {
        // Static character mesh — same as CreateModel but with a name + layer.
        e = g_editor.world().createEntityWithComponents<
            ecs::TransformComponent, ecs::ModelComponent,
            ecs::NameComponent, ecs::LayerComponent>();
        if (!e.isValid()) {
            EditorConsole::Log("Failed to create character entity (no slots)", 2);
            return e;
        }
        auto* t  = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e);
        auto* m  = g_editor.world().getComponentArchetype<ecs::ModelComponent>(e);
        auto* n  = g_editor.world().getComponentArchetype<ecs::NameComponent>(e);
        auto* lc = g_editor.world().getComponentArchetype<ecs::LayerComponent>(e);

        if (t) { t->position = pos; t->scale = scale; t->setEulerAngles(glm::radians(rotation)); }
        if (m) { m->modelHandle = handle; m->visible = true; m->setModelPath(modelPath); }
        if (n) n->setName(EntityManager::DeriveEntityNameFromAsset(modelPath));
        if (lc) lc->layer = ecs::LayerComponent::Characters;
    }

    UndoRedo::RecordCreate(e.id);
    EditorConsole::Log("Spawned character: " + modelPath +
                       " | Meshes: " + std::to_string(model->GetMeshCount()) +
                       " | Animations: " + std::to_string(model->GetAnimationCount()) +
                       " | Skeleton: " + std::string(model->GetSkeleton().bones.empty() ? "no" : "yes"));
    return e;
}

ecs::Entity CreatePrimitive(ecs::MeshType type, const glm::vec3& pos) {
    auto e = g_editor.world().createEntityWithComponents<ecs::TransformComponent, ecs::MeshComponent, ecs::LayerComponent>();
    if (!e.isValid()) return e;

    if (auto* t = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e)) {
        t->position = pos;
    }
    if (auto* m = g_editor.world().getComponentArchetype<ecs::MeshComponent>(e)) {
        m->meshType = type;
        m->visible = true;
    }
    UndoRedo::RecordCreate(e.id);
    EditorConsole::Log("Created primitive entity " + std::to_string(e.id));
    if (auto* lc = g_editor.world().getComponentArchetype<ecs::LayerComponent>(e)) {
        lc->layer = ecs::LayerComponent::Props;   // spawned primitives -> Props layer
    }
    return e;
}

/**
 * Spawn a geospatial "AssetKind mesh twin" - an ECS entity that is BOTH
 * renderable (transform) and tracked by the geo pipeline (GeospatialComponent
 * with an AssetKind classification + PredictionComponent). Registered with the
 * GeospatialSystem so it streams into the dashboard physics[] panel, the mine-
 * safety geofence, and the 3D prediction cone - i.e. it is no longer a stub.
 *
 * lat/lon/alt seed the twin's WGS84 position (origin metres for altitude are
 * interpreted by the GeospatialConverter). External sim data later updates it
 * by entityId (entity_<id>) via the REST/ingest path.
 */
ecs::Entity CreateGeospatialTwin(geo::AssetKind kind, const glm::vec3& pos,
                                 double lat, double lon, double alt) {
    // Create the twin with ALL components up front (mesh + name included). We
    // deliberately do NOT call addComponentArchetype after creation: in the
    // archetype ECS that moves the entity to a new pool, which invalidates every
    // previously-fetched component pointer and makes registerEntity store a
    // dangling geo pointer (crash on the first render / entityId assignment).
    // Building the full component set in one shot yields stable pointers for the
    // twin's whole lifetime.
    // Personnel geo twin is an animated NPC (AnimatedCharacterComponent ->
    // Shaw.fbx + locomotion clips), so it is spawned with that extra component
    // up front. In the archetype ECS adding a component after creation repools
    // the entity and invalidates every previously-fetched component pointer
    // (dangling refs into registerEntity / parity). Building the full component
    // set per kind yields stable pointers for the twin's lifetime. Non-personnel
    // kinds stay static (ModelComponent only) and keep the parity-scaled mesh.
    const bool isAnimatedNpc = (kind == geo::AssetKind::Personnel);
    auto e = isAnimatedNpc
        ? g_editor.world().createEntityWithComponents<
              ecs::TransformComponent, ecs::GeospatialComponent,
              ecs::PredictionComponent, ecs::ModelComponent,
              ecs::AnimatedCharacterComponent, ecs::NameComponent,
              ecs::LayerComponent>()
        : g_editor.world().createEntityWithComponents<
              ecs::TransformComponent, ecs::GeospatialComponent,
              ecs::PredictionComponent, ecs::ModelComponent,
              ecs::NameComponent, ecs::LayerComponent>();
    if (!e.isValid()) return e;

    auto* t  = g_editor.world().getComponentArchetype<ecs::TransformComponent>(e);
    auto* g  = g_editor.world().getComponentArchetype<ecs::GeospatialComponent>(e);
    auto* p  = g_editor.world().getComponentArchetype<ecs::PredictionComponent>(e);
    auto* mc = g_editor.world().getComponentArchetype<ecs::ModelComponent>(e);
    auto* ac = isAnimatedNpc
        ? g_editor.world().getComponentArchetype<ecs::AnimatedCharacterComponent>(e)
        : nullptr;

    if (t) {
        t->position = pos;
        t->scale = glm::vec3(1.0f);
        t->rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    if (g) {
        g->latitude = lat;
        g->longitude = lon;
        g->altitude = alt;
        g->assetKind = static_cast<int>(kind);
    }
    if (p) {
        p->confidence = 0.0f;
    }
    if (mc) {
        // Bind the canonical 3D asset for this AssetKind (excavator.fbx for
        // machinery, utility_car.fbx for vehicles, sensor_node.fbx for sensors,
        // bot321.fbx for personnel) instead of a coloured Cube primitive. Static
        // FBX (no skeleton) keeps leg-IK out of it (IK is play-character only).
        // resolvePlayCharacterMeshPath always returns a path so the twin renders
        // *something*; if that asset is missing on disk, fall back to the humanoid
        // mesh so it still renders as a 3D mesh rather than degrading to a dot.
        const std::string meshPath = Editor::resolvePlayCharacterMeshPath("", kind);
        mc->setModelPath(meshPath);
        mc->modelHandle = ModelSystem::ModelRegistry::getInstance().load(meshPath);
        if (!mc->isValid()) {
            const std::string fbMesh = std::string(Editor::fallbackMeshPath());
            mc->setModelPath(fbMesh);
            mc->modelHandle = ModelSystem::ModelRegistry::getInstance().load(fbMesh);
        }
        mc->visible = true;
        mc->castShadow = true;
        mc->receiveShadow = true;
    }

    // Personnel geo twin -> animated NPC. Bind Shaw.fbx (skeleton + 1960-clip
    // locomotion pack) so the twin is tracked by geospatial telemetry and
    // animates idle/walk/run instead of the T-pose static bot. ModelComponent
    // (bot.fbx, set above) is kept only as the GeoViz point-marker gate;
    // renderAnimated() draws the skinned Shaw mesh via ch->modelMatrix(), which
    // already bakes 1.26 m parity (targetHeight 1.8 * visualScale 0.7), matching
    // the play bot exactly.
    if (ac) {
        const std::string charModel = "assets/Male Locomotion Pack/Shaw.fbx";
        ac->renderHandle = ModelSystem::ModelRegistry::getInstance().load(charModel);
        // Point the GeoViz gate ModelComponent at the SAME Shaw.fbx the AnimatedCharacter
        // renders, so the bbox-based camera torso focus (focusFromEntity) and any
        // GeoViz mesh marker match the skinned mesh, not the bot.fbx stand-in.
        if (mc) {
            mc->setModelPath(charModel);
            mc->modelHandle = ac->renderHandle;
        }
        ac->clipsDir = "assets";
        ac->character = std::make_unique<AnimatedCharacter>();
        if (ac->character->load(charModel)) {
            ac->character->loadLocomotion("assets");
            ac->character->setMotionContext(AnimatedCharacter::MotionContext::LOCOMOTION);
            ac->character->visualScale = 0.7f;   // 1.8 m authored -> 1.26 m render
            ac->character->position = pos;
            ac->character->heading = 0.0f;
            ac->character->velocity = glm::vec3(0.0f);
            ac->geoDriven = false; // enabled by GeospatialSystem feed / sim drive
        } else {
            EditorConsole::Log("GeoPersonnel: AnimatedCharacter failed to load "
                               + charModel, 2);
            ac->character.reset();
        }
    }

    // ---- Visual scale parity with the play bot (see characterParityScale) ----
    // The animated play character normalizes its authored height to
    // targetHeight(1.8)*visualScale(0.7) = 1.26 m on load, so Shaw.fbx always
    // renders at 1.26 m. AnimatedCharacterComponent geo Personnel twins match
    // (visualScale 0.7, set above); renderAnimated() draws them via
    // ch->modelMatrix() so the ECS transform scale is unused for those. Static
    // geo twins (heavy machinery, vehicles, sensors) get the SAME 1.26 m
    // normalization + AssetKind multiplier applied to the authored mesh, else
    // they drift to raw FBX meters (excavator.fbx is huge) while the play bot
    // stays 1.26 m — the "huge scale difference" bug. Same forward skin-shader
    // path (getModelShader); no forward/deferred buffer split.
    if (t) {
        if (ac && ac->character && ac->character->ready()) {
            // Animated NPC: character owns scale (1.26 m via visualScale).
            t->scale = glm::vec3(1.0f);
            std::cout << "[GeoTwin] entity " << std::to_string(e.id)
                      << " personnel (animated NPC) Shaw.fbx @ 1.26 m, idle\n";
        } else if (mc && mc->isValid()) {
            Model* model = mc->getModel();
            const glm::vec3 parityScale = Editor::characterParityScale(model, glm::vec3(1.0f));
            const float kindMult = Editor::assetKindSizeMultiplier(kind);
            t->scale = parityScale * kindMult;
            std::cout << "[GeoTwin] entity " << std::to_string(e.id)
                      << " " << std::string(geo::AssetKindToString(kind))
                      << " mesh=" << mc->getModelPath()
                      << " parityScale=" << parityScale.x
                      << " kindMult=" << kindMult
                      << " -> transform.scale=" << t->scale.x << "\n";
        }
        t->markDirty();  // flush cached model matrix so parity scale renders frame 1
    }

    if (auto* lc = g_editor.world().getComponentArchetype<ecs::LayerComponent>(e)) {
        lc->layer = ecs::LayerComponent::GeoTracked;  // geo twins -> Geo Tracked layer
    }

    // Register with the geo orchestrator: store/observe/predict + dashboard
    // physics[] + geofence + prediction cone. registerEntity also stamps
    // geo->entityId = "entity_<id>" and registers the visualization.
    g_editor.geospatialSystem().registerEntity(e.id, g, t, p, mc, ac);

    if (auto* n = g_editor.world().getComponentArchetype<ecs::NameComponent>(e)) {
        n->setName(std::string(geo::AssetKindToString(kind)) +
                   " twin " + std::to_string(e.id));
    }

    // New geo twin -> outliner must re-index so the asset-kind twin appears as a
    // distinct, named row (previously the cache stayed stale and the machinery +
    // personnel twins showed up as a single "None" object, so the gizmo / Details
    // panel never saw them). Mirrors ui.cpp's other create/delete paths.
    g_editor.entityCache().dirty = true;

    UndoRedo::RecordCreate(e.id);
    EditorConsole::Log("Created geo twin (" +
                       std::string(geo::AssetKindToString(kind)) + "): " +
                       std::to_string(e.id));
    return e;
}

bool CreateBlueprintFromSelected(ecs::EntityID id, const std::string& name) {
    if (!EntityExists(id)) return false;

    ecs::World& world = g_editor.world();
    ecs::Entity ent{id};
    auto blueprint = std::make_unique<ecs::Blueprint>(name);
    ecs::BlueprintEntity be;
    be.name = GetEntityName(id);

    if (auto* t = world.getComponentArchetype<ecs::TransformComponent>(ent)) {
        ecs::ComponentOverride co;
        co.typeID = ecs::getComponentTypeID<ecs::TransformComponent>();
        co.componentName = "TransformComponent";
        co.data["position"][0] = t->position.x;
        co.data["position"][1] = t->position.y;
        co.data["position"][2] = t->position.z;
        co.data["scale"][0] = t->scale.x;
        co.data["scale"][1] = t->scale.y;
        co.data["scale"][2] = t->scale.z;
        co.data["rotation"][0] = t->rotation.x;
        co.data["rotation"][1] = t->rotation.y;
        co.data["rotation"][2] = t->rotation.z;
        co.data["rotation"][3] = t->rotation.w;
        be.componentOverrides.push_back(std::move(co));
    }

    if (auto* n = world.getComponentArchetype<ecs::NameComponent>(ent)) {
        ecs::ComponentOverride co;
        co.typeID = ecs::getComponentTypeID<ecs::NameComponent>();
        co.componentName = "NameComponent";
        co.data["name"] = n->getName();
        be.componentOverrides.push_back(std::move(co));
    }

    blueprint->addEntity(be);
    world.registerBlueprint(std::move(blueprint));
    EditorConsole::Log("Created blueprint '" + name + "' from entity " + std::to_string(id));
    return true;
}

ecs::EntityID SpawnBlueprint(const std::string& name, const glm::vec3& pos) {
    ecs::World& world = g_editor.world();
    ecs::Blueprint* bp = world.getBlueprint(name);
    if (!bp) return ecs::INVALID_ENTITY_ID;

    const auto& entities = bp->getEntities();
    if (entities.empty()) return ecs::INVALID_ENTITY_ID;
    const ecs::BlueprintEntity& be = entities[0];

    ecs::Entity e = world.createEntityWithComponents<ecs::TransformComponent, ecs::NameComponent>();
    if (!e.isValid()) return ecs::INVALID_ENTITY_ID;

    if (auto* t = world.getComponentArchetype<ecs::TransformComponent>(e)) {
        for (const auto& co : be.componentOverrides) {
            if (co.typeID != ecs::getComponentTypeID<ecs::TransformComponent>()) continue;
            if (co.data.isMember("position")) {
                t->position = glm::vec3(co.data["position"][0].asFloat(),
                                        co.data["position"][1].asFloat(),
                                        co.data["position"][2].asFloat());
            }
            if (co.data.isMember("scale")) {
                t->scale = glm::vec3(co.data["scale"][0].asFloat(),
                                     co.data["scale"][1].asFloat(),
                                     co.data["scale"][2].asFloat());
            }
            if (co.data.isMember("rotation")) {
                t->rotation = glm::quat(co.data["rotation"][3].asFloat(),
                                        co.data["rotation"][0].asFloat(),
                                        co.data["rotation"][1].asFloat(),
                                        co.data["rotation"][2].asFloat());
            }
        }
        t->position += pos;  // spawn offset
    }

    if (auto* n = world.getComponentArchetype<ecs::NameComponent>(e)) {
        for (const auto& co : be.componentOverrides) {
            if (co.typeID == ecs::getComponentTypeID<ecs::NameComponent>() && co.data.isMember("name")) {
                n->setName(co.data["name"].asString());
                break;
            }
        }
    }

    EditorConsole::Log("Spawned blueprint '" + name + "' -> entity " + std::to_string(e.id));
    return e.id;
}

} // namespace EntityManager
