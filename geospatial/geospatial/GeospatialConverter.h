#pragma once

/**
 * Geospatial Converter - WGS84 ↔ Local Engine Space
 *
 * Bridges double-precision geographic coordinates to float-based engine space.
 * Uses ECEF (Earth-Centered Earth-Fixed) → ENU (East-North-Up) conversion
 * relative to a configurable local origin.
 *
 * KEY DESIGN: Existing engine systems never see doubles. They operate in
 * local float space relative to the origin. No precision debt incurred.
 *
 * Conversion pipeline:
 *   WGS84 (lat/lon/alt, double) → ECEF (x/y/z, double) → ENU (e/n/u, float) → Engine Space
 *   Engine Space (x/y/z, float) → ENU (e/n/u, float) → ECEF (x/y/z, double) → WGS84
 *
 * Usage:
 *   GeospatialConverter converter;
 *   converter.setOrigin(-33.8688, 151.2093, 50.0);  // Sydney Opera House
 *   glm::vec3 localPos = converter.geospatialToLocal(-33.8690, 151.2100, 55.0);
 *   glm::dvec3 geoPos = converter.localToGeospatial(localPos);
 */

#include <glm/glm.hpp>
#include <cmath>
#include <iostream>

// WGS84 Ellipsoid Constants
static constexpr double WGS84_A = 6378137.0;              // Semi-major axis (meters)
static constexpr double WGS84_F = 1.0 / 298.257223563;    // Flattening
static constexpr double WGS84_B = WGS84_A * (1.0 - WGS84_F);  // Semi-minor axis
static constexpr double WGS84_E2 = 2.0 * WGS84_F - WGS84_F * WGS84_F;  // First eccentricity squared

class GeospatialConverter {
public:
    GeospatialConverter() = default;

    /**
     * Set the local origin in WGS84 coordinates.
     * All local space positions are relative to this point.
     * Call this once when loading a geospatial scene.
     */
    void setOrigin(double lat, double lon, double alt) {
        originLat = lat;
        originLon = lon;
        originAlt = alt;
        originECEF = latLonAltToECEF(lat, lon, alt);
        originSet = true;

        std::cout << "[GeospatialConverter] Origin set: "
                  << lat << "°, " << lon << "°, " << alt << "m\n";
    }

    /**
     * Convert WGS84 coordinates to local engine space (float).
     * Returns position in meters relative to the origin.
     * Convention: +X = East, +Y = Up, +Z = South (right-handed)
     */
    glm::vec3 geospatialToLocal(double lat, double lon, double alt) const {
        if (!originSet) {
            std::cerr << "[GeospatialConverter] Origin not set! Returning zero.\n";
            return glm::vec3(0.0f);
        }

        // Convert to ECEF
        glm::dvec3 ecef = latLonAltToECEF(lat, lon, alt);

        // Convert ECEF to ENU relative to origin
        glm::dvec3 enu = ecefToENU(ecef);

        // ENU → Engine space: +X=East, +Y=Up, +Z=South (ENU is +X=East, +Y=North, +Z=Up)
        // We map: East→X, Up→Y, -North→Z (so +Z faces south, right-handed)
        return glm::vec3(
            static_cast<float>(enu.x),   // East → X
            static_cast<float>(enu.z),   // Up → Y
            static_cast<float>(-enu.y)   // -North → Z
        );
    }

    /**
     * Convert local engine space (float) back to WGS84 coordinates (double).
     */
    glm::dvec3 localToGeospatial(const glm::vec3& localPos) const {
        if (!originSet) {
            std::cerr << "[GeospatialConverter] Origin not set! Returning origin.\n";
            return glm::dvec3(originLat, originLon, originAlt);
        }

        // Engine space → ENU: X=East, Y=Up, Z=-North
        glm::dvec3 enu(
            static_cast<double>(localPos.x),    // East
            static_cast<double>(-localPos.z),   // North = -Z
            static_cast<double>(localPos.y)     // Up
        );

        // ENU → ECEF
        glm::dvec3 ecef = enuToECEF(enu);

        // ECEF → WGS84
        return ecefToLatLonAlt(ecef);
    }

    /**
     * Convert a GeospatialComponent to local space.
     */
    template<typename GeoComponent>
    glm::vec3 componentToLocal(const GeoComponent& geo) const {
        return geospatialToLocal(geo.latitude, geo.longitude, geo.altitude);
    }

    /**
     * Get the origin in WGS84.
     */
    glm::dvec3 getOriginWGS84() const {
        return glm::dvec3(originLat, originLon, originAlt);
    }

    double getOriginLat() const { return originLat; }
    double getOriginLon() const { return originLon; }
    double getOriginAlt() const { return originAlt; }

    /**
     * Get the origin in ECEF.
     */
    glm::dvec3 getOriginECEF() const {
        return originECEF;
    }

    /**
     * Check if origin has been set.
     */
    bool isOriginSet() const { return originSet; }

