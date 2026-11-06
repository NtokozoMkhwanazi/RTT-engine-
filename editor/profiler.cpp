#include "profiler.h"
#include "console.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sstream>

Profiler::Profiler() {
    // Register a built-in OS memory provider (Linux /proc/self/status).
    // This supplements the MemoryTracker which only counts instrumented
    // allocations — the OS-level RSS gives creators the real picture.
    m_providers.push_back([this](Profiler&) {
        FILE* f = fopen("/proc/self/status", "r");
        if (!f) return;
        char line[256];
        size_t vmRSS = 0, vmPeak = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "VmRSS:", 6) == 0)
                sscanf(line + 6, "%zu", &vmRSS);
            else if (strncmp(line, "VmPeak:", 7) == 0)
                sscanf(line + 7, "%zu", &vmPeak);
        }
        fclose(f);
        setStat("Memory.VmRSS_KB",   static_cast<double>(vmRSS),   "KB resident");
        setStat("Memory.VmPeak_KB",  static_cast<double>(vmPeak),  "KB peak");
    });
}

// ------------------------------------------------------------------ Frame lifecycle
void Profiler::beginFrame() {
    if (!m_enabled) return;
    // Only the outermost scope owns the frame clock (see profiler.h).
    if (m_frameDepth.fetch_add(1, std::memory_order_acq_rel) != 0) return;
    m_frameStart = std::chrono::high_resolution_clock::now();
}

void Profiler::endFrame() {
    if (!m_enabled) return;
    // Still inside a nested scope: the outer endFrame() finalises the frame.
    if (m_frameDepth.fetch_sub(1, std::memory_order_acq_rel) != 1) return;

    auto now = std::chrono::high_resolution_clock::now();
    double frameMs = std::chrono::duration<double, std::milli>(now - m_frameStart).count();

    // Update rolling average (simple EMA, 0.9 decay — smooths a single spike)
    m_rollingFrameMs = m_rollingFrameMs * 0.9 + frameMs * 0.1;
    m_minFrameMs = std::min(m_minFrameMs, frameMs);
    m_maxFrameMs = std::max(m_maxFrameMs, frameMs);
    m_frameCount++;
    m_frameAccum += frameMs;

    // Call provider callbacks so subsystems can push fresh stats
    for (auto& fn : m_providers) fn(*this);

    // Periodic console flush
    m_timeSinceFlush += static_cast<float>(frameMs / 1000.0);
    if (m_timeSinceFlush >= m_logInterval) {
        flushToConsole();
        m_timeSinceFlush = 0.0f;
    }

    (void)m_logLevel;  // reserved for future per-frame level gating
}

// ------------------------------------------------------------------ Scope timer
Profiler::ScopedTimer::ScopedTimer(const std::string& name)
    : m_name(name)
    , m_start(std::chrono::high_resolution_clock::now())
{
    // NOTE: the old m_activeStack.push_back() was removed (see profiler.h) — a
    // shared std::vector on this singleton raced across worker threads when
    // PROFILE_CPU_SCOPE was used concurrently.  Flat per-region stats do not
    // need nesting depth.
    Profiler& p = Profiler::Instance();
    if (p.m_enabled) {
        std::lock_guard<std::mutex> lk(p.m_statsMutex);
        p.m_cpuStats[p.m_cpuWriteIndex.load()].try_emplace(name, TimingStat{});
    }
}

Profiler::ScopedTimer::~ScopedTimer() {
    Profiler& p = Profiler::Instance();
    if (!p.m_enabled) return;
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - m_start).count();
    if (!m_name.empty()) {
        std::lock_guard<std::mutex> lk(p.m_statsMutex);
        const int idx = p.m_cpuWriteIndex.load();
        auto it = p.m_cpuStats[idx].find(m_name);
        if (it != p.m_cpuStats[idx].end()) {
            TimingStat& s = it->second;
            s.totalMs += ms;
            s.lastMs  = ms;
            s.callCount++;
            if (s.callCount == 1 || ms < s.minMs) s.minMs = ms;
            if (ms > s.maxMs) s.maxMs = ms;
            s.avgMs = s.totalMs / s.callCount;
        }
    }
}

// ------------------------------------------------------------------ Numeric stats
void Profiler::setStat(const std::string& key, double value, const std::string& label) {
    if (!m_enabled) return;
    std::lock_guard<std::mutex> lk(m_statsMutex);
    auto& v = m_numericStats[key];
    v.value = value;
    if (!label.empty()) v.label = label;
}

void Profiler::addStat(const std::string& key, double delta, const std::string& label) {
    if (!m_enabled) return;
    std::lock_guard<std::mutex> lk(m_statsMutex);
    auto it = m_numericStats.find(key);
    if (it == m_numericStats.end()) {
        it = m_numericStats.emplace(key, StatValue{}).first;
        if (!label.empty()) it->second.label = label;
    }
    it->second.value += delta;
}

void Profiler::setStringStat(const std::string& key, const std::string& value) {
    if (!m_enabled) return;
    std::lock_guard<std::mutex> lk(m_statsMutex);
    m_stringStats[key].value = value;
}

