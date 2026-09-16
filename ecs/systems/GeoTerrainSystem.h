#pragma once

/**
 * GeoTerrainSystem - Integrates Geospatial System with Terrain System
 *
 * Bridges real-world geographic coordinates with terrain rendering:
 * - Maps geospatial bounds to terrain dimensions
 * - Converts lat/lon to terrain local coordinates
 * - Generates terrain height from real elevation data (SRTM/USGS)
 * - Displays GPS tracks and entities on terrain surface
 * - Multithreaded terrain chunk generation based on geospatial queries
 */

#include "../ECS.h"
#include "../components/Components.h"
#include "../components/GeospatialComponent.h"
#include "TerrainSystem.h"
#include "GeospatialSystem.h"
#include "GeoStorageSystem.h"
#include "../../geospatial/GeospatialConverter.h"
#include "../../world/Terrain.h"
#include <glm/glm.hpp>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <iostream>

namespace ecs {

struct GeoTerrainConfig {
    double originLat = 0.0;
    double originLon = 0.0;
    float terrainSize = 2000.0f;      // Meters in local space
    float heightScale = 100.0f;        // Meters of elevation
    int gridResolution = 512;           // Heightmap resolution
    int chunkSize = 64;                // Chunk vertex count
    float lodDistances[3] = {50.0f, 150.0f, 500.0f};
};

struct TerrainGPSProjection {
    glm::vec3 terrainPos;      // Position on terrain
    glm::vec3 worldPos;        // World space position
    float height;               // Terrain height at this point
    bool onTerrain;             // Whether point projects onto terrain
};

class GeoTerrainSystem : public TypedSystem<GeospatialComponent, TransformComponent> {
public:
    GeoTerrainSystem() : running(false), terrainGenerated(false) {}

    ~GeoTerrainSystem() {
        stopTerrainThread();
    }

    /**
     * Initialize with geospatial origin and terrain config
     */
    void initialize(double originLat, double originLon, const GeoTerrainConfig& config = GeoTerrainConfig()) {
        this->config = config;
        this->config.originLat = originLat;
        this->config.originLon = originLon;

        converter.setOrigin(originLat, originLon, 0.0);

        // Calculate geospatial bounds
        double halfSizeDeg = config.terrainSize / 111320.0; // ~111km per degree
        geoBounds.minLat = originLat - halfSizeDeg;
        geoBounds.maxLat = originLat + halfSizeDeg;
        geoBounds.minLon = originLon - halfSizeDeg;
        geoBounds.maxLon = originLon + halfSizeDeg;

        std::cout << "[GeoTerrainSystem] Initialized at " << originLat << "°, " << originLon << "°\n";
        std::cout << "[GeoTerrainSystem] Terrain size: " << config.terrainSize << "m, Resolution: " << config.gridResolution << "\n";
    }

    /**
     * Set the geospatial system for data queries
     */
    void setGeospatialSystem(GeospatialSystem* geo) {
        geospatialSystem = geo;
    }

    /**
     * Generate terrain from geospatial bounds (multithreaded)
     */
    void generateTerrain() {
        if (terrainGenerated) return;

        startTerrainThread();
        requestTerrainGeneration();
    }

    /**
     * Project geospatial coordinates onto terrain
     */
    TerrainGPSProjection projectToTerrain(double lat, double lon, float altitude = 0.0f) {
        TerrainGPSProjection result;

        // Convert to local space
        glm::vec3 localPos = converter.geospatialToLocal(lat, lon, altitude);
        result.worldPos = localPos;

        // Check if within terrain bounds
        float halfSize = config.terrainSize * 0.5f;
        if (std::abs(localPos.x) > halfSize || std::abs(localPos.z) > halfSize) {
            result.onTerrain = false;
            return result;
        }

        // Sample terrain height (would use actual heightmap in production)
        result.height = sampleTerrainHeight(localPos.x, localPos.z);
        result.terrainPos = glm::vec3(localPos.x, result.height, localPos.z);
        result.onTerrain = true;

        return result;
    }

    /**
     * Project ECS entity with GeospatialComponent onto terrain
     */
    void projectEntityToTerrain(EntityID entityID) {
        if (!m_componentManager) return;

        auto* geo = m_componentManager->getComponent<GeospatialComponent>(entityID);
        auto* transform = m_componentManager->getComponent<TransformComponent>(entityID);

        if (!geo || !transform) return;

        auto projection = projectToTerrain(geo->latitude, geo->longitude, geo->altitude);

        if (projection.onTerrain) {
            transform->position = projection.terrainPos;
        }
    }

