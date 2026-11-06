#pragma once

/**
 * GeoVisualizationSystem - Handles geospatial visualization (Phase 4)
 *
 * Dedicated system for rendering geospatial data:
 * - Trajectory lines (history + predictions)
 * - Heatmaps for density/uncertainty
 * - Uncertainty ellipses
 * - Entity markers via point renderer
 * - Runs rendering on main thread (OpenGL requirement)
 */

#include "../ECS.h"
#include "../components/GeospatialComponent.h"
#include "../components/PredictionComponent.h"
#include "../components/TransformComponent.h"
#include "../../renderer/GeospatialVisualizer.h"
#include "../../renderer/GeospatialPointRenderer.h"
#include "../../geospatial/GeospatialConverter.h"
#include <glm/glm.hpp>
#include <vector>
#include <map>
#include <mutex>
#include <iostream>
#include "../components/ModelComponent.h"   // ModelComponent* in GeoVisualEntity (fallback gating)

namespace ecs {

struct GeoVisualEntity {
    EntityID id;
    GeospatialComponent* geo = nullptr;
    TransformComponent* transform = nullptr;
    PredictionComponent* prediction = nullptr;
    ModelComponent* model = nullptr;   // asset mesh (nullptr/invalid => point-marker fallback)
    bool isActive = false;
};

class GeoVisualizationSystem : public TypedSystem<GeospatialComponent, TransformComponent> {
public:
    GeoVisualizationSystem() {}

    ~GeoVisualizationSystem() = default;

    /**
     * Initialize visualizer
     */
    void init() override {
        visualizer.initialize();
        std::cout << "[GeoVisualizationSystem] Initialized\n";
    }

    /**
     * Configure which geospatial overlays the viewport draws. Mirrors the Geo
     * panel checkboxes (geo_panel.cfg: showPoints/showTrajectory/showPrediction/
     * showUncertainty/showMonteCarlo). Pushing BEFORE the ECS tick (world.update)
     * makes the change take effect this frame; otherwise it lags by one tick.
     * With Prediction + Monte Carlo + Uncertainty OFF, the viewport shows ONLY
     * the raw-telemetry twin meshes at their true transform positions, so a
     * layman reads A↔B proximity without the Kalman/ML cone dragging the eye.
     */
    void setToggles(bool points, bool trajectory, bool prediction,
                    bool uncertainty, bool monteCarlo) {
        m_toggles = GeoVizToggles{points, trajectory, prediction, uncertainty, monteCarlo};
    }

    /**
     * Bind the WGS84->local ENU converter so prediction cones are drawn in LOCAL
     * camera space. Plotting raw lat/lon as world meters placed cones ~100s of
     * meters from the twin mesh, making it LOOK like the entity moved with its
     * own prediction. Set once from GeospatialSystem::initialize.
     */
    void setConverter(const GeospatialConverter* c) { m_converter = c; }

    /**
     * Register entity for visualization
     */
    void registerEntity(EntityID id, GeospatialComponent* geo, TransformComponent* transform,
                         PredictionComponent* prediction = nullptr,
                         ModelComponent* model = nullptr) {
        if (!geo || !transform) return;

        GeoVisualEntity entity;
        entity.id = id;
        entity.geo = geo;
        entity.transform = transform;
        entity.prediction = prediction;
        entity.model = model;
        entity.isActive = true;

        std::lock_guard<std::mutex> lock(entitiesMutex);
        visualEntities.push_back(entity);
    }

    /**
     * Unregister entity
     */
    void unregisterEntity(EntityID id) {
        std::lock_guard<std::mutex> lock(entitiesMutex);
        visualEntities.erase(
            std::remove_if(visualEntities.begin(), visualEntities.end(),
                [id](const GeoVisualEntity& e) { return e.id == id; }),
            visualEntities.end());
    }

    /**
     * Main update - renders all geospatial visualizations
     * Must run on main thread (OpenGL context)
     */
    void update(float dt) override {
        std::lock_guard<std::mutex> lock(entitiesMutex);
        (void)dt;

        for (auto& entity : visualEntities) {
            if (!entity.isActive || !entity.geo || !entity.transform) continue;

            // Raw-telemetry marker: ONLY as a fallback when the twin has no
            // valid 3D asset mesh (ModelComponent::isValid() == false). With
            // excavator.fbx / bot.fbx bound, ModelRenderSystem draws the mesh and
            // the marker stays hidden — no "dot-on-mesh" artifact.
            if (m_toggles.points && (!entity.model || !entity.model->isValid())) {
                visualizer.renderPointMarker(entity.transform->position,
                                             glm::vec3(0.0f, 1.0f, 0.0f), 5.0f);
            }

            // Raw-telemetry trajectory history (the twin's actual past path).
            if (m_toggles.trajectory && entity.geo->timestamp > 0) {
                std::vector<glm::vec3> historyPoints;
                historyPoints.push_back(entity.transform->position);
                visualizer.renderTrajectorySimple(historyPoints,
                                             glm::vec3(0.0f, 1.0f, 0.0f), 2.0f);
            }

            // Predictions / Monte Carlo / uncertainty are NOT raw telemetry —
            // they are the Kalman+ML cone. Gated behind the panel checkboxes so
            // a layman can switch them off and see ONLY raw A↔B motion. Each
            // point is converted WGS84->local so the cone stays pinned to the
            // twin instead of drifting in lat/lon-as-meters space.
            if (m_toggles.prediction && m_converter &&
                entity.prediction && entity.prediction->isValid) {
                std::vector<glm::vec3> predPoints;
                for (const auto& state : entity.prediction->predictions) {
                    predPoints.push_back(m_converter->geospatialToLocal(
                        state.latitude, state.longitude, state.altitude));
                }
                if (!predPoints.empty()) {
                    visualizer.renderPredictionsSimple(predPoints,
                                                  glm::vec3(1.0f, 0.5f, 0.0f), 2.0f);
                }

                // Monte Carlo paths (off by default)
                if (m_toggles.monteCarlo) {
                    for (const auto& path : entity.prediction->monteCarloPaths) {
                        std::vector<glm::vec3> mcPoints;
                        for (const auto& state : path) {
                            mcPoints.push_back(m_converter->geospatialToLocal(
                                state.latitude, state.longitude, state.altitude));
                        }
                        if (!mcPoints.empty()) {
                            visualizer.renderMonteCarloPathSimple(mcPoints,
                                                             glm::vec3(0.5f, 0.5f, 0.5f), 1.0f);
                        }
                    }
                }

                // Uncertainty ellipse
                if (m_toggles.uncertainty &&
                    entity.prediction->uncertaintyEllipseSemiAxes.x > 0) {
                    visualizer.renderUncertaintyEllipse(
                        entity.transform->position,
                        entity.prediction->uncertaintyEllipseSemiAxes.x,
                        entity.prediction->uncertaintyEllipseSemiAxes.y,
                        entity.prediction->uncertaintyOrientation,
                        glm::mat4(1.0f));
                }
            }
        }
    }

    GeospatialVisualizer& getVisualizer() { return visualizer; }
    const GeospatialVisualizer& getVisualizer() const { return visualizer; }

    const char* getName() const override { return "GeoVisualizationSystem"; }

private:
    GeospatialVisualizer visualizer;
    std::vector<GeoVisualEntity> visualEntities;
    std::mutex entitiesMutex;

    struct GeoVizToggles {
        bool points = true;
        bool trajectory = true;
        bool prediction = true;
        bool uncertainty = false;
        bool monteCarlo = false;
    } m_toggles{};

    const GeospatialConverter* m_converter = nullptr;
};

} // namespace ecs