// ------------------------------------------------------------------ Direct timing
void Profiler::recordCpuTime(const std::string& name, double ms) {
    if (!m_enabled) return;
    std::lock_guard<std::mutex> lk(m_statsMutex);
    const int idx = m_cpuWriteIndex.load();
    auto& s = m_cpuStats[idx].try_emplace(name, TimingStat{}).first->second;
    s.totalMs += ms;
    s.lastMs  = ms;
    s.callCount++;
    if (s.callCount == 1 || ms < s.minMs) s.minMs = ms;
    if (ms > s.maxMs) s.maxMs = ms;
    s.avgMs = s.totalMs / s.callCount;
}

// ------------------------------------------------------------------ Snapshot
Profiler::Snapshot Profiler::snapshot() const {
    // Verify.txt Fix: reuse a cached Snapshot (mutable member) so its std::vector
    // scratch capacity survives across frames instead of re-reserving a fresh
    // Snapshot (and its vectors) every call -- that heap churn ran inside the
    // render loop we are trying to measure. m_snapCache is touched only under
    // m_statsMutex (snapshot runs on the main thread; flushToConsole too).
    Snapshot& snap = m_snapCache;
    {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        const int idx = m_cpuWriteIndex.load();
        // Use the most recent EMA as the "last frame" for display
        snap.frameTimeMs = m_rollingFrameMs;
        snap.avgFrameMs  = (m_frameCount > 0) ? (m_frameAccum / m_frameCount) : 0.0;
        snap.minFrameMs  = m_minFrameMs;
        snap.maxFrameMs  = m_maxFrameMs;
        snap.fps         = (m_rollingFrameMs > 0.0) ? (1000.0 / m_rollingFrameMs) : 0.0;
        snap.frameCount  = m_frameCount;

        snap.cpuRegions.clear();
        snap.cpuRegions.reserve(m_cpuStats[idx].size());
        for (const auto& kv : m_cpuStats[idx]) {
            snap.cpuRegions.emplace_back(kv.first, kv.second);
        }

        snap.numericStats.clear();
        snap.numericStats.reserve(m_numericStats.size());
        for (const auto& kv : m_numericStats) {
            snap.numericStats.emplace_back(kv.first, kv.second);
        }
        snap.stringStats.clear();
        snap.stringStats.reserve(m_stringStats.size());
        for (const auto& kv : m_stringStats) {
            snap.stringStats.emplace_back(kv.first, kv.second);
        }
    }
    return snap;  // by-value copy; m_snapCache keeps its capacity for next frame
}

// ------------------------------------------------------------------ Console flush
void Profiler::flushToConsole() {
    if (!m_enabled || m_logInterval <= 0.0f) return;

    auto s = snapshot();

    // --- Profiling category: frame timing ---
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "Frame: %.1f FPS | %.2f ms (min %.2f / max %.2f / avg %.2f) | %d frames since flush",
            s.fps, s.frameTimeMs, s.minFrameMs, s.maxFrameMs, s.avgFrameMs,
            s.frameCount);
        EditorConsole::Log(buf, 0);
    }

    // --- CPU timing regions ---
    for (const auto& [name, stat] : s.cpuRegions) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[CPU] %s: avg %.3f ms (min %.3f / max %.3f / last %.3f) x%d",
            name.c_str(), stat.avgMs, stat.minMs, stat.maxMs, stat.lastMs,
            static_cast<int>(stat.callCount));
        EditorConsole::Log(buf, 0);
    }

    // --- Numeric stats ---
    for (const auto& [key, val] : s.numericStats) {
        // Skip string-valued stat keys (those are logged separately below).
        if (key.rfind("MotionMatch.", 0) == 0 &&
            (key == "MotionMatch.CurrentClip" || key == "MotionMatch.CurrentClipName"))
            continue;

        char buf[256];
        if (!val.label.empty()) {
            std::snprintf(buf, sizeof(buf), "%s: %g %s",
                key.c_str(), val.value, val.label.c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "%s: %g", key.c_str(), val.value);
        }
        // Route to the appropriate category based on the stat name prefix
        if (key.find("KDTree") != std::string::npos ||
            key.find("KD-Tree") != std::string::npos)
            EditorConsole::Log(buf, 0);
        else if (key.find("SIMD") != std::string::npos)
            EditorConsole::Log(buf, 0);
        else if (key.find("Memory") != std::string::npos)
            EditorConsole::Log(buf, 0);
        else if (key.find("Motion") != std::string::npos ||
                 key.find("Pose") != std::string::npos ||
                 key.find("Animation") != std::string::npos)
            EditorConsole::Log(buf, 0);
        else
            EditorConsole::Log(buf, 0);
    }

    // --- String-valued stats ---
    for (const auto& [key, val] : s.stringStats) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s: %s", key.c_str(), val.value.c_str());
        if (key.find("Motion") != std::string::npos ||
            key.find("Animation") != std::string::npos)
            EditorConsole::Log(buf, 0);
        else
            EditorConsole::Log(buf, 0);
    }

    // verify.txt Fix: NEVER clear the live write buffer (m_cpuStats[writeIndex])
    // here -- worker-thread ScopedTimer scopes may be writing to it right now.
    // Flip the write index so new scopes resume into the other slot, then clear
    // only the frozen slot we just snapshotted. This is the atomic double-buffer
    // swap that eliminates the flushToConsole() data race.
    {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        const int readIdx = m_cpuWriteIndex.load();
        m_cpuWriteIndex.store(1 - readIdx);
        m_cpuStats[readIdx].clear();
    }
    m_frameCount = 0;
    m_frameAccum = 0.0;
    m_minFrameMs = 1e9;
    m_maxFrameMs = 0.0;
}
