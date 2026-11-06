// =============================================================================
// SafetyGeofenceSystem - Mine-safety proximity geofencing
// -----------------------------------------------------------------------------
// Standalone, header-only geofence system. Monitors tracked entity snapshots and
// emits a ProximityAlert whenever a Personnel twin comes within a concentric
// horizontal safety cylinder around a HeavyMachinery twin (default 15m), with
// a per-pair cooldown so the EditorConsole is not flooded at 60Hz.
//
// Design notes:
//  * Decoupled from the editor layer: alerts are pushed through a configurable
//    AlertSink (std::function). The editor wires it to EditorConsole::Log; the
//    standalone test binary / diagnostic runs can wire it to stderr or a file.
//  * Distances use GeospatialConverter::haversineDistance (great-circle, metres)
//    plus a vertical delta from WGS84 altitude (metres), giving a true 3D
//    separation without dragging in a local-Euclidean frame.
//  * O(n * m) cross product of Personnel x HeavyMachinery. For a mine site this
//    is typically tiny (a handful of each), so no spatial hash is warranted.
//
// Todo 5 (Proximity Geofencing).
// =============================================================================
#ifndef GEO_SAFETY_GEOFENCE_H
#define GEO_SAFETY_GEOFENCE_H

#include "GeoAPI.h"              // EntitySnapshot, AssetKind, AssetKindToString
#include "GeospatialConverter.h" // haversineDistance

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace geo {

class SafetyGeofenceSystem {
public:
    struct ProximityAlert {
        uint32_t entityA;       // personnel id
        uint32_t entityB;       // heavy-machinery id
        AssetKind kindA;
        AssetKind kindB;
        double distanceMeters;  // 3D separation (horiz + vert)
        double horizontalMeters;
        double verticalDeltaM;
        double timestamp;
        int level;              // 0=info, 1=warning, 2=critical

        // --- Evacuation interlock vector (forGeo.txt §2) ---
        // escapeBearingDeg is the bearing (clockwise from North) the personnel
        // twin should move along to EXIT the concentric safety cylinder: it is
        // the reciprocal of the machinery->personnel bearing. The evacuation
        // target (evacLatitude/Longitude/Altitude) is one safety radius stepped
        // outward from the personnel along that bearing, giving a concrete WGS84
        // fix to steer toward. Surfaced on the dashboard + recorded to the
        // time-series DB as a verifiable evacuation trail.
        double escapeBearingDeg    = 0.0;
        double evacLatitude        = 0.0;
        double evacLongitude       = 0.0;
        double evacAltitude        = 0.0;
        double evacuationDistance  = 0.0;
    };

    using AlertSink = std::function<void(const ProximityAlert&)>;

    SafetyGeofenceSystem() = default;

    // Where alerts are delivered (e.g. EditorConsole::Log). Empty = silent.
    void setAlertSink(AlertSink s) { sink_ = std::move(s); }

    // Concentric safety cylinder radius in metres (horizontal).
    void setSafetyRadiusMeters(float r) { safetyRadius_ = r; }

    // Minimum seconds between two alerts for the same (personnel, machinery) pair.
    void setPairCooldownSeconds(double s) { cooldownSec_ = s; }

    // Evaluate the full snapshot population. Returns the number of alerts
    // emitted during this call (subject to the per-pair cooldown). Safe to
    // call every frame from the geo sync block.
    size_t checkMineSafetyConcentric(const std::vector<EntitySnapshot>& snaps,
                                     double timestamp) {
        recent_.clear();
        size_t emitted = 0;

        // Index Personnel first, then cross against HeavyMachinery (and vice
        // versa via the single j-loop), classifying each valid twin.
        // Personnel/HeavyMachinery twins only. A zeroed position (lat==0 &&
        // lon==0) means the twin has no live telemetry snapshot yet (e.g. a
        // headless run with no feed) — reject it so the zeroed default position
        // doesn't trigger a bogus "0.000000m apart" proximity alert between two
        // cold-started twins. A real surface twin is never exactly at 0,0.
        const auto classify = [](const EntitySnapshot& s) -> bool {
            return s.isValid &&
                   (s.latitude != 0.0 || s.longitude != 0.0) &&
                   (s.kind == AssetKind::Personnel || s.kind == AssetKind::HeavyMachinery);
        };

        for (size_t i = 0; i < snaps.size(); ++i) {
            const EntitySnapshot& a = snaps[i];
            if (!classify(a)) continue;
            const bool aIsPersonnel = (a.kind == AssetKind::Personnel);

            for (size_t j = i + 1; j < snaps.size(); ++j) {
                const EntitySnapshot& b = snaps[j];
                if (!classify(b)) continue;
                const bool bIsPersonnel = (b.kind == AssetKind::Personnel);
                // Only Personnel <-> HeavyMachinery pairs are safety-relevant.
                if (aIsPersonnel == bIsPersonnel) continue;

                const double horiz = GeospatialConverter::haversineDistance(
                    a.latitude, a.longitude, b.latitude, b.longitude);
                if (horiz > static_cast<double>(safetyRadius_)) continue;

                const double vert = std::fabs(a.altitude - b.altitude);
                const double dist3d = std::sqrt(horiz * horiz + vert * vert);

                // Stable per-pair key (order-independent).
                const uint64_t key =
                    (static_cast<uint64_t>(std::min(a.id, b.id)) << 32) |
                     static_cast<uint64_t>(std::max(a.id, b.id));

                // Rate-limit per pair: only suppress if this pair was alerted
                // within the cooldown window. A pair never alerted before is
                // always allowed through (the map default of 0.0 must not be
                // mistaken for a real prior timestamp).
                auto it = cooldown_.find(key);
                if (it != cooldown_.end() &&
                    (timestamp - it->second) < cooldownSec_) {
                    continue;
                }

                ProximityAlert alert{};
                alert.entityA        = aIsPersonnel ? a.id : b.id;
                alert.entityB        = aIsPersonnel ? b.id : a.id;
                alert.kindA          = aIsPersonnel ? a.kind : b.kind;
                alert.kindB          = aIsPersonnel ? b.kind : a.kind;
                alert.distanceMeters  = dist3d;
                alert.horizontalMeters = horiz;
                alert.verticalDeltaM  = vert;
                alert.timestamp       = timestamp;
                // Escalate to critical inside half the safety radius.
                alert.level = (dist3d < static_cast<double>(safetyRadius_) * 0.5) ? 2 : 1;

                // ---- Evacuation interlock vector (forGeo.txt §2) ----
                // Resolve which snapshot is the personnel twin (entityA) so the
                // escape bearing/target are anchored to the crew, not the mech.
                const EntitySnapshot& pers = aIsPersonnel ? a : b;
                const EntitySnapshot& mach = aIsPersonnel ? b : a;
                // Reciprocal evacuation heading: invert the inbound bearing
                // personnel->machinery so the crew moves AWAY from the machinery,
                // out of the concentric safety cylinder (the "+180" reciprocal is
                // exactly the "escape" direction at the personnel's location).
                const double inboundBearing = GeospatialConverter::bearing(
                    pers.latitude, pers.longitude, mach.latitude, mach.longitude);
                alert.escapeBearingDeg = std::fmod(inboundBearing + 180.0, 360.0);
                // Step one safety radius outward from the personnel along the
                // evacuation bearing: guarantees exit from the 15 m cylinder.
                alert.evacuationDistance = static_cast<double>(safetyRadius_);
                GeospatialConverter::destinationPoint(
                    pers.latitude, pers.longitude,
                    alert.escapeBearingDeg, alert.evacuationDistance,
                    alert.evacLatitude, alert.evacLongitude);
                alert.evacAltitude = pers.altitude;

                if (sink_) sink_(alert);
                recent_.push_back(alert);
                cooldown_[key] = timestamp;
                ++emitted;
            }
        }
        return emitted;
    }

    // Alerts fired during the most recent checkMineSafetyConcentric() call.
    const std::vector<ProximityAlert>& recentAlerts() const { return recent_; }

private:
    AlertSink sink_;
    float safetyRadius_ = 15.0f;
    double cooldownSec_ = 1.0;
    std::unordered_map<uint64_t, double> cooldown_;  // pair key -> next-allowed t
    std::vector<ProximityAlert> recent_;
};

} // namespace geo

#endif // GEO_SAFETY_GEOFENCE_H
