#pragma once
/**
 * playback_recorder.h
 *
 * GL-free animation-timeline recording and playback:
 *   - Playback::Recorder : records clip segments with flicker hysteresis
 *   - Playback::Player   : loops through segments over time
 *   - JSON save/load round-tripping via jsoncpp
 */

#include <vector>
#include <string>
#include <fstream>
#include <cstdio>
#include <json/json.h>

namespace Playback {

struct Segment {
    int clipIndex = 0;
    float duration = 0.0f;
};

// Min frames a clip must be held to start a new segment (flicker filter).
inline constexpr int kMinHold = 5;

class Recorder {
public:
    void start() {
        recording_ = true;
        currentClip_ = -1;
        holdFrames_ = 0;
        pendingClip_ = -1;
        pendingFrames_ = 0;
        segments_.clear();
        totalDuration_ = 0.0f;
    }

    void stop() {
        recording_ = false;
        // Flush the current segment; discard any uncommitted flicker (its
        // frames were already absorbed into the current segment's duration).
        if (currentClip_ >= 0) {
            addSegment(currentClip_, holdFrames_ * dt_);
            currentClip_ = -1;
            holdFrames_ = 0;
        }
        pendingClip_ = -1;
        pendingFrames_ = 0;
    }

    void record(float dt, int clipIndex) {
        dt_ = dt;
        if (!recording_) return;

        if (currentClip_ < 0) {
            // First frame: start the segment immediately.
            currentClip_ = clipIndex;
            holdFrames_ = 1;
            return;
        }

        if (clipIndex == currentClip_) {
            // Back on the current clip: absorb any pending flicker frames into
            // the current segment (they're real elapsed time), then continue.
            if (pendingClip_ >= 0) {
                holdFrames_ += pendingFrames_;
                pendingClip_ = -1;
                pendingFrames_ = 0;
            }
            holdFrames_++;
            return;
        }

        // Clip changed. Track the flicker: only commit a new segment once the
        // new clip has been held past kMinHold.
        if (pendingClip_ != clipIndex) {
            pendingClip_ = clipIndex;
            pendingFrames_ = 1;
        } else {
            pendingFrames_++;
        }

        if (pendingFrames_ >= kMinHold) {
            addSegment(currentClip_, holdFrames_ * dt_);
            currentClip_ = pendingClip_;
            holdFrames_ = pendingFrames_;
            pendingClip_ = -1;
            pendingFrames_ = 0;
        }
        // Else: swallow the flicker (frames accumulate in pending; they'll be
        // absorbed if we return to the current clip or become a new segment
        // if the new clip is finally held).
    }

    int segmentCount() const { return (int)segments_.size(); }
    const std::vector<Segment>& segments() const { return segments_; }

    // Total recorded time (sum of segment durations).
    float duration() const {
        float d = totalDuration_;
        if (currentClip_ >= 0) d += holdFrames_ * dt_;
        return d;
    }

    // JSON round-trip ------------------------------------------------------
    bool saveToFile(const std::string& path) const {
        Json::Value root;
        root["duration"] = duration();
        Json::Value arr(Json::arrayValue);
        for (const auto& s : segments_) {
            Json::Value seg;
            seg["clipIndex"] = s.clipIndex;
            seg["duration"] = s.duration;
            arr.append(seg);
        }
        root["segments"] = arr;

        std::ofstream out(path);
        if (!out) return false;
        out << root.toStyledString();
        return out.good();
    }

    bool loadFromFile(const std::string& path) {
        std::ifstream in(path);
        if (!in) return false;
        Json::Value root;
        in >> root;
        if (root.isNull()) return false;

        segments_.clear();
        totalDuration_ = 0.0f;
        const Json::Value arr = root["segments"];
        for (const auto& seg : arr) {
            Segment s;
            s.clipIndex = seg["clipIndex"].asInt();
            s.duration = seg["duration"].asFloat();
            segments_.push_back(s);
            totalDuration_ += s.duration;
        }
        currentClip_ = segments_.empty() ? -1 : segments_.back().clipIndex;
        holdFrames_ = 0;
        pendingClip_ = -1;
        return true;
    }

private:
    void addSegment(int clipIndex, float dur) {
        if (dur < 0.0001f) return;
        segments_.push_back({clipIndex, dur});
        totalDuration_ += dur;
    }

    bool recording_ = false;
    int currentClip_ = -1;
    int holdFrames_ = 0;
    int pendingClip_ = -1;
    int pendingFrames_ = 0;
    float dt_ = 1.0f / 60.0f;
    float totalDuration_ = 0.0f;
    std::vector<Segment> segments_;
};

class Player {
public:
    void setSegments(const std::vector<Segment>& segments) {
        segments_ = segments;
        elapsed_ = 0.0f;
    }

    // Advance by dt seconds; returns the active clip index (looping).
    int step(float dt) {
        if (segments_.empty()) return 0;
        elapsed_ += dt;
        float t = elapsed_;
        float total = 0.0f;
        for (const auto& s : segments_) total += s.duration;
        if (total <= 0.0f) return segments_[0].clipIndex;
        while (t >= total) t -= total;
        float acc = 0.0f;
        for (const auto& s : segments_) {
            acc += s.duration;
            if (t < acc) return s.clipIndex;
        }
        return segments_.back().clipIndex;
    }

private:
    std::vector<Segment> segments_;
    float elapsed_ = 0.0f;
};

} // namespace Playback