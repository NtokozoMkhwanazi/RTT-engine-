#pragma once
// ============================================================================
// ui_batch_processor.h  --  forUI.txt Part 2: Multi-Object Batch Modification
// ----------------------------------------------------------------------------
// The details panel must edit several entities at once without clobbering each
// entity's *unique* offset. This is the "Multi-Object Value Reflector" the
// forUI.txt Part 2 blueprint proposed (UI_BatchProcessor.h) -- the file that was
// missing from the codebase until now.
//
// Three passes per field, exactly per the forUI.txt Part 2 strategy:
//   Pass 1 — query the active selection; confirm each target owns the component.
//   Pass 2 — compare values across the array; if any element varies by more
//            than the tolerance epsilon (kBatchEpsilon) activate the
//            indeterminate "[Mixed Values]" display (relative-delta edits so
//            per-entity offsets are preserved).
//   Pass 3 — on a committed ImGui edit, propagate an ABSOLUTE override (uniform)
//            or a RELATIVE delta (mixed) across every batched reference.
//
// Header-only: lives in the UI namespace and is consumed by editor/ui.cpp's
// RenderRightPanel multi-selection branch. Only entities that actually own the
// targeted component are read/written; the rest are skipped so a heterogeneous
// selection degrades gracefully instead of crashing.
//
// NOTE on member pointers: forUI.txt's Part 2 integration example referenced
//   &ecs::TransformComponent::position.x
// but a pointer-to-*nested-sub-member* is ill-formed C++, so the transform
// position/scale are edited as whole glm::vec3 members (the faithful,
// compiling equivalent). Rotation is stored as a quat and edited as Euler
// angles; equality is tested on the quaternion (dot-product) to dodge gimbal
// ambiguity while the slider still drives degrees.
// ============================================================================
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>      // glm::value_ptr for DragFloat3/4
#include <glm/gtc/quaternion.hpp>    // glm::eulerAngles, glm::quat(vec3)
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <cstring>
#include "ecs/ECS.h"
#include "ecs/components/Components.h"

namespace UI {

// Tolerance for "are these two values equal across the selection?"
// (forUI.txt Part 2 §1 explicitly calls out a tolerance epsilon -- using exact
// `==` on floats would almost always read "mixed" for transforms authored at
// different times, which is correct behaviour, but breaks batch-edit of
// genuinely-co-located props snapped to the same rounded value).
inline constexpr float kBatchEpsilon = 1e-4f;

namespace detail {

// Scalar / integral equality with an epsilon for floating point.
template <typename T>
inline bool batchEqual(const T& a, const T& b) {
    if constexpr (std::is_floating_point_v<T>) {
        return std::fabs(a - b) <= kBatchEpsilon;
    } else {
        return a == b;
    }
}

// glm::vec3 overload (component-wise).
inline bool batchEqual(const glm::vec3& a, const glm::vec3& b) {
    return batchEqual(a.x, b.x) && batchEqual(a.y, b.y) && batchEqual(a.z, b.z);
}

// Fetch a component for an entity id, null-safe (invalid ids / missing
// components return nullptr so callers can `if (c)`-guard inline).
template <typename C>
inline C* batchGet(ecs::World& world, ecs::EntityID id) {
    if (id == ecs::INVALID_ENTITY_ID) return nullptr;
    return world.getComponentArchetype<C>(id);
}

} // namespace detail

// ----------------------------------------------------------------------------
// RenderBatchPropertyField  --  scalar field (forUI.txt Part 2 blueprint shape)
// ----------------------------------------------------------------------------
// Faithful to the forUI.txt Part 2 blueprint: reads the primary entity's value,
// scans the selection for variation, then either edits a uniform value
// (absolute override) or edits the primary and propagates a relative delta.
//
// Works for any *direct* scalar member of ComponentType, e.g.
//     &ecs::MeshComponent::metallic
// (a pointer-to-nested-sub-member like &TransformComponent::position.x is
// ill-formed C++, so the vec3 overload below is used for transform vectors).
// ----------------------------------------------------------------------------
template <typename ComponentType, typename ValueType>
void RenderBatchPropertyField(
    const std::vector<ecs::EntityID>& selection,
    ecs::World& world,
    const char* label,
    ValueType (ComponentType::*propertyField),
    float speed, float minVal, float maxVal, const char* format)
{
    if (selection.empty()) return;

    ComponentType* firstComp = detail::batchGet<ComponentType>(world, selection[0]);
    if (!firstComp) return;

    const ValueType referenceValue = firstComp->*propertyField;

    // Pass 2: analyze whether values are mixed across the selection array.
    bool mixedValues = false;
    for (size_t i = 1; i < selection.size(); ++i) {
        ComponentType* comp = detail::batchGet<ComponentType>(world, selection[i]);
        if (!comp) continue;
        if (!detail::batchEqual(comp->*propertyField, referenceValue)) {
            mixedValues = true;
            break;
        }
    }

    // Assign unique text ids based on mixed matrix states (matches the
    // blueprint's "##batch_<label>" id; the visible label is drawn separately).
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);

