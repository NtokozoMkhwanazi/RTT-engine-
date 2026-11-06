#pragma once
// ============================================================================
// Profiler — lightweight CPU/GPU timing + stats aggregation for creators.
//
// Provides:
//   * RAII scope timers (PROFILE_CPU_SCOPE / Profiler::ScopedTimer)
//   * Per-frame accumulators for named timing regions
//   * Periodic flushing of aggregated stats to the EditorConsole
//   * A Snapshot() struct that the UI Profiler panel reads every frame
//
// Design goals:
//   * Zero dependencies on subsystems being profiled — producers push
//     numeric stats via registerStat()/addStat(); the Profiler just
//     records and forwards them.
//   * Subsystems that can't know about the Profiler pull their stats
//     through a callback registered via registerProvider().
// ============================================================================
#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <mutex>
#include <atomic>

// Forward declarations for optional stats providers
class GPUProfiler;
class MemoryTracker;

class Profiler {
public:
    struct TimingStat {
        uint64_t callCount{0};
        double  totalMs{0.0};     // sum of all invocations this flush period
        double  lastMs{0.0};      // last invocation
        double  avgMs{0.0};       // totalMs / callCount
        double  minMs{0.0};
        double  maxMs{0.0};
    };

    struct StatValue {
        double value{0.0};
        std::string label;
    };

    // String-valued stat (for things like "current clip name" that can't
    // be represented as a number).
    struct StringStat {
        std::string value;
    };

    struct Snapshot {
        // Frame timing
        double frameTimeMs  = 0.0;     // last frame
        double avgFrameMs   = 0.0;     // rolling average
        double minFrameMs   = 0.0;
        double maxFrameMs   = 0.0;
        double fps          = 0.0;
        int    frameCount   = 0;

        // Per-region CPU timings (region name -> stats)
        std::vector<std::pair<std::string, TimingStat>> cpuRegions;

        // Generic numeric stats pushed by subsystems (KD-tree nodes,
        // DB pose count, SIMD width, etc.)
        std::vector<std::pair<std::string, StatValue>> numericStats;

        // String-valued stats (current clip name, etc.)
        std::vector<std::pair<std::string, StringStat>> stringStats;
    };

    // --- Singleton ---
    static Profiler& Instance() {
        static Profiler inst;
        return inst;
    }

    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;

    // --- Frame lifecycle ---
    // Call at the start of each frame (resets per-frame timers).
    // Re-entrant: the OUTERMOST beginFrame()/endFrame() pair owns the frame
    // timer, so subsystems that already open a frame scope (RenderPipeline::
    // beginFrame/endFrame from inside the editor loop) nest instead of
    // restarting the clock and reporting a partial frame.
    void beginFrame();
    // Call at the end of each frame (computes frame time, flushes periodic
    // stats to the console, updates rolling averages).
    void endFrame();

    // --- RAII scope timer ---
    class ScopedTimer {
    public:
        explicit ScopedTimer(const std::string& name);
        ~ScopedTimer();
        ScopedTimer(const ScopedTimer&) = delete;
        ScopedTimer& operator=(const ScopedTimer&) = delete;
    private:
        std::string m_name;   // owned copy — the ctor arg is often a temporary
        std::chrono::high_resolution_clock::time_point m_start;
    };

    // --- Direct numeric stats (non-timing) ---
    // Subsystems push a value each frame; the profiler records the last
    // value and forwards it to the console on flush.
    void setStat(const std::string& key, double value, const std::string& label = {});
    void addStat(const std::string& key, double delta, const std::string& label = {});
    void setStringStat(const std::string& key, const std::string& value);

    // --- Provider callbacks ---
    // Register a function that pushes stats into the profiler.  Called once
    // per endFrame() so the provider can supply fresh values without knowing
    // the Profiler's internals.
    using ProviderFn = std::function<void(Profiler&)>;
    void registerProvider(ProviderFn fn) { m_providers.push_back(std::move(fn)); }

    // --- Configuration ---
    void setEnabled(bool e)         { m_enabled.store(e, std::memory_order_release); }
    bool isEnabled() const          { return m_enabled.load(std::memory_order_acquire); }
    void setLogIntervalSeconds(float s) { m_logInterval = s; }
    float getLogIntervalSeconds() const { return m_logInterval; }
    void setLogLevel(int lvl)       { m_logLevel = lvl; }   // 0=all, 1=warn+, 2=err only
    int  getLogLevel() const        { return m_logLevel; }

    // --- Query (for UI panel) ---
    Snapshot snapshot() const;

    // --- Manual flush (writes aggregated stats to the console) ---
    void flushToConsole();

    // --- Timing helpers (used by ScopedTimer) ---
    void recordCpuTime(const std::string& name, double ms);

private:
    Profiler();
    ~Profiler() = default;

    std::atomic<bool> m_enabled{true};
    float m_logInterval{5.0f};       // seconds between console stat flushes
    float m_timeSinceFlush{0.0f};
    int   m_logLevel{0};

    // Frame timing (rolling window)
    std::atomic<int> m_frameDepth{0};   // nested beginFrame()/endFrame() pairing
    std::chrono::high_resolution_clock::time_point m_frameStart;
    double m_rollingFrameMs{16.67};
    double m_minFrameMs{1e9};
    double m_maxFrameMs{0.0};
    int    m_frameCount{0};
    double m_frameAccum{0.0};

    // Per-region CPU stats, keyed by region name. Double-buffered so that
    // flushToConsole() can hand the just-filled buffer to the main thread for
    // logging while worker-thread ScopedTimer scopes immediately resume writing
    // into the other (cleared) slot. The live write buffer is NEVER wiped while a
    // worker may be mid-write -- the data race that the flat clear() caused.
    std::unordered_map<std::string, TimingStat> m_cpuStats[2];
    std::atomic<int> m_cpuWriteIndex{0};

    // Generic numeric stats (last value)
    std::unordered_map<std::string, StatValue> m_numericStats;

    // String-valued stats (last value)
    std::unordered_map<std::string, StringStat> m_stringStats;

    // Provider callbacks (called before building a Snapshot)
    std::vector<ProviderFn> m_providers;

    // Guards m_cpuStats / m_numericStats / m_stringStats, which are mutated by
    // worker-thread ScopedTimer scopes (PROFILE_CPU_SCOPE) and read by the
    // main-thread snapshot()/flushToConsole().  Previously a shared, unlocked
    // std::vector<TimingStat*> m_activeStack was pushed/popped here from every
    // worker thread (data race).  It is removed: flat per-region stats do not
    // need nesting depth, and the mutex below now serialises all stat access.
    mutable std::mutex m_statsMutex;

    // Cached snapshot built by snapshot() so its std::vector scratch capacity is
    // reused across frames instead of re-reserving on a fresh Snapshot each
    // call (the old `snap.cpuRegions.reserve(...)` churned the heap inside the
    // render loop we are trying to measure).
    mutable Snapshot m_snapCache;
};

// --- Convenience macros ---
#define PROFILE_CPU_SCOPE(name) \
    Profiler::ScopedTimer _profiler_timer_##__LINE__(name)

// Provider registration shortcut for subsystems that have an Instance().
// Usage:  REGISTER_PROFILER_PROVIDER(MySystem::Instance().registerProfilerProvider);
#define REGISTER_PROFILER_PROVIDER(registerFn) \
    do { static bool _registered = false; \
         if (!_registered) { Profiler::Instance().registerProvider(registerFn); _registered = true; } } while(0)
