# Geospatial System - Digital Twin Platform

Real-time geospatial tracking and digital twin visualization for the RTT Engine. Converts WGS84 geographic coordinates (latitude/longitude/altitude) to local engine space using double-precision math, avoiding float precision issues entirely.

## Architecture (Multithreaded)

### Zero Float Precision Debt

Geospatial data stays `double`, existing systems stay `float`.

```
WGS84 (double)              Local Engine Space (float)
┌──────────────────────┐    ┌──────────────────────────┐
│ lat:  -33.8568       │    │ TransformComponent       │
│ lon:  151.2153       │───►│ position: glm::vec3      │
│ alt:  50.0           │    │ (relative to origin)     │
└──────────────────────┘    └──────────────────────────┘
         ▲                              │
         │                       All existing systems
         │                       (Render, Physics, ECS)
         │                       work unchanged
   GeospatialConverter
   - setOrigin(lat, lon, alt)
   - geospatialToLocal() → glm::vec3
   - localToGeospatial() → glm::dvec3
```

### Multithreaded Pipeline

```
┌─────────────────────────────────────────────────────────┐
│              GeospatialSystem (Orchestrator)              │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────┐  │
│  │ GeoIngestion │  │ GeoStorage   │  │ GeoPredic│  │
│  │ System       │  │ System       │  │ tion     │  │
│  │ (Thread 1)   │  │ (Thread 2)   │  │ System   │  │
│  │              │  │              │  │ (Thread 3│  │
│  │ GPS Feeds    │  │ InfluxDB     │  │          │  │
│  │ REST/WS      │  │ Batch Write  │  │ Kalman/ML│  │
│  └──────┬──────┘  └──────┬──────┘  └────┬─────┘  │
│         │                │              │               │
│         └────────────────┼──────────────┘               │
│                          │                              │
│              ┌───────────▼──────────┐                 │
│              │ GeoVisualization   │                 │
│              │ System             │                 │
│              │ (Main Thread - GL) │                 │
└─────────────────────────────────────────────────────────┘
                          │
                          ▼
              ┌──────────────────────┐
              │ GeoTerrainSystem   │
              │ (Map/Terrain View) │
              └──────────────────────┘
```

### Conversion Pipeline

```
WGS84 (lat/lon/alt, double)
    │
    ▼
ECEF (Earth-Centered Earth-Fixed, double)
    │
    ▼
ENU (East-North-Up relative to origin, double)
    │
    ▼
Engine Space (glm::vec3, float)
```

## Files

```
geospatial/
├── GeospatialConverter.h    # WGS84 ↔ ECEF ↔ ENU conversion engine
├── GPSTracker.h             # Simulated GPS feed (5 modes)
├── DataFeedManager.h        # Real-time data ingestion (Phase 1)
├── TimeSeriesDB.h           # InfluxDB storage (Phase 2)
├── InfluxDBClient.h         # Lightweight InfluxDB v1 HTTP client
├── PredictiveModel.h        # Kalman filter + prediction (Phase 3)
├── TFLitePredictor.h        # TensorFlow Lite ML predictor
└── trajectory_predict.tflite # Pre-trained ML model (43KB)

ecs/systems/
├── GeospatialSystem.h       # Orchestrator (backward compatible API)
├── GeoIngestionSystem.h    # Phase 1: GPS input (threaded)
├── GeoStorageSystem.h      # Phase 2: Time-series DB (threaded)
├── GeoPredictionSystem.h   # Phase 3: ML prediction (threaded)
├── GeoVisualizationSystem.h # Phase 4: OpenGL rendering
├── GeoTerrainSystem.h     # Phase 5: Terrain integration (NEW!)
└── GeoTerrainRenderer.h   # Map viewport with GPS overlay (NEW!)
```

## Usage

### Initialize Digital Twin Platform

```cpp
#include "ecs/systems/GeospatialSystem.h"
#include "ecs/systems/GeoTerrainSystem.h"
#include "renderer/GeoTerrainRenderer.h"

// Main orchestrator (backward compatible)
ecs::GeospatialSystem geoSystem;
geoSystem.initialize(-33.8568, 151.2153, 50.0);  // Sydney Opera House
geoSystem.setGPSMode(GPSTracker::Mode::SIMULATED_WALK);
world.addSystem(&geoSystem);

// Terrain integration (NEW!)
ecs::GeoTerrainSystem geoTerrain;
GeoTerrainConfig config;
config.terrainSize = 2000.0f;
config.heightScale = 100.0f;
config.gridResolution = 256;
geoTerrain.initialize(-33.8568, 151.2153, config);
geoTerrain.setGeospatialSystem(&geoSystem);
geoTerrain.generateTerrain();

// Map viewport renderer
GeoTerrainRenderer terrainRenderer;
terrainRenderer.initialize();
```

### Per-Frame Update

```cpp
world.update(dt);  // Updates all systems

// Render map viewport (top-right corner)
terrainRenderer.renderMapViewport(&geoTerrain, &geoSystem, view, proj, width, height);

// Render 3D terrain
terrainRenderer.render3DTerrain(&geoTerrain, view, proj);
```

## Phase 1: Real-Time Data Ingestion (GeoIngestionSystem)

- **Dedicated thread** for non-blocking data ingestion
- REST API polling with custom parsers
- WebSocket stream support (simulated)
- NMEA 0183 GPS sentence parsing ($GPRMC)
- Multi-threaded polling with callbacks
- Thread-safe data queue

```cpp
auto& ingestion = geoSystem.getIngestionSystem();
ingestion.addRESTFeed("https://api.example.com/gps", 1000,
    [](const std::string& json) -> GeoDataPoint {
        // Parse JSON to GeoDataPoint
        GeoDataPoint point;
        // ... parse ...
        return point;
    });
ingestion.startIngestion();  // Starts thread
```

