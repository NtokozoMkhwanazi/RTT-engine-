#pragma once

/**
 * GeoTerrainRenderer - Renders geospatial terrain with map viewport
 *
 * Features:
 * - Top-down map view of terrain with GPS tracks
 * - Height-colored terrain visualization
 * - GPS trajectory overlay on terrain
 * - Entity markers on map
 * - Coordinate grid with lat/lon labels
 * - Multithreaded terrain mesh generation
 */

#include "../ecs/ECS.h"
#include "../ecs/components/Components.h"
#include "../ecs/components/GeospatialComponent.h"
#include "../ecs/systems/GeoTerrainSystem.h"
#include "../geospatial/GeospatialConverter.h"
#include "../editor/gl_context_lifecycle.h"
#include <glm/glm.hpp>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <algorithm>
#include <iostream>

class GeoTerrainRenderer {
public:
    GeoTerrainRenderer() : initialized(false), terrainVAO(0), terrainVBO(0), terrainEBO(0),
                           mapShader(0), gridVAO(0), gridVBO(0), running(false) {}

    ~GeoTerrainRenderer() {
        // stopTerrainThread is purely CPU-safe; it joins the worker. safe even
        // after the GL context is gone.
        stopTerrainThread();
        // cleanup() issues GL calls. Only run it if the context is still alive;
        // otherwise the OS-level GL calls after glfwTerminate() will crash.
        if (glctx::isAlive()) {
            cleanup();
        }
    }

    /**
     * Initialize renderer
     */
    void initialize() {
        if (initialized) return;

        createMapShader();
        initialized = true;

        std::cout << "[GeoTerrainRenderer] Initialized\n";
    }

    /**
     * Check if initialized (public accessor)
     */
    bool isInitialized() const { return initialized; }

    /**
     * Get map shader program (public accessor)
     */
    GLuint getMapShader() const { return mapShader; }

    /**
     * Render terrain only (lightweight, no GPS tracks/markers/grid)
     */
    void renderTerrainOnly(ecs::GeoTerrainSystem* geoTerrain) {
        if (!geoTerrain || !initialized) return;
        renderTerrainHeightmap(geoTerrain);
    }

    /**
     * Render map viewport (top-down view)
     */
    void renderMapViewport(ecs::GeoTerrainSystem* geoTerrain,
                          ecs::GeospatialSystem* geoSystem,
                          const glm::mat4& view, const glm::mat4& proj,
                          float viewportW, float viewportH) {
        if (!geoTerrain || !geoSystem || !initialized) return;

        glUseProgram(mapShader);

        // Set viewport for map (top-right corner)
        float mapX = viewportW - 320.0f;
        float mapY = 10.0f;
        float mapW = 310.0f;
        float mapH = 310.0f;

        glViewport(mapX, mapY, mapW, mapH);

        // Create orthographic projection for map
        glm::mat4 mapProj = glm::ortho(-mapW/2, mapW/2, -mapH/2, mapH/2, -100.0f, 100.0f);
        glm::mat4 mapView = glm::mat4(1.0f);
        // Top-down: rotate 90 degrees around X axis
        mapView = glm::rotate(mapView, -glm::radians(90.0f), glm::vec3(1, 0, 0));

        GLint projLoc = glGetUniformLocation(mapShader, "projection");
        GLint viewLoc = glGetUniformLocation(mapShader, "view");
        GLint modelLoc = glGetUniformLocation(mapShader, "model");

        glUniformMatrix4fv(projLoc, 1, GL_FALSE, &mapProj[0][0]);
        glUniformMatrix4fv(viewLoc, 1, GL_FALSE, &mapView[0][0]);

        // Render terrain heightmap
        renderTerrainHeightmap(geoTerrain);

        // Render GPS track overlay
        renderGPSTrackOverlay(geoSystem, geoTerrain);

        // Render entity markers
        renderEntityMarkers(geoSystem, geoTerrain);

        // Render coordinate grid
        renderCoordinateGrid(geoTerrain);

        glUseProgram(0);
        glViewport(0, 0, viewportW, viewportH);
    }