    char labelId[128];
    std::snprintf(labelId, sizeof(labelId), "##batch_%s", label);

    // Shared mutator that marks a TransformComponent dirty after a write.
    auto markDirtyIfTransform = [](ComponentType* c) {
        if constexpr (std::is_same_v<ComponentType, ecs::TransformComponent>)
            c->markDirty();
    };

    if (mixedValues) {
        // Display indeterminate state with a clear visual style hint. Editing
        // the primary value emits a *relative* delta so every entity's unique
        // offset is preserved.
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.40f, 0.26f, 0.14f, 1.0f));
        ValueType tempValue = referenceValue;
        if (ImGui::DragFloat(labelId, &tempValue, speed, minVal, maxVal, "[Mixed Values]")) {
            if constexpr (std::is_floating_point_v<ValueType>) {
                const ValueType deltaValue = tempValue - referenceValue;
                for (ecs::EntityID id : selection) {
                    ComponentType* comp = detail::batchGet<ComponentType>(world, id);
                    if (comp) {
                        comp->*propertyField += deltaValue;
                        markDirtyIfTransform(comp);
                    }
                }
            } else {
                // Non-float scalar fallback: absolute override across the batch.
                for (ecs::EntityID id : selection) {
                    ComponentType* comp = detail::batchGet<ComponentType>(world, id);
                    if (comp) {
                        comp->*propertyField = tempValue;
                        markDirtyIfTransform(comp);
                    }
                }
            }
        }
        ImGui::PopStyleColor();
    } else {
        // Values uniform across the batch: render a standard direct slider.
        ValueType currentValue = referenceValue;
        if (ImGui::DragFloat(labelId, &currentValue, speed, minVal, maxVal, format)) {
            for (ecs::EntityID id : selection) {
                ComponentType* comp = detail::batchGet<ComponentType>(world, id);
                if (comp) {
                    comp->*propertyField = currentValue;
                    markDirtyIfTransform(comp);
                }
            }
        }
    }
}

// ----------------------------------------------------------------------------
// RenderBatchPropertyFieldVec3  --  vec3 member (DragFloat3)
// ----------------------------------------------------------------------------
// Edits a whole glm::vec3 member (e.g. TransformComponent::position / scale).
// Used for the transform Location/Scale rows and any vec3 property; the
// PositionX/PositionY/PositionZ split from the Part 2 integration example is
// collapsed into a single DragFloat3 for a cleaner batch UX while keeping the
// identical mixed-value / relative-delta semantics.
// ----------------------------------------------------------------------------
template <typename ComponentType>
void RenderBatchPropertyFieldVec3(
    const std::vector<ecs::EntityID>& selection,
    ecs::World& world,
    const char* label,
    glm::vec3 (ComponentType::*propertyField),
    float speed, float minVal, float maxVal, const char* format)
{
    if (selection.empty()) return;

    ComponentType* firstComp = detail::batchGet<ComponentType>(world, selection[0]);
    if (!firstComp) return;

    const glm::vec3 referenceValue = firstComp->*propertyField;

    bool mixedValues = false;
    for (size_t i = 1; i < selection.size(); ++i) {
        ComponentType* comp = detail::batchGet<ComponentType>(world, selection[i]);
        if (!comp) continue;
        if (!detail::batchEqual(comp->*propertyField, referenceValue)) {
            mixedValues = true;
            break;
        }
    }

    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);

    char labelId[128];
    std::snprintf(labelId, sizeof(labelId), "##batch_vec3_%s", label);

    auto markDirtyIfTransform = [](ComponentType* c) {
        if constexpr (std::is_same_v<ComponentType, ecs::TransformComponent>)
            c->markDirty();
    };

    if (mixedValues) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.40f, 0.26f, 0.14f, 1.0f));
        glm::vec3 tempValue = referenceValue;
        if (ImGui::DragFloat3(labelId, glm::value_ptr(tempValue), speed, minVal, maxVal, "[Mixed Values]")) {
            const glm::vec3 deltaValue = tempValue - referenceValue;
            for (ecs::EntityID id : selection) {
                ComponentType* comp = detail::batchGet<ComponentType>(world, id);
                if (comp) {
                    comp->*propertyField += deltaValue;
                    markDirtyIfTransform(comp);
                }
            }
        }
        ImGui::PopStyleColor();
    } else {
        glm::vec3 currentValue = referenceValue;
        if (ImGui::DragFloat3(labelId, glm::value_ptr(currentValue), speed, minVal, maxVal, format)) {
            for (ecs::EntityID id : selection) {
                ComponentType* comp = detail::batchGet<ComponentType>(world, id);
                if (comp) {
                    comp->*propertyField = currentValue;
                    markDirtyIfTransform(comp);
                }
            }
        }
    }
}

