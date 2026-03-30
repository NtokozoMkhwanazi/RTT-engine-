#include "GPUProfilerAdvanced.h"
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <fstream>
#include <numeric>
#include <cmath>

// ============================================================================
// Query Pool Implementation
// ============================================================================

GLuint AdvancedGPUProfiler::QueryPool::acquire() {
    if (!freeQueries.empty()) {
        GLuint query = freeQueries.back();
        freeQueries.pop_back();
        activeQueries.push_back(query);
        return query;
    }
    
    GLuint query = 0;
    glGenQueries(1, &query);
    if (query != 0) {
        activeQueries.push_back(query);
    }
    return query;
}

void AdvancedGPUProfiler::QueryPool::release(GLuint query) {
    auto it = std::find(activeQueries.begin(), activeQueries.end(), query);
    if (it != activeQueries.end()) {
        activeQueries.erase(it);
        freeQueries.push_back(query);
    }
}

void AdvancedGPUProfiler::QueryPool::cleanup() {
    for (GLuint query : freeQueries) {
        if (query != 0) glDeleteQueries(1, &query);
    }
    for (GLuint query : activeQueries) {
        if (query != 0) glDeleteQueries(1, &query);
    }
    freeQueries.clear();
    activeQueries.clear();
}

// ============================================================================
// Advanced GPU Profiler Implementation
// ============================================================================

AdvancedGPUProfiler::AdvancedGPUProfiler() 
    : m_rootScope(std::make_unique<ProfileScope>()) {
    m_rootScope->name = "Root";
    m_rootScope->id = 0;
    m_currentScope = m_rootScope.get();
}

AdvancedGPUProfiler::~AdvancedGPUProfiler() {
    shutdown();
}

AdvancedGPUProfiler& AdvancedGPUProfiler::getInstance() {
    static AdvancedGPUProfiler instance;
    return instance;
}

bool AdvancedGPUProfiler::initialize() {
    if (m_initialized) return true;

    // Check if timestamp queries are supported
    GLint bits = 0;
    glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);

    if (bits == 0) {
        std::cerr << "[GPUProfiler] Timestamp queries not supported!\n";
        return false;
    }

    // Create frame timing queries
    glGenQueries(1, &m_frameStartQuery);
    glGenQueries(1, &m_frameEndQuery);

    m_initialized = true;
    m_lastFPSTime = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();

    std::cout << "[GPUProfiler] Advanced mode initialized (" 
              << bits << " timestamp bits, hierarchical profiling enabled)\n";
    return true;
}

void AdvancedGPUProfiler::shutdown() {
    if (m_frameStartQuery != 0) glDeleteQueries(1, &m_frameStartQuery);
    if (m_frameEndQuery != 0) glDeleteQueries(1, &m_frameEndQuery);
    m_queryPool.cleanup();
    m_initialized = false;
}

GLuint AdvancedGPUProfiler::acquireQuery() {
    return m_queryPool.acquire();
}

void AdvancedGPUProfiler::releaseQuery(GLuint query) {
    m_queryPool.release(query);
}

uint64_t AdvancedGPUProfiler::generateScopeId() {
    return m_nextScopeId++;
}

void AdvancedGPUProfiler::beginFrame() {
    if (!m_enabled || !m_initialized) return;
    if (m_inFrame) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    
    m_inFrame = true;
    m_currentFrame++;
    
    // Reset root scope
    m_rootScope->children.clear();
    m_currentScope = m_rootScope.get();
    
    // Start frame timing
    glQueryCounter(m_frameStartQuery, GL_TIMESTAMP);
}

void AdvancedGPUProfiler::endFrame() {
    if (!m_enabled || !m_initialized) return;
    if (!m_inFrame) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    // End frame timing
    glQueryCounter(m_frameEndQuery, GL_TIMESTAMP);

    // Collect results
    collectResults();
    
    // Update statistics
    updateStats();

    m_inFrame = false;

    // Calculate FPS
    double now = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();

    double frameTime = (now - m_lastFPSTime) * 1000.0;
    m_lastFPSTime = now;

    m_frameCount++;
    m_frameTimeAccumulator += frameTime;

    if (m_frameTimeAccumulator >= 1000.0) {
        m_fps = m_frameCount;
        m_avgFrameTimeMs = m_frameTimeAccumulator / m_frameCount;
        m_frameCount = 0;
        m_frameTimeAccumulator = 0.0;
    }

    m_currentFrameTimeMs = frameTime;
}

