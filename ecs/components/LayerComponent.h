#pragma once

#include <cstdint>

namespace ecs {

/**
 * LayerComponent — scene‑layer membership for an entity.
 *
 * The editor owns eight named layers driven from the Layers panel
 * (Editor::UIState::layerVisible[8]). RenderSystem / ModelRenderSystem read
 * this component to cull an entity when its layer is hidden, so toggling a
 * layer in the UI actually hides that layer's primitives, NPCs and
 * geospatial twins.
 *
 * Entities without a LayerComponent fall back to the Default layer (0),
 * which is always visible, so legacy / external entities keep rendering and
 * nothing regresses.
 *
 * Layer indices mirror the editor Layers panel (editor/ui.cpp):
 *   0 Default, 1 Environment, 2 Characters, 3 Props, 4 Effects,
 *   5 Geo Tracked, 6 UI, 7 Hidden
 */
struct LayerComponent {
    int layer = 0;
    static constexpr int kMaxLayers = 8;

    static constexpr int Default      = 0;
    static constexpr int Environment   = 1;
    static constexpr int Characters    = 2;
    static constexpr int Props         = 3;
    static constexpr int Effects       = 4;
    static constexpr int GeoTracked    = 5;
    static constexpr int UI            = 6;
    static constexpr int Hidden        = 7;
};

} // namespace ecs
