#pragma once

/**
 * Advanced GPU Profiler - Hierarchical, Persistent GPU Timing
 * 
 * Features:
 * - Hierarchical timing scopes (nested profiling)
 * - Persistent history (frame history for graphs)
 * - Thread-safe query management
 * - Automatic query pool recycling
 * - Real-time statistics (avg, min, max, variance)
 * - Frame graph visualization data
 */

#include <glad/glad.h>
#include <string>
#include <vector>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <array>
#include <cmath>
#include <limits>
#include <functional>

class AdvancedGPUProfiler {
public:
    // Statistics for a profiled scope
    struct ProfileStats {
        std::string name;
        double gpuTimeMs = 0.0;
        double cpuTimeMs = 0.0;
        
        // Running statistics
        double avgTimeMs = 0.0;
        double minTimeMs = std::numeric_limits<double>::max();
        double maxTimeMs = 0.0;
        double stdDevMs = 0.0;
        
        // History (for graphs)
        std::vector<double> history;
        static constexpr size_t MAX_HISTORY = 120;  // 2 seconds at 60 FPS
        
        // Hierarchy
        int depth = 0;
        uint64_t parentId = 0;
        uint64_t scopeId = 0;
        
        // Frame data
        int frameCount = 0;
        int lastSeenFrame = 0;
        
        void addSample(double timeMs) {
            gpuTimeMs = timeMs;
            frameCount++;
            
            // Update running statistics using Welford's algorithm
            double delta = timeMs - avgTimeMs;
            avgTimeMs += delta / frameCount;
            double delta2 = timeMs - avgTimeMs;
            stdDevMs = std::sqrt((stdDevMs * stdDevMs * (frameCount - 1) + delta * delta2) / frameCount);
            
            minTimeMs = std::min(minTimeMs, timeMs);
            maxTimeMs = std::max(maxTimeMs, timeMs);
            
            // Add to history
            history.push_back(timeMs);
            if (history.size() > MAX_HISTORY) {
                history.erase(history.begin());
            }
        }
        
        void reset() {
            avgTimeMs = 0.0;
            minTimeMs = std::numeric_limits<double>::max();
            maxTimeMs = 0.0;
            stdDevMs = 0.0;
            frameCount = 0;
            history.clear();
        }
    };

    // Hierarchical scope for nested profiling
    struct ProfileScope {
        std::string name;
        uint64_t id = 0;
        uint64_t parentId = 0;
        int depth = 0;
        
        GLuint startQuery = 0;
        GLuint endQuery = 0;
        bool isOpen = false;
        
        std::chrono::high_resolution_clock::time_point cpuStart;
        std::chrono::high_resolution_clock::time_point cpuEnd;
        
        std::vector<std::unique_ptr<ProfileScope>> children;
    };

    AdvancedGPUProfiler();
    ~AdvancedGPUProfiler();

    // Singleton
    static AdvancedGPUProfiler& getInstance();

    // Initialization
    bool initialize();
    void shutdown();

    // Frame management
    void beginFrame();
    void endFrame();

    // Hierarchical scoping
    void beginScope(const std::string& name);
    void endScope();
    
    // One-shot timing (auto-closes scope)
    void profileScope(const std::string& name, std::function<void()> callback);

    // Query results
    const ProfileStats& getStats(const std::string& name) const;
    std::vector<ProfileStats> getAllStats() const;
    std::vector<ProfileStats> getTopScopes(int count = 10) const;
    
    // Frame data
    double getFrameTimeMs() const { return m_currentFrameTimeMs; }
    double getAverageFrameTimeMs() const { return m_avgFrameTimeMs; }
    int getFPS() const { return m_fps; }
    int getCurrentFrame() const { return m_currentFrame; }

    // Control
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }
    
    void setHistorySize(size_t size);
    size_t getHistorySize() const { return m_historySize; }

    // Output
    void printResults() const;
    void printFrameSummary() const;
    void exportToCSV(const std::string& filename) const;
    
    // Reset
    void reset();
    void resetFrame();
    void resetStats(const std::string& name);

private:
    // Query management
    struct QueryPool {
        std::vector<GLuint> freeQueries;
        std::vector<GLuint> activeQueries;
        
        GLuint acquire();
        void release(GLuint query);
        void cleanup();
    };

    GLuint acquireQuery();
    void releaseQuery(GLuint query);
    void collectResults();
    
    // Scope management
    ProfileScope* getCurrentScope();
    ProfileScope* findOrCreateScope(const std::string& name, uint64_t parentId);
    uint64_t generateScopeId();
    
    // Hierarchy
    void buildHierarchy();
    void calculateDepth(ProfileScope* scope, int depth);
    
    // Statistics
    void updateStats();
    
    bool m_enabled = true;
    bool m_initialized = false;
    bool m_inFrame = false;
    
    // Current frame
    int m_currentFrame = 0;
    std::unique_ptr<ProfileScope> m_rootScope;
    ProfileScope* m_currentScope = nullptr;
    
    // Query management
    QueryPool m_queryPool;
    std::vector<GLuint> m_pendingQueries;  // Queries waiting for results
    
    // Statistics
    std::unordered_map<std::string, ProfileStats> m_stats;
    std::unordered_map<uint64_t, std::string> m_scopeIdToName;
    
    // Frame timing
    double m_currentFrameTimeMs = 0.0;
    double m_avgFrameTimeMs = 0.0;
    int m_fps = 0;
    int m_frameCount = 0;
    double m_frameTimeAccumulator = 0.0;
    double m_lastFPSTime = 0.0;
    
    // Frame timing queries
    GLuint m_frameStartQuery = 0;
    GLuint m_frameEndQuery = 0;
    
    // Configuration
    size_t m_historySize = 120;
    uint64_t m_nextScopeId = 1;
    
    // Thread safety
    mutable std::mutex m_mutex;
};

// ============================================================================
// RAII Scope Guard
// ============================================================================

class ScopedGPUProfile {
public:
    explicit ScopedGPUProfile(const std::string& name) 
        : m_name(name) {
        AdvancedGPUProfiler::getInstance().beginScope(name);
    }

    ~ScopedGPUProfile() {
        AdvancedGPUProfiler::getInstance().endScope();
    }

private:
    std::string m_name;
};

// ============================================================================
// Macro Helpers
// ============================================================================

#define PROFILE_GPU_SCOPE(name) ScopedGPUProfile _gpu_profiler_##__LINE__(name)
#define PROFILE_GPU_FUNCTION() PROFILE_GPU_SCOPE(__FUNCTION__)
#define BEGIN_GPU_FRAME() AdvancedGPUProfiler::getInstance().beginFrame()
#define END_GPU_FRAME() AdvancedGPUProfiler::getInstance().endFrame()
#define PRINT_GPU_STATS() AdvancedGPUProfiler::getInstance().printResults()
#define GET_GPU_STATS(name) AdvancedGPUProfiler::getInstance().getStats(name)
#define GET_FPS() AdvancedGPUProfiler::getInstance().getFPS()
