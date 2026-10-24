#include "core.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
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
    assert(!entry->outputs.empty());
    // The decompressed copy must live in a private staging directory, NOT inside
    // the shared entry directory, so concurrent readers never collide.
    assert(fs::exists(entry->outputs[0]) && "decompressed copy exists while in use");
    assert(!fs::exists(key_dir / "r.o") && "no decompressed copy inside the shared entry dir");

    // Size accounting must not double just because a copy was materialised.
    assert(cache.get_stats().total_size == stored_size && "transient copy must not inflate the cache size");

    // Caller finished with the outputs: the regenerable copy is released.
    fs::path staged = entry->outputs[0];
    fs::path stage_dir = staged.parent_path();
    cache.release(*entry);
    assert(!fs::exists(staged) && "staged copy should be released");
    assert(!fs::exists(stage_dir) && "staging directory should be removed");
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

// 3. Max-age eviction is a policy of its own: an entry older than max_age_days
//    must be removed even when the cache is well below its size limit.
static void test_prune_age_below_size_limit() {
    std::cout << "Testing age-based prune below the size limit...\n";

    fs::path work = fs::temp_directory_path() / "skipbuild-prune-age-work";
    fs::path cdir = fs::temp_directory_path() / "skipbuild-prune-age-cache";
    fs::remove_all(work);
    fs::remove_all(cdir);
    fs::create_directories(work);
    fs::create_directories(cdir);

    skipbuild::CacheConfig config;
    config.max_cache_size_bytes = 1LL << 30;  // never over budget
    config.max_age_days = 30;
    config.auto_clear_ratio = 0.0;
    skipbuild::Cache cache(cdir, config);

    fs::path src = work / "old.cpp";
    std::ofstream(src) << "int old_symbol;";
    auto key = skipbuild::Cache::compute_key(src, {}, {"-O2"}, "test-compiler-prune");
    fs::path obj = work / "old.o";
    std::ofstream(obj, std::ios::binary) << std::string(4096, 'x');

    assert(cache.put(key, {obj}));
    assert(cache.get_stats().entries == 1);

    fs::path key_dir = cdir / fs::path(key.hash.substr(0, 2)) /
                       fs::path(key.hash.substr(2, 2)) / fs::path(key.hash);
    fs::last_write_time(key_dir / "meta.json",
                        fs::file_time_type::clock::now() - std::chrono::hours(24 * 60));

    cache.prune();
    assert(cache.get_stats().entries == 0 && "expired entry must be pruned even under the size limit");
    std::cout << "  PASS: expired entry pruned below the size limit\n";

    fs::remove_all(work);
    fs::remove_all(cdir);
}

// 4. LRU eviction must stop at the target ratio and must not subtract the size
//    of an entry twice (previously entries already removed by the age pass were
//    visited again in the LRU pass).
static void test_prune_lru_target_and_no_double_count() {
    std::cout << "Testing LRU prune target...\n";

    fs::path work = fs::temp_directory_path() / "skipbuild-prune-lru-work";
    fs::path cdir = fs::temp_directory_path() / "skipbuild-prune-lru-cache";
    fs::remove_all(work);
    fs::remove_all(cdir);
    fs::create_directories(work);
    fs::create_directories(cdir);

    skipbuild::CacheConfig config;
    config.max_cache_size_bytes = 9000;
    config.eviction_target_ratio = 0.85;
    config.max_age_days = 3650;
    config.auto_clear_ratio = 0.0;
    skipbuild::Cache cache(cdir, config);

    std::vector<fs::path> key_dirs;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> dist(0, 255);

    for (int i = 0; i < 4; ++i) {
        fs::path src = work / ("lru" + std::to_string(i) + ".cpp");
        std::ofstream(src) << "int lru_symbol_" << i << ";";
        auto key = skipbuild::Cache::compute_key(src, {}, {"-O2"}, "test-compiler-lru");

        fs::path obj = work / ("lru" + std::to_string(i) + ".o");
        std::string data(4096, '\0');
        for (auto& c : data) c = static_cast<char>(dist(rng));
        std::ofstream(obj, std::ios::binary) << data;

        assert(cache.put(key, {obj}));
        key_dirs.push_back(cdir / fs::path(key.hash.substr(0, 2)) /
                           fs::path(key.hash.substr(2, 2)) / fs::path(key.hash));
    }
    assert(cache.get_stats().entries == 4);

    // Deterministic LRU order: key_dirs[0] is the oldest.
    for (int i = 0; i < 4; ++i) {
        fs::last_write_time(key_dirs[static_cast<size_t>(i)] / "meta.json",
                            fs::file_time_type::clock::now() - std::chrono::hours(40 - 10 * i));
    }

    cache.prune();

    auto stats = cache.get_stats();
    int64_t target = static_cast<int64_t>(9000 * 0.85);
    assert(stats.total_size <= target && "prune must reach the eviction target");
    assert(stats.entries >= 1 && "the newest entry must survive");
    assert(fs::exists(key_dirs[3] / "meta.json") && "newest entry must be kept");
    for (size_t i = 0; i + 1 < key_dirs.size(); ++i) {
        assert(!fs::exists(key_dirs[i]) && "older entries must be evicted first");
    }
    std::cout << "  PASS: LRU evicted " << (4 - stats.entries)
              << " entries down to " << stats.total_size << " bytes\n";

    fs::remove_all(work);
    fs::remove_all(cdir);
}

