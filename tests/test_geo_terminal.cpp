// ============================================================================
// Unit tests for the Geo Terminal pure logic layer:
//   - NmeaChecksum / BuildNmeaGGA / BuildNmeaRMC (NMEA-0183 sentence builders)
//   - ExecuteGeoCommand (terminal command executor against a GeoAPI facade)
//
// These functions are GL/ImGui-free by design (see editor/geo_terminal.h) so
// they can be driven directly from a gtest binary with no windowing context.
// ============================================================================

#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include <vector>

#include "editor/geo_terminal.h"
#include "geospatial/GPSTracker.h"

namespace {

using UI::ExecuteGeoCommand;
using UI::GeoTerminalState;
using UI::BuildNmeaGGA;
using UI::BuildNmeaRMC;
using UI::NmeaChecksum;

// A plausible live fix for sentence-builder tests (Sydney, walking east).
geo::GPSStatus MakeFix(bool valid = true) {
    geo::GPSStatus st{};
    st.latitude = -33.8568;
    st.longitude = 151.2153;
    st.altitude = 50.0;
    st.speed = 1.4;
    st.heading = 90.0;
    st.timestamp = 1700000000.0;  // 2023-11-14 22:13:20 UTC
    st.isValid = valid;
    return st;
}

// Parse the "*XX" checksum suffix off a "$...*XX" sentence.
unsigned ParseChecksumHex(const std::string& sentence) {
    const size_t star = sentence.rfind('*');
    EXPECT_NE(star, std::string::npos);
    return static_cast<unsigned>(std::strtoul(sentence.substr(star + 1).c_str(), nullptr, 16));
}

// Extract the body between '$' and '*' and re-compute its checksum.
unsigned RecomputedChecksum(const std::string& sentence) {
    const size_t star = sentence.rfind('*');
    EXPECT_GT(star, 1u);
    return NmeaChecksum(sentence.substr(1, star - 1));
}

} // namespace

// ============================================================================
// NmeaChecksum
// ============================================================================

TEST(GeoTerminalNmea, ChecksumKnownValue) {
    // Canonical NMEA example from the GGA spec: body XOR == 0x47.
    const std::string body =
        "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,";
    EXPECT_EQ(NmeaChecksum(body), 0x47);
}

TEST(GeoTerminalNmea, ChecksumEmptyBody) {
    EXPECT_EQ(NmeaChecksum(""), 0x00);
}

TEST(GeoTerminalNmea, ChecksumIsXorOfEveryByte) {
    // XOR is order-insensitive and self-inverse: checksum(b) ^ checksum(c)
    // must equal checksum(concatenation) because xor groups.
    const std::string a = "GPGGA,123519,4807.038,";
    const std::string b = "N,01131.000,E,1,08";
    EXPECT_EQ(NmeaChecksum(a + b), NmeaChecksum(a) ^ NmeaChecksum(b));
}

// ============================================================================
// BuildNmeaGGA
// ============================================================================

TEST(GeoTerminalNmea, GGAFormatWithValidFix) {
    const std::string s = BuildNmeaGGA(MakeFix(true));
    EXPECT_EQ(s.substr(0, 7), "$GPGGA,");
    // Sentence ends with the two-hex-digit checksum.
    EXPECT_EQ(s.size() - s.rfind('*'), 3);
    const char last = s.back();
    EXPECT_TRUE((last >= '0' && last <= '9') || (last >= 'A' && last <= 'F'));
    EXPECT_EQ(ParseChecksumHex(s), RecomputedChecksum(s));
    // Fix quality 1 = valid fix; hemisphere letters for the S/E sample.
    EXPECT_NE(s.find(",1,12,"), std::string::npos);
    EXPECT_NE(s.find(",S,"), std::string::npos);
    EXPECT_NE(s.find(",E,"), std::string::npos);
}

TEST(GeoTerminalNmea, GGAFormatWithoutFix) {
    const std::string s = BuildNmeaGGA(MakeFix(false));
    EXPECT_EQ(s.substr(0, 7), "$GPGGA,");
    EXPECT_NE(s.find(",0,0,"), std::string::npos);  // quality 0, 0 satellites
    EXPECT_EQ(ParseChecksumHex(s), RecomputedChecksum(s));
}

