#include "AssetManager.h"
#include "MemoryArena.h"
#include <iostream>
#include <algorithm>

// ============================================================================
// Asset Manager Implementation
// ============================================================================

AssetManager::AssetManager()
    : maxCacheSize(512 * 1024 * 1024)  // 512 MB default
    , currentCacheSize(0)
    , assetCount(0)
    , loadHits(0)
    , loadMisses(0)
    , cacheEvictions(0)
{
    std::cout << "[AssetManager] Initialized (cache limit: " 
              << (maxCacheSize / (1024 * 1024)) << " MB)\n";
}

AssetManager::~AssetManager() {
    clearAll();
}

AssetManager::Stats AssetManager::getStats() const {
    std::lock_guard<std::mutex> lock(cacheMutex);
    
    Stats stats;
    stats.totalAssets = assetCount;
    stats.totalCacheBytes = currentCacheSize;
    stats.maxCacheBytes = maxCacheSize;
    stats.loadingQueueSize = getLoadingQueueSize();
    stats.loadHits = loadHits;
    stats.loadMisses = loadMisses;
    stats.cacheEvictions = cacheEvictions;
    
    return stats;
}

void AssetManager::printStats() const {
    Stats stats = getStats();
    
    std::cout << "\n========== ASSET MANAGER STATS ==========\n";
    std::cout << "Assets loaded: " << stats.totalAssets << "\n";
    std::cout << "Cache usage: " << (stats.totalCacheBytes / 1024 / 1024) 
              << " MB / " << (stats.maxCacheBytes / 1024 / 1024) << " MB\n";
    std::cout << "Loading queue: " << stats.loadingQueueSize << " assets\n";
    std::cout << "Cache hits: " << stats.loadHits << "\n";
    std::cout << "Cache misses: " << stats.loadMisses << "\n";
    
    if (stats.loadHits + stats.loadMisses > 0) {
        float hitRate = 100.0f * stats.loadHits / (stats.loadHits + stats.loadMisses);
        std::cout << "Cache hit rate: " << hitRate << "%\n";
    }
    
    std::cout << "Cache evictions: " << stats.cacheEvictions << "\n";
    std::cout << "========================================\n\n";
}

size_t AssetManager::getLoadingQueueSize() const {
    std::lock_guard<std::mutex> lock(queueMutex);
    return loadingQueue.size();
}

LoadingProgress AssetManager::getLoadingProgress() const {
    std::lock_guard<std::mutex> lock(progressMutex);
    return progress;
}

void AssetManager::setMaxCacheSize(size_t maxBytes) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    maxCacheSize = maxBytes;
}

void AssetManager::clearAll() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    
    for (auto& pair : assetCache) {
        delete pair.second;
    }
    
    assetCache.clear();
    currentCacheSize = 0;
    assetCount = 0;
    
    std::cout << "[AssetManager] Cleared all assets\n";
}

void AssetManager::waitForAllLoads() {
    // Wait for active loads
    std::lock_guard<std::mutex> lock(queueMutex);
    for (auto& future : activeLoads) {
        if (future.valid()) {
            future.wait();
        }
    }
    activeLoads.clear();
}
