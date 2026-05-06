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

namespace ecs {

struct GeoVisualEntity {
    EntityID id;
    GeospatialComponent* geo = nullptr;
    TransformComponent* transform = nullptr;
    PredictionComponent* prediction = nullptr;
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
     * Register entity for visualization
     */
    void registerEntity(EntityID id, GeospatialComponent* geo, TransformComponent* transform,
                         PredictionComponent* prediction = nullptr) {
        if (!geo || !transform) return;

        GeoVisualEntity entity;
        entity.id = id;
        entity.geo = geo;
        entity.transform = transform;
        entity.prediction = prediction;
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

        for (auto& entity : visualEntities) {
            if (!entity.isActive || !entity.geo || !entity.transform) continue;

            // Render entity marker
            visualizer.renderPointMarker(entity.transform->position,
                                    glm::vec3(0.0f, 1.0f, 0.0f), 5.0f);

            // Render trajectory history
            if (entity.geo->timestamp > 0) {
                std::vector<glm::vec3> historyPoints;
                historyPoints.push_back(entity.transform->position);
                visualizer.renderTrajectorySimple(historyPoints,
                                             glm::vec3(0.0f, 1.0f, 0.0f), 2.0f);
            }

            // Render predictions
            if (entity.prediction && entity.prediction->isValid) {
                std::vector<glm::vec3> predPoints;
                for (const auto& state : entity.prediction->predictions) {
                    predPoints.push_back(glm::vec3(
                        static_cast<float>(state.latitude),
                        static_cast<float>(state.longitude),
                        0.0f));
                }
                visualizer.renderPredictionsSimple(predPoints,
                                              glm::vec3(1.0f, 0.5f, 0.0f), 2.0f);

                // Render Monte Carlo paths
                for (const auto& path : entity.prediction->monteCarloPaths) {
                    std::vector<glm::vec3> mcPoints;
                    for (const auto& state : path) {
                        mcPoints.push_back(glm::vec3(
                            static_cast<float>(state.latitude),
                            static_cast<float>(state.longitude),
                            0.0f));
                    }
                    visualizer.renderMonteCarloPathSimple(mcPoints,
                                                     glm::vec3(0.5f, 0.5f, 0.5f), 1.0f);
                }

                // Render uncertainty ellipse
                if (entity.prediction->uncertaintyEllipseSemiAxes.x > 0) {
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
};

} // namespace ecs
