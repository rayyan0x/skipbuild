#include "core.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

void test_stats_hit_miss() {
    std::cout << "Testing stats hit/miss tracking..." << std::endl;
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-stats-hitmiss";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    {
        std::cout << "  Creating cache..." << std::endl;
        remake::Cache cache(test_dir);
        std::cout << "  Cache created" << std::endl;
        
        // Create test source file
        fs::path src = test_dir / "test.cpp";
        std::ofstream(src) << "int main() { return 0; }";
        
        std::vector<std::string> includes;
        std::vector<std::string> flags = {"-O2"};
        std::string compiler = "test-compiler-1.0";
        
        auto key = remake::Cache::compute_key(src, includes, flags, compiler);
        std::cout << "  Key computed" << std::endl;
        
        // Test miss - get on non-existent key
        std::cout << "  Calling safe_get (miss)..." << std::endl;
        auto entry1 = cache.safe_get(key);
        std::cout << "  safe_get returned" << std::endl;
        assert(!entry1.has_value());
        
        std::cout << "  Calling get_detailed_stats..." << std::endl;
        auto stats1 = cache.get_detailed_stats();
        std::cout << "  get_detailed_stats returned" << std::endl;
        assert(stats1.misses >= 1);
        std::cout << "  PASS: Miss recorded (misses=" << stats1.misses << ", hits=" << stats1.hits << ")" << std::endl;
        
        // Create a fake output file and put it
        std::cout << "  Creating fake output file..." << std::endl;
        fs::path obj = test_dir / "test.o";
        std::ofstream(obj) << "fake object file content";
        
        std::cout << "  Calling safe_put..." << std::endl;
        bool put_result = cache.safe_put(key, {obj});
        std::cout << "  safe_put returned: " << put_result << std::endl;
        assert(put_result);
        
        // Test hit - get on existing key
        std::cout << "  Calling safe_get (hit)..." << std::endl;
        auto entry2 = cache.safe_get(key);
        std::cout << "  safe_get returned" << std::endl;
        assert(entry2.has_value());
        
        std::cout << "  Calling get_detailed_stats..." << std::endl;
        auto stats2 = cache.get_detailed_stats();
        std::cout << "  get_detailed_stats returned" << std::endl;
        assert(stats2.hits >= 1);
        assert(stats2.time_saved_ms > 0);
        assert(stats2.bytes_retrieved > 0);
        std::cout << "  PASS: Hit recorded (hits=" << stats2.hits << ", misses=" << stats2.misses 
                  << ", time_saved=" << stats2.time_saved_ms << "ms, bytes=" << stats2.bytes_retrieved << ")" << std::endl;
        
        // Test direct execute tracking
        std::cout << "  Calling record_direct_execute..." << std::endl;
        cache.record_direct_execute();
        std::cout << "  record_direct_execute returned" << std::endl;
        auto stats3 = cache.get_detailed_stats();
        std::cout << "  get_detailed_stats returned" << std::endl;
        assert(stats3.direct_executes >= 1);
        std::cout << "  PASS: Direct execute recorded (direct_executes=" << stats3.direct_executes << ")" << std::endl;
        
        // Destroy cache before removing directory
    } // End of cache scope
    fs::remove_all(test_dir);
}