void AdvancedGPUProfiler::beginScope(const std::string& name) {
    if (!m_enabled || !m_initialized) return;
    if (!m_inFrame) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    // Create or find scope
    uint64_t parentId = m_currentScope ? m_currentScope->id : 0;
    ProfileScope* scope = findOrCreateScope(name, parentId);
    
    if (!scope || scope->isOpen) return;

    // Setup queries
    scope->startQuery = acquireQuery();
    scope->endQuery = acquireQuery();
    
    if (scope->startQuery == 0 || scope->endQuery == 0) {
        std::cerr << "[GPUProfiler] Failed to create queries for scope: " << name << "\n";
        return;
    }

    // Start GPU query
    glQueryCounter(scope->startQuery, GL_TIMESTAMP);
    
    // Start CPU timing
    scope->cpuStart = std::chrono::high_resolution_clock::now();
    scope->isOpen = true;
    
    // Push to stack
    m_currentScope = scope;
}

void AdvancedGPUProfiler::endScope() {
    if (!m_enabled || !m_initialized) return;
    if (!m_inFrame) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_currentScope || m_currentScope == m_rootScope.get()) return;

    // End CPU timing
    m_currentScope->cpuEnd = std::chrono::high_resolution_clock::now();
    
    // End GPU query
    glQueryCounter(m_currentScope->endQuery, GL_TIMESTAMP);
    
    m_currentScope->isOpen = false;
    
    // Store for result collection
    m_pendingQueries.push_back(m_currentScope->startQuery);
    m_pendingQueries.push_back(m_currentScope->endQuery);
    
    // Pop to parent
    AdvancedGPUProfiler::ProfileScope* parent = m_currentScope;
    m_currentScope = m_rootScope.get();  // Simplified - in production, track parent pointer
    
    // Find actual parent
    for (auto& child : m_rootScope->children) {
        if (child.get() == parent) {
            // Found it
            break;
        }
    }
}

void AdvancedGPUProfiler::profileScope(const std::string& name, std::function<void()> callback) {
    beginScope(name);
    callback();
    endScope();
}

AdvancedGPUProfiler::ProfileScope* AdvancedGPUProfiler::getCurrentScope() {
    return m_currentScope;
}

AdvancedGPUProfiler::ProfileScope* AdvancedGPUProfiler::findOrCreateScope(const std::string& name, uint64_t parentId) {
    // Check if scope exists in current frame
    for (auto& child : m_rootScope->children) {
        if (child->name == name && child->parentId == parentId) {
            return child.get();
        }
    }

    // Create new scope
    auto scope = std::make_unique<ProfileScope>();
    scope->name = name;
    scope->id = generateScopeId();
    scope->parentId = parentId;
    scope->depth = 0;  // Will be calculated later

    AdvancedGPUProfiler::ProfileScope* ptr = scope.get();
    m_rootScope->children.push_back(std::move(scope));
    m_scopeIdToName[ptr->id] = name;

    return ptr;
}

void AdvancedGPUProfiler::collectResults() {
    // Get frame time
    GLuint64 frameStart = 0, frameEnd = 0;
    
    if (glGetQueryObjectui64v) {
        glGetQueryObjectui64v(m_frameStartQuery, GL_QUERY_RESULT, &frameStart);
        glGetQueryObjectui64v(m_frameEndQuery, GL_QUERY_RESULT, &frameEnd);
    }

    double frameTimeNs = frameEnd - frameStart;
    m_currentFrameTimeMs = frameTimeNs / 1000000.0;

    // Collect query results for all scopes
    for (auto& scope : m_rootScope->children) {
        if (scope->startQuery == 0 || scope->endQuery == 0) continue;
        
        GLuint64 startTime = 0, endTime = 0;
        
        // Check if query result is available
        GLint available = 0;
        glGetQueryObjectiv(scope->endQuery, GL_QUERY_RESULT_AVAILABLE, &available);
        
        if (!available) {
            // Query not ready yet, skip this frame
            continue;
        }
        
        glGetQueryObjectui64v(scope->startQuery, GL_QUERY_RESULT, &startTime);
        glGetQueryObjectui64v(scope->endQuery, GL_QUERY_RESULT, &endTime);

        double elapsedNs = endTime - startTime;
        double elapsedMs = elapsedNs / 1000000.0;
        
        // Get CPU time
        auto cpuElapsed = std::chrono::duration<double, std::milli>(
            scope->cpuEnd - scope->cpuStart
        ).count();

        // Update statistics
        ProfileStats& stats = m_stats[scope->name];
        stats.name = scope->name;
        stats.addSample(elapsedMs);
        stats.cpuTimeMs = cpuElapsed;
        stats.depth = scope->depth;
        stats.scopeId = scope->id;
        stats.parentId = scope->parentId;
        stats.lastSeenFrame = m_currentFrame;
        
        // Release queries back to pool
        releaseQuery(scope->startQuery);
        releaseQuery(scope->endQuery);
        scope->startQuery = 0;
        scope->endQuery = 0;
    }
    
    m_pendingQueries.clear();
}

