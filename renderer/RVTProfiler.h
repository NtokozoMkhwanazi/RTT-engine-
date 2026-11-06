#pragma once

// forUI.txt "Bake Progress Diagnostic HUD": thread-safe RVT page-bake timing
// telemetry. Implemented as a lightweight singleton (rather than an
// Editor::UIState field) so Terrain.cpp in the world layer can record bake
// durations without pulling in the heavy editor_state.h header (keeps
// editor->world layering clean: world owns Terrain, never editor state).
class RVTProfiler {
    float lastBakeDurationMs_ = 0.0f;
    float maxBakeDurationMs_ = 0.0f;
    int   totalPagesCompiledCount_ = 0;
    float historicalBakeTimes_[60] = { 0.0f };
    int   historyOffsetIndex_ = 0;
    RVTProfiler() = default;
public:
    static RVTProfiler& Instance() {
        static RVTProfiler instance;
        return instance;
    }

    float lastBakeDurationMs() const  { return lastBakeDurationMs_; }
    float maxBakeDurationMs() const   { return maxBakeDurationMs_; }
    int   totalPagesCompiledCount() const { return totalPagesCompiledCount_; }
    const float* historicalBakeTimes() const { return historicalBakeTimes_; }
    int   historyOffsetIndex() const  { return historyOffsetIndex_; }

    void recordBake(float elapsedMs) {
        lastBakeDurationMs_ = elapsedMs;
        if (elapsedMs > maxBakeDurationMs_) maxBakeDurationMs_ = elapsedMs;
        totalPagesCompiledCount_++;
        historicalBakeTimes_[historyOffsetIndex_] = elapsedMs;
        historyOffsetIndex_ = (historyOffsetIndex_ + 1) % 60;
    }
};
