#pragma once

/**
 * Geospatial Component - Real-world coordinates stored in double precision
 *
 * This component stores WGS84 geographic coordinates (lat/lon/alt) as doubles.
 * Conversion to local engine space (float) is handled by GeospatialConverter
 * relative to a local origin, avoiding float precision issues entirely.
 *
 * Existing engine systems (Transform, Physics, Renderer) remain float-based
 * and operate in local space relative to the converter's origin.
 */

#include <glm/glm.hpp>
#include <string>

namespace ecs {

struct GeospatialComponent {
    // WGS84 coordinates (double precision)
    double latitude = 0.0;    // Degrees: -90 to +90
    double longitude = 0.0;   // Degrees: -180 to +180
    double altitude = 0.0;    // Meters above WGS84 ellipsoid

    // Optional metadata
    std::string coordinateSystem = "WGS84";
    double horizontalAccuracy = 0.0;  // Meters (0 = unknown)
    double verticalAccuracy = 0.0;    // Meters (0 = unknown)
    double timestamp = 0.0;           // Unix epoch seconds (0 = not set)

    // Entity identifier for tracking
    std::string entityId = "";

    GeospatialComponent() = default;

    GeospatialComponent(double lat, double lon, double alt)
        : latitude(lat), longitude(lon), altitude(alt) {}

    // Validation
    bool isValid() const {
        return latitude >= -90.0 && latitude <= 90.0 &&
               longitude >= -180.0 && longitude <= 180.0;
    }

    // Set from degrees
    void setLatLon(double lat, double lon) {
        latitude = lat;
        longitude = lon;
    }

    // Set altitude
    void setAltitude(double alt) {
        altitude = alt;
    }

    // Get as glm::dvec3 (lat, lon, alt)
    glm::dvec3 toDVec3() const {
        return glm::dvec3(latitude, longitude, altitude);
    }

    // Get display string
    std::string toString() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "%.8f°, %.8f°, %.2fm",
                 latitude, longitude, altitude);
        return std::string(buf);
    }

    // DMS (Degrees Minutes Seconds) format
    std::string toDMS() const {
        char buf[256];

        // Latitude
        char latDir = latitude >= 0 ? 'N' : 'S';
        double latAbs = std::abs(latitude);
        int latDeg = (int)latAbs;
        int latMin = (int)((latAbs - latDeg) * 60);
        double latSec = (latAbs - latDeg - latMin / 60.0) * 3600;

        // Longitude
        char lonDir = longitude >= 0 ? 'E' : 'W';
        double lonAbs = std::abs(longitude);
        int lonDeg = (int)lonAbs;
        int lonMin = (int)((lonAbs - lonDeg) * 60);
        double lonSec = (lonAbs - lonDeg - lonMin / 60.0) * 3600;

        snprintf(buf, sizeof(buf),
                 "%d°%d'%.2f\"%c %d°%d'%.2f\"%c ALT:%.2fm",
                 latDeg, latMin, latSec, latDir,
                 lonDeg, lonMin, lonSec, lonDir,
                 altitude);
        return std::string(buf);
    }
};

} // namespace ecs