void AdvancedGPUProfiler::updateStats() {
    // Build hierarchy and calculate depths
    for (auto& scope : m_rootScope->children) {
        calculateDepth(scope.get(), 1);
    }
}

void AdvancedGPUProfiler::calculateDepth(AdvancedGPUProfiler::ProfileScope* scope, int depth) {
    if (!scope) return;
    scope->depth = depth;
    
    // Update stats depth
    auto it = m_stats.find(scope->name);
    if (it != m_stats.end()) {
        it->second.depth = depth;
    }
    
    for (auto& child : scope->children) {
        calculateDepth(child.get(), depth + 1);
    }
}

const AdvancedGPUProfiler::ProfileStats& AdvancedGPUProfiler::getStats(const std::string& name) const {
    static ProfileStats empty;
    auto it = m_stats.find(name);
    if (it == m_stats.end()) return empty;
    return it->second;
}

std::vector<AdvancedGPUProfiler::ProfileStats> AdvancedGPUProfiler::getAllStats() const {
    std::vector<ProfileStats> results;
    for (const auto& [name, stats] : m_stats) {
        results.push_back(stats);
    }
    
    // Sort by average time (descending)
    std::sort(results.begin(), results.end(),
        [](const ProfileStats& a, const ProfileStats& b) {
            return a.avgTimeMs > b.avgTimeMs;
        });
    
    return results;
}

std::vector<AdvancedGPUProfiler::ProfileStats> AdvancedGPUProfiler::getTopScopes(int count) const {
    auto all = getAllStats();
    if (static_cast<int>(all.size()) <= count) {
        return all;
    }
    return std::vector<ProfileStats>(all.begin(), all.begin() + count);
}

void AdvancedGPUProfiler::setHistorySize(size_t size) {
    m_historySize = size;
    for (auto& [name, stats] : m_stats) {
        while (stats.history.size() > size) {
            stats.history.erase(stats.history.begin());
        }
    }
}

void AdvancedGPUProfiler::printResults() const {
    std::cout << "\n========== ADVANCED GPU PROFILER RESULTS ==========\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Frame: " << m_currentFrame << " | FPS: " << m_fps 
              << " | Frame Time: " << m_currentFrameTimeMs << " ms\n\n";

    auto results = getAllStats();
    
    // Print hierarchical view
    std::cout << "Hierarchical View:\n";
    std::cout << "─────────────────────────────────────────────────────────\n";
    
    for (const auto& result : results) {
        std::string indent(result.depth * 2, ' ');
        std::cout << indent << std::setw(30) << result.name << ": "
                  << std::setw(8) << result.avgTimeMs << " ms"
                  << " ±" << std::setw(5) << result.stdDevMs
                  << " [" << result.frameCount << "]";
        
        // Show CPU/GPU ratio if significant difference
        if (result.cpuTimeMs > 0 && result.avgTimeMs > 0) {
            double ratio = result.cpuTimeMs / result.avgTimeMs;
            if (ratio < 0.8 || ratio > 1.2) {
                std::cout << " (CPU: " << result.cpuTimeMs << " ms)";
            }
        }
        
        std::cout << "\n";
    }
    
    std::cout << "─────────────────────────────────────────────────────────\n\n";
}

void AdvancedGPUProfiler::printFrameSummary() const {
    std::cout << "[GPU] Frame: " << m_currentFrameTimeMs << " ms"
              << " | FPS: " << m_fps
              << " | Avg: " << m_avgFrameTimeMs << " ms"
              << " | Scopes: " << m_stats.size() << "\n";
}

void AdvancedGPUProfiler::exportToCSV(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "[GPUProfiler] Failed to open " << filename << " for writing\n";
        return;
    }
    
    // Header
    file << "Scope,Depth,Avg(ms),Min(ms),Max(ms),StdDev(ms),Samples\n";
    
    // Data
    auto results = getAllStats();
    for (const auto& result : results) {
        file << result.name << ","
             << result.depth << ","
             << std::fixed << std::setprecision(3)
             << result.avgTimeMs << ","
             << result.minTimeMs << ","
             << result.maxTimeMs << ","
             << result.stdDevMs << ","
             << result.frameCount << "\n";
    }
    
    file.close();
    std::cout << "[GPUProfiler] Exported results to " << filename << "\n";
}

void AdvancedGPUProfiler::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stats.clear();
    m_scopeIdToName.clear();
    m_frameCount = 0;
    m_fps = 0;
    m_avgFrameTimeMs = 0.0;
    m_frameTimeAccumulator = 0.0;
    m_nextScopeId = 1;
}

void AdvancedGPUProfiler::resetFrame() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rootScope->children.clear();
    m_currentScope = m_rootScope.get();
    m_pendingQueries.clear();
}

void AdvancedGPUProfiler::resetStats(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_stats.find(name);
    if (it != m_stats.end()) {
        it->second.reset();
    }
}
