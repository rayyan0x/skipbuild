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
        bool allow_insecure = false;        // Allow sending auth over plaintext HTTP
    };

    struct RemoteStats {
        int64_t remote_hits = 0;
        int64_t remote_misses = 0;
        int64_t uploads = 0;
        int64_t downloads = 0;
        int64_t errors = 0;
    };

    // A single cached artifact: the original filename plus its (compressed)
    // bytes. Outputs and debug sidecars travel together so a remote hit can
    // restore every artifact a compile/link produced.
    struct Artifact {
        std::string name;                 // Original filename, e.g. "foo.o"
        bool is_sidecar = false;          // Debug sidecar (.pdb/.idb/.dwo)
        std::vector<uint8_t> data;        // Compressed contents
    };

    // Result of a remote get operation
    struct GetResult {
        bool success = false;
        std::vector<Artifact> artifacts;    // All outputs and sidecars
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

    // Put entry to remote - uploads every artifact and then a meta.json that
    // lists them, so original filenames survive the round trip and a compile
    // that produces several outputs is fully restorable.
    bool put(const std::string& key_hash, const std::string& toolchain_id,
             const std::vector<Artifact>& artifacts);

    // Get statistics
    RemoteStats get_stats() const;

    // Configure from environment variables
    // SKIPBUILD_REMOTE_URL (or legacy BUILDCACHE_REMOTE_URL), SKIPBUILD_REMOTE_READ_ONLY, 
    // SKIPBUILD_REMOTE_CONNECT_TIMEOUT, SKIPBUILD_REMOTE_READ_TIMEOUT,
    // SKIPBUILD_REMOTE_MAX_RETRIES, SKIPBUILD_REMOTE_AUTH_HEADER,
    // SKIPBUILD_REMOTE_VERIFY_SSL
    static Config from_env();

    // Test connectivity to remote endpoint
    bool test_connection() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
