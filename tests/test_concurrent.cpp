#include "core.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

namespace fs = std::filesystem;

int main() {
    fs::path test_cache = fs::temp_directory_path() / "remake-test-concurrent";
    if (fs::exists(test_cache)) fs::remove_all(test_cache);
    fs::create_directories(test_cache);
    
    skipbuild::Cache cache(test_cache);
    
    // Create test source file
    fs::path src = test_cache / "test.cpp";
    std::ofstream(src) << "int main() { return 0; }";
    
    std::vector<std::string> includes;
    std::vector<std::string> flags = {"-O2"};
    std::string compiler = "test-compiler-1.0";
    
    auto key = skipbuild::Cache::compute_key(src, includes, flags, compiler);
    std::cout << "Key computed: " << key.hash.substr(0, 16) << "...\n";
    
    // Create fake output
    fs::path obj = test_cache / "test.o";
    std::ofstream(obj) << "fake object file";
    
    // Test concurrent put/get from multiple threads
    const int num_threads = 8;
    const int iterations_per_thread = 50;
    std::atomic<int> successful_puts{0};
    std::atomic<int> successful_gets{0};
    std::atomic<int> failed_operations{0};
    
    auto worker = [&](int thread_id) {
        for (int i = 0; i < iterations_per_thread; ++i) {
            // Alternate between put and get
            if (i % 2 == 0) {
                if (cache.put(key, {obj})) {
                    successful_puts++;
                } else {
                    failed_operations++;
                }
            } else {
                auto entry = cache.get(key);
                if (entry.has_value()) {
                    successful_gets++;
                } else {
                    failed_operations++;
                }
            }
            
            // Small delay to increase chance of contention
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };
    
    std::cout << "Starting " << num_threads << " threads with " << iterations_per_thread << " iterations each...\n";
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    std::cout << "Successful puts: " << successful_puts << "\n";
    std::cout << "Successful gets: " << successful_gets << "\n";
    std::cout << "Failed operations: " << failed_operations << "\n";
    
    // Verify final state
    auto entry = cache.get(key);
    assert(entry.has_value());
    assert(entry->outputs.size() == 1);
    std::cout << "Final cache state verified\n";
    
    // Test concurrent prune
    std::cout << "Testing concurrent prune...\n";
    cache.prune(1024 * 1024, 30); // Should not crash
    
    // Cleanup
    fs::remove_all(test_cache);
    
    std::cout << "All concurrent tests passed!\n";
    return 0;
}