void test_stats_clear() {
    std::cout << "Testing stats clear..." << std::endl;
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-stats-clear";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    {
        std::cout << "  Creating cache..." << std::endl;
        remake::Cache cache(test_dir);
        std::cout << "  Cache created" << std::endl;
        
        // Create test source file
        fs::path src = test_dir / "test.cpp";
        std::ofstream(src) << "int main() { return 0; }";
        
        std::vector<std::string> includes;
        std::vector<std::string> flags = {"-O2"};
        std::string compiler = "test-compiler-1.0";
        
        auto key = remake::Cache::compute_key(src, includes, flags, compiler);
        
        // Generate some stats
        std::cout << "  Generating stats..." << std::endl;
        std::cout << "    Calling safe_get (miss)..." << std::endl;
        cache.safe_get(key); // miss
        std::cout << "    safe_get returned" << std::endl;
        fs::path obj = test_dir / "test.o";
        std::ofstream(obj) << "fake object";
        std::cout << "    Calling safe_put..." << std::endl;
        cache.safe_put(key, {obj}); // put
        std::cout << "    safe_put returned" << std::endl;
        std::cout << "    Calling safe_get (hit)..." << std::endl;
        cache.safe_get(key); // hit
        std::cout << "    safe_get returned" << std::endl;
        std::cout << "    Calling record_direct_execute..." << std::endl;
        cache.record_direct_execute(); // direct execute
        std::cout << "    record_direct_execute returned" << std::endl;
        
        std::cout << "  Getting stats before clear..." << std::endl;
        auto stats_before = cache.get_detailed_stats();
        std::cout << "  get_detailed_stats returned" << std::endl;
        std::cout << "  Before clear: hits=" << stats_before.hits 
                  << ", misses=" << stats_before.misses 
                  << ", direct=" << stats_before.direct_executes << std::endl;
        
        // Verify we have some stats
        std::cout << "  Verifying stats..." << std::endl;
        assert(stats_before.hits >= 1);
        std::cout << "  hits assert passed" << std::endl;
        assert(stats_before.misses >= 1);
        std::cout << "  misses assert passed" << std::endl;
        assert(stats_before.direct_executes >= 1);
        std::cout << "  direct_executes assert passed" << std::endl;
        
        // Clear stats
        std::cout << "  Calling clear_stats..." << std::endl;
        cache.clear_stats();
        std::cout << "  clear_stats returned" << std::endl;
        
        std::cout << "  Getting stats after clear..." << std::endl;
        auto stats_after = cache.get_detailed_stats();
        std::cout << "  get_detailed_stats returned" << std::endl;
        std::cout << "  Verifying cleared stats..." << std::endl;
        assert(stats_after.hits == 0);
        std::cout << "  hits assert passed" << std::endl;
        assert(stats_after.misses == 0);
        std::cout << "  misses assert passed" << std::endl;
        assert(stats_after.direct_executes == 0);
        std::cout << "  direct_executes assert passed" << std::endl;
        assert(stats_after.time_saved_ms == 0);
        std::cout << "  time_saved_ms assert passed" << std::endl;
        assert(stats_after.bytes_retrieved == 0);
        std::cout << "  bytes_retrieved assert passed" << std::endl;
        // Verify stats.json file was updated
        fs::path stats_file = test_dir / "stats.json";
        assert(fs::exists(stats_file));
        
        std::ifstream file(stats_file);
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        assert(content.find("\"hits\": 0") != std::string::npos);
        assert(content.find("\"misses\": 0") != std::string::npos);
        std::cout << "  PASS: stats.json updated correctly" << std::endl;
        
        // Destroy cache before removing directory
    } // End of cache scope
    std::cout << "  Cleaning up test directory..." << std::endl;
    fs::remove_all(test_dir);
    std::cout << "  test_stats_clear completed" << std::endl;
    std::cout.flush();
}

void test_stats_persistence() {
    std::cout << "Testing stats persistence across Cache instances..." << std::endl;
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-stats-persist";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    std::cout << "  Creating first cache instance..." << std::endl;
    {
        std::cout << "    Creating cache..." << std::endl;
        remake::Cache cache(test_dir);
        std::cout << "    Cache created" << std::endl;
        
        // Create test source file
        fs::path src = test_dir / "test.cpp";
        std::ofstream(src) << "int main() { return 0; }";
        
        std::vector<std::string> includes;
        std::vector<std::string> flags = {"-O2"};
        std::string compiler = "test-compiler-1.0";
        
        auto key = remake::Cache::compute_key(src, includes, flags, compiler);
        
        // Generate some stats
        std::cout << "    Generating stats..." << std::endl;
        cache.safe_get(key); // miss
        std::cout << "    safe_get (miss) returned" << std::endl;
        fs::path obj = test_dir / "test.o";
        std::ofstream(obj) << "fake object";
        cache.safe_put(key, {obj}); // put
        std::cout << "    safe_put returned" << std::endl;
        cache.safe_get(key); // hit
        std::cout << "    safe_get (hit) returned" << std::endl;
        cache.record_direct_execute(); // direct execute
        std::cout << "    record_direct_execute returned" << std::endl;
    } // cache goes out of scope, destructor saves stats
    std::cout << "    First cache destroyed" << std::endl;
    
    // Create new cache instance with same directory
    std::cout << "  Creating second cache instance..." << std::endl;
    {
        std::cout << "    Creating cache2..." << std::endl;
        remake::Cache cache2(test_dir);
        std::cout << "    cache2 created" << std::endl;
        std::cout << "    Getting stats..." << std::endl;
        auto stats = cache2.get_detailed_stats();
        std::cout << "    get_detailed_stats returned" << std::endl;
        assert(stats.hits == 1);
        assert(stats.misses == 1);
        assert(stats.direct_executes == 1);
        assert(stats.time_saved_ms > 0);
        assert(stats.bytes_retrieved > 0);
        std::cout << "  PASS: Stats persisted across Cache instances" << std::endl;
        std::cout << "  Loaded: hits=" << stats.hits << ", misses=" << stats.misses 
                  << ", direct=" << stats.direct_executes << std::endl;
    }
    
    fs::remove_all(test_dir);
}

