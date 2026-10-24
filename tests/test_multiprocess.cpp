// Exercises the cache from several processes at once. The parent launches
// workers of itself; each worker hammers put()/get() on the same key and the
// parent fails if any worker exits non-zero or leaves a corrupted entry.
#include "core.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static constexpr int kWorkers = 4;
static constexpr int kIterations = 30;

static fs::path cache_dir() {
    return fs::temp_directory_path() / "skipbuild-test-multiprocess";
}

static skipbuild::CacheKey make_key(const fs::path& cache_root) {
    fs::path src = cache_root / "test.cpp";
    std::ofstream(src) << "int main() { return 0; }";
    return skipbuild::Cache::compute_key(src, {}, {"-O2"}, "test-compiler-1.0");
}

static int run_worker(int id) {
    fs::path root = cache_dir();
    skipbuild::Cache cache(root);
    auto key = make_key(root);

    fs::path obj = root / "test.o";
    std::ofstream(obj) << "fake object file";

    int success = 0;
    int failure = 0;
    for (int i = 0; i < kIterations; ++i) {
        if (cache.put(key, {obj})) ++success;
        else ++failure;

        if (cache.get(key).has_value()) ++success;
        else ++failure;
    }

    auto final_entry = cache.get(key);
    if (!final_entry.has_value() || final_entry->outputs.size() != 1) {
        std::cerr << "worker " << id << ": final cache state corrupted\n";
        return 1;
    }

    std::cout << "worker " << id << ": " << success << " ok, " << failure << " failed\n";
    return 0;
}

static std::string quote_arg(const std::string& arg) {
    return "\"" + arg + "\"";
}

int main(int argc, char* argv[]) {
    if (argc > 2 && std::string(argv[1]) == "--worker") {
        return run_worker(std::atoi(argv[2]));
    }

    fs::path root = cache_dir();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    if (ec) {
        std::cerr << "failed to create test cache: " << ec.message() << "\n";
        return 1;
    }

    fs::path self = fs::absolute(argv[0], ec);
    std::string self_arg = quote_arg(self.string());

    std::vector<int> codes;
    for (int i = 0; i < kWorkers; ++i) {
        std::string cmd = self_arg + " --worker " + std::to_string(i);
        std::cout << "launching: " << cmd << "\n";
        codes.push_back(std::system(cmd.c_str()));
    }

    int failed = 0;
    for (int i = 0; i < kWorkers; ++i) {
        if (codes[i] != 0) {
            std::cerr << "worker " << i << " exited with status " << codes[i] << "\n";
            ++failed;
        }
    }

    skipbuild::Cache cache(root);
    auto entry = cache.get(make_key(root));
    if (!entry.has_value() || entry->outputs.size() != 1) {
        std::cerr << "final cache state corrupted\n";
        ++failed;
    }

    fs::remove_all(root, ec);
    if (failed != 0) {
        std::cerr << failed << " worker(s) failed\n";
        return 1;
    }
    std::cout << "multiprocess test passed\n";
    return 0;
}