    /**
     * Render full terrain with 3D perspective
     */
    void render3DTerrain(ecs::GeoTerrainSystem* geoTerrain,
                          const glm::mat4& view, const glm::mat4& proj) {
        if (!geoTerrain || !initialized) return;

        if (!geoTerrain->isTerrainGenerated()) return;

        glUseProgram(mapShader);

        GLint projLoc = glGetUniformLocation(mapShader, "projection");
        GLint viewLoc = glGetUniformLocation(mapShader, "view");
        GLint modelLoc = glGetUniformLocation(mapShader, "model");

        glUniformMatrix4fv(projLoc, 1, GL_FALSE, &proj[0][0]);
        glUniformMatrix4fv(viewLoc, 1, GL_FALSE, &view[0][0]);

        glm::mat4 model = glm::mat4(1.0f);
        glUniformMatrix4fv(modelLoc, 1, GL_FALSE, &model[0][0]);

        // Render terrain mesh
        if (terrainVAO) {
            glBindVertexArray(terrainVAO);
            glDrawElements(GL_TRIANGLES, terrainIndexCount, GL_UNSIGNED_INT, 0);
            glBindVertexArray(0);
        }
    }

    /**
     * Set terrain data from GeoTerrainSystem
     */
    void setTerrainData(const std::vector<float>& heightData,
                        int gridWidth, int gridHeight,
                        float terrainSize) {
        std::lock_guard<std::mutex> lock(terrainMutex);

        if (terrainVAO == 0) {
            glGenVertexArrays(1, &terrainVAO);
            glGenBuffers(1, &terrainVBO);
            glGenBuffers(1, &terrainEBO);
        }

        glBindVertexArray(terrainVAO);
        glBindBuffer(GL_ARRAY_BUFFER, terrainVBO);

        // Build vertices with positions and colors based on height
        std::vector<float> vertices;
        vertices.reserve(gridWidth * gridHeight * 6); // x,y,z + r,g,b

        float scaleX = terrainSize / (gridWidth - 1);
        float scaleZ = terrainSize / (gridHeight - 1);

        for (int z = 0; z < gridHeight; z++) {
            for (int x = 0; x < gridWidth; x++) {
                float h = heightData[z * gridWidth + x];
                float px = (x - gridWidth/2) * scaleX;
                float pz = (z - gridHeight/2) * scaleZ;

                // Position
                vertices.push_back(px);
                vertices.push_back(h);
                vertices.push_back(pz);

                // Color based on height (green for low, brown for high)
                float normalizedH = (h + 50.0f) / 100.0f; // Assuming height range -50 to 50
                normalizedH = std::max(0.0f, std::min(1.0f, normalizedH));

                float r, g, b;
                if (normalizedH < 0.5f) {
                    // Water to sand to green
                    r = 0.0f; g = normalizedH * 2.0f; b = 0.5f - normalizedH;
                } else {
                    // Green to brown to white
                    r = (normalizedH - 0.5f) * 2.0f;
                    g = 1.0f - (normalizedH - 0.5f);
                    b = (normalizedH - 0.5f) * 0.5f;
                }

                vertices.push_back(r); vertices.push_back(g); vertices.push_back(b);
            }
        }

        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);

        // Build indices
        std::vector<unsigned int> indices;
        indices.reserve((gridWidth - 1) * (gridHeight - 1) * 6);

        for (int z = 0; z < gridHeight - 1; z++) {
            for (int x = 0; x < gridWidth - 1; x++) {
                int tl = z * gridWidth + x;
                int tr = tl + 1;
                int bl = (z + 1) * gridWidth + x;
                int br = bl + 1;

                indices.push_back(tl); indices.push_back(bl); indices.push_back(tr);
                indices.push_back(tr); indices.push_back(bl); indices.push_back(br);
            }
        }

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, terrainEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

        // Position attribute
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        // Color attribute
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glBindVertexArray(0);

        terrainIndexCount = indices.size();

        std::cout << "[GeoTerrainRenderer] Terrain mesh created: "
                  << gridWidth << "x" << gridHeight << " vertices\n";
    }

    void shutdown() {
        stopTerrainThread();
        cleanup();
        initialized = false;
    }

    const char* getName() const { return "GeoTerrainRenderer"; }