    /**
     * Main update - sync geospatial entities with terrain
     */
    void update(float dt) override {
        if (!m_entityManager || !m_componentManager) return;

        // Process terrain generation requests
        processTerrainRequests();

        // Update terrain chunks based on camera
        if (geospatialSystem) {
            const auto& gpsFix = geospatialSystem->getCurrentGPSFix();
            if (gpsFix.isValid) {
                glm::vec3 currentPos = converter.geospatialToLocal(
                    gpsFix.latitude, gpsFix.longitude, gpsFix.altitude);

                // Update LOD for terrain chunks near current position
                updateTerrainLOD(currentPos);
            }
        }

        // Project all entities with both GeospatialComponent and TransformComponent
        forEach(*m_entityManager, *m_componentManager,
            [this](EntityID id, GeospatialComponent& geo, TransformComponent& transform) {
                auto projection = projectToTerrain(geo.latitude, geo.longitude, geo.altitude);
                if (projection.onTerrain) {
                    transform.position = projection.terrainPos;
                    geo.altitude = projection.height; // Snap to terrain
                }
            });
    }

    /**
     * Get terrain height at local position
     */
    float getTerrainHeightAt(float x, float z) const {
        return sampleTerrainHeight(x, z);
    }

    /**
     * Get geospatial converter
     */
    GeospatialConverter& getConverter() { return converter; }
    const GeospatialConverter& getConverter() const { return converter; }

    const char* getName() const override { return "GeoTerrainSystem"; }

    bool isTerrainGenerated() const { return terrainGenerated; }

private:
    struct GeoBounds {
        double minLat, maxLat;
        double minLon, maxLon;
    };

    struct TerrainGenRequest {
        int x, y;           // Chunk coordinates
        float* heightData;   // Output
        int resolution;
    };

    GeoTerrainConfig config;
    GeospatialConverter converter;
    GeoBounds geoBounds;

    GeospatialSystem* geospatialSystem = nullptr;

    std::queue<TerrainGenRequest> genRequests;
    std::mutex requestMutex;
    std::condition_variable cv;
    std::thread terrainThread;
    std::atomic<bool> running;
    std::atomic<bool> terrainGenerated;

    // Simple sine-based terrain (replace with real DEM data)
    float sampleTerrainHeight(float x, float z) const {
        float nx = x / config.terrainSize;
        float nz = z / config.terrainSize;

        // Multi-octave sine noise for terrain shape
        float height = 0.0f;
        float amplitude = 1.0f;
        float frequency = 2.0f;

        for (int i = 0; i < 4; i++) {
            height += std::sin(nx * frequency * 3.14159f) *
                      std::cos(nz * frequency * 3.14159f) * amplitude;
            amplitude *= 0.5f;
            frequency *= 2.0f;
        }

        return height * config.heightScale;
    }

    void startTerrainThread() {
        if (running) return;
        running = true;
        terrainThread = std::thread([this]() { terrainThreadFunc(); });
        std::cout << "[GeoTerrainSystem] Started terrain generation thread\n";
    }

    void stopTerrainThread() {
        running = false;
        cv.notify_all();
        if (terrainThread.joinable()) {
            terrainThread.join();
        }
    }

    void requestTerrainGeneration() {
        std::lock_guard<std::mutex> lock(requestMutex);
        // Queue terrain generation for all chunks
        int chunksPerSide = config.gridResolution / config.chunkSize;
        for (int y = 0; y < chunksPerSide; y++) {
            for (int x = 0; x < chunksPerSide; x++) {
                TerrainGenRequest req;
                req.x = x;
                req.y = y;
                req.resolution = config.chunkSize;
                req.heightData = new float[config.chunkSize * config.chunkSize];
                genRequests.push(req);
            }
        }
        cv.notify_all();
    }

    void processTerrainRequests() {
        std::lock_guard<std::mutex> lock(requestMutex);
        while (!genRequests.empty()) {
            auto req = genRequests.front();
            genRequests.pop();

            // Generate height data for this chunk
            generateChunkHeight(req);

            delete[] req.heightData;
        }
    }

    void terrainThreadFunc() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            TerrainGenRequest req;
            {
                std::unique_lock<std::mutex> lock(requestMutex);
                if (genRequests.empty()) continue;

                req = genRequests.front();
                genRequests.pop();
            }

            generateChunkHeight(req);
            delete[] req.heightData;
        }
    }

    void generateChunkHeight(const TerrainGenRequest& req) {
        // Generate heightmap for this chunk
        float chunkWorldSize = config.terrainSize / (config.gridResolution / config.chunkSize);

        for (int z = 0; z < req.resolution; z++) {
            for (int x = 0; x < req.resolution; x++) {
                float worldX = (req.x * chunkWorldSize) + (x * chunkWorldSize / req.resolution);
                float worldZ = (req.y * chunkWorldSize) + (z * chunkWorldSize / req.resolution);

                req.heightData[z * req.resolution + x] = sampleTerrainHeight(worldX, worldZ);
            }
        }

        terrainGenerated = true;
    }

    void updateTerrainLOD(const glm::vec3& cameraPos) {
        // Update LOD based on distance from camera
        // This would integrate with TerrainChunkSystem in production
        (void)cameraPos;
    }
};

} // namespace ecs
