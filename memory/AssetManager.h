#pragma once
/**
 * Asset Manager - Game-Standard Asset Loading & Caching
 * 
 * Features:
 * - Async loading with progress tracking
 * - Reference counting for automatic cleanup
 * - Memory-aware caching (LRU eviction)
 * - Loading queues for batch operations
 * 
 * Usage:
 *   AssetManager assets;
 *   assets.loadAsync<Model>("character.fbx");
 *   Model* model = assets.get<Model>("character.fbx");
 */

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <future>
#include <queue>
#include <functional>
#include <chrono>
#include <atomic>

// Forward declarations
class Model;
class Animation;
class Texture;
class Shader;

// ============================================================================
// Asset Handle - Reference-counted asset wrapper
// ============================================================================

template<typename T>
class AssetHandle {
public:
    AssetHandle() : asset(nullptr), refCount(nullptr) {}
    
    explicit AssetHandle(T* asset) 
        : asset(asset)
        , refCount(asset ? new size_t(1) : nullptr) {}
    
    AssetHandle(const AssetHandle& other)
        : asset(other.asset)
        , refCount(other.refCount) {
        if (refCount) {
            ++(*refCount);
        }
    }
    
    AssetHandle& operator=(const AssetHandle& other) {
        if (this != &other) {
            release();
            asset = other.asset;
            refCount = other.refCount;
            if (refCount) {
                ++(*refCount);
            }
        }
        return *this;
    }
    
    ~AssetHandle() {
        release();
    }
    
    T* get() const { return asset; }
    T* operator->() const { return asset; }
    T& operator*() const { return *asset; }
    explicit operator bool() const { return asset != nullptr; }
    
    size_t useCount() const { return refCount ? *refCount : 0; }
    
private:
    void release() {
        if (refCount) {
            --(*refCount);
            if (*refCount == 0) {
                delete asset;
                delete refCount;
            }
            refCount = nullptr;
        }
        asset = nullptr;
    }
    
    T* asset;
    size_t* refCount;
};

// ============================================================================
// Loading Progress
// ============================================================================

struct LoadingProgress {
    enum class State {
        Pending,
        Loading,
        Complete,
        Failed
    };
    
    State state = State::Pending;
    float percent = 0.0f;
    std::string currentAsset;
    std::string error;
    
    bool isComplete() const { return state == State::Complete; }
    bool isFailed() const { return state == State::Failed; }
    bool isLoading() const { return state == State::Loading; }
};

// ============================================================================
// Asset Manager
// ============================================================================

class AssetManager {
public:
    AssetManager();
    ~AssetManager();

    // ========================================================================
    // Asset Loading
    // ========================================================================
    
    // Load asset synchronously (blocks until complete)
    template<typename T>
    AssetHandle<T> load(const std::string& path);
    
    // Load asset asynchronously (returns immediately)
    template<typename T>
    std::future<AssetHandle<T>> loadAsync(const std::string& path);
    
    // Load multiple assets asynchronously
    template<typename T>
    void loadAllAsync(const std::vector<std::string>& paths);
    
    // Get loading progress
    LoadingProgress getLoadingProgress() const;
    
    // Wait for all async loads to complete
    void waitForAllLoads();
    
    // ========================================================================
    // Asset Access
    // ========================================================================
    
    // Get asset by path (returns null handle if not loaded)
    template<typename T>
    AssetHandle<T> get(const std::string& path);
    
    // Get or load asset (loads if not cached)
    template<typename T>
    AssetHandle<T> getOrLoad(const std::string& path);
    
    // Check if asset is loaded
    template<typename T>
    bool isLoaded(const std::string& path) const;
    
    // ========================================================================
    // Statistics
    // ========================================================================
    
    size_t getAssetCount() const { return assetCount; }
    size_t getLoadingQueueSize() const;
    
    // Cache management
    void setMaxCacheSize(size_t maxBytes);
    void clearAll();
    