private:
    bool initialized;
    GLuint terrainVAO, terrainVBO, terrainEBO;
    GLuint gridVAO, gridVBO;
    GLuint mapShader;
    int terrainIndexCount;

    std::queue<std::vector<float>> terrainUpdateQueue;
    std::mutex terrainMutex;
    std::thread terrainThread;
    std::atomic<bool> running;

    void createMapShader() {
        const char* vs = R"(
            #version 430 core
            layout(location=0) in vec3 aPos;
            layout(location=1) in vec3 aColor;
            uniform mat4 projection;
            uniform mat4 view;
            uniform mat4 model;
            out vec3 fragColor;
            void main() {
                fragColor = aColor;
                gl_Position = projection * view * model * vec4(aPos, 1.0);
            }
        )";

        const char* fs = R"(
            #version 430 core
            in vec3 fragColor;
            out vec4 FragColor;
            void main() {
                FragColor = vec4(fragColor, 1.0);
            }
        )";

        GLuint vsObj = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vsObj, 1, &vs, nullptr);
        glCompileShader(vsObj);

        GLuint fsObj = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fsObj, 1, &fs, nullptr);
        glCompileShader(fsObj);

        mapShader = glCreateProgram();
        glAttachShader(mapShader, vsObj);
        glAttachShader(mapShader, fsObj);
        glLinkProgram(mapShader);

        glDeleteShader(vsObj);
        glDeleteShader(fsObj);
    }

    void renderTerrainHeightmap(ecs::GeoTerrainSystem* geoTerrain) {
        (void)geoTerrain;
        if (terrainVAO) {
            glBindVertexArray(terrainVAO);
            glDrawElements(GL_TRIANGLES, terrainIndexCount, GL_UNSIGNED_INT, 0);
            glBindVertexArray(0);
        }
    }

    void renderGPSTrackOverlay(ecs::GeospatialSystem* geoSystem,
                               ecs::GeoTerrainSystem* geoTerrain) {
        auto history = geoSystem->getEntityTrajectory("gps_tracker", 200);
        if (history.empty()) return;

        const auto& converter = geoSystem->getConverter();

        glDisable(GL_DEPTH_TEST);
        glLineWidth(2.0f);

        // Render track as line strip
        std::vector<float> lineVerts;
        for (const auto& point : history) {
            auto projection = geoTerrain->projectToTerrain(point.latitude, point.longitude);
            if (projection.onTerrain) {
                lineVerts.push_back(projection.terrainPos.x);
                lineVerts.push_back(projection.terrainPos.y + 0.1f); // Slightly above terrain
                lineVerts.push_back(projection.terrainPos.z);
            }
        }

        if (lineVerts.size() >= 9) { // At least 3 vertices
            GLuint lineVAO, lineVBO;
            glGenVertexArrays(1, &lineVAO);
            glGenBuffers(1, &lineVBO);
            glBindVertexArray(lineVAO);
            glBindBuffer(GL_ARRAY_BUFFER, lineVBO);
            glBufferData(GL_ARRAY_BUFFER, lineVerts.size() * sizeof(float), lineVerts.data(), GL_STREAM_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
            glEnableVertexAttribArray(0);
            glDrawArrays(GL_LINE_STRIP, 0, lineVerts.size() / 3);
            glBindVertexArray(0);
            glDeleteVertexArrays(1, &lineVAO);
            glDeleteBuffers(1, &lineVBO);
        }

        glEnable(GL_DEPTH_TEST);
    }

    void renderEntityMarkers(ecs::GeospatialSystem* geoSystem,
                             ecs::GeoTerrainSystem* geoTerrain) {
        (void)geoSystem; (void)geoTerrain;
        // TODO: Render markers for all entities with GeospatialComponent
    }

    void renderCoordinateGrid(ecs::GeoTerrainSystem* geoTerrain) {
        (void)geoTerrain;
        // TODO: Render lat/lon grid
    }

    void stopTerrainThread() {
        running = false;
        if (terrainThread.joinable()) {
            terrainThread.join();
        }
    }

    void cleanup() {
        // Belt-and-suspenders: if a caller reaches us after the GL context
        // has been torn down (e.g., shutdown() called twice, or destructor
        // running after glfwTerminate), skip every GL call here.
        if (!glctx::isAlive()) return;

        if (terrainVAO) { glDeleteVertexArrays(1, &terrainVAO); terrainVAO = 0; }
        if (terrainVBO) { glDeleteBuffers(1, &terrainVBO); terrainVBO = 0; }
        if (terrainEBO) { glDeleteBuffers(1, &terrainEBO); terrainEBO = 0; }
        if (gridVAO) { glDeleteVertexArrays(1, &gridVAO); gridVAO = 0; }
        if (gridVBO) { glDeleteBuffers(1, &gridVBO); gridVBO = 0; }
        if (mapShader) { glDeleteProgram(mapShader); mapShader = 0; }
    }
};
