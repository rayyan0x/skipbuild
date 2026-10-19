#include "core.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

void print_cache_contents(const fs::path& root) {
    std::cout << "\nCache directory contents:\n";
    std::error_code ec;
    for (auto& d1 : fs::directory_iterator(root, ec)) {
        std::cout << "  " << d1.path().filename() << "\n";
        if (fs::is_directory(d1)) {
            for (auto& d2 : fs::directory_iterator(d1.path(), ec)) {
                std::cout << "    " << d2.path().filename() << "\n";
                if (fs::is_directory(d2)) {
                    for (auto& d3 : fs::directory_iterator(d2.path(), ec)) {
                        std::cout << "      " << d3.path().filename() << "\n";
                        for (auto& f : fs::directory_iterator(d3.path(), ec)) {
                            auto size = fs::file_size(f.path(), ec);
                            std::cout << "        " << f.path().filename() << " (" << size << " bytes)\n";
                        }
                    }
                }
            }
        }
    }
    std::cout << std::endl;
}

int main() {
    // Separate working directory and cache directory
    fs::path work_dir = fs::temp_directory_path() / "test_eviction_work";
    fs::path cache_dir = fs::temp_directory_path() / "test_eviction_cache";
    
    if (fs::exists(work_dir)) fs::remove_all(work_dir);
    if (fs::exists(cache_dir)) fs::remove_all(cache_dir);
    fs::create_directories(work_dir);
    fs::create_directories(cache_dir);
    
    // Set small cache limit: 500 bytes (smaller than 3 compressed entries of ~315 bytes each)
    skipbuild::CacheConfig config;
    config.max_cache_size_bytes = 500;  // Small enough to trigger eviction with compressed data
    config.eviction_target_ratio = 0.85;  // Target 85% = 425 bytes
    config.max_age_days = 30;
    config.auto_clear_ratio = 0.0;  // exercise LRU prune, not the auto-clear-to-zero path
    
    skipbuild::Cache cache(cache_dir, config);
    
    // Create test source files in work_dir
    fs::path src1 = work_dir / "test1.cpp";
    std::ofstream(src1) << "int main() { return 1; }";
    
    fs::path src2 = work_dir / "test2.cpp";
    std::ofstream(src2) << "int main() { return 2; }";
    
    fs::path src3 = work_dir / "test3.cpp";
    std::ofstream(src3) << "int main() { return 3; }";
    
    std::vector<std::string> includes;
    std::vector<std::string> flags = {"-O2"};
    std::string compiler = "test-compiler-1.0";
    
    auto key1 = skipbuild::Cache::compute_key(src1, includes, flags, compiler);
    auto key2 = skipbuild::Cache::compute_key(src2, includes, flags, compiler);
    auto key3 = skipbuild::Cache::compute_key(src3, includes, flags, compiler);
    
    std::cout << "Key1 hash: " << key1.hash.substr(0, 16) << "...\n";
    std::cout << "Key2 hash: " << key2.hash.substr(0, 16) << "...\n";
    std::cout << "Key3 hash: " << key3.hash.substr(0, 16) << "...\n";
    std::cout << "Key1 dir: " << (cache_dir / key1.hash.substr(0, 2) / key1.hash.substr(2, 2) / key1.hash).string() << "\n";
    std::cout << "Key2 dir: " << (cache_dir / key2.hash.substr(0, 2) / key2.hash.substr(2, 2) / key2.hash).string() << "\n";
    std::cout << "Key3 dir: " << (cache_dir / key3.hash.substr(0, 2) / key3.hash.substr(2, 2) / key3.hash).string() << "\n";
    
    // Create fake output files in work_dir (each ~400 KB)
    fs::path obj1 = work_dir / "test1.o";
    fs::path obj2 = work_dir / "test2.o";
    fs::path obj3 = work_dir / "test3.o";
    
    // Write ~400 KB of incompressible random data to each
    // (ZSTD compresses repeated 'x' to almost nothing, so use random bytes)
    std::string data(400 * 1024, 'x');
    // Fill with pseudo-random data that won't compress well
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>((i * 1103515245 + 12345) % 256);
    }
    std::ofstream(obj1) << data;
    std::ofstream(obj2) << data;
    std::ofstream(obj3) << data;
    
    std::cout << "Cache limit: " << config.max_cache_size_bytes << " bytes\n";
    std::cout << "Eviction target: " << (config.max_cache_size_bytes * config.eviction_target_ratio) << " bytes\n";
    std::cout << "Each compressed object: ~300-400 bytes\n";
    
    // Put first entry
    std::cout << "Putting entry 1...\n";
    bool result1 = cache.put(key1, {obj1});
    std::cout << "Put result 1: " << (result1 ? "success" : "FAILED") << "\n";
    print_cache_contents(cache_dir);
    auto stats = cache.get_stats();
    std::cout << "After put 1: " << stats.entries << " entries, " << stats.total_size << " bytes\n";

    // Measure the on-disk size of a single entry (all three are the same data,
    // so they compress to the same size). The eviction budget is derived from
    // this instead of a hard-coded byte count, which made the test depend on
    // how well zstd happened to compress the fixture.
    const int64_t entry_size = stats.total_size;
    // Space the writes so the meta.json mtimes (used as LRU timestamps) are
    // strictly ordered: key1 oldest, key3 newest.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    
    // Put second entry
    std::cout << "Putting entry 2...\n";
    bool result2 = cache.put(key2, {obj2});
    std::cout << "Put result 2: " << (result2 ? "success" : "FAILED") << "\n";
    print_cache_contents(cache_dir);
    stats = cache.get_stats();
    std::cout << "After put 2: " << stats.entries << " entries, " << stats.total_size << " bytes\n";

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    
    // Put third entry - this should trigger eviction (total would be ~1.2 MB > 1 MB limit)
    std::cout << "Putting entry 3 (should trigger eviction)...\n";
    bool result3 = cache.put(key3, {obj3});
    std::cout << "Put result 3: " << (result3 ? "success" : "FAILED") << "\n";
    print_cache_contents(cache_dir);
    stats = cache.get_stats();
    std::cout << "After put 3: " << stats.entries << " entries, " << stats.total_size << " bytes\n";
    
    // Call prune with a budget of three entries; the configured 85% target
    // then requires exactly one (the oldest) entry to be evicted.
    const int64_t max_size = 3 * entry_size;
    std::cout << "Calling prune()...\n";
    cache.prune(max_size);
    print_cache_contents(cache_dir);
    stats = cache.get_stats();
    std::cout << "After prune: " << stats.entries << " entries, " << stats.total_size << " bytes\n";

    // Check the size straight after eviction: a later get() decompresses the
    // cached .zst blobs back into the entry directory, so measuring after the
    // presence checks below would report the (much larger) decompressed size.
    int64_t target = static_cast<int64_t>(max_size * config.eviction_target_ratio);
    assert(stats.total_size <= target && "Cache size should be under eviction target!");
    std::cout << "Post-eviction cache size: " << stats.total_size << " bytes (target: " << target << ")\n";
    
    // Verify that LRU entry (key1) was evicted
    std::cout << "Checking which entries remain...\n";
    auto entry1 = cache.get(key1);
    auto entry2 = cache.get(key2);
    auto entry3 = cache.get(key3);
    
    std::cout << "Entry 1 (oldest): " << (entry1.has_value() ? "PRESENT" : "EVICTED") << "\n";
    std::cout << "Entry 2: " << (entry2.has_value() ? "PRESENT" : "EVICTED") << "\n";
    std::cout << "Entry 3 (newest): " << (entry3.has_value() ? "PRESENT" : "EVICTED") << "\n";
    
    // The oldest entry (key1) should be evicted first
    assert(!entry1.has_value() && "LRU entry should have been evicted!");
    assert(entry2.has_value() && "Entry 2 should still be present!");
    assert(entry3.has_value() && "Entry 3 (newest) should still be present!");
    
    // Test zero-byte file purging
    std::cout << "\nTesting zero-byte file purging...\n";
    fs::path src4 = work_dir / "test4.cpp";
    std::ofstream(src4) << "int main() { return 4; }";
    auto key4 = skipbuild::Cache::compute_key(src4, includes, flags, compiler);
    
    fs::path obj4 = work_dir / "test4.o";
    std::ofstream(obj4) << "";  // Zero-byte file
    
    bool result4 = cache.put(key4, {obj4});
    std::cout << "Put result 4 (zero-byte): " << (result4 ? "success" : "FAILED") << "\n";
    auto entry4 = cache.get(key4);
    assert(!entry4.has_value() && "Zero-byte file should be purged on get!");
    std::cout << "Zero-byte file correctly purged!\n";
    
    // Test corrupted meta.json purging
    std::cout << "\nTesting corrupted meta.json purging...\n";
    fs::path src5 = work_dir / "test5.cpp";
    std::ofstream(src5) << "int main() { return 5; }";
    auto key5 = skipbuild::Cache::compute_key(src5, includes, flags, compiler);
    
    fs::path obj5 = work_dir / "test5.o";
    std::ofstream(obj5) << data;
    
    bool result5 = cache.put(key5, {obj5});
    std::cout << "Put result 5: " << (result5 ? "success" : "FAILED") << "\n";
    
    // Corrupt the meta.json
    fs::path key5_dir = cache_dir / key5.hash.substr(0, 2) / key5.hash.substr(2, 2) / key5.hash;
    fs::path meta5 = key5_dir / "meta.json";
    std::ofstream(meta5) << "corrupted json";
    
    auto entry5 = cache.get(key5);
    assert(!entry5.has_value() && "Corrupted meta.json should be purged on get!");
    std::cout << "Corrupted meta.json correctly purged!\n";
    
    // Cleanup
    fs::remove_all(work_dir);
    fs::remove_all(cache_dir);
    
    std::cout << "\nAll eviction tests passed!\n";
    return 0;
}