// ----------------------------------------------------------------------------
// RenderBatchBoolField  --  boolean member (Checkbox)
// ----------------------------------------------------------------------------
// Uniform: bound checkbox, absolute write on toggle. Mixed: amber-tinted
// checkbox bound to the primary entity's value (toggling writes the absolute
// value to every selected owner), with an inline "[Mixed Values]" indicator so
// the artist knows the batch is heterogeneous.
// ----------------------------------------------------------------------------
template <typename ComponentType>
void RenderBatchBoolField(
    const std::vector<ecs::EntityID>& selection,
    ecs::World& world,
    const char* label,
    bool (ComponentType::*propertyField))
{
    if (selection.empty()) return;

    ComponentType* firstComp = detail::batchGet<ComponentType>(world, selection[0]);
    if (!firstComp) return;

    bool referenceValue = firstComp->*propertyField;

    bool mixedValues = false;
    for (size_t i = 1; i < selection.size(); ++i) {
        ComponentType* comp = detail::batchGet<ComponentType>(world, selection[i]);
        if (!comp) continue;
        if (!detail::batchEqual(comp->*propertyField, referenceValue)) {
            mixedValues = true;
            break;
        }
    }

    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);

    char labelId[128];
    std::snprintf(labelId, sizeof(labelId), "##batch_bool_%s", label);

    if (mixedValues) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
        bool changed = ImGui::Checkbox(labelId, &referenceValue);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "[Mixed Values]");
        if (changed) {
            for (ecs::EntityID id : selection) {
                ComponentType* comp = detail::batchGet<ComponentType>(world, id);
                if (comp) comp->*propertyField = referenceValue;
            }
        }
    } else {
        if (ImGui::Checkbox(labelId, &referenceValue)) {
            for (ecs::EntityID id : selection) {
                ComponentType* comp = detail::batchGet<ComponentType>(world, id);
                if (comp) comp->*propertyField = referenceValue;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// RenderBatchPropertyFieldColor  --  vec3 color (ColorEdit3)
// ----------------------------------------------------------------------------
// Batch-editable albedo/override color. Mixed -> tinted frame, still edits the
// primary with a relative delta so all objects shift hue together.
// ----------------------------------------------------------------------------
template <typename ComponentType>
void RenderBatchPropertyFieldColor(
    const std::vector<ecs::EntityID>& selection,
    ecs::World& world,
    const char* label,
    glm::vec3 (ComponentType::*propertyField),
    const char* floatFormat = "%.3f")
{
    if (selection.empty()) return;

    ComponentType* firstComp = detail::batchGet<ComponentType>(world, selection[0]);
    if (!firstComp) return;

    const glm::vec3 referenceValue = firstComp->*propertyField;

    bool mixedValues = false;
    for (size_t i = 1; i < selection.size(); ++i) {
        ComponentType* comp = detail::batchGet<ComponentType>(world, selection[i]);
        if (!comp) continue;
        if (!detail::batchEqual(comp->*propertyField, referenceValue)) {
            mixedValues = true;
            break;
        }
    }

    char labelId[128];
    std::snprintf(labelId, sizeof(labelId), "##batch_color_%s", label);

    if (mixedValues) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.40f, 0.26f, 0.14f, 1.0f));
        glm::vec3 tempValue = referenceValue;
        if (ImGui::ColorEdit3(labelId, glm::value_ptr(tempValue),
                              ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB)) {
            const glm::vec3 deltaValue = tempValue - referenceValue;
            for (ecs::EntityID id : selection) {
                ComponentType* comp = detail::batchGet<ComponentType>(world, id);
                if (comp) comp->*propertyField += deltaValue;
            }
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "[Mixed Values]");
    } else {
        glm::vec3 currentValue = referenceValue;
        if (ImGui::ColorEdit3(labelId, glm::value_ptr(currentValue),
                              ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB)) {
            for (ecs::EntityID id : selection) {
                ComponentType* comp = detail::batchGet<ComponentType>(world, id);
                if (comp) comp->*propertyField = currentValue;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// RenderBatchRotationField  --  quaternion rotation edited as Euler degrees
// ----------------------------------------------------------------------------
// Equality is tested on the quaternion (dot product > ~cos(0.5deg)) so that
// gimbal-ambiguous but equivalent rotations are NOT reported as "mixed"; the
// slider still drives degrees for artist familiarity.
// ----------------------------------------------------------------------------
inline void RenderBatchRotationField(
    const std::vector<ecs::EntityID>& selection,
    ecs::World& world)
{
    // Resolve the primary entity's Euler (degrees) for the reference value.
    ecs::TransformComponent* firstComp = detail::batchGet<ecs::TransformComponent>(world, selection[0]);
    if (!firstComp) return;

    const glm::vec3 referenceEuler = glm::degrees(glm::eulerAngles(firstComp->rotation));

    // Pass 2: mixed if any quaternion disagrees with the primary's orientation.
    // fabs(dot) handles the q/-q double-cover (both represent the same rotation);
    // anything below ~cos(0.5deg) is treated as a distinct orientation.
    constexpr float kSameOrientation = 0.9999f;
    bool mixedValues = false;
    for (size_t i = 1; i < selection.size(); ++i) {
        ecs::TransformComponent* comp = detail::batchGet<ecs::TransformComponent>(world, selection[i]);
        if (!comp) continue;
        const float alignment = std::fabs(glm::dot(comp->rotation, firstComp->rotation));
        if (alignment < kSameOrientation) {
            mixedValues = true;
            break;
        }
    }

    ImGui::TextUnformatted("Rotation");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    char labelId[128];
    std::snprintf(labelId, sizeof(labelId), "##batch_Rotation");

    if (mixedValues) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.40f, 0.26f, 0.14f, 1.0f));
        glm::vec3 tempValue = referenceEuler;
        if (ImGui::DragFloat3(labelId, glm::value_ptr(tempValue), 0.5f, -360.0f, 360.0f, "[Mixed Values]")) {
            const glm::vec3 deltaValue = tempValue - referenceEuler;
            for (ecs::EntityID id : selection) {
                ecs::TransformComponent* comp = detail::batchGet<ecs::TransformComponent>(world, id);
                if (comp) {
                    glm::vec3 e = glm::degrees(glm::eulerAngles(comp->rotation)) + deltaValue;
                    comp->setEulerAngles(glm::radians(e));
                }
            }
        }
        ImGui::PopStyleColor();
    } else {
        glm::vec3 currentValue = referenceEuler;
        if (ImGui::DragFloat3(labelId, glm::value_ptr(currentValue), 0.5f, -360.0f, 360.0f, "%.1f deg")) {
            for (ecs::EntityID id : selection) {
                ecs::TransformComponent* comp = detail::batchGet<ecs::TransformComponent>(world, id);
                if (comp) {
                    comp->setEulerAngles(glm::radians(currentValue));
                }
            }
        }
    }
}

// ----------------------------------------------------------------------------
// RenderBatchTransformSection  --  Location / Rotation / Scale (position,
// rotation, scale) as a single collapsible block driven by a std::vector of
// EntityIDs. Replaces the per-entity RenderTransformSection for multi-select.
// ----------------------------------------------------------------------------
inline void RenderBatchTransformSection(
    const std::vector<ecs::EntityID>& selection,
    ecs::World& world)
{
    if (selection.empty()) return;

    // Only render the block if at least one selected entity owns a transform.
    bool hasAny = false;
    for (ecs::EntityID id : selection) {
        if (detail::batchGet<ecs::TransformComponent>(world, id)) {
            hasAny = true;
            break;
        }
    }
    if (!hasAny) return;

    if (!ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::PushID("BatchTransform");
    RenderBatchPropertyFieldVec3<ecs::TransformComponent>(
        selection, world, "Location", &ecs::TransformComponent::position,
        0.1f, -100000.0f, 100000.0f, "%.2f m");
    RenderBatchRotationField(selection, world);
    RenderBatchPropertyFieldVec3<ecs::TransformComponent>(
        selection, world, "Scale", &ecs::TransformComponent::scale,
        0.01f, 0.001f, 100000.0f, "%.3f");
    ImGui::PopID();
}

} // namespace UI
