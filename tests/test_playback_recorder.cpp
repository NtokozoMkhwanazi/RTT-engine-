/**
 * Playback Recorder / Player Tests (GL-free)
 *
 * Exercises the animation-timeline Recorder (segment hysteresis, duration
 * accounting), the looping Player, and JSON save/load round-tripping.
 *
 * Run with: make test
 */

#include <gtest/gtest.h>
#include <cstdio>

#include "../editor/playback_recorder.h"

namespace {

constexpr float kDt = 1.0f / 60.0f;

TEST(PlaybackRecorder, RecordsSegmentsOnClipChange) {
    Playback::Recorder rec;
    rec.start();

    // ~1s of clip 0 (idle).
    rec.record(kDt, 0);
    for (int i = 0; i < 60; ++i) rec.record(kDt, 0);

    // Switch to clip 1 and hold past the hysteresis.
    for (int i = 0; i < 70; ++i) rec.record(kDt, 1);

    // A short flicker back to clip 0 (below kMinHold) must be swallowed.
    for (int i = 0; i < 3; ++i) rec.record(kDt, 0);
    for (int i = 0; i < 60; ++i) rec.record(kDt, 1);

    rec.stop();

    ASSERT_EQ(rec.segmentCount(), 2) << "flicker must not create a third segment";
    EXPECT_EQ(rec.segments()[0].clipIndex, 0);
    EXPECT_NEAR(rec.segments()[0].duration, 1.0f, 0.1f);
    EXPECT_EQ(rec.segments()[1].clipIndex, 1);
    EXPECT_NEAR(rec.segments()[1].duration, 2.2f, 0.15f);
    // Total recorded time must equal the real elapsed time exactly.
    EXPECT_NEAR(rec.duration(), 194.0f / 60.0f, 0.05f);
}

TEST(PlaybackRecorder, IgnoresFlickerBelowMinHold) {
    Playback::Recorder rec;
    rec.start();
    rec.record(kDt, 0);
    for (int i = 0; i < 60; ++i) rec.record(kDt, 0);
    // 2 frames of clip 3 (< kMinHold) then back.
    rec.record(kDt, 3);
    rec.record(kDt, 3);
    for (int i = 0; i < 60; ++i) rec.record(kDt, 0);
    rec.stop();
    EXPECT_EQ(rec.segmentCount(), 1) << "sub-kMinHold flicker must be ignored";
    EXPECT_EQ(rec.segments()[0].clipIndex, 0);
}

TEST(PlaybackPlayer, LoopsSegments) {
    Playback::Player p;
    p.setSegments({{0, 0.5f}, {1, 0.25f}, {2, 0.25f}});
    EXPECT_EQ(p.step(0.1f), 0);
    EXPECT_EQ(p.step(0.4f), 1);   // 0.5s boundary -> segment 1
    EXPECT_EQ(p.step(0.3f), 2);   // 0.75s boundary -> segment 2
    EXPECT_EQ(p.step(0.3f), 0);   // 1.0s boundary -> loops back to segment 0
}

TEST(PlaybackRecorder, JsonRoundTrip) {
    Playback::Recorder rec;
    rec.start();
    rec.record(kDt, 0);
    for (int i = 0; i < 60; ++i) rec.record(kDt, 0);
    for (int i = 0; i < 60; ++i) rec.record(kDt, 2);  // jump straight to clip 2
    rec.stop();
    ASSERT_EQ(rec.segmentCount(), 2);

    const std::string path = "/tmp/rtt_timeline_test.json";
    ASSERT_TRUE(rec.saveToFile(path));

    Playback::Recorder loaded;
    ASSERT_TRUE(loaded.loadFromFile(path));
    ASSERT_EQ(loaded.segmentCount(), rec.segmentCount());
    EXPECT_EQ(loaded.segments()[0].clipIndex, rec.segments()[0].clipIndex);
    EXPECT_NEAR(loaded.segments()[0].duration, rec.segments()[0].duration, 0.01f);
    EXPECT_EQ(loaded.segments()[1].clipIndex, rec.segments()[1].clipIndex);
    EXPECT_NEAR(loaded.segments()[1].duration, rec.segments()[1].duration, 0.01f);
    EXPECT_EQ(loaded.segments()[0].clipIndex, 0);
    EXPECT_EQ(loaded.segments()[1].clipIndex, 2);

    std::remove(path.c_str());
}

}  // namespace

// Note: main() is in test_main.cpp - don't duplicate
