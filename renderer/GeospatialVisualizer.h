#pragma once

/**
 * Geospatial Visualizer - Advanced Visualization (Phase 4)
 *
 * Renders geospatial data in the 3D engine:
 * - Trajectory lines (history + predictions)
 * - Heatmaps for density/uncertainty
 * - Real-time data overlays
 * - Uncertainty ellipses
 */

#include <vector>
#include <glm/glm.hpp>
#include "../ecs/components/GeospatialComponent.h"
#include "../ecs/components/TransformComponent.h"
#include "Renderer.h"
#include "../editor/gl_context_lifecycle.h"

// A single cached line segment for the main loop to draw via DebugRenderer.
struct GeoLineSegment {
    glm::vec3 a;
    glm::vec3 b;
    glm::vec3 color;
};

// Trajectory point for visualization
struct TrajectoryPoint {
    glm::vec3 position;
    float timestamp;
    float confidence;  // 0-1
    glm::vec3 color;
};

// Heatmap point
struct HeatmapPoint {
    glm::vec3 position;
    float intensity;  // 0-1
};

class GeospatialVisualizer {
public:
    GeospatialVisualizer() : lineVAO(0), lineVBO(0), heatmapVAO(0), heatmapVBO(0) {}

    ~GeospatialVisualizer() {
        // These GL calls must never run on a dead context (static destruction
        // happens after glfwTerminate). Zero the handles either way so repeated
        // teardown paths are idempotent.
        if (glctx::isAlive()) {
            if (lineVAO) glDeleteVertexArrays(1, &lineVAO);
            if (lineVBO) glDeleteBuffers(1, &lineVBO);
            if (heatmapVAO) glDeleteVertexArrays(1, &heatmapVAO);
            if (heatmapVBO) glDeleteBuffers(1, &heatmapVBO);
        }
        lineVAO = lineVBO = heatmapVAO = heatmapVBO = 0;
    }

    void shutdown() {
        // Same guard as the destructor: shutdown may be called during teardown
        // after the GL context is gone (static destruction order).
        if (glctx::isAlive()) {
            if (lineVAO) glDeleteVertexArrays(1, &lineVAO);
            if (lineVBO) glDeleteBuffers(1, &lineVBO);
            if (heatmapVAO) glDeleteVertexArrays(1, &heatmapVAO);
            if (heatmapVBO) glDeleteBuffers(1, &heatmapVBO);
        }
        lineVAO = lineVBO = heatmapVAO = heatmapVBO = 0;
    }

    /**
     * Initialize GPU resources
     */
    void initialize() {
        // Line rendering
        glGenVertexArrays(1, &lineVAO);
        glGenBuffers(1, &lineVBO);

        // Heatmap rendering
        glGenVertexArrays(1, &heatmapVAO);
        glGenBuffers(1, &heatmapVBO);

        std::cout << "[GeospatialVisualizer] Initialized\n";
    }

    /**
     * Render trajectory lines
     */
    void renderTrajectory(const std::vector<TrajectoryPoint>& points,
                          const glm::mat4& viewProj,
                          const glm::vec3& cameraPos) {
        if (points.size() < 2) return;

        // Build vertex data
        std::vector<float> vertices;
        for (const auto& p : points) {
            vertices.push_back(p.position.x);
            vertices.push_back(p.position.y);
            vertices.push_back(p.position.z);
            vertices.push_back(p.color.r);
            vertices.push_back(p.color.g);
            vertices.push_back(p.color.b);
            vertices.push_back(p.confidence);
        }

        // Update GPU buffer
        glBindVertexArray(lineVAO);
        glBindBuffer(GL_ARRAY_BUFFER, lineVBO);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_DYNAMIC_DRAW);