// 5. prune() reclaims directories left behind by failed stores (no meta.json)
//    and stale "*.tmp" files inside otherwise valid entries.
static void test_orphan_and_temp_cleanup() {
    std::cout << "Testing orphan directory and stale temp cleanup...\n";

    fs::path work = fs::temp_directory_path() / "skipbuild-orphan-work";
    fs::path cdir = fs::temp_directory_path() / "skipbuild-orphan-cache";
    fs::remove_all(work);
    fs::remove_all(cdir);
    fs::create_directories(work);
    fs::create_directories(cdir);

    skipbuild::CacheConfig config;
    config.max_cache_size_bytes = 1024 * 1024;
    config.max_age_days = 30;
    skipbuild::Cache cache(cdir, config);

    // A store that failed before writing meta.json leaves blobs behind. Such a
    // directory is invisible to collect_entries() and would leak forever.
    fs::path orphan = cdir / "ab" / "cd" / "abcdef";
    fs::create_directories(orphan);
    std::ofstream(orphan / "partial.o.zst") << "partial";
    auto stale_time = fs::file_time_type::clock::now() - std::chrono::hours(2);
    fs::last_write_time(orphan, stale_time);

    // A valid entry that also contains a leftover temp file.
    fs::path src = work / "keep.cpp";
    std::ofstream(src) << "int keep;";
    fs::path obj = work / "keep.o";
    std::ofstream(obj) << std::string(64, 'k');
    auto key = skipbuild::Cache::compute_key(src, {}, {"-O2"}, "test-compiler-orphan");
    assert(cache.put(key, {obj}));

    fs::path key_dir = cdir / key.hash.substr(0, 2) / key.hash.substr(2, 2) / key.hash;
    fs::path stale_tmp = key_dir / "cache_keep.o_1_2.tmp";
    std::ofstream(stale_tmp) << "";
    fs::last_write_time(key_dir, stale_time);

    cache.prune();

    assert(!fs::exists(orphan) && "orphan entry directory must be removed");
    assert(!fs::exists(stale_tmp) && "stale temp file must be removed");
    assert(fs::exists(key_dir / "meta.json") && "valid entry must be kept");

    auto entry = cache.get(key);
    assert(entry.has_value() && "valid entry must remain readable");
    cache.release(*entry);

    std::cout << "  PASS: orphan directory and stale temp file removed\n";

    fs::remove_all(work);
    fs::remove_all(cdir);
}

int main() {
    test_auto_clear_to_zero();
    test_transient_decompression();
    test_prune_age_below_size_limit();
    test_prune_lru_target_and_no_double_count();
    test_orphan_and_temp_cleanup();
    std::cout << "\nAll auto-clear tests passed!\n";
    return 0;
}