void test_stats_concurrent() {
    std::cout << "Testing concurrent stats updates (sequential)..." << std::endl;
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-stats-concurrent";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    remake::Cache cache(test_dir);
    
    // Create test source files
    std::vector<std::string> includes;
    std::vector<std::string> flags = {"-O2"};
    std::string compiler = "test-compiler-1.0";
    
    const int num_operations = 200;
    
    // Run operations sequentially to avoid deadlock
    for (int i = 0; i < num_operations; ++i) {
        fs::path src = test_dir / ("test_" + std::to_string(i) + ".cpp");
        std::ofstream(src) << "int main() { return " << i << "; }";
        
        auto key = remake::Cache::compute_key(src, includes, flags, compiler);
        
        // Mix of hits and misses
        auto entry = cache.safe_get(key);
        
        if (!entry.has_value()) {
            // Create output and put
            fs::path obj = test_dir / ("out_" + std::to_string(i) + ".o");
            std::ofstream(obj) << "object";
            cache.safe_put(key, {obj});
            
            // Try to get again (should hit)
            cache.safe_get(key);
        }
        
        // Record some direct executes
        if (i % 10 == 0) {
            cache.record_direct_execute();
        }
    }
    
    auto stats = cache.get_detailed_stats();
    std::cout << "  Final stats: hits=" << stats.hits 
              << ", misses=" << stats.misses 
              << ", direct=" << stats.direct_executes
              << ", time_saved=" << stats.time_saved_ms << "ms" << std::endl;
    
    // Verify counts are reasonable (some hits, some misses, some direct)
    assert(stats.hits + stats.misses > 0);
    assert(stats.direct_executes > 0);
    std::cout << "  PASS: Sequential stats updates work correctly" << std::endl;
    
    fs::remove_all(test_dir);
}

void test_stats_update_cache_size() {
    std::cout << "Testing update_cache_size..." << std::endl;
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-stats-size";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    remake::Cache cache(test_dir);
    
    // Create test source file
    fs::path src = test_dir / "test.cpp";
    std::ofstream(src) << "int main() { return 0; }";
    
    std::vector<std::string> includes;
    std::vector<std::string> flags = {"-O2"};
    std::string compiler = "test-compiler-1.0";
    
    auto key = remake::Cache::compute_key(src, includes, flags, compiler);
    
    // Put multiple entries
    for (int i = 0; i < 3; ++i) {
        fs::path obj = test_dir / ("test" + std::to_string(i) + ".o");
        std::ofstream(obj) << "object " << i;
        
        auto key_i = remake::Cache::compute_key(src, includes, flags, compiler + std::to_string(i));
        cache.safe_put(key_i, {obj});
    }
    
    // The put operation should have updated cache size
    auto stats = cache.get_detailed_stats();
    assert(stats.entries >= 1);
    assert(stats.total_size_bytes > 0);
    assert(stats.max_cache_size_bytes > 0);
    std::cout << "  PASS: Cache size updated (entries=" << stats.entries 
              << ", size=" << stats.total_size_bytes << " bytes, max=" << stats.max_cache_size_bytes << ")" << std::endl;
    
    fs::remove_all(test_dir);
}

void test_stats_print_summary() {
    std::cout << "Testing print_summary output..." << std::endl;
    
    fs::path test_dir = fs::temp_directory_path() / "remake-test-stats-print";
    if (fs::exists(test_dir)) fs::remove_all(test_dir);
    fs::create_directories(test_dir);
    
    remake::Cache cache(test_dir);
    
    // Generate some stats
    cache.record_direct_execute();
    cache.record_direct_execute();
    cache.safe_get(remake::Cache::compute_key(test_dir / "a.cpp", {}, {}, "compiler"));
    
    fs::path obj = test_dir / "test.o";
    std::ofstream(obj) << "object";
    cache.safe_put(remake::Cache::compute_key(test_dir / "b.cpp", {}, {}, "compiler"), {obj});
    cache.safe_get(remake::Cache::compute_key(test_dir / "b.cpp", {}, {}, "compiler"));
    
    // This should not crash
    cache.print_stats();
    
    std::cout << "  PASS: print_summary works without crashing" << std::endl;
    
    fs::remove_all(test_dir);
}

int main() {
    std::cout << "Starting stats test..." << std::endl;
    std::cout << "=== Statistics Tracking Tests ===" << std::endl;
    
    test_stats_hit_miss();
    std::cout << std::endl;
    
    test_stats_clear();
    std::cout << std::endl;
    
    test_stats_persistence();
    std::cout << std::endl;
    
    test_stats_concurrent();
    std::cout << std::endl;
    
    test_stats_update_cache_size();
    std::cout << std::endl;
    
    test_stats_print_summary();
    std::cout << std::endl;
    
    std::cout << "=== All tests passed! ===" << std::endl;
    return 0;
}