/**
 * @file utils.cpp
 * @brief Implements utility functions including optimized thread-safe memory validation.
 */

#include "utils.h"
#include "logger.h"
#include <windows.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <atomic>

// --- Memory Region Cache Implementation ---

// Constants for cache configuration
constexpr size_t MEMORY_CACHE_SIZE = 32;       // Fixed number of cache entries
constexpr unsigned int CACHE_EXPIRY_MS = 5000; // Cache entry expiry time (5 seconds)

// Memory region cache and synchronization
static std::array<MemoryRegionInfo, MEMORY_CACHE_SIZE> g_memoryCache;
static std::mutex g_cacheMutex;       // Mutex for thread-safe cache access
std::once_flag g_memoryCacheInitFlag; // For std::call_once initialization

// Optional cache statistics for tuning (debug only)
#ifdef _DEBUG
static std::atomic<uint64_t> g_cacheHits{0};
static std::atomic<uint64_t> g_cacheMisses{0};
#endif

/**
 * @brief Initialize the memory region cache.
 * @details Thread-safe, one-time initialization using std::call_once.
 */
void initMemoryCache()
{
    std::call_once(g_memoryCacheInitFlag, []()
                   {
         std::lock_guard<std::mutex> lock(g_cacheMutex);

         for (auto& entry : g_memoryCache) {
             entry.valid = false;
         }

         Logger::getInstance().log(LOG_DEBUG, "Memory region cache initialized with " +
                                              std::to_string(MEMORY_CACHE_SIZE) + " entries"); });
}

/**
 * @brief Clears all cache entries.
 * @details Thread-safe clearing operation for cleanup.
 */
void clearMemoryCache()
{
    std::lock_guard<std::mutex> lock(g_cacheMutex);

    for (auto &entry : g_memoryCache)
    {
        entry.valid = false;
    }
    Logger::getInstance().log(LOG_DEBUG, "Memory region cache cleared");
}

/**
 * @brief Get cache performance statistics.
 * @return String with hit/miss counts and hit rate percentage.
 */
std::string getMemoryCacheStats()
{
#ifdef _DEBUG
    uint64_t hits = g_cacheHits.load(std::memory_order_relaxed);
    uint64_t misses = g_cacheMisses.load(std::memory_order_relaxed);
    uint64_t total = hits + misses;

    std::ostringstream oss;
    oss << "Cache hits: " << hits << ", misses: " << misses;

    if (total > 0)
    {
        double hitRate = (static_cast<double>(hits) / static_cast<double>(total)) * 100.0;
        oss << ", hit rate: " << std::fixed << std::setprecision(2) << hitRate << "%";
    }

    return oss.str();
#else
    return "Cache statistics not available in release build";
#endif
}

/**
 * @brief Find a cache entry that contains the given memory range.
 * @return Pointer to cache entry if found, nullptr otherwise.
 * @note Caller must hold g_cacheMutex lock before calling this function.
 */
static MemoryRegionInfo *findCacheEntry(uintptr_t address, size_t size)
{
    uintptr_t endAddress = address + size;

    // Check expiry time for cache entries
    auto now = std::chrono::steady_clock::now();

    for (auto &entry : g_memoryCache)
    {
        if (!entry.valid)
            continue;

        // Check if entry is expired
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - entry.timestamp)
                       .count();
        if (age > CACHE_EXPIRY_MS)
        {
            entry.valid = false;
            continue;
        }

        // Check if address range is contained within this entry
        uintptr_t entryEnd = entry.baseAddress + entry.regionSize;
        if (address >= entry.baseAddress && endAddress <= entryEnd)
        {
            // Update timestamp for LRU behavior
            entry.timestamp = now;
            return &entry;
        }
    }

    return nullptr;
}

/**
 * @brief Add or update a cache entry.
 * @note Caller must hold g_cacheMutex lock before calling this function.
 */
static void updateCacheEntry(const MEMORY_BASIC_INFORMATION &mbi)
{
    auto now = std::chrono::steady_clock::now();

    // First, try to find an invalid entry to reuse
    auto it = std::find_if(g_memoryCache.begin(), g_memoryCache.end(),
                           [](const MemoryRegionInfo &entry)
                           { return !entry.valid; });

    // If no invalid entries, find the oldest entry
    if (it == g_memoryCache.end())
    {
        it = std::min_element(g_memoryCache.begin(), g_memoryCache.end(),
                              [](const MemoryRegionInfo &a, const MemoryRegionInfo &b)
                              {
                                  return a.timestamp < b.timestamp;
                              });
    }

    // Update the selected entry
    it->baseAddress = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    it->regionSize = mbi.RegionSize;
    it->protection = mbi.Protect;
    it->timestamp = now;
    it->valid = true;
}