TEST(GeoTerminalNmea, GGAFormatsLatLonAsDdm) {
    // -33.8568 -> "3351.4080,S"; 151.2153 -> "15112.9180,E"
    const std::string s = BuildNmeaGGA(MakeFix(true));
    EXPECT_NE(s.find("3351.4080,S"), std::string::npos);
    EXPECT_NE(s.find("15112.9180,E"), std::string::npos);
}

// ============================================================================
// BuildNmeaRMC
// ============================================================================

TEST(GeoTerminalNmea, RMCFormatActive) {
    const std::string s = BuildNmeaRMC(MakeFix(true));
    EXPECT_EQ(s.substr(0, 7), "$GPRMC,");
    EXPECT_EQ(ParseChecksumHex(s), RecomputedChecksum(s));
    EXPECT_NE(s.find(",A,"), std::string::npos);  // A = active/valid
    EXPECT_NE(s.find("3351.4080,S"), std::string::npos);
    EXPECT_NE(s.find("15112.9180,E"), std::string::npos);
}

TEST(GeoTerminalNmea, RMCFormatVoid) {
    const std::string s = BuildNmeaRMC(MakeFix(false));
    EXPECT_EQ(s.substr(0, 7), "$GPRMC,");
    EXPECT_EQ(ParseChecksumHex(s), RecomputedChecksum(s));
    EXPECT_NE(s.find(",V,"), std::string::npos);  // V = void
}

TEST(GeoTerminalNmea, RMCConvertsSpeedToKnots) {
    // 1.4 m/s * 1.943844 ~= 2.7 knots -> "2.7"
    const std::string s = BuildNmeaRMC(MakeFix(true));
    EXPECT_NE(s.find(",2.7,"), std::string::npos);
}

// ============================================================================
// ExecuteGeoCommand
// ============================================================================

TEST(GeoTerminalCommands, HelpListsCommands) {
    geo::GeoAPI api;
    GeoTerminalState state;
    const auto out = ExecuteGeoCommand("help", api, state);
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.front(), "Geo Terminal - NMEA-style control for the geospatial pipeline");
    // Every line lands in the scrollback too.
    EXPECT_EQ(state.output.size(), out.size());
}

TEST(GeoTerminalCommands, UnknownCommandIsTaggedError) {
    geo::GeoAPI api;
    GeoTerminalState state;
    const auto out = ExecuteGeoCommand("frobnicate", api, state);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out.front().rfind("[ERR]", 0), 0u);
}

TEST(GeoTerminalCommands, EchoJoinsArgs) {
    geo::GeoAPI api;
    GeoTerminalState state;
    const auto out = ExecuteGeoCommand("echo hello   world", api, state);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out.front(), "hello world");
}

TEST(GeoTerminalCommands, ModeSetsTracker) {
    geo::GeoAPI api;
    GeoTerminalState state;
    ExecuteGeoCommand("mode 2", api, state);
    EXPECT_EQ(api.getGPSStatus().mode, GPSTracker::Mode::SIMULATED_WALK);
    EXPECT_EQ(api.getGPSStatus().modeName, std::string("Walk (Sim)"));
}

TEST(GeoTerminalCommands, ModeRejectsOutOfRange) {
    geo::GeoAPI api;
    GeoTerminalState state;
    const auto out = ExecuteGeoCommand("mode 9", api, state);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out.front().rfind("[ERR]", 0), 0u);
    EXPECT_NE(out.front().find("0-4"), std::string::npos);
    // Mode untouched.
    EXPECT_EQ(api.getGPSStatus().mode, GPSTracker::Mode::DISABLED);
}

TEST(GeoTerminalCommands, ModeQueryReportsCurrent) {
    geo::GeoAPI api;
    api.setGPSMode(GPSTracker::Mode::SIMULATED_VEHICLE);
    GeoTerminalState state;
    const auto out = ExecuteGeoCommand("mode", api, state);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_NE(out.front().find("Vehicle (Sim)"), std::string::npos);
    EXPECT_NE(out.front().find("(3)"), std::string::npos);
}

