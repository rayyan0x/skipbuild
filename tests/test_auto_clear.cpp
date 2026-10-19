#include "core.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <random>

namespace fs = std::filesystem;

// 1. Once usage reaches auto_clear_ratio of the configured limit the cache is
//    purged back to zero.
static void test_auto_clear_to_zero() {
    std::cout << "Testing auto-clear to zero at the threshold...\n";

    fs::path work = fs::temp_directory_path() / "skipbuild-autoclear-work";
    fs::path cdir = fs::temp_directory_path() / "skipbuild-autoclear-cache";
    fs::remove_all(work);
    fs::remove_all(cdir);
    fs::create_directories(work);
    fs::create_directories(cdir);

    skipbuild::CacheConfig config;
    config.max_cache_size_bytes = 8192;
    config.eviction_target_ratio = 0.85;
    config.auto_clear_ratio = 0.5;  // trigger early so the test stays small
    config.max_age_days = 30;

    skipbuild::Cache cache(cdir, config);

    std::vector<std::string> includes;
    std::vector<std::string> flags = {"-O2"};
    std::string compiler = "test-compiler-autoclear";

    bool cleared = false;
    for (int i = 0; i < 20 && !cleared; ++i) {
        fs::path src = work / ("a" + std::to_string(i) + ".cpp");
        std::ofstream(src) << "int main() { return " << i << "; }";
        auto key = skipbuild::Cache::compute_key(src, includes, flags, compiler);

        fs::path obj = work / ("a" + std::to_string(i) + ".o");
        // Incompressible so each entry has a predictable (near-raw) size.
        std::string data(4096, '\0');
        std::mt19937 rng(static_cast<unsigned>(i) * 2654435761u + 1u);
        std::uniform_int_distribution<int> dist(0, 255);
        for (size_t j = 0; j < data.size(); ++j) {
            data[j] = static_cast<char>(dist(rng));
        }
        std::ofstream(obj, std::ios::binary) << data;

        cache.put(key, {obj});

        auto stats = cache.get_stats();
        std::cout << "  after put " << (i + 1) << ": " << stats.entries
                  << " entries, " << stats.total_size << " bytes" << std::endl;
        if (stats.entries == 0) cleared = true;
    }

    assert(cleared && "cache should have been cleared to zero once the threshold was reached");
    auto stats = cache.get_stats();
    assert(stats.entries == 0);
    assert(stats.total_size == 0);
    std::cout << "  PASS: cache cleared to zero\n";

    fs::remove_all(work);
    fs::remove_all(cdir);
}

// 2. A cache hit must not permanently inflate the entry with a decompressed
//    copy that duplicates the stored compressed blob.
static void test_transient_decompression() {
    std::cout << "Testing transient decompressed copies...\n";

    fs::path work = fs::temp_directory_path() / "skipbuild-release-work";
    fs::path cdir = fs::temp_directory_path() / "skipbuild-release-cache";
    fs::remove_all(work);
    fs::remove_all(cdir);
    fs::create_directories(work);
    fs::create_directories(cdir);

    skipbuild::CacheConfig config;
    config.max_cache_size_bytes = 1LL << 30;  // effectively unlimited
    config.auto_clear_ratio = 0.0;            // disable auto-clear for this part
    skipbuild::Cache cache(cdir, config);

    fs::path src = work / "r.cpp";
    std::ofstream(src) << "int main() { return 0; }";
    auto key = skipbuild::Cache::compute_key(src, {}, {"-O2"}, "test-compiler-release");

    fs::path obj = work / "r.o";
    std::ofstream(obj) << std::string(4096, 'q');

    assert(cache.put(key, {obj}));

    fs::path key_dir = cdir / key.hash.substr(0, 2) / key.hash.substr(2, 2) / key.hash;
    const int64_t stored_size = cache.get_stats().total_size;
    assert(stored_size > 0 && "entry should have a stored size");

#if REMAKE_HAVE_ZSTD
    assert(fs::exists(key_dir / "r.o.zst") && "compressed blob should be stored");

    auto entry = cache.get(key);
    assert(entry.has_value());
    assert(!entry->transient_files.empty() && "a hit should mark its transient files");
    assert(fs::exists(key_dir / "r.o") && "decompressed copy exists while in use");

    // Size accounting must not double just because a copy was materialised.
    assert(cache.get_stats().total_size == stored_size && "transient copy must not inflate the cache size");

    // Caller finished with the outputs: the regenerable copy is released.
    cache.release(*entry);
    assert(!fs::exists(key_dir / "r.o") && "decompressed copy should be released");
    assert(fs::exists(key_dir / "r.o.zst") && "compressed blob must remain");

    // The entry is still served afterwards.
    auto again = cache.get(key);
    assert(again.has_value());
    cache.release(*again);
    assert(cache.get_stats().total_size == stored_size && "size stays stable across hits");
    std::cout << "  PASS: decompressed copies are transient\n";
#else
    std::cout << "  SKIP: built without zstd\n";
#endif

    fs::remove_all(work);
    fs::remove_all(cdir);
}

int main() {
    test_auto_clear_to_zero();
    test_transient_decompression();
    std::cout << "\nAll auto-clear tests passed!\n";
    return 0;
}
