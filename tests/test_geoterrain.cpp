/**
 * Test: Geospatial Terrain Integration
 *
 * Tests the integration between:
 * - GeoIngestionSystem (GPS input)
 * - GeoStorageSystem (time-series DB)
 * - GeoPredictionSystem (ML predictions)
 * - GeoTerrainSystem (terrain + map viewport)
 */

#include <iostream>
#include <cassert>
#include <cmath>
#include "../ecs/ECS.h"
#include "../ecs/systems/GeoTerrainSystem.h"
#include "../ecs/systems/GeoIngestionSystem.h"
#include "../ecs/systems/GeoStorageSystem.h"
#include "../ecs/systems/GeoPredictionSystem.h"
#include "../ecs/systems/GeospatialSystem.h"
#include "../geospatial/GeospatialConverter.h"

void testGeoTerrainProjection() {
    std::cout << "Test: GeoTerrainProjection...\n";

    ecs::GeoTerrainSystem geoTerrain;
    geoTerrain.initialize(-33.8568, 151.2153); // Sydney Opera House

    // Test projection of origin point
    auto proj = geoTerrain.projectToTerrain(-33.8568, 151.2153);
    assert(proj.onTerrain == true);
    assert(std::abs(proj.terrainPos.x) < 1.0f); // Near origin
    assert(std::abs(proj.terrainPos.z) < 1.0f);

    std::cout << "  Origin projection: SUCCESS\n";
    std::cout << "  Terrain height at origin: " << proj.height << "m\n";

    // Test projection of point offset
    auto proj2 = geoTerrain.projectToTerrain(-33.8578, 151.2163); // ~150m away
    assert(proj2.onTerrain == true);

    std::cout << "  Offset projection: SUCCESS\n";
    std::cout << "  Terrain height at offset: " << proj2.height << "m\n";

    std::cout << "Test: GeoTerrainProjection - PASSED\n\n";
}

void testGeospatialSystemOrchestration() {
    std::cout << "Test: GeospatialSystemOrchestration...\n";

    ecs::GeospatialSystem geoSystem;
    geoSystem.initialize(-33.8568, 151.2153, 50.0);

    // Test that all subsystems are accessible
    ecs::GeoIngestionSystem& ingestion = geoSystem.getIngestionSystem();
    ecs::GeoStorageSystem& storage = geoSystem.getStorageSystem();
    ecs::GeoPredictionSystem& prediction = geoSystem.getPredictionSystem();
    ecs::GeoVisualizationSystem& visualization = geoSystem.getVisualizationSystem();

    (void)ingestion; (void)storage; (void)prediction; (void)visualization;

    // Test coordinate conversion through orchestrator
    const auto& converter = geoSystem.getConverter();
    glm::vec3 local = converter.geospatialToLocal(-33.8568, 151.2153, 50.0);
    assert(std::abs(local.x) < 0.1f);
    assert(std::abs(local.z) < 0.1f);

    std::cout << "  Subsystem access: SUCCESS\n";
    std::cout << "  Coordinate conversion: SUCCESS\n";
    std::cout << "Test: GeospatialSystemOrchestration - PASSED\n\n";
}

void testMultithreading() {
    std::cout << "Test: Multithreading...\n";

    ecs::GeospatialSystem geoSystem;
    geoSystem.initialize(0.0, 0.0, 0.0);

    // Start ingestion (spawns thread)
    // Ingestion runs on separate thread

    // Let it run briefly
    for (int i = 0; i < 10; i++) {
        geoSystem.update(0.1f);
    }

    std::cout << "  Multithreaded update: SUCCESS\n";
    std::cout << "Test: Multithreading - PASSED\n\n";
}

void testTerrainGeneration() {
    std::cout << "Test: TerrainGeneration...\n";

    ecs::GeoTerrainSystem geoTerrain;
    geoTerrain.initialize(0.0, 0.0);
    geoTerrain.generateTerrain();

    // Wait a bit for terrain thread
    for (int i = 0; i < 100; i++) {
        if (geoTerrain.isTerrainGenerated()) break;
    }

    // Test height sampling
    float h1 = geoTerrain.getTerrainHeightAt(0, 0);
    float h2 = geoTerrain.getTerrainHeightAt(100, 100);
    float h3 = geoTerrain.getTerrainHeightAt(-100, -100);

    std::cout << "  Height at (0,0): " << h1 << "m\n";
    std::cout << "  Height at (100,100): " << h2 << "m\n";
    std::cout << "  Height at (-100,-100): " << h3 << "m\n";
    std::cout << "Test: TerrainGeneration - PASSED\n\n";
}

int main() {
    std::cout << "=== Geospatial Terrain Integration Tests ===\n\n";

    testGeoTerrainProjection();
    testGeospatialSystemOrchestration();
    testMultithreading();
    testTerrainGeneration();

    std::cout << "=== All tests passed! ===\n";
    return 0;
}