TEST(GeoTerminalCommands, SpeedSetsTracker) {
    geo::GeoAPI api;
    GeoTerminalState state;
    const auto out = ExecuteGeoCommand("speed 3.5", api, state);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_NE(out.front().find("3.5"), std::string::npos);
    EXPECT_DOUBLE_EQ(api.getGPSTracker().getSpeed(), 3.5);
}

TEST(GeoTerminalCommands, NoiseSetsTracker) {
    geo::GeoAPI api;
    GeoTerminalState state;
    ExecuteGeoCommand("noise 12", api, state);
    EXPECT_DOUBLE_EQ(api.getGPSTracker().getNoiseLevel(), 12.0);
}

TEST(GeoTerminalCommands, NmeaTogglesStream) {
    geo::GeoAPI api;
    GeoTerminalState state;
    ExecuteGeoCommand("nmea on", api, state);
    EXPECT_TRUE(state.nmeaStream);
    ExecuteGeoCommand("nmea off", api, state);
    EXPECT_FALSE(state.nmeaStream);
    const auto out = ExecuteGeoCommand("nmea maybe", api, state);
    EXPECT_EQ(out.front().rfind("[ERR]", 0), 0u);
}

TEST(GeoTerminalCommands, TrackTogglesEntityTracking) {
    geo::GeoAPI api;
    GeoTerminalState state;
    ExecuteGeoCommand("track on", api, state);
    EXPECT_TRUE(api.isTrackingEntity());
    ExecuteGeoCommand("track off", api, state);
    EXPECT_FALSE(api.isTrackingEntity());
}

TEST(GeoTerminalCommands, IntervalSetsPredictionPeriod) {
    geo::GeoAPI api;
    GeoTerminalState state;
    const auto out = ExecuteGeoCommand("interval 5", api, state);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_NE(out.front().find("5.0 s"), std::string::npos);
}

TEST(GeoTerminalCommands, StatusWithoutFixReportsNoFix) {
    geo::GeoAPI api;
    GeoTerminalState state;
    const auto out = ExecuteGeoCommand("status", api, state);
    ASSERT_GE(out.size(), 1u);
    EXPECT_NE(out.front().find("no fix"), std::string::npos);
}

TEST(GeoTerminalCommands, ClearEmptiesScrollback) {
    geo::GeoAPI api;
    GeoTerminalState state;
    ExecuteGeoCommand("echo one", api, state);
    ExecuteGeoCommand("echo two", api, state);
    ASSERT_GT(state.output.size(), 1u);
    const auto out = ExecuteGeoCommand("clear", api, state);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out.front(), "-- terminal cleared --");
    EXPECT_EQ(state.output.size(), 1u);
}

TEST(GeoTerminalCommands, ScrollbackIsCapped) {
    geo::GeoAPI api;
    GeoTerminalState state;
    state.maxLines = 4;
    for (int i = 0; i < 10; ++i)
        ExecuteGeoCommand("echo line" + std::to_string(i), api, state);
    EXPECT_EQ(state.output.size(), 4u);
    // Newest lines survive; the oldest are dropped.
    EXPECT_EQ(state.output.back(), "line9");
    EXPECT_EQ(state.output.front(), "line6");
}

TEST(GeoTerminalCommands, EmptyLineProducesNoOutput) {
    geo::GeoAPI api;
    GeoTerminalState state;
    const auto out = ExecuteGeoCommand("   ", api, state);
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(state.output.empty());
}

TEST(GeoTerminalCommands, PredictRequestEmitsConfirmation) {
    geo::GeoAPI api;
    GeoTerminalState state;
    const auto out = ExecuteGeoCommand("predict 30 25", api, state);
    ASSERT_GE(out.size(), 1u);
    EXPECT_NE(out.front().find("Prediction requested"), std::string::npos);
    EXPECT_NE(out.front().find("30"), std::string::npos);
}
