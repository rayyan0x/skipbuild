#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include <mutex>
#include <cstdint>

// Simplified remote cache interface - no dependency on core types
// Uses WinHTTP on Windows, can be extended for other platforms

class RemoteCache {
public:
    struct Config {
        std::string url;                    // e.g., "http://cache.internal:8080/artifacts"
        bool read_only = false;             // If true, only download, never upload
        int connect_timeout_ms = 3000;      // Connection timeout
        int read_timeout_ms = 3000;         // Read timeout
        int max_retries = 1;                // Max retry attempts
        std::string auth_header;            // Optional Authorization header
        bool verify_ssl = true;             // SSL verification (for HTTPS)
    };

    struct RemoteStats {
        int64_t remote_hits = 0;
        int64_t remote_misses = 0;
        int64_t uploads = 0;
        int64_t downloads = 0;
        int64_t errors = 0;
    };

    // Result of a remote get operation
    struct GetResult {
        bool success = false;
        std::vector<uint8_t> primary_data;      // Compressed primary artifact
        std::vector<std::pair<std::string, std::vector<uint8_t>>> sidecars; // name -> compressed data
        std::string error;
    };

    explicit RemoteCache(Config config, std::filesystem::path local_cache);
    ~RemoteCache();

    // Non-copyable, movable
    RemoteCache(const RemoteCache&) = delete;
    RemoteCache& operator=(const RemoteCache&) = delete;
    RemoteCache(RemoteCache&&) noexcept;
    RemoteCache& operator=(RemoteCache&&) noexcept;

    // Check if remote has key (HEAD request) - fast existence check
    bool has(const std::string& key_hash) const;

    // Get entry from remote - downloads compressed artifacts
    GetResult get(const std::string& key_hash, const std::string& toolchain_id) const;

    // Put entry to remote - compresses and uploads artifacts
    // primary_data: compressed primary artifact
    // sidecars: vector of (filename, compressed_data) pairs
    bool put(const std::string& key_hash, const std::string& toolchain_id,
             const std::vector<uint8_t>& primary_data,
             const std::vector<std::pair<std::string, std::vector<uint8_t>>>& sidecars = {});

    // Get statistics
    RemoteStats get_stats() const;

    // Configure from environment variables
    // BUILDCACHE_REMOTE_URL, BUILDCACHE_REMOTE_READ_ONLY, 
    // BUILDCACHE_REMOTE_CONNECT_TIMEOUT, BUILDCACHE_REMOTE_READ_TIMEOUT,
    // BUILDCACHE_REMOTE_MAX_RETRIES, BUILDCACHE_REMOTE_AUTH_HEADER
    static Config from_env();

    // Test connectivity to remote endpoint
    bool test_connection() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
