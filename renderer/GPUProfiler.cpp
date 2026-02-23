#include "GPUProfiler.h"
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <limits>

GPUProfiler::GPUProfiler() {}

GPUProfiler::~GPUProfiler() {
    for (auto& pair : m_currentQueries) {
        destroyQueryPair(pair);
    }
    if (m_frameStartQuery != 0) glDeleteQueries(1, &m_frameStartQuery);
    if (m_frameEndQuery != 0) glDeleteQueries(1, &m_frameEndQuery);
}

GPUProfiler& GPUProfiler::getInstance() {
    static GPUProfiler instance;
    return instance;
}

bool GPUProfiler::initialize() {
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

    std::cout << "[GPUProfiler] Initialized (" << bits << " timestamp bits)\n";
    return true;
}

void GPUProfiler::createQueryPair(QueryPair& pair) {
    glGenQueries(1, &pair.startQuery);
    glGenQueries(1, &pair.endQuery);
}

void GPUProfiler::destroyQueryPair(QueryPair& pair) {
    if (pair.startQuery != 0) glDeleteQueries(1, &pair.startQuery);
    if (pair.endQuery != 0) glDeleteQueries(1, &pair.endQuery);
    pair.startQuery = 0;
    pair.endQuery = 0;
}

void GPUProfiler::beginFrame() {
    if (!m_enabled || !m_initialized) return;
    if (m_insideFrame) return;

    m_insideFrame = true;
    m_currentQueries.clear();
    m_queryIndex.clear();

    // Start frame timing
    glQueryCounter(m_frameStartQuery, GL_TIMESTAMP);
}

void GPUProfiler::endFrame() {
    if (!m_enabled || !m_initialized) return;
    if (!m_insideFrame) return;

    // End frame timing
    glQueryCounter(m_frameEndQuery, GL_TIMESTAMP);

    // Collect results
    collectResults();

    m_insideFrame = false;

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

void GPUProfiler::beginQuery(const std::string& name) {
    if (!m_enabled || !m_initialized) return;
    if (!m_insideFrame) return;

    auto it = m_queryIndex.find(name);
    if (it != m_queryIndex.end()) {
        // Reuse existing query
        QueryPair& pair = m_currentQueries[it->second];
        glBeginQuery(GL_TIME_ELAPSED, pair.startQuery);
    } else {
        // Create new query
        size_t index = m_currentQueries.size();
        m_currentQueries.emplace_back();
        QueryPair& pair = m_currentQueries.back();
        pair.name = name;
        createQueryPair(pair);
        m_queryIndex[name] = index;
        glBeginQuery(GL_TIME_ELAPSED, pair.startQuery);
    }
}

void GPUProfiler::endQuery(const std::string& name) {
    if (!m_enabled || !m_initialized) return;
    if (!m_insideFrame) return;

    auto it = m_queryIndex.find(name);
    if (it != m_queryIndex.end()) {
        glEndQuery(GL_TIME_ELAPSED);
    }
}

void GPUProfiler::collectResults() {
    // Get frame time
    GLuint64 frameStart, frameEnd;
    glGetQueryObjectui64v(m_frameStartQuery, GL_QUERY_RESULT, &frameStart);
    glGetQueryObjectui64v(m_frameEndQuery, GL_QUERY_RESULT, &frameEnd);
    
    double frameTimeNs = frameEnd - frameStart;
    m_currentFrameTimeMs = frameTimeNs / 1000000.0;

    // Collect query results
    for (auto& pair : m_currentQueries) {
        GLuint64 startTime, endTime;
        glGetQueryObjectui64v(pair.startQuery, GL_QUERY_RESULT, &startTime);
        glGetQueryObjectui64v(pair.endQuery, GL_QUERY_RESULT, &endTime);

        double elapsedNs = endTime - startTime;
        double elapsedMs = elapsedNs / 1000000.0;

        ProfileResult& result = m_results[pair.name];
        result.name = pair.name;
        result.gpuTimeMs = elapsedMs;
        result.frameCount++;
        
        // Update average
        result.avgGpuTimeMs = (result.avgGpuTimeMs * (result.frameCount - 1) + elapsedMs) 
                             / result.frameCount;
        
        // Update min/max
        result.minGpuTimeMs = std::min(result.minGpuTimeMs, elapsedMs);
        result.maxGpuTimeMs = std::max(result.maxGpuTimeMs, elapsedMs);
    }
}

const GPUProfiler::ProfileResult& GPUProfiler::getResult(const std::string& name) const {
    static ProfileResult empty;
    auto it = m_results.find(name);
    if (it == m_results.end()) return empty;
    return it->second;
}

std::vector<GPUProfiler::ProfileResult> GPUProfiler::getAllResults() const {
    std::vector<ProfileResult> results;
    for (const auto& [name, result] : m_results) {
        results.push_back(result);
    }
    std::sort(results.begin(), results.end(), 
        [](const ProfileResult& a, const ProfileResult& b) {
            return a.avgGpuTimeMs > b.avgGpuTimeMs;
        });
    return results;
}

void GPUProfiler::printResults() const {
    std::cout << "\n========== GPU PROFILER RESULTS ==========\n";
    std::cout << std::fixed << std::setprecision(2);
    
    auto results = getAllResults();
    for (const auto& result : results) {
        std::cout << std::setw(30) << result.name << ": "
                  << std::setw(8) << result.avgGpuTimeMs << " ms"
                  << " (min: " << result.minGpuTimeMs 
                  << ", max: " << result.maxGpuTimeMs << ")"
                  << " [" << result.frameCount << " frames]\n";
    }
    
    std::cout << "========================================\n\n";
}

void GPUProfiler::printFrameSummary() const {
    std::cout << "[GPU] Frame: " << m_currentFrameTimeMs << " ms"
              << " | FPS: " << m_fps
              << " | Avg: " << m_avgFrameTimeMs << " ms\n";
}

void GPUProfiler::reset() {
    m_results.clear();
    m_frameCount = 0;
    m_fps = 0;
    m_avgFrameTimeMs = 0.0;
    m_frameTimeAccumulator = 0.0;
}

void GPUProfiler::resetFrame() {
    m_currentQueries.clear();
    m_queryIndex.clear();
}
