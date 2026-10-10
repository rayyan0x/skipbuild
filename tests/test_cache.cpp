#include "core.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

int main() {
    fs::path test_cache = fs::temp_directory_path() / "remake-test-cache";
    if (fs::exists(test_cache)) fs::remove_all(test_cache);
    fs::create_directories(test_cache);
    
    remake::Cache cache(test_cache);
    
    // Create test source file
    fs::path src = test_cache / "test.cpp";
    std::ofstream(src) << "int main() { return 0; }";
    
    std::vector<std::string> includes;
    std::vector<std::string> flags = {"-O2"};
    std::string compiler = "test-compiler-1.0";
    
    auto key = remake::Cache::compute_key(src, includes, flags, compiler);
    assert(!key.hash.empty());
    assert(key.toolchain_id == compiler);
    std::cout << "Key computed: " << key.hash.substr(0, 16) << "...\n";
    
    // Test miss
    auto entry = cache.get(key);
    assert(!entry.has_value());
    std::cout << "Cache miss as expected\n";
    
    // Create fake output
    fs::path obj = test_cache / "test.o";
    std::ofstream(obj) << "fake object file";
    
    // Test put
    bool ok = cache.put(key, {obj});
    assert(ok);
    std::cout << "Put successful\n";
    
    // Test hit
    entry = cache.get(key);
    assert(entry.has_value());
    assert(entry->outputs.size() == 1);
    std::cout << "Cache hit: " << entry->outputs[0] << "\n";
    
    // Cleanup
    fs::remove_all(test_cache);
    
    std::cout << "All tests passed!\n";
    return 0;
}