# Geospatial System

Real-time geospatial tracking for the RTT Engine. Converts WGS84 geographic coordinates (latitude/longitude/altitude) to local engine space using double-precision math, avoiding float precision issues entirely.

## 📁 Files

```
geospatial/
├── GeospatialConverter.h    # WGS84 ↔ ECEF ↔ ENU conversion engine
├── GPSTracker.h             # Simulated GPS feed (4 modes)
└── README.md                # This file

ecs/components/
└── GeospatialComponent.h    # Double-precision lat/lon/alt component

ecs/systems/
└── GeospatialSystem.h       # ECS system bridging GPS with entities
```

## 🏗️ Architecture

### Zero Float Precision Debt

The key design decision: **geospatial data stays `double`, existing systems stay `float`**.

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

## 🚀 Usage

### Basic Setup

```cpp
#include "ecs/components/GeospatialComponent.h"
#include "ecs/systems/GeospatialSystem.h"
#include "geospatial/GeospatialConverter.h"
#include "geospatial/GPSTracker.h"

// In your engine initialization:
ecs::GeospatialSystem geoSystem;

// Set origin (e.g., Sydney Opera House)
geoSystem.initialize(-33.8568, 151.2153, 50.0);

// Set GPS simulation mode
geoSystem.setGPSMode(GPSTracker::Mode::SIMULATED_WALK);

// Add to ECS world
world.addSystem(&geoSystem);
```

### Per-Frame Update

```cpp
// In your main loop (handled automatically if added to ECS world):
world.update(dt);

// Or manually:
geoSystem.update(dt);
const GPSFix& fix = geoSystem.getCurrentGPSFix();
```

### Converting Coordinates

```cpp
// WGS84 → Local space
glm::vec3 localPos = geoSystem.getConverter().geospatialToLocal(
    -33.8570,  // latitude
    151.2155,  // longitude
    55.0       // altitude (meters)
);

// Local space → WGS84
glm::dvec3 geoPos = geoSystem.getConverter().localToGeospatial(localPos);
// geoPos.x = latitude, geoPos.y = longitude, geoPos.z = altitude
```

### Creating Geospatial Entities

```cpp
auto entity = world.createEntityWithComponents<
    ecs::TransformComponent,
    ecs::GeospatialComponent
>();

auto* transform = world.getComponentArchetype<ecs::TransformComponent>(entity);
auto* geo = world.getComponentArchetype<ecs::GeospatialComponent>(entity);

// Set geographic position
geo->setLatLon(-33.8570, 151.2155);
geo->setAltitude(55.0);
geo->entityId = "player_1";

// Convert to local space and update transform
geoSystem.updateEntityFromGeospatial(entity.id, *geo, *transform);
```

## 📡 GPS Tracker Modes

| Mode | Description | Use Case |
|------|-------------|----------|
| `DISABLED` | No GPS output | Testing without geospatial |
| `SIMULATED_STATIC` | Fixed position with realistic noise | Stationary entity testing |
| `SIMULATED_WALK` | Figure-8 walking pattern (~1.4 m/s) | Pedestrian simulation |
| `SIMULATED_VEHICLE` | Circular vehicle path (~7 m/s) | Vehicle simulation |
| `SIMULATED_AIRCRAFT` | Large circle at altitude (~70 m/s) | Aircraft simulation |

### Configuring GPS

```cpp
// Change mode
geoSystem.setGPSMode(GPSTracker::Mode::SIMULATED_VEHICLE);

// Adjust speed (meters/second)
geoSystem.setGPSSpeed(15.0);  // 15 m/s = 54 km/h

// Adjust noise level (meters of position jitter)
geoSystem.setGPSNoise(5.0f);  // 5m accuracy (poor GPS)
```

## 📐 Coordinate Math

### Distance Between Points

```cpp
double dist = GeospatialConverter::haversineDistance(
    lat1, lon1,  // Point A
    lat2, lon2   // Point B
);
// Returns distance in meters
```

### Bearing Between Points

```cpp
double bearing = GeospatialConverter::bearing(
    lat1, lon1,  // From
    lat2, lon2   // To
);
// Returns degrees clockwise from North (0-360)
```

## 🎮 Editor Integration

### Toolbar
- **GPS Button**: Cycles through GPS simulation modes
- Hover for current mode tooltip

### Left Panel → "Geo" Tab
- GPS mode and satellite count
- WGS84 position (decimal degrees)
- DMS format display
- Speed, heading, simulation time
- Distance and bearing from origin

### Viewport Overlay
- Camera position shown in top-left corner
- Wireframe mode indicator

## 🔬 Technical Details

### WGS84 Ellipsoid Parameters

| Parameter | Value |
|-----------|-------|
| Semi-major axis (a) | 6,378,137.0 m |
| Flattening (f) | 1/298.257223563 |
| Semi-minor axis (b) | 6,356,752.3142 m |
| Eccentricity² (e²) | 0.00669437999014 |

### ECEF Conversion

Earth-Centered Earth-Fixed coordinates use the standard WGS84 ellipsoid model:

```
X = (N + h) · cos(φ) · cos(λ)
Y = (N + h) · cos(φ) · sin(λ)
Z = (N·(1-e²) + h) · sin(φ)
```

Where:
- φ = latitude, λ = longitude, h = altitude
- N = radius of curvature in the prime vertical

### ENU Convention

The converter uses East-North-Up convention, mapped to engine space as:
- **East → +X**
- **Up → +Y**
- **South → +Z** (negative North)

This maintains a right-handed coordinate system consistent with OpenGL conventions.

## 🛣️ Future Phases

### Phase 2 (Planned)
- Origin rebasing for large-scale worlds
- Real terrain heightmap import (SRTM data)
- Satellite imagery overlay
- Large-scale world chunking

### Phase 3 (Planned)
- Network layer for live data feeds
- Real GPS device integration (NMEA serial)
- Time-series entity tracking
- Multi-entity geospatial queries

## 🐛 Known Limitations

1. **Single origin**: Currently supports one origin per scene. Multi-origin (continental scale) requires origin rebasing (Phase 2).

2. **Simulated GPS only**: Real GPS device support planned for Phase 3.

3. **No map tiles**: Satellite imagery overlay not yet implemented.

## 📚 References

- [WGS84 Standard](https://earth-info.nga.mil/GandG/wgs84.html)
- [ECEF to ENU Conversion](https://gssc.esa.int/navipedia/index.php/Transformations_between_ECEF_and_ENU_coordinates)
- [Haversine Formula](https://en.wikipedia.org/wiki/Haversine_formula)
- [Bowring's Method](https://en.wikipedia.org/wiki/Geographic_coordinate_conversion)
