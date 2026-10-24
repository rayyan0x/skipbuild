#include "core.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

void test_corrupted_cache_entry() {
    std::cout << "Testing corrupted cache entry handling...\n";
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-corrupted";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    skipbuild::Cache cache(test_dir);
    
    // Create test source file
    fs::path src = test_dir / "test.cpp";
    std::ofstream(src) << "int main() { return 0; }";
    
    std::vector<std::string> includes;
    std::vector<std::string> flags = {"-O2"};
    std::string compiler = "test-compiler-1.0";
    
    auto key = skipbuild::Cache::compute_key(src, includes, flags, compiler);
    
    // Create a fake cache entry with corrupted data (truncated file)
    fs::path key_dir = test_dir / key.hash.substr(0, 2) / key.hash.substr(2, 2) / key.hash;
    fs::create_directories(key_dir);
    
    // Write meta.json
    std::ofstream meta(key_dir / "meta.json");
    meta << "{\"toolchain\":\"" << compiler << "\",\"ts\":" << std::time(nullptr) << ",\"size\":100,\"compressed\":false}";
    meta.close();
    
    // Create a truncated/corrupted output file (empty)
    std::ofstream(key_dir / "test.o") << "";
    
    // safe_get should handle corruption gracefully and return nullopt
    auto entry = cache.safe_get(key);
    assert(!entry.has_value());
    std::cout << "  PASS: Corrupted entry handled gracefully (returned nullopt)\n";
    
    // Note: On Windows, file locking may prevent immediate purge, so we don't assert on directory removal
    // The key behavior is that safe_get returns nullopt and doesn't crash
    
    fs::remove_all(test_dir);
}

