#pragma once

#include <glm/glm.hpp>
#include <string>
#include <cmath>

namespace ecs {

struct GeospatialComponent {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;

    std::string coordinateSystem = "WGS84";
    double horizontalAccuracy = 0.0;
    double verticalAccuracy = 0.0;
    double timestamp = 0.0;

    std::string entityId = "";

    GeospatialComponent() = default;

    GeospatialComponent(double lat, double lon, double alt)
        : latitude(lat), longitude(lon), altitude(alt) {}

    bool isValid() const {
        return latitude >= -90.0 && latitude <= 90.0 &&
               longitude >= -180.0 && longitude <= 180.0;
    }

    void setLatLon(double lat, double lon) {
        latitude = lat;
        longitude = lon;
    }

    void setAltitude(double alt) {
        altitude = alt;
    }

    glm::dvec3 toDVec3() const {
        return glm::dvec3(latitude, longitude, altitude);
    }

    std::string toString() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "%.8f°, %.8f°, %.2fm",
                 latitude, longitude, altitude);
        return std::string(buf);
    }

    std::string toDMS() const {
        char buf[256];

        char latDir = latitude >= 0 ? 'N' : 'S';
        double latAbs = std::abs(latitude);
        int latDeg = (int)latAbs;
        int latMin = (int)((latAbs - latDeg) * 60);
        double latSec = (latAbs - latDeg - latMin / 60.0) * 3600;

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
