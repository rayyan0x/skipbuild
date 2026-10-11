#pragma once
#include <string>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <optional>
#include <chrono>
#include <fstream>

namespace fs = std::filesystem;
namespace skipbuild {

struct CacheStats {
    int64_t hits = 0;
    int64_t misses = 0;
    int64_t time_saved_ms = 0;
    int64_t bytes_retrieved = 0;
    int64_t direct_executes = 0;
    int64_t entries = 0;
    int64_t total_size_bytes = 0;
    int64_t max_cache_size_bytes = 5LL * 1024 * 1024 * 1024;
    
    // Derived values
    double hit_rate() const {
        int64_t total = hits + misses;
        return total > 0 ? (static_cast<double>(hits) / total) * 100.0 : 0.0;
    }
    
    double time_saved_seconds() const {
        return time_saved_ms / 1000.0;
    }
    
    double cache_usage_percent() const {
        return max_cache_size_bytes > 0 ? (static_cast<double>(total_size_bytes) / max_cache_size_bytes) * 100.0 : 0.0;
    }
};

class StatsTracker {
    fs::path stats_file_;
    std::atomic<int64_t> hits_{0};
    std::atomic<int64_t> misses_{0};
    std::atomic<int64_t> time_saved_ms_{0};
    std::atomic<int64_t> bytes_retrieved_{0};
    std::atomic<int64_t> direct_executes_{0};
    std::atomic<int64_t> entries_{0};
    std::atomic<int64_t> total_size_bytes_{0};
    std::atomic<int64_t> max_cache_size_bytes_{5LL * 1024 * 1024 * 1024};
    
    mutable std::mutex file_mutex_;  // Only for file I/O
    
    void load_from_file();
    void save_to_file() const;
    
public:
    explicit StatsTracker(const fs::path& cache_root);
    ~StatsTracker();
    
    // Record a cache hit with time saved and bytes retrieved
    void record_hit(int64_t time_saved_ms, int64_t bytes_retrieved);
    
    // Record a cache miss
    void record_miss();
    
    // Record a direct compiler execution (fallback)
    void record_direct_execute();
    
    // Update cache size stats (called during prune or put)
    void update_cache_size(int64_t entries, int64_t total_size_bytes, int64_t max_size_bytes);
    
    // Get current stats (lock-free)
    CacheStats get_stats() const;
    
    // Reset all counters to zero
    void clear();
    
    // Print formatted summary to stdout
    void print_summary() const;
};

} // namespace remake