        // Set up attributes
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);

        // Draw lines
        glDrawArrays(GL_LINE_STRIP, 0, points.size());

        glBindVertexArray(0);
    }

    /**
     * Render a simple trajectory from positions and color
     */
    void renderTrajectorySimple(const std::vector<glm::vec3>& positions,
                               const glm::vec3& color, float lineWidth) {
        if (positions.size() < 2) return;

        std::vector<TrajectoryPoint> points;
        for (const auto& pos : positions) {
            TrajectoryPoint tp;
            tp.position = pos;
            tp.timestamp = 0.0f;
            tp.confidence = 1.0f;
            tp.color = color;
            points.push_back(tp);
        }
        renderTrajectory(points, glm::mat4(1.0f), glm::vec3(0));
    }

    /**
     * Render a single point marker
     */
    void renderPointMarker(const glm::vec3& position, const glm::vec3& color, float size) {
        std::vector<HeatmapPoint> points;
        HeatmapPoint hp;
        hp.position = position;
        hp.intensity = 1.0f;
        points.push_back(hp);
        renderHeatmap(points, glm::mat4(1.0f));
    }

    /**
     * Render predictions (wrapper for renderPredictedTrajectory)
     */
    void renderPredictionsSimple(const std::vector<glm::vec3>& predictions,
                                const glm::vec3& color, float lineWidth) {
        if (predictions.empty()) return;

        std::vector<TrajectoryPoint> points;
        for (const auto& pos : predictions) {
            TrajectoryPoint tp;
            tp.position = pos;
            tp.timestamp = 0.0f;
            tp.confidence = 0.8f;
            tp.color = color;
            points.push_back(tp);
        }
        renderTrajectory(points, glm::mat4(1.0f), glm::vec3(0));
    }

    /**
     * Render Monte Carlo path
     */
    void renderMonteCarloPathSimple(const std::vector<glm::vec3>& path,
                                    const glm::vec3& color, float lineWidth) {
        renderTrajectorySimple(path, color, lineWidth);
    }

    /**
     * Render predicted trajectory with uncertainty
     */
    void renderPredictedTrajectory(const std::vector<PredictedState>& predictions,
                                   const GeospatialConverter& converter,
                                   const glm::mat4& viewProj) {
        if (predictions.empty()) return;

        std::vector<TrajectoryPoint> points;
        for (const auto& pred : predictions) {
            TrajectoryPoint tp;
            tp.position = converter.geospatialToLocal(pred.latitude, pred.longitude, pred.altitude);
            tp.timestamp = pred.timestamp;
            tp.confidence = pred.confidence;

            // Color: green (high confidence) to red (low confidence)
            float r = 1.0f - pred.confidence;
            float g = pred.confidence;
            tp.color = glm::vec3(r, g, 0.0f);

            points.push_back(tp);
        }

        renderTrajectory(points, viewProj, glm::vec3(0));
    }

    /**
     * Render heatmap (density/uncertainty visualization)
     */
    void renderHeatmap(const std::vector<HeatmapPoint>& points,
                       const glm::mat4& viewProj) {
        if (points.empty()) return;

        std::vector<float> vertices;
        for (const auto& p : points) {
            vertices.push_back(p.position.x);
            vertices.push_back(p.position.y);
            vertices.push_back(p.position.z);
            vertices.push_back(p.intensity);
        }

        glBindVertexArray(heatmapVAO);
        glBindBuffer(GL_ARRAY_BUFFER, heatmapVBO);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_DYNAMIC_DRAW);

        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);

        // Points for now - could be quads or custom shader
        glDrawArrays(GL_POINTS, 0, points.size());

        glBindVertexArray(0);
    }

    /**
     * Render uncertainty ellipse
     */
    void renderUncertaintyEllipse(const glm::vec3& center,
                                  float semiMajor, float semiMinor,
                                  float orientation,
                                  const glm::mat4& viewProj) {
        const int numSegments = 64;
        std::vector<float> vertices;

        for (int i = 0; i <= numSegments; i++) {
            float angle = (2.0f * M_PI * i) / numSegments + orientation;

            float x = center.x + semiMajor * std::cos(angle);
            float y = center.y + semiMinor * std::sin(angle);

            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(center.z);
        }

        glBindVertexArray(lineVAO);
        glBindBuffer(GL_ARRAY_BUFFER, lineVBO);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_DYNAMIC_DRAW);

        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        glDrawArrays(GL_LINE_LOOP, 0, numSegments + 1);

        glBindVertexArray(0);
    }

    /**
     * Convert history to trajectory points
     */
    static std::vector<TrajectoryPoint> historyToTrajectory(
        const std::vector<TimeSeriesPoint>& history,
        const GeospatialConverter& converter) {
        std::vector<TrajectoryPoint> result;

        for (const auto& point : history) {
            TrajectoryPoint tp;
            tp.position = converter.geospatialToLocal(point.latitude, point.longitude, point.altitude);
            tp.timestamp = point.timestamp;
            tp.confidence = 1.0f - (point.accuracy / 100.0f);
            tp.confidence = std::max(0.1f, std::min(1.0f, tp.confidence));

            // Color by speed: blue (slow) to red (fast)
            float speedNorm = std::min(1.0f, static_cast<float>(point.speed / 30.0f));
            tp.color = glm::vec3(speedNorm, 0.0f, 1.0f - speedNorm);

            result.push_back(tp);
        }

        return result;
    }

private:
    GLuint lineVAO, lineVBO;
    GLuint heatmapVAO, heatmapVBO;
};
