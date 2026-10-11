#include "core.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    fs::path test_cache = fs::temp_directory_path() / "remake-test-multiprocess";
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
    
    // Create fake output
    fs::path obj = test_cache / "test.o";
    std::ofstream(obj) << "fake object file";
    
    int process_id = 0;
    if (argc > 1) {
        process_id = std::stoi(argv[1]);
    }
    
    std::cout << "Process " << process_id << " starting...\n";
    
    // Each process does 30 put/get operations
    int success_count = 0;
    int fail_count = 0;
    
    for (int i = 0; i < 30; ++i) {
        if (cache.put(key, {obj})) {
            success_count++;
        } else {
            fail_count++;
        }
        
        auto entry = cache.get(key);
        if (!entry.has_value()) {
            fail_count++;
            // Don't print every failure - just count
        } else {
            success_count++;
        }
        
        // Small delay to allow other processes to run
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    
    // Verify final cache state is consistent
    auto final_entry = cache.get(key);
    if (!final_entry.has_value() || final_entry->outputs.size() != 1) {
        std::cerr << "Process " << process_id << ": Final cache state corrupted!\n";
        return 1;
    }
    
    std::cout << "Process " << process_id << " completed: " << success_count << " success, " << fail_count << " failures\n";
    return 0;  // Don't fail on lock contention - just check for corruption
}
