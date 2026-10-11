#include "stats.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace remake {

StatsTracker::StatsTracker(const fs::path& cache_root) 
    : stats_file_(cache_root / "stats.json") {
    load_from_file();
}

StatsTracker::~StatsTracker() {
    try {
        std::lock_guard<std::mutex> lock(file_mutex_);
        
        // Write to temp file first for atomicity
        fs::path temp_file = stats_file_;
        temp_file += ".tmp";
        
        std::ofstream file(temp_file);
        if (!file) return;
        
        file << "{\n";
        file << "  \"hits\": " << hits_.load(std::memory_order_relaxed) << ",\n";
        file << "  \"misses\": " << misses_.load(std::memory_order_relaxed) << ",\n";
        file << "  \"time_saved_ms\": " << time_saved_ms_.load(std::memory_order_relaxed) << ",\n";
        file << "  \"bytes_retrieved\": " << bytes_retrieved_.load(std::memory_order_relaxed) << ",\n";
        file << "  \"direct_executes\": " << direct_executes_.load(std::memory_order_relaxed) << ",\n";
        file << "  \"entries\": " << entries_.load(std::memory_order_relaxed) << ",\n";
        file << "  \"total_size_bytes\": " << total_size_bytes_.load(std::memory_order_relaxed) << ",\n";
        file << "  \"max_cache_size_bytes\": " << max_cache_size_bytes_.load(std::memory_order_relaxed) << "\n";
        file << "}\n";
        file.flush();
        file.close();
        
        // Atomic rename - use platform-specific approach for robustness
        std::error_code ec;
#ifdef _WIN32
        std::wstring wtemp = temp_file.wstring();
        std::wstring wdest = stats_file_.wstring();
        if (MoveFileExW(wtemp.c_str(), wdest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return;
        }
        // Fallback to std::filesystem if MoveFileEx fails
        if (fs::exists(stats_file_)) {
            fs::remove(stats_file_, ec);
        }
        fs::rename(temp_file, stats_file_, ec);
#else
        fs::rename(temp_file, stats_file_, ec);
#endif
    } catch (...) {
        // Ignore errors on destruction
    }
}

void StatsTracker::load_from_file() {
    std::lock_guard<std::mutex> lock(file_mutex_);
    
    std::error_code ec;
    if (!fs::exists(stats_file_, ec)) {
        return;
    }
    
    std::ifstream file(stats_file_);
    if (!file) return;
    
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    
    // Simple JSON parsing for our known fields
    auto extract_int64 = [&](const std::string& key, std::atomic<int64_t>& out) {
        std::string search = "\"" + key + "\":";
        size_t pos = content.find(search);
        if (pos != std::string::npos) {
            pos += search.size();
            while (pos < content.size() && std::isspace(content[pos])) pos++;
            size_t end = pos;
            while (end < content.size() && (std::isdigit(content[end]) || content[end] == '-')) end++;
            if (end > pos) {
                try {
                    int64_t value = std::stoll(content.substr(pos, end - pos));
                    out.store(value, std::memory_order_relaxed);
                } catch (...) {}
            }
        }
    };
    
    extract_int64("hits", hits_);
    extract_int64("misses", misses_);
    extract_int64("time_saved_ms", time_saved_ms_);
    extract_int64("bytes_retrieved", bytes_retrieved_);
    extract_int64("direct_executes", direct_executes_);
    extract_int64("entries", entries_);
    extract_int64("total_size_bytes", total_size_bytes_);
    extract_int64("max_cache_size_bytes", max_cache_size_bytes_);
}

void StatsTracker::save_to_file() const {
    std::lock_guard<std::mutex> lock(file_mutex_);
    
    // Write to temp file first for atomicity
    fs::path temp_file = stats_file_;
    temp_file += ".tmp";
    
    std::ofstream file(temp_file);
    if (!file) return;
    
    file << "{\n";
    file << "  \"hits\": " << hits_.load(std::memory_order_relaxed) << ",\n";
    file << "  \"misses\": " << misses_.load(std::memory_order_relaxed) << ",\n";
    file << "  \"time_saved_ms\": " << time_saved_ms_.load(std::memory_order_relaxed) << ",\n";
    file << "  \"bytes_retrieved\": " << bytes_retrieved_.load(std::memory_order_relaxed) << ",\n";
    file << "  \"direct_executes\": " << direct_executes_.load(std::memory_order_relaxed) << ",\n";
    file << "  \"entries\": " << entries_.load(std::memory_order_relaxed) << ",\n";
    file << "  \"total_size_bytes\": " << total_size_bytes_.load(std::memory_order_relaxed) << ",\n";
    file << "  \"max_cache_size_bytes\": " << max_cache_size_bytes_.load(std::memory_order_relaxed) << "\n";
    file << "}\n";
    file.flush();
    file.close();
    
    // Atomic rename - use platform-specific approach for robustness
    std::error_code ec;
#ifdef _WIN32
    std::wstring wtemp = temp_file.wstring();
    std::wstring wdest = stats_file_.wstring();
    if (MoveFileExW(wtemp.c_str(), wdest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return;
    }
    // Fallback to std::filesystem if MoveFileEx fails
    if (fs::exists(stats_file_)) {
        fs::remove(stats_file_, ec);
    }
    fs::rename(temp_file, stats_file_, ec);
#else
    fs::rename(temp_file, stats_file_, ec);
#endif
}

void StatsTracker::record_hit(int64_t time_saved_ms, int64_t bytes_retrieved) {
    hits_.fetch_add(1, std::memory_order_relaxed);
    time_saved_ms_.fetch_add(time_saved_ms, std::memory_order_relaxed);
    bytes_retrieved_.fetch_add(bytes_retrieved, std::memory_order_relaxed);
    save_to_file();
}

void StatsTracker::record_miss() {
    misses_.fetch_add(1, std::memory_order_relaxed);
    save_to_file();
}

void StatsTracker::record_direct_execute() {
    direct_executes_.fetch_add(1, std::memory_order_relaxed);
    save_to_file();
}

void StatsTracker::update_cache_size(int64_t entries, int64_t total_size_bytes, int64_t max_size_bytes) {
    entries_.store(entries, std::memory_order_relaxed);
    total_size_bytes_.store(total_size_bytes, std::memory_order_relaxed);
    if (max_size_bytes > 0) {
        max_cache_size_bytes_.store(max_size_bytes, std::memory_order_relaxed);
    }
    save_to_file();
}

CacheStats StatsTracker::get_stats() const {
    CacheStats s;
    s.hits = hits_.load(std::memory_order_relaxed);
    s.misses = misses_.load(std::memory_order_relaxed);
    s.time_saved_ms = time_saved_ms_.load(std::memory_order_relaxed);
    s.bytes_retrieved = bytes_retrieved_.load(std::memory_order_relaxed);
    s.direct_executes = direct_executes_.load(std::memory_order_relaxed);
    s.entries = entries_.load(std::memory_order_relaxed);
    s.total_size_bytes = total_size_bytes_.load(std::memory_order_relaxed);
    s.max_cache_size_bytes = max_cache_size_bytes_.load(std::memory_order_relaxed);
    return s;
}

void StatsTracker::clear() {
    hits_.store(0, std::memory_order_relaxed);
    misses_.store(0, std::memory_order_relaxed);
    time_saved_ms_.store(0, std::memory_order_relaxed);
    bytes_retrieved_.store(0, std::memory_order_relaxed);
    direct_executes_.store(0, std::memory_order_relaxed);
    entries_.store(0, std::memory_order_relaxed);
    total_size_bytes_.store(0, std::memory_order_relaxed);
    // max_cache_size_bytes_ is not reset
    save_to_file();
}

void StatsTracker::print_summary() const {
    CacheStats s = get_stats();
    
    auto format_bytes = [](int64_t bytes) -> std::string {
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        double size = static_cast<double>(bytes);
        int unit = 0;
        while (size >= 1024.0 && unit < 4) {
            size /= 1024.0;
            unit++;
        }
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return oss.str();
    };
    
    auto format_time = [](int64_t ms) -> std::string {
        if (ms < 1000) return std::to_string(ms) + " ms";
        double sec = ms / 1000.0;
        if (sec < 60) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << sec << " s";
            return oss.str();
        }
        double min = sec / 60.0;
        if (min < 60) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << min << " min";
            return oss.str();
        }
        double hr = min / 60.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << hr << " hr";
        return oss.str();
    };
    
    std::cout << "\n=== Build Cache Statistics ===\n\n" << std::flush;
    
    std::cout << "Cache Hit Rate:     " << std::fixed << std::setprecision(1) << s.hit_rate() << "%" << std::endl;
    std::cout << "Total Hits:         " << s.hits << std::endl;
    std::cout << "Total Misses:       " << s.misses << std::endl;
    std::cout << "Direct Executes:    " << s.direct_executes << std::endl;
    std::cout << std::endl;
    std::cout << "Estimated Time Saved: " << format_time(s.time_saved_ms) << " (" << s.time_saved_ms << " ms)" << std::endl;
    std::cout << "Data Retrieved:     " << format_bytes(s.bytes_retrieved) << std::endl;
    std::cout << std::endl;
    std::cout << "Cache Entries:      " << s.entries << std::endl;
    std::cout << "Cache Usage:        " << format_bytes(s.total_size_bytes) << " / " << format_bytes(s.max_cache_size_bytes) 
              << " (" << std::fixed << std::setprecision(1) << s.cache_usage_percent() << "%)" << std::endl;
    std::cout << std::endl;
    std::cout << "Stats file:         " << stats_file_ << std::endl;
    std::cout << "==============================" << std::endl;
}

} // namespace remake