/**
 * @brief Checks if memory at the specified address is readable.
 * @details Uses a hybrid approach for thread safety: checks cache with minimal locking,
 *          performs VirtualQuery outside lock, then updates cache with minimal locking.
 * @param address Starting address to check (const volatile to indicate memory
 *                might change even though function doesn't modify it).
 * @param size Number of bytes to check.
 * @return true if all bytes are readable, false otherwise.
 */
bool isMemoryReadable(const volatile void *address, size_t size)
{
    if (!address || size == 0)
        return false;

    // Ensure cache is initialized (thread-safe, executes only once)
    std::call_once(g_memoryCacheInitFlag, initMemoryCache);

    uintptr_t addrValue = reinterpret_cast<uintptr_t>(address);

    // Create a local copy to store cache result if found
    bool cacheHit = false;
    bool isRegionReadable = false;

    // First lock scope - cache lookup only
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);

        // Check cache first
        MemoryRegionInfo *cachedInfo = findCacheEntry(addrValue, size);
        if (cachedInfo)
        {
            // Check if the region is readable based on cached protection flags
            const DWORD READ_FLAGS = (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                      PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
            isRegionReadable = ((cachedInfo->protection & READ_FLAGS) &&
                                !(cachedInfo->protection & PAGE_NOACCESS) &&
                                !(cachedInfo->protection & PAGE_GUARD));
            cacheHit = true;

#ifdef _DEBUG
            g_cacheHits.fetch_add(1, std::memory_order_relaxed);
#endif
        }
    }

    // If cache hit, return result immediately
    if (cacheHit)
    {
        return isRegionReadable;
    }

    // Cache miss, perform actual VirtualQuery outside of lock
#ifdef _DEBUG
    g_cacheMisses.fetch_add(1, std::memory_order_relaxed);
#endif

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(const_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0)
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    const DWORD READ_FLAGS = (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    if (!(mbi.Protect & READ_FLAGS) || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD))
        return false;

    uintptr_t start = addrValue;
    uintptr_t end = start + size;
    if (end < start) // Overflow check
        return false;

    uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t region_end = region_start + mbi.RegionSize;

    bool result = (start >= region_start && end <= region_end);

    // Cache the result if memory is readable
    if (result)
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        updateCacheEntry(mbi);
    }

    return result;
}

/**
 * @brief Checks if memory at the specified address is writable.
 * @details Uses a hybrid approach for thread safety: checks cache with minimal locking,
 *          performs VirtualQuery outside lock, then updates cache with minimal locking.
 * @param address Starting address to check (volatile to indicate memory
 *                might change even though function doesn't modify it).
 * @param size Number of bytes to check.
 * @return true if all bytes are writable, false otherwise.
 */
bool isMemoryWritable(volatile void *address, size_t size)
{
    if (!address || size == 0)
        return false;

    // Ensure cache is initialized (thread-safe, executes only once)
    std::call_once(g_memoryCacheInitFlag, initMemoryCache);

    uintptr_t addrValue = reinterpret_cast<uintptr_t>(address);

    // Create a local copy to store cache result if found
    bool cacheHit = false;
    bool isRegionWritable = false;

    // First lock scope - cache lookup only
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);

        // Check cache first
        MemoryRegionInfo *cachedInfo = findCacheEntry(addrValue, size);
        if (cachedInfo)
        {
            // Check if the region is writable based on cached protection flags
            const DWORD WRITE_FLAGS = (PAGE_READWRITE | PAGE_WRITECOPY |
                                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
            isRegionWritable = ((cachedInfo->protection & WRITE_FLAGS) &&
                                !(cachedInfo->protection & PAGE_NOACCESS) &&
                                !(cachedInfo->protection & PAGE_GUARD));
            cacheHit = true;

#ifdef _DEBUG
            g_cacheHits.fetch_add(1, std::memory_order_relaxed);
#endif
        }
    }

    // If cache hit, return result immediately
    if (cacheHit)
    {
        return isRegionWritable;
    }

    // Cache miss, perform actual VirtualQuery outside of lock
#ifdef _DEBUG
    g_cacheMisses.fetch_add(1, std::memory_order_relaxed);
#endif

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(const_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0)
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    const DWORD WRITE_FLAGS = (PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    if (!(mbi.Protect & WRITE_FLAGS) || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD))
        return false;

    uintptr_t start = addrValue;
    uintptr_t end = start + size;
    if (end < start) // Overflow check
        return false;

    uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t region_end = region_start + mbi.RegionSize;

    bool result = (start >= region_start && end <= region_end);

    // Cache the result if memory is writable
    if (result)
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        updateCacheEntry(mbi);
    }

    return result;
}
