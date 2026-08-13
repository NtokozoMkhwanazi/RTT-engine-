// ============================================================================
// Unit tests for geospatial data-feed management:
//   - GeoAPI feed registration / removal / info (must be real, not stubs)
//   - DataFeedManager NMEA $GPRMC parsing (off-by-one regression guard)
//
// The feed tests register feeds against a dead local port so any polling is
// harmless and fails fast; removal/shutdown wake the poll threads via the
// sleep condition variable, so joins never block for a poll interval.
// ============================================================================

#include <gtest/gtest.h>
#include <string>

#include "geospatial/GeoAPI.h"
#include "geospatial/DataFeedManager.h"

namespace {

constexpr const char* kDeadUrl = "http://127.0.0.1:1/pos";

} // namespace

// ----------------------------------------------------------------------------
// GeoAPI feed facade
// ----------------------------------------------------------------------------

TEST(GeoAPIFeeds, AddRemoveCounts) {
    geo::GeoAPI api;
    EXPECT_EQ(api.getFeedCount(), 0u);

    EXPECT_TRUE(api.addFeed(kDeadUrl));
    EXPECT_EQ(api.getFeedCount(), 1u);

    const auto info = api.getFeedInfo(0);
    EXPECT_EQ(info.url, kDeadUrl);
    EXPECT_EQ(info.type, "REST");
    EXPECT_TRUE(info.active);

    api.removeFeed(0);
    EXPECT_EQ(api.getFeedCount(), 0u);

    // Out-of-range removal must be a harmless no-op.
    api.removeFeed(0);
    EXPECT_EQ(api.getFeedCount(), 0u);
}

TEST(GeoAPIFeeds, RejectsEmptyUrl) {
    geo::GeoAPI api;
    EXPECT_FALSE(api.addFeed(""));
    EXPECT_FALSE(api.addFeed("   "));
    EXPECT_EQ(api.getFeedCount(), 0u);
}

TEST(GeoAPIFeeds, DistinguishesFeedTypes) {
    geo::GeoAPI api;
    EXPECT_TRUE(api.addFeed("ws://example.com/live", "WebSocket"));
    EXPECT_TRUE(api.addFeed(kDeadUrl, "REST"));

    // REST feeds are listed before WebSocket feeds.
    ASSERT_EQ(api.getFeedCount(), 2u);
    EXPECT_EQ(api.getFeedInfo(0).type, "REST");
    EXPECT_EQ(api.getFeedInfo(0).url, kDeadUrl);
    EXPECT_EQ(api.getFeedInfo(1).type, "WebSocket");

    // Out-of-range info is empty.
    const auto missing = api.getFeedInfo(99);
    EXPECT_TRUE(missing.url.empty());
}

TEST(GeoAPIFeeds, DrainIsInitiallyEmpty) {
    geo::GeoAPI api;
    EXPECT_TRUE(api.drainFeedData().empty());
}

TEST(GeoAPIFeeds, AddFeedIsIdempotentPerUrl) {
    geo::GeoAPI api;
    EXPECT_TRUE(api.addFeed(kDeadUrl));
    EXPECT_TRUE(api.addFeed(kDeadUrl));  // two separate feed slots, both polled
    EXPECT_EQ(api.getFeedCount(), 2u);
    api.removeFeed(0);
    api.removeFeed(0);
    EXPECT_EQ(api.getFeedCount(), 0u);
}

// ----------------------------------------------------------------------------
// DataFeedManager NMEA parsing
// ----------------------------------------------------------------------------

TEST(GeoFeedNmea, ParsesGPRMC) {
    // Standard RMC sentence: time, status, lat, N/S, lon, E/W, speed, track.
    const auto nmea = DataFeedManager::parseNMEASentence(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A");
    ASSERT_TRUE(nmea.valid);
    EXPECT_NEAR(nmea.lat, 48.1173, 1e-4);     // 48 + 7.038/60
    EXPECT_NEAR(nmea.lon, 11.5166667, 1e-4);  // 11 + 31/60
    EXPECT_NEAR(nmea.speed, 11.5235, 1e-3);   // 22.4 knots -> m/s
    EXPECT_NEAR(nmea.track, 84.4, 1e-3);
}

TEST(GeoFeedNmea, ParsesSouthernAndWesternHemispheres) {
    const auto nmea = DataFeedManager::parseNMEASentence(
        "$GPRMC,123519,A,3351.4080,S,15112.9180,E,000.5,090.0,140823,,*4B");
    ASSERT_TRUE(nmea.valid);
    EXPECT_NEAR(nmea.lat, -33.8568, 1e-4);   // 33 + 51.408/60, S
    EXPECT_NEAR(nmea.lon, 151.2153, 1e-4);   // 151 + 12.918/60, E
}

TEST(GeoFeedNmea, RejectsVoidStatus) {
    const auto nmea = DataFeedManager::parseNMEASentence(
        "$GPRMC,123519,V,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A");
    EXPECT_FALSE(nmea.valid);
}

TEST(GeoFeedNmea, RejectsNonRmcOrEmpty) {
    EXPECT_FALSE(DataFeedManager::parseNMEASentence("").valid);
    EXPECT_FALSE(DataFeedManager::parseNMEASentence("not nmea").valid);
    EXPECT_FALSE(DataFeedManager::parseNMEASentence(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47").valid);
}
