#pragma once
/**
 * geo_terminal.h
 *
 * GL/ImGui-free geo terminal logic:
 *   - NmeaChecksum / BuildNmeaGGA / BuildNmeaRMC (NMEA-0183 sentence builders)
 *   - ExecuteGeoCommand (terminal command executor against a GeoAPI facade)
 *
 * Designed to be unit-testable without a windowing context.
 */

#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <sstream>

#include "geospatial/GeoAPI.h"
#include "geospatial/GPSTracker.h"

namespace UI {

// Terminal state: scrollback + flags the executor mutates.
struct GeoTerminalState {
    std::vector<std::string> output;  // scrollback (newest at the back)
    int maxLines = 100;               // scrollback cap
    bool nmeaStream = false;          // NMEA stream toggle
};

// ---- NMEA-0183 helpers -----------------------------------------------------

// XOR checksum of a sentence body (between '$' and '*').
inline unsigned NmeaChecksum(const std::string& body) {
    unsigned c = 0;
    for (char ch : body) c ^= static_cast<unsigned char>(ch);
    return c;
}

// Format a latitude/longitude as NMEA DDMM.MMMM with hemisphere letter.
inline std::string FormatDDM(double deg, char posHem, char negHem) {
    const char hemi = (deg < 0.0) ? negHem : posHem;
    const double absDeg = std::fabs(deg);
    const int d = static_cast<int>(absDeg);
    const double mm = (absDeg - d) * 60.0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02d%07.4f,%c", d, mm, hemi);
    return std::string(buf);
}

// Build a $GPGGA sentence from a GPS fix.
inline std::string BuildNmeaGGA(const geo::GPSStatus& fix) {
    const std::string lat = FormatDDM(fix.latitude, 'N', 'S');
    const std::string lon = FormatDDM(fix.longitude, 'E', 'W');
    char buf[256];
    if (fix.isValid) {
        std::snprintf(buf, sizeof(buf),
                      "GPGGA,123519,%s,%s,1,12,0.9,%.1f,M,46.9,M,,",
                      lat.c_str(), lon.c_str(), fix.altitude);
    } else {
        std::snprintf(buf, sizeof(buf),
                      "GPGGA,123519,%s,%s,0,0,0.9,,M,46.9,M,,",
                      lat.c_str(), lon.c_str());
    }
    std::string s = "$" + std::string(buf);
    char cs[8];
    std::snprintf(cs, sizeof(cs), "*%02X", NmeaChecksum(buf));
    s += cs;
    return s;
}

// Build a $GPRMC sentence from a GPS fix. Speed is converted to knots.
inline std::string BuildNmeaRMC(const geo::GPSStatus& fix) {
    const std::string lat = FormatDDM(fix.latitude, 'N', 'S');
    const std::string lon = FormatDDM(fix.longitude, 'E', 'W');
    const double knots = fix.speed * 1.943844;
    const int heading = static_cast<int>(std::fmod(fix.heading, 360.0));
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "GPRMC,123519,%c,%s,%s,%.1f,%03d,141123,000.0,W",
                  fix.isValid ? 'A' : 'V',
                  lat.c_str(), lon.c_str(), knots, heading);
    std::string s = "$" + std::string(buf);
    char cs[8];
    std::snprintf(cs, sizeof(cs), "*%02X", NmeaChecksum(buf));
    s += cs;
    return s;
}

// ---- Command executor -------------------------------------------------------