    /**
     * Calculate distance between two WGS84 points (Haversine, meters).
     * Useful for validation and UI display.
     */
    static double haversineDistance(double lat1, double lon1,
                                     double lat2, double lon2) {
        constexpr double R = 6371000.0;  // Earth radius in meters
        double dLat = deg2rad(lat2 - lat1);
        double dLon = deg2rad(lon2 - lon1);

        double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
                   std::cos(deg2rad(lat1)) * std::cos(deg2rad(lat2)) *
                   std::sin(dLon / 2) * std::sin(dLon / 2);
        double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));

        return R * c;
    }

    /**
     * Calculate bearing from point 1 to point 2 (degrees, clockwise from North).
     */
    static double bearing(double lat1, double lon1, double lat2, double lon2) {
        double dLon = deg2rad(lon2 - lon1);
        double lat1Rad = deg2rad(lat1);
        double lat2Rad = deg2rad(lat2);

        double x = std::sin(dLon) * std::cos(lat2Rad);
        double y = std::cos(lat1Rad) * std::sin(lat2Rad) -
                   std::sin(lat1Rad) * std::cos(lat2Rad) * std::cos(dLon);

        return rad2deg(std::atan2(x, y));
    }

private:
    // Origin state
    double originLat = 0.0;
    double originLon = 0.0;
    double originAlt = 0.0;
    glm::dvec3 originECEF{0.0};
    bool originSet = false;

    // Utility
    static double deg2rad(double deg) { return deg * M_PI / 180.0; }
    static double rad2deg(double rad) { return rad * 180.0 / M_PI; }

    /**
     * WGS84 (lat/lon/alt) → ECEF (x/y/z in meters)
     */
    static glm::dvec3 latLonAltToECEF(double lat, double lon, double alt) {
        double latRad = deg2rad(lat);
        double lonRad = deg2rad(lon);

        double sinLat = std::sin(latRad);
        double cosLat = std::cos(latRad);
        double sinLon = std::sin(lonRad);
        double cosLon = std::cos(lonRad);

        // Radius of curvature in the prime vertical
        double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sinLat * sinLat);

        return glm::dvec3(
            (N + alt) * cosLat * cosLon,
            (N + alt) * cosLat * sinLon,
            (N * (1.0 - WGS84_E2) + alt) * sinLat
        );
    }

    /**
     * ECEF → WGS84 (lat/lon/alt)
     * Uses Bowring's method for iterative convergence.
     */
    static glm::dvec3 ecefToLatLonAlt(const glm::dvec3& ecef) {
        double x = ecef.x;
        double y = ecef.y;
        double z = ecef.z;

        double p = std::sqrt(x * x + y * y);
        double lon = std::atan2(y, x);

        // Initial guess
        double lat = std::atan2(z, p * (1.0 - WGS84_E2));
        double alt = 0.0;

        // Iterate (usually converges in 2-3 iterations)
        for (int i = 0; i < 5; i++) {
            double sinLat = std::sin(lat);
            double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sinLat * sinLat);
            alt = p / std::cos(lat) - N;
            lat = std::atan2(z, p * (1.0 - WGS84_E2 * N / (N + alt)));
        }

        return glm::dvec3(rad2deg(lat), rad2deg(lon), alt);
    }

    /**
     * ECEF → ENU relative to the origin.
     * Returns (East, North, Up) in meters.
     */
    glm::dvec3 ecefToENU(const glm::dvec3& ecef) const {
        glm::dvec3 delta = ecef - originECEF;

        double latRad = deg2rad(originLat);
        double lonRad = deg2rad(originLon);

        double sinLat = std::sin(latRad);
        double cosLat = std::cos(latRad);
        double sinLon = std::sin(lonRad);
        double cosLon = std::cos(lonRad);

        // Rotation matrix from ECEF to ENU
        double east  = -sinLon * delta.x + cosLon * delta.y;
        double north = -sinLat * cosLon * delta.x - sinLat * sinLon * delta.y + cosLat * delta.z;
        double up    =  cosLat * cosLon * delta.x + cosLat * sinLon * delta.y + sinLat * delta.z;

        return glm::dvec3(east, north, up);
    }

    /**
     * ENU → ECEF relative to the origin.
     */
    glm::dvec3 enuToECEF(const glm::dvec3& enu) const {
        double latRad = deg2rad(originLat);
        double lonRad = deg2rad(originLon);

        double sinLat = std::sin(latRad);
        double cosLat = std::cos(latRad);
        double sinLon = std::sin(lonRad);
        double cosLon = std::cos(lonRad);

        // Inverse rotation matrix (ENU to ECEF)
        double dx = -sinLon * enu.x - sinLat * cosLon * enu.y + cosLat * cosLon * enu.z;
        double dy =  cosLon * enu.x - sinLat * sinLon * enu.y + cosLat * sinLon * enu.z;
        double dz =  cosLat * enu.y + sinLat * enu.z;

        return originECEF + glm::dvec3(dx, dy, dz);
    }
};