    struct Stats {
        size_t totalAssets;
        size_t totalCacheBytes;
        size_t maxCacheBytes;
        size_t loadingQueueSize;
        size_t loadHits;
        size_t loadMisses;
        size_t cacheEvictions;
    };
    
    Stats getStats() const;
    void printStats() const;

private:
    // Asset info for caching
    struct AssetInfo {
        std::string path;
        size_t sizeBytes;
        std::chrono::steady_clock::time_point lastAccess;
        size_t refCount;
    };
    
    // Base class for type-erased asset storage
    struct AssetEntryBase {
        virtual ~AssetEntryBase() = default;
        virtual size_t getSize() const = 0;
        virtual std::string getPath() const = 0;
        virtual std::chrono::steady_clock::time_point getLastAccess() const = 0;
    };
    
    // Type-specific asset entry
    template<typename T>
    struct AssetEntry : AssetEntryBase {
        T* asset;
        std::string path;
        std::chrono::steady_clock::time_point lastAccess;
        
        AssetEntry(T* a, const std::string& p) : asset(a), path(p), lastAccess(std::chrono::steady_clock::now()) {}
        ~AssetEntry() { delete asset; }
        
        size_t getSize() const override { return sizeof(T); }
        std::string getPath() const override { return path; }
        std::chrono::steady_clock::time_point getLastAccess() const override { return lastAccess; }
    };
    
    // Loading job
    struct LoadJob {
        std::string path;
        std::promise<void> promise;
        std::chrono::steady_clock::time_point startTime;
    };
    
    // Internal methods
    void updateCacheLRU();
    void evictLRU();
    
    // Asset loading functions (specialized per type)
    Model* loadModel(const std::string& path);
    Animation* loadAnimation(const std::string& path);
    
    // Thread safety
    mutable std::mutex cacheMutex;
    mutable std::mutex queueMutex;
    
    // Asset cache (path -> asset)
    std::unordered_map<std::string, AssetEntryBase*> assetCache;
    
    // Loading queue
    std::queue<std::unique_ptr<LoadJob>> loadingQueue;
    std::vector<std::future<void>> activeLoads;
    
    // Loading progress
    mutable std::mutex progressMutex;
    LoadingProgress progress;
    
    // Cache limits
    size_t maxCacheSize;
    size_t currentCacheSize;
    size_t assetCount;
    
    // Statistics
    size_t loadHits;
    size_t loadMisses;
    size_t cacheEvictions;
};

// ============================================================================
// Template Implementations
// ============================================================================

template<typename T>
AssetHandle<T> AssetManager::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    
    // Check cache first
    auto it = assetCache.find(path);
    if (it != assetCache.end()) {
        ++loadHits;
        auto entry = static_cast<AssetEntry<T>*>(it->second);
        entry->lastAccess = std::chrono::steady_clock::now();
        return AssetHandle<T>(entry->asset);
    }
    
    ++loadMisses;
    
    // For now, return null (actual loading requires asset type definitions)
    return AssetHandle<T>(nullptr);
}

template<typename T>
std::future<AssetHandle<T>> AssetManager::loadAsync(const std::string& path) {
    std::promise<AssetHandle<T>> promise;
    std::future<AssetHandle<T>> result = promise.get_future();
    
    // For now, immediately return null
    promise.set_value(AssetHandle<T>(nullptr));
    
    return result;
}

template<typename T>
void AssetManager::loadAllAsync(const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        loadAsync<T>(path);
    }
}

template<typename T>
AssetHandle<T> AssetManager::get(const std::string& path) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    
    auto it = assetCache.find(path);
    if (it != assetCache.end()) {
        ++loadHits;
        auto entry = static_cast<AssetEntry<T>*>(it->second);
        entry->lastAccess = std::chrono::steady_clock::now();
        return AssetHandle<T>(entry->asset);
    }
    
    ++loadMisses;
    return AssetHandle<T>(nullptr);
}

template<typename T>
bool AssetManager::isLoaded(const std::string& path) const {
    std::lock_guard<std::mutex> lock(cacheMutex);
    return assetCache.find(path) != assetCache.end();
}
