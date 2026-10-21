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
    
    skipbuild::Cache cache(test_cache);
    
    // Create test source file
    fs::path src = test_cache / "test.cpp";
    std::ofstream(src) << "int main() { return 0; }";
    
    std::vector<std::string> includes;
    std::vector<std::string> flags = {"-O2"};
    std::string compiler = "test-compiler-1.0";
    
    auto key = skipbuild::Cache::compute_key(src, includes, flags, compiler);
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
    
    // Read-only caches can still read, but must not write new entries
    skipbuild::CacheConfig ro_config;
    ro_config.read_only = true;
    skipbuild::Cache ro_cache(test_cache, ro_config);

    auto ro_hit = ro_cache.get(key);
    assert(ro_hit.has_value());
    std::cout << "Read-only cache read existing entry\n";

    fs::path src2 = test_cache / "test2.cpp";
    std::ofstream(src2) << "int main() { return 1; }";
    auto key2 = skipbuild::Cache::compute_key(src2, includes, flags, compiler);
    fs::path obj2 = test_cache / "test2.o";
    std::ofstream(obj2) << "fake object file 2";

    assert(!ro_cache.put(key2, {obj2}));
    assert(!ro_cache.get(key2).has_value());
    std::cout << "Read-only cache refused write\n";
    
    // Cleanup
    fs::remove_all(test_cache);
    
    std::cout << "All tests passed!\n";
    return 0;
}