// Execute a terminal command against the GeoAPI. Returns the lines produced
// this call (also appended to state.output, capped at state.maxLines).
inline std::vector<std::string> ExecuteGeoCommand(const std::string& line,
                                                  geo::GeoAPI& api,
                                                  GeoTerminalState& state) {
    std::vector<std::string> out;

    // Tokenize (strip leading/trailing whitespace).
    std::istringstream iss(line);
    std::vector<std::string> args;
    std::string tok;
    while (iss >> tok) args.push_back(tok);

    if (args.empty()) return out;  // blank line -> no output

    auto push = [&](const std::string& s) {
        out.push_back(s);
        state.output.push_back(s);
        if (state.maxLines > 0 && (int)state.output.size() > state.maxLines)
            state.output.erase(state.output.begin(),
                               state.output.begin() + (state.output.size() - state.maxLines));
    };

    const std::string& cmd = args[0];

    if (cmd == "help") {
        push("Geo Terminal - NMEA-style control for the geospatial pipeline");
        push("  mode [0-4]   - set GPS simulation mode");
        push("  speed X      - set simulation speed");
        push("  noise X      - set GPS noise level");
        push("  nmea on/off  - toggle NMEA sentence stream");
        push("  track on/off - toggle entity tracking");
        push("  interval X   - set prediction interval");
        push("  predict H P  - request a prediction");
        push("  status       - show current fix status");
        push("  echo ...     - echo arguments");
        push("  clear        - clear the scrollback");
    } else if (cmd == "echo") {
        std::string joined;
        for (size_t i = 1; i < args.size(); ++i) {
            if (i > 1) joined += " ";
            joined += args[i];
        }
        push(joined);
    } else if (cmd == "mode") {
        if (args.size() < 2) {
            const auto st = api.getGPSStatus();
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Mode: %s (%d)",
                          st.modeName ? st.modeName : "Unknown",
                          static_cast<int>(st.mode));
            push(buf);
        } else {
            const int m = std::atoi(args[1].c_str());
            if (m < 0 || m > 4) {
                push("[ERR] mode must be 0-4");
            } else {
                api.setGPSMode(static_cast<GPSTracker::Mode>(m));
                push("Mode set");
            }
        }
    } else if (cmd == "speed") {
        if (args.size() < 2) {
            push("[ERR] usage: speed <value>");
        } else {
            const double v = std::atof(args[1].c_str());
            api.getGPSTracker().setSpeed(v);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Speed set to %.1f", v);
            push(buf);
        }
    } else if (cmd == "noise") {
        if (args.size() < 2) {
            push("[ERR] usage: noise <value>");
        } else {
            api.getGPSTracker().setNoiseLevel(std::atof(args[1].c_str()));
            push("Noise set");
        }
    } else if (cmd == "nmea") {
        if (args.size() < 2 || (args[1] != "on" && args[1] != "off")) {
            push("[ERR] usage: nmea on|off");
        } else {
            state.nmeaStream = (args[1] == "on");
            push(state.nmeaStream ? "NMEA stream ON" : "NMEA stream OFF");
        }
    } else if (cmd == "track") {
        if (args.size() < 2 || (args[1] != "on" && args[1] != "off")) {
            push("[ERR] usage: track on|off");
        } else {
            if (args[1] == "on") api.setTrackEntity(true);
            else api.setTrackEntity(false);
            push(args[1] == "on" ? "Entity tracking ON" : "Entity tracking OFF");
        }
    } else if (cmd == "interval") {
        if (args.size() < 2) {
            push("[ERR] usage: interval <seconds>");
        } else {
            const double v = std::atof(args[1].c_str());
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Prediction interval %.1f s", v);
            push(buf);
        }
    } else if (cmd == "predict") {
        if (args.size() < 3) {
            push("[ERR] usage: predict <horizon> <points>");
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Prediction requested (horizon %s, points %s)",
                          args[1].c_str(), args[2].c_str());
            push(buf);
        }
    } else if (cmd == "status") {
        const auto st = api.getGPSStatus();
        if (!st.isValid) {
            push("GPS: no fix");
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "GPS: %.6f, %.6f alt %.1f speed %.1f",
                          st.latitude, st.longitude, st.altitude, st.speed);
            push(buf);
        }
    } else if (cmd == "clear") {
        state.output.clear();
        push("-- terminal cleared --");
    } else {
        push("[ERR] unknown command: " + cmd);
    }

    return out;
}

} // namespace UI