#include "core.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

int main() {
    fs::path test_cache = fs::temp_directory_path() / "remake-test-simple";
    if (fs::exists(test_cache)) fs::remove_all(test_cache);
    fs::create_directories(test_cache);
    
    skipbuild::CacheConfig config;
    config.max_cache_size_bytes = 1024 * 1024;
    config.eviction_target_ratio = 0.85;
    config.max_age_days = 30;
    
    skipbuild::Cache cache(test_cache, config);
    
    // Create test source file
    fs::path src = test_cache / "test.cpp";
    std::ofstream(src) << "int main() { return 0; }";
    
    std::vector<std::string> includes;
    std::vector<std::string> flags = {"-O2"};
    std::string compiler = "test-compiler-1.0";
    
    auto key = skipbuild::Cache::compute_key(src, includes, flags, compiler);
    std::cout << "Key hash: " << key.hash << "\n";
    std::cout << "Key dir should be: " << (test_cache / key.hash.substr(0, 2) / key.hash.substr(2, 2) / key.hash).string() << "\n";
    
    // Create fake output
    fs::path obj = test_cache / "test.o";
    std::ofstream(obj) << "fake object file";
    
    // Put
    std::cout << "Putting...\n";
    bool result = cache.put(key, {obj});
    std::cout << "Put result: " << (result ? "success" : "failed") << "\n";
    
    // Check filesystem
    std::cout << "\nCache directory contents:\n";
    std::error_code ec;
    for (auto& d1 : fs::directory_iterator(test_cache, ec)) {
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
    
    // Get stats
    auto stats = cache.get_stats();
    std::cout << "\nStats: " << stats.entries << " entries, " << stats.total_size << " bytes\n";
    
    // Get
    std::cout << "\nGetting...\n";
    auto entry = cache.get(key);
    std::cout << "Get result: " << (entry.has_value() ? "success" : "failed") << "\n";
    if (entry.has_value()) {
        std::cout << "Entry size: " << entry->size_bytes << "\n";
        std::cout << "Entry outputs: " << entry->outputs.size() << "\n";
        for (auto& o : entry->outputs) {
            std::cout << "  " << o.string() << "\n";
        }
    }
    
    // Cleanup
    fs::remove_all(test_cache);
    
    return 0;
}