## Phase 2: Time-Series Storage (GeoStorageSystem)

Uses **InfluxDB** for persistent time-series storage:

- **Dedicated thread** for batch writes
- In-memory circular buffer (10,000 points) for fast recent access
- InfluxDB v1 HTTP API for persistent storage
- Entity-indexed buffers for per-entity queries
- Time-range queries, trajectory retrieval
- Playback controller with variable speed

### InfluxDB Connection

```cpp
auto& storage = geoSystem.getStorageSystem();
storage.initialize("http://localhost", 8086, "geospatial");

// Batch write (automatic in thread)
storage.store(point);

// Query trajectory
auto history = storage.getTrajectory("gps_tracker", 1000);
```

## Phase 3: Prediction (GeoPredictionSystem)

**Dedicated thread** for prediction engine:

1. **Kalman Filter** (default): 2D position/velocity tracking
2. **TensorFlow Lite** (optional): ML-based prediction when model is loaded

### Kalman Filter Features
- Constant velocity model
- Growing uncertainty with prediction horizon
- Monte Carlo simulation (100+ simulations)
- Confidence scoring per predicted point
- Uncertainty ellipse calculation

### ML Prediction (TensorFlow Lite)

```cpp
auto& prediction = geoSystem.getPredictionSystem();
prediction.initialize(-33.8568, 151.2153, "geospatial/trajectory_predict.tflite");

// Async prediction (runs on thread)
prediction.requestPrediction("entity_1", timestamp, 60.0, 50,
    [](const PredictionResult& result) {
        // Handle prediction result
    });
```

## Phase 4: Visualization (GeoVisualizationSystem)

- **Runs on main thread** (OpenGL requirement)
- OpenGL trajectory line rendering (history + predictions)
- Color-coded confidence (green=high, red=low)
- Heatmap point rendering
- Uncertainty ellipse rendering
- Speed-based coloring for historical trajectories

## Phase 5: Terrain Integration (GeoTerrainSystem) - NEW!

- **Project geospatial coordinates onto 3D terrain**
- Map viewport with GPS track overlay (top-right corner)
- Height-colored terrain visualization
- LOD streaming based on camera position
- Multithreaded terrain chunk generation
- GPS trajectories draped over terrain surface

```cpp
// Project GPS coordinates to terrain
auto projection = geoTerrain.projectToTerrain(lat, lon, alt);
if (projection.onTerrain) {
    glm::vec3 terrainPos = projection.terrainPos;
    float height = projection.height;
}

// Auto-project ECS entities with GeospatialComponent
ecs::World world;
world.update(dt);  // Entities auto-snap to terrain
```

## GPS Tracker Modes

| Mode | Description | Speed |
|------|-------------|-------|
| `DISABLED` | No GPS output | - |
| `SIMULATED_STATIC` | Fixed position with noise | 0 m/s |
| `SIMULATED_WALK` | Figure-8 walking pattern | ~1.4 m/s |
| `SIMULATED_VEHICLE` | Circular vehicle path | ~7 m/s |
| `SIMULATED_AIRCRAFT` | Large circle at altitude | ~70 m/s |

## Digital Twin Use Cases

### 1. Fleet Tracking
- Ingest real-time GPS from multiple vehicles
- Visualize trajectories on 3D terrain
- Predict future positions with uncertainty

### 2. Drone Simulation
- Simulate aerial vehicles with altitude
- Plan flight paths with ML prediction
- Monitor battery, speed, and heading

### 3. Smart City
- Track public transport in real-time
- Analyze traffic patterns with time-series data
- Visualize heatmaps of activity

### 4. Environmental Monitoring
- Weather station data ingestion
- Ocean current prediction
- Wildlife tracking with GPS collars

## Dependencies

| Library | Purpose | Required |
|---------|---------|----------|
| libcurl | InfluxDB HTTP client, REST feeds | Yes |
| InfluxDB v1+ | Time-series persistence | Optional |
| libtensorflowlite_c | ML prediction | Optional |
| OpenGL 4.5 | Visualization | Yes |

## Known Limitations

1. **Single origin**: One origin per scene. Multi-origin requires origin rebasing.
2. **InfluxDB optional**: Falls back to memory-only if InfluxDB is not running.
3. **TensorFlow Lite optional**: Falls back to Kalman Filter if library/model not found.
4. **No map tiles**: Satellite imagery overlay not yet implemented.

## 🛠️ Build & Test

From the repository root:

```bash
make            # build bin/test_runner + bin/engine (debug)
make editor     # build bin/editor_app (ImGui editor)
make test       # run the full unit-test suite (731 tests / 99 suites, incl. Geo suites)
make run        # self-check tests, then boot the engine
make run-headless  # bounded headless engine run (CI-friendly)
make geoterrain-test   # bin/geoterrain_test — GeoTerrain Phase 5 demo app
```

The engine entry point (`make run`) initializes `geo::GeoAPI` at a Sydney origin with a
simulated GPS walking track and advances it every frame, alongside the character demo.

GeoHTTP server, NMEA feeds, GPS modes, Kalman/ML prediction and panel persistence are
covered by `make test` (the `GEO*` suites: `GeoAPIFeeds`, `GeoFeedNmea`, `GeoHTTPServer`,
`GeoPanelPersistence`, `GeoTerminalCommands`, `GeoTerminalNmea`). See the
[root README](../README.md#test-suites) for the full target list and prerequisites.

In the editor, click **GPS** to cycle modes and enable **Geo** to see the visualization
(map viewport in the top-right corner).