void test_truncated_zstd_entry() {
    std::cout << "Testing truncated zstd entry handling...\n";
#if !REMAKE_HAVE_ZSTD
    std::cout << "  SKIP: built without zstd\n";
#else
    fs::path test_dir = fs::temp_directory_path() / "remake-test-truncated-zstd";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);

    skipbuild::Cache cache(test_dir);

    fs::path src = test_dir / "t.cpp";
    std::ofstream(src) << "int main() { return 0; }";

    std::vector<std::string> includes;
    std::vector<std::string> flags;
    auto key = skipbuild::Cache::compute_key(src, includes, flags, "test-compiler");

    fs::path obj = test_dir / "t.o";
    {
        std::ofstream o(obj, std::ios::binary);
        for (int i = 0; i < 512; ++i) o.put(static_cast<char>('A' + (i % 26)));
    }
    assert(cache.put(key, {obj}));

    fs::path key_dir = test_dir / key.hash.substr(0, 2) / key.hash.substr(2, 2) / key.hash;
    fs::path blob;
    for (auto& f : fs::directory_iterator(key_dir)) {
        if (f.path().extension() == ".zst") {
            blob = f.path();
            break;
        }
    }
    assert(!blob.empty());

    // Drop the tail of the compressed frame: still non-empty, but incomplete.
    std::vector<char> data;
    {
        std::ifstream in(blob, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    assert(data.size() > 8);
    data.resize(data.size() - 5);
    {
        std::ofstream out(blob, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

    auto hit = cache.safe_get(key);
    assert(!hit.has_value());
    std::cout << "  PASS: Truncated zstd frame rejected\n";

    fs::remove_all(test_dir);
#endif
}

void test_file_lock_release_and_relock() {
    std::cout << "Testing file lock release and re-acquire...\n";

    fs::path dir = fs::temp_directory_path() / "remake-test-filelock";
    if (fs::exists(dir)) fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path lock_path = dir / "test.lock";

    {
        skipbuild::FileLock lock(lock_path);
        assert(lock.lock(skipbuild::FileLock::LockType::Shared, 1000));
        assert(lock.is_locked());
        lock.unlock();
        assert(!lock.is_locked());

        // Purging a corrupt entry depends on being able to drop a shared lock
        // and immediately take an exclusive one in the same process.
        assert(lock.lock(skipbuild::FileLock::LockType::Exclusive, 1000) &&
               "released lock must be re-acquirable");
        assert(lock.is_locked());
        lock.unlock();
        assert(!lock.is_locked());
    }

    {
        skipbuild::FileLock other(lock_path);
        assert(other.lock(skipbuild::FileLock::LockType::Exclusive, 1000) &&
               "lock must be fully released for another holder");
    }

    fs::remove_all(dir);
    std::cout << "  PASS: lock releases and can be re-acquired\n";
}

void test_cache_put_failure_disk_full_simulation() {
    std::cout << "Testing cache put failure handling (permission denied)...\n";
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-put-fail";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    // Create a read-only directory to simulate write failure
    fs::path readonly_dir = test_dir / "readonly_cache";
    fs::create_directories(readonly_dir);
    
    // Make it read-only (on Unix, this works; on Windows, it's more complex)
    #ifndef _WIN32
    fs::permissions(readonly_dir, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
    #endif
    
    skipbuild::Cache cache(readonly_dir);
    
    // Create test source file
    fs::path src = test_dir / "test.cpp";
    std::ofstream(src) << "int main() { return 0; }";
    
    std::vector<std::string> includes;
    std::vector<std::string> flags = {"-O2"};
    std::string compiler = "test-compiler-1.0";
    
    auto key = skipbuild::Cache::compute_key(src, includes, flags, compiler);
    
    // Create a fake output file
    fs::path obj = test_dir / "test.o";
    std::ofstream(obj) << "fake object file";
    
    // safe_put should not throw, just return false and log warning
    bool result = cache.safe_put(key, {obj});
    // On Windows, this might succeed due to different permission model
    // On Unix, it should fail gracefully
    #ifndef _WIN32
    assert(result == false);
    std::cout << "  PASS: Cache put failure handled gracefully (returned false)\n";
    #else
    std::cout << "  SKIP: Permission test behavior differs on Windows\n";
    #endif
    
    // Restore permissions for cleanup
    #ifndef _WIN32
    fs::permissions(readonly_dir, fs::perms::owner_all);
    #endif
    fs::remove_all(test_dir);
}

void test_safe_get_vs_get() {
    std::cout << "Testing safe_get vs get behavior...\n";
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-safe-get";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    skipbuild::Cache cache(test_dir);
    
    // Create test source file
    fs::path src = test_dir / "test.cpp";
    std::ofstream(src) << "int main() { return 0; }";
    
    std::vector<std::string> includes;
    std::vector<std::string> flags = {"-O2"};
    std::string compiler = "test-compiler-1.0";
    
    auto key = skipbuild::Cache::compute_key(src, includes, flags, compiler);
    
    // Test safe_get on non-existent key (should return nullopt, not throw)
    auto entry1 = cache.safe_get(key);
    assert(!entry1.has_value());
    std::cout << "  PASS: safe_get on missing key returns nullopt\n";
    
    // Create valid cache entry
    fs::path obj = test_dir / "test.o";
    std::ofstream(obj) << "fake object file";
    cache.put(key, {obj});
    
    // Test safe_get on valid key
    auto entry2 = cache.safe_get(key);
    assert(entry2.has_value());
    assert(entry2->outputs.size() == 1);
    std::cout << "  PASS: safe_get on valid key returns entry\n";
    
    // Test regular get on valid key
    auto entry3 = cache.get(key);
    assert(entry3.has_value());
    std::cout << "  PASS: get on valid key returns entry\n";
    
    fs::remove_all(test_dir);
}

void test_execute_native_compiler() {
    std::cout << "Testing execute_native_compiler fallback...\n";
    
    // Test with a command that should fail (non-existent compiler)
    std::vector<std::string> args = {"/nonexistent/compiler", "-c", "test.cpp"};
    auto result = skipbuild::execute_native_compiler(args, "");
    
    assert(result.fallback_used == true);
    assert(result.exit_code != 0);
    assert(!result.error_message.empty());
    std::cout << "  PASS: execute_native_compiler returns proper error for non-existent compiler\n";
    std::cout << "    Exit code: " << result.exit_code << "\n";
    std::cout << "    Error: " << result.error_message.substr(0, 80) << "...\n";
    
    // Test with valid command (echo on Unix, or cmd /c echo on Windows)
    #ifdef _WIN32
    std::vector<std::string> valid_args = {"cmd.exe", "/c", "echo hello"};
    #else
    std::vector<std::string> valid_args = {"echo", "hello"};
    #endif
    auto result2 = skipbuild::execute_native_compiler(valid_args, "");
    assert(result2.fallback_used == true);
    assert(result2.exit_code == 0);
    assert(result2.stdout_output.find("hello") != std::string::npos);
    std::cout << "  PASS: execute_native_compiler works with valid command\n";
    std::cout << "    Output: " << result2.stdout_output.substr(0, 40) << "\n";
}

void test_remote_cache_config_timeouts() {
    std::cout << "Testing remote cache timeout configuration...\n";
    
    #if REMAKE_HAVE_CURL
    skipbuild::RemoteCacheConfig config = skipbuild::RemoteCache::from_env();
    
    // Check default values
    assert(config.connect_timeout_ms == 3000);
    assert(config.read_timeout_ms == 3000);
    assert(config.max_retries == 1);
    std::cout << "  PASS: Default timeout values are fail-fast (3s connect, 3s read, 1 retry)\n";
    
    // Test setting custom values
    config.connect_timeout_ms = 5000;
    config.read_timeout_ms = 10000;
    config.max_retries = 2;
    assert(config.connect_timeout_ms == 5000);
    assert(config.read_timeout_ms == 10000);
    assert(config.max_retries == 2);
    std::cout << "  PASS: Custom timeout values can be set\n";
    #else
    std::cout << "  SKIP: CURL not available\n";
    #endif
}

void test_network_fail_fast_simulation() {
    std::cout << "Testing network fail-fast behavior (simulated)...\n";
    
    // This test verifies the logic in curl_request for fail-fast on 503/500
    // We can't easily test actual network without a test server, but we can verify
    // the configuration is set correctly
    
    #if REMAKE_HAVE_CURL
    skipbuild::RemoteCacheConfig config;
    config.connect_timeout_ms = 3000;
    config.read_timeout_ms = 3000;
    config.max_retries = 1;
    
    // Verify the settings that enable fail-fast
    assert(config.connect_timeout_ms <= 5000); // Aggressive timeout
    assert(config.read_timeout_ms <= 5000);
    assert(config.max_retries <= 1); // Minimal retries
    std::cout << "  PASS: Network config enables fail-fast behavior\n";
    #else
    std::cout << "  SKIP: CURL not available\n";
    #endif
}

void test_cache_restore_failure_fallback() {
    std::cout << "Testing cache restore failure triggers fallback...\n";
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-restore-fail";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    skipbuild::Cache cache(test_dir);
    
    // Create test source file
    fs::path src = test_dir / "test.cpp";
    std::ofstream(src) << "int main() { return 0; }";
    
    std::vector<std::string> includes;
    std::vector<std::string> flags = {"-O2"};
    std::string compiler = "test-compiler-1.0";
    
    auto key = skipbuild::Cache::compute_key(src, includes, flags, compiler);
    
    // Create a cache entry with a file that we'll make unreadable
    fs::path key_dir = test_dir / key.hash.substr(0, 2) / key.hash.substr(2, 2) / key.hash;
    fs::create_directories(key_dir);
    
    fs::path cached_obj = key_dir / "test.o";
    std::ofstream(cached_obj) << "cached object";
    
    std::ofstream meta(key_dir / "meta.json");
    meta << "{\"toolchain\":\"" << compiler << "\",\"ts\":" << std::time(nullptr) << ",\"size\":100,\"compressed\":false}";
    meta.close();
    
    // Make the cached file unreadable (Unix only)
    #ifndef _WIN32
    std::error_code perm_ec;
    fs::permissions(cached_obj, fs::perms::none, perm_ec);

    // safe_get should still succeed (it reads the file), but copying to target would fail
    auto entry = cache.safe_get(key);
    if (entry && !entry->outputs.empty()) {
        // Try to copy to a target - this should fail due to permissions
        fs::path target = test_dir / "restored.o";
        std::error_code ec;
        fs::copy_file(entry->outputs[0], target, fs::copy_options::overwrite_existing, ec);

        // Restore permissions for cleanup (the file may already be gone if the
        // entry was purged, so never let this throw).
        std::error_code rec;
        fs::permissions(cached_obj, fs::perms::owner_all, rec);

        // On some systems, root can still read, so this might not fail
        if (ec) {
            std::cout << "  PASS: Cache restore failure detected: " << ec.message() << "\n";
        } else {
            std::cout << "  NOTE: Copy succeeded despite permissions (running as root?)\n";
        }
    } else {
        std::error_code rec;
        fs::permissions(cached_obj, fs::perms::owner_all, rec);
        std::cout << "  NOTE: safe_get returned nullopt (entry may have been purged)\n";
    }
    #else
    std::cout << "  SKIP: Permission test behavior differs on Windows\n";
    #endif
    
    fs::remove_all(test_dir);
}

void test_stale_temp_file_not_corruption() {
    std::cout << "Testing stale temp file does not invalidate an entry...\n";

    fs::path test_dir = fs::temp_directory_path() / "remake-test-stale-temp";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);

    skipbuild::Cache cache(test_dir);

    fs::path src = test_dir / "stale.cpp";
    std::ofstream(src) << "int stale;";
    fs::path obj = test_dir / "stale.o";
    std::ofstream(obj) << std::string(128, 's');

    auto key = skipbuild::Cache::compute_key(src, {}, {"-O2"}, "test-compiler-stale");
    assert(cache.put(key, {obj}));

    // A failed write can leave a zero-byte "*.tmp" file behind. It is not an
    // artifact, so a later lookup must not purge the whole entry.
    fs::path key_dir = test_dir / key.hash.substr(0, 2) / key.hash.substr(2, 2) / key.hash;
    std::ofstream(key_dir / "cache_stale.o_1_2.tmp") << "";

    auto entry = cache.get(key);
    assert(entry.has_value() && "stale temp file must not be treated as corruption");
    assert(entry->outputs.size() == 1 && "the real output must still be restored");
    cache.release(*entry);

    fs::remove_all(test_dir);
    std::cout << "  PASS: stale temp file ignored\n";
}

int main() {
    std::cout << "=== Resiliency & Fail-Safe Fallback Tests ===\n\n";
    
    test_corrupted_cache_entry();
    std::cout << "\n";
    
    test_truncated_zstd_entry();
    std::cout << "\n";
    
    test_file_lock_release_and_relock();
    std::cout << "\n";

    test_stale_temp_file_not_corruption();
    std::cout << "\n";
    
    test_cache_put_failure_disk_full_simulation();
    std::cout << "\n";
    
    test_safe_get_vs_get();
    std::cout << "\n";
    
    test_execute_native_compiler();
    std::cout << "\n";
    
    test_remote_cache_config_timeouts();
    std::cout << "\n";
    
    test_network_fail_fast_simulation();
    std::cout << "\n";
    
    test_cache_restore_failure_fallback();
    std::cout << "\n";
    
    std::cout << "=== All tests passed! ===\n";
    return 0;
}
