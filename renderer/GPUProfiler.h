#pragma once
#include <glad/glad.h>
#include <string>
#include <vector>
#include <chrono>
#include <unordered_map>

// GPU Profiler - Measures rendering performance with OpenGL queries
class GPUProfiler {
public:
    struct ProfileResult {
        std::string name;
        double gpuTimeMs{0.0};
        double cpuTimeMs{0.0};
        int frameCount{0};
        double avgGpuTimeMs{0.0};
        double minGpuTimeMs{std::numeric_limits<double>::max()};
        double maxGpuTimeMs{0.0};
    };

    GPUProfiler();
    ~GPUProfiler();

    // Initialize profiler (call after OpenGL context created)
    bool initialize();

    // Start/stop GPU timing
    void beginFrame();
    void endFrame();

    // Begin/end named GPU query
    void beginQuery(const std::string& name);
    void endQuery(const std::string& name);

    // Get results
    const ProfileResult& getResult(const std::string& name) const;
    std::vector<ProfileResult> getAllResults() const;

    // Print results
    void printResults() const;
    void printFrameSummary() const;

    // Reset
    void reset();
    void resetFrame();

    // Enable/disable
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    // Get frame time
    double getFrameTimeMs() const { return m_currentFrameTimeMs; }
    double getAverageFrameTimeMs() const { return m_avgFrameTimeMs; }
    int getFPS() const { return m_fps; }

    // Singleton
    static GPUProfiler& getInstance();

private:
    struct QueryPair {
        GLuint startQuery{0};
        GLuint endQuery{0};
        std::string name;
    };

    void createQueryPair(QueryPair& pair);
    void destroyQueryPair(QueryPair& pair);
    void collectResults();

    bool m_enabled{true};
    bool m_initialized{false};
    bool m_insideFrame{false};

    // Current frame queries
    std::vector<QueryPair> m_currentQueries;
    std::unordered_map<std::string, size_t> m_queryIndex;

    // Results
    std::unordered_map<std::string, ProfileResult> m_results;

    // Frame timing
    double m_currentFrameTimeMs{0.0};
    double m_avgFrameTimeMs{0.0};
    int m_fps{0};
    int m_frameCount{0};
    double m_frameTimeAccumulator{0.0};
    double m_lastFPSTime{0.0};

    // Timestamp query for frame timing
    GLuint m_frameStartQuery{0};
    GLuint m_frameEndQuery{0};
};

// ============================================================================
// Scoped Profiler - RAII-style GPU profiling
// ============================================================================

class ScopedGPUProfile {
public:
    ScopedGPUProfile(const std::string& name) : m_name(name) {
        GPUProfiler::getInstance().beginQuery(name);
    }

    ~ScopedGPUProfile() {
        GPUProfiler::getInstance().endQuery(m_name);
    }

private:
    std::string m_name;
};

// ============================================================================
// Macro helpers
// ============================================================================

#define PROFILE_GPU(name) ScopedGPUProfile _profiler_##__LINE__(name)
#define BEGIN_GPU_FRAME() GPUProfiler::getInstance().beginFrame()
#define END_GPU_FRAME() GPUProfiler::getInstance().endFrame()
#define PRINT_GPU_STATS() GPUProfiler::getInstance().printResults()
