#include "core.hpp"
#include "crypto.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <cstdlib>
#include <array>
#include <filesystem>
#include <atomic>
#include <unordered_set>
#include <map>
#include <thread>
#include <chrono>
#include <mutex>
#include <memory>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#endif

#if REMAKE_HAVE_CURL
#include <curl/curl.h>
#endif

#if REMAKE_HAVE_ZSTD
#include <zstd.h>
#endif

namespace fs = std::filesystem;
namespace remake {

// Helper functions for remote cache integration
static bool read_file(const fs::path& path, std::vector<uint8_t>& data) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    data.resize(size);
    f.read(reinterpret_cast<char*>(data.data()), size);
    return f.good();
}

static bool write_file(const fs::path& path, const std::vector<uint8_t>& data) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    return f.good();
}

static bool compress_data(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
#if REMAKE_HAVE_ZSTD
    size_t max_compressed = ZSTD_compressBound(input.size());
    output.resize(max_compressed);
    size_t compressed_size = ZSTD_compress(output.data(), output.size(), input.data(), input.size(), 3);
    if (ZSTD_isError(compressed_size)) return false;
    output.resize(compressed_size);
    return true;
#else
    output = input;
    return true;
#endif
}

static bool decompress_data(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
#if REMAKE_HAVE_ZSTD
    size_t decompressed_size = ZSTD_getFrameContentSize(input.data(), input.size());
    if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN || decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
        decompressed_size = input.size() * 10;
    }
    output.resize(decompressed_size);
    size_t decompressed = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(decompressed)) return false;
    output.resize(decompressed);
    return true;
#else
    output = input;
    return true;
#endif
}

// --- File Lock Implementation ---

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>

struct FileLock::Impl {
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::filesystem::path lock_path_;
    bool locked_ = false;
    FileLock::LockType lock_type_ = FileLock::LockType::Exclusive;
    
    explicit Impl(const std::filesystem::path& path) : lock_path_(path) {}
    
    ~Impl() {
        if (locked_) unlock();
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    
    bool lock(FileLock::LockType type, int timeout_ms) {
        if (locked_) return true;
        lock_type_ = type;
        
        // Windows LockFileEx supports true shared locks:
        // - Shared (read): dwFlags = 0
        // - Exclusive (write): dwFlags = LOCKFILE_EXCLUSIVE_LOCK
        DWORD lock_flags = (type == FileLock::LockType::Shared) ? 0 : LOCKFILE_EXCLUSIVE_LOCK;
        DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE;  // Allow concurrent opens
        
        auto start = std::chrono::steady_clock::now();
        int attempt = 0;
        
        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeout_ms) return false;
            
            std::error_code ec;
            std::filesystem::create_directories(lock_path_.parent_path(), ec);
            
            std::wstring wpath = lock_path_.wstring();
            handle_ = CreateFileW(
                wpath.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                share_mode,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,  // Removed FILE_FLAG_DELETE_ON_CLOSE
                nullptr
            );
            
            if (handle_ == INVALID_HANDLE_VALUE) {
                DWORD error = GetLastError();
                if (error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10 * (attempt + 1)));
                    attempt++;
                    continue;
                }
                return false;
            }
            
            OVERLAPPED overlapped = {0};
            DWORD flags = lock_flags | LOCKFILE_FAIL_IMMEDIATELY;
            
            // Lock 1 byte at offset 0
            if (!LockFileEx(handle_, flags, 0, 1, 0, &overlapped)) {
                DWORD error = GetLastError();
                CloseHandle(handle_);
                handle_ = INVALID_HANDLE_VALUE;
                
                if (error == ERROR_LOCK_VIOLATION || error == ERROR_SHARING_VIOLATION || 
                    error == ERROR_IO_PENDING || error == ERROR_ACCESS_DENIED) {
                    int sleep_ms = (50 < 10 * (attempt + 1)) ? 50 : 10 * (attempt + 1);
                    if (elapsed + sleep_ms > timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                    attempt++;
                    continue;
                }
                return false;
            }
            
            locked_ = true;
            return true;
        }
    }
    
    bool try_lock(FileLock::LockType type) {
        if (locked_) return true;
        lock_type_ = type;
        
        DWORD lock_flags = (type == FileLock::LockType::Shared) ? 0 : LOCKFILE_EXCLUSIVE_LOCK;
        DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE;
        
        std::error_code ec;
        std::filesystem::create_directories(lock_path_.parent_path(), ec);
        
        std::wstring wpath = lock_path_.wstring();
        handle_ = CreateFileW(
            wpath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            share_mode,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,  // Removed FILE_FLAG_DELETE_ON_CLOSE
            nullptr
        );
        
        if (handle_ == INVALID_HANDLE_VALUE) {
            return false;
        }
        
        OVERLAPPED overlapped = {0};
        DWORD flags = lock_flags | LOCKFILE_FAIL_IMMEDIATELY;
        
        if (!LockFileEx(handle_, flags, 0, 1, 0, &overlapped)) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
        
        locked_ = true;
        return true;
    }
    
    void unlock() {
        if (!locked_ || handle_ == INVALID_HANDLE_VALUE) return;
        
        OVERLAPPED overlapped = {0};
        if (!UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped)) {
            // Ignore errors on unlock
        }
        locked_ = false;
    }
    
    bool is_locked() const { return locked_; }
};

#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <cerrno>

struct FileLock::Impl {
    int fd_ = -1;
    std::filesystem::path lock_path_;
    bool locked_ = false;
    FileLock::LockType lock_type_ = FileLock::LockType::Exclusive;
    
    explicit Impl(const std::filesystem::path& path) : lock_path_(path) {}
    
    ~Impl() {
        if (locked_) unlock();
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    
    bool lock(FileLock::LockType type, int timeout_ms) {
        if (locked_) return true;
        lock_type_ = type;
        
        auto start = std::chrono::steady_clock::now();
        int attempt = 0;
        
        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeout_ms) return false;
            
            std::error_code ec;
            std::filesystem::create_directories(lock_path_.parent_path(), ec);
            
            fd_ = open(lock_path_.c_str(), O_CREAT | O_RDWR, 0644);
            if (fd_ < 0) {
                if (errno == ENOENT) {
                    int sleep_ms = std::min(50, 10 * (attempt + 1));
                    if (elapsed + sleep_ms > timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                    attempt++;
                    continue;
                }
                return false;
            }
            
            int flock_op = (type == FileLock::LockType::Shared) ? LOCK_SH : LOCK_EX;
            
            if (flock(fd_, flock_op) < 0) {
                int err = errno;
                close(fd_);
                fd_ = -1;
                if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK || err == EDEADLK) {
                    int sleep_ms = (50 < 10 * (attempt + 1)) ? 50 : 10 * (attempt + 1);
                    if (elapsed + sleep_ms > timeout_ms) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                    attempt++;
                    continue;
                }
                return false;
            }
            
            locked_ = true;
            return true;
        }
    }
    
    bool try_lock(FileLock::LockType type) {
        if (locked_) return true;
        lock_type_ = type;
        
        std::error_code ec;
        std::filesystem::create_directories(lock_path_.parent_path(), ec);
        
        fd_ = open(lock_path_.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd_ < 0) return false;
        
        int flock_op = (type == FileLock::LockType::Shared) ? (LOCK_SH | LOCK_NB) : (LOCK_EX | LOCK_NB);
        
        if (flock(fd_, flock_op) < 0) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        
        locked_ = true;
        return true;
    }
    
    void unlock() {
        if (!locked_ || fd_ < 0) return;
        
        flock(fd_, LOCK_UN);
        locked_ = false;
    }
    
    bool is_locked() const { return locked_; }
};

#endif

FileLock::FileLock(const std::filesystem::path& lock_path) : impl_(std::make_unique<Impl>(lock_path)) {}
FileLock::~FileLock() = default;

FileLock::FileLock(FileLock&& other) noexcept : impl_(std::move(other.impl_)) {}
FileLock& FileLock::operator=(FileLock&& other) noexcept {
    impl_ = std::move(other.impl_);
    return *this;
}

bool FileLock::lock(LockType type, int timeout_ms) { return impl_->lock(type, timeout_ms); }
bool FileLock::try_lock(LockType type) { return impl_->try_lock(type); }
void FileLock::unlock() { impl_->unlock(); }
bool FileLock::is_locked() const { return impl_->is_locked(); }

// --- Local Cache Implementation ---

static std::string compute_sha256(const std::string& data) {
    return crypto::sha256_hex(data);
}

static std::string compute_sha256(const std::vector<uint8_t>& data) {
    return crypto::sha256_hex(data.data(), data.size());
}

// Atomic file replacement using MoveFileExW on Windows
static bool atomic_replace_file(const std::filesystem::path& src, const std::filesystem::path& dst) {
#ifdef _WIN32
    std::wstring wsrc = src.wstring();
    std::wstring wdst = dst.wstring();
    DWORD attrs = GetFileAttributesW(wdst.c_str());
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        // Destination exists, replace it
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    return MoveFileExW(wsrc.c_str(), wdst.c_str(), flags);
#else
    std::error_code ec;
    std::filesystem::rename(src, dst, ec);
    return !ec;
#endif
}

// Compression/Decompression using ZSTD
#if REMAKE_HAVE_ZSTD

static bool compress_buffer(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, int level = 3) {
    size_t max_compressed = ZSTD_compressBound(input.size());
    output.resize(max_compressed);
    
    size_t compressed_size = ZSTD_compress(output.data(), output.size(), input.data(), input.size(), level);
    if (ZSTD_isError(compressed_size)) {
        return false;
    }
    output.resize(compressed_size);
    return true;
}

static bool decompress_buffer(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    // Get decompressed size from frame header
    size_t decompressed_size = ZSTD_getFrameContentSize(input.data(), input.size());
    if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN || decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
        // Fallback: try to decompress with a reasonable upper bound
        decompressed_size = input.size() * 10; // Assume max 10x compression ratio
    }
    
    output.resize(decompressed_size);
    
    size_t decompressed = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(decompressed)) {
        return false;
    }
    output.resize(decompressed);
    return true;
}

static bool compress_file(const std::filesystem::path& input_path, const std::filesystem::path& output_path, int level = 3) {
    std::ifstream in(input_path, std::ios::binary);
    if (!in) return false;
    
    std::vector<uint8_t> input_data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    
    std::vector<uint8_t> compressed;
    if (!compress_buffer(input_data, compressed, level)) {
        return false;
    }
    
    std::ofstream out(output_path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    out.close();
    return true;
}

static bool decompress_file(const std::filesystem::path& input_path, const std::filesystem::path& output_path) {
    std::ifstream in(input_path, std::ios::binary);
    if (!in) return false;
    
    std::vector<uint8_t> compressed_data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    
    std::vector<uint8_t> decompressed;
    if (!decompress_buffer(compressed_data, decompressed)) {
        return false;
    }
    
    std::ofstream out(output_path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(decompressed.data()), decompressed.size());
    out.close();
    return true;
}

#endif // REMAKE_HAVE_ZSTD

// Helper to compute file hash
static std::string compute_file_hash(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return compute_sha256(content);
}

// Helper to write meta.json with sidecar info
static bool write_meta_json(const std::filesystem::path& meta_file, const std::filesystem::path& meta_temp,
                            const std::string& toolchain_id, int64_t total_size,
                            const std::vector<std::pair<std::filesystem::path, SidecarType>>& sidecars) {
    std::ofstream meta(meta_temp);
    if (!meta) return false;
    
    meta << "{\"toolchain\":\"" << toolchain_id << "\",\"ts\":" << std::time(nullptr) << ",\"size\":" << total_size;
    meta << ",\"compressed\":true";
    
    if (!sidecars.empty()) {
        meta << ",\"sidecars\":[";
        for (size_t i = 0; i < sidecars.size(); ++i) {
            const auto& [path, type] = sidecars[i];
            std::string type_str;
            switch (type) {
                case SidecarType::PDB: type_str = "pdb"; break;
                case SidecarType::IDB: type_str = "idb"; break;
                case SidecarType::DWO: type_str = "dwo"; break;
                default: type_str = "other"; break;
            }
            if (i > 0) meta << ",";
            meta << "{\"name\":\"" << path.filename().string() 
                 << "\",\"type\":\"" << type_str 
                 << "\",\"hash\":\"" << compute_file_hash(path) << "\"}";
        }
        meta << "]";
    }
    meta << "}";
    meta.flush();
    meta.close();
    
    return atomic_replace_file(meta_temp, meta_file);
}

// Parse sidecars from meta.json
static std::vector<std::tuple<std::string, SidecarType, std::string>> parse_sidecars_from_meta(const std::filesystem::path& meta_file) {
    std::vector<std::tuple<std::string, SidecarType, std::string>> sidecars;
    
    std::ifstream f(meta_file);
    if (!f) return sidecars;
    
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    
    // Simple JSON parsing for sidecars array
    size_t sidecars_pos = content.find("\"sidecars\":");
    if (sidecars_pos == std::string::npos) return sidecars;
    
    size_t start = content.find('[', sidecars_pos);
    size_t end = content.find(']', start);
    if (start == std::string::npos || end == std::string::npos) return sidecars;
    
    std::string arr = content.substr(start + 1, end - start - 1);
    
    // Simple parsing of array elements
    size_t pos = 0;
    while (pos < arr.size()) {
        size_t obj_start = arr.find('{', pos);
        if (obj_start == std::string::npos) break;
        size_t obj_end = arr.find('}', obj_start);
        if (obj_end == std::string::npos) break;
        
        std::string obj = arr.substr(obj_start + 1, obj_end - obj_start - 1);
        
        // Extract name
        size_t name_pos = obj.find("\"name\":");
        if (name_pos == std::string::npos) { pos = obj_end + 1; continue; }
        size_t name_start = obj.find('"', name_pos + 7);
        size_t name_end = obj.find('"', name_start + 1);
        if (name_start == std::string::npos || name_end == std::string::npos) { pos = obj_end + 1; continue; }
        std::string name = obj.substr(name_start + 1, name_end - name_start - 1);
        
        // Extract type
        size_t type_pos = obj.find("\"type\":");
        if (type_pos == std::string::npos) { pos = obj_end + 1; continue; }
        size_t type_start = obj.find('"', type_pos + 7);
        size_t type_end = obj.find('"', type_start + 1);
        if (type_start == std::string::npos || type_end == std::string::npos) { pos = obj_end + 1; continue; }
        std::string type_str = obj.substr(type_start + 1, type_end - type_start - 1);
        
        // Extract hash
        size_t hash_pos = obj.find("\"hash\":");
        if (hash_pos == std::string::npos) { pos = obj_end + 1; continue; }
        size_t hash_start = obj.find('"', hash_pos + 7);
        size_t hash_end = obj.find('"', hash_start + 1);
        if (hash_start == std::string::npos || hash_end == std::string::npos) { pos = obj_end + 1; continue; }
        std::string hash = obj.substr(hash_start + 1, hash_end - hash_start - 1);
        
        SidecarType type = SidecarType::Other;
        if (type_str == "pdb") type = SidecarType::PDB;
        else if (type_str == "idb") type = SidecarType::IDB;
        else if (type_str == "dwo") type = SidecarType::DWO;
        
        sidecars.emplace_back(name, type, hash);
        pos = obj_end + 1;
    }
    
    return sidecars;
}

CacheKey Cache::compute_key(const std::filesystem::path& source,
                            const std::vector<std::string>& includes,
                            const std::vector<std::string>& flags,
                            const std::string& compiler_hash,
                            CompilerType compiler_type) {
    std::string source_hash = compute_file_hash(source);
    if (source_hash.empty()) {
        source_hash = compute_sha256(source.string());
    }
    
    // Normalize source path for key
    auto build_root = find_build_root(source);
    std::string norm_source = normalize_path_for_key(source, build_root);
    
    // Normalize include paths
    std::vector<std::string> norm_includes;
    norm_includes.reserve(includes.size());
    for (const auto& inc : includes) {
        norm_includes.push_back(normalize_path_for_key(inc, build_root));
    }
    std::sort(norm_includes.begin(), norm_includes.end());
    
    // Normalize flags (sort for determinism)
    std::vector<std::string> norm_flags = flags;
    std::sort(norm_flags.begin(), norm_flags.end());
    
    // Get relevant environment hash
    std::string env_hash = get_relevant_env_hash();

    std::ostringstream oss;
    oss << source_hash << "|" << norm_source;
    for (const auto& inc : norm_includes) {
        oss << "|" << inc;
    }
    for (const auto& flag : norm_flags) {
        oss << "|" << flag;
    }
    oss << "|" << compiler_hash;
    oss << "|" << static_cast<int>(compiler_type);
    oss << "|" << env_hash;

    CacheKey key;
    key.hash = compute_sha256(oss.str());
    key.toolchain_id = compiler_hash;
    return key;
}

CacheKey Cache::compute_key_from_command(const CompileCommand& cmd) {
    // Create a modified copy with normalized paths
    std::vector<std::string> norm_includes;
    norm_includes.reserve(cmd.includes.size());
    auto build_root = find_build_root(cmd.source);
    for (const auto& inc : cmd.includes) {
        norm_includes.push_back(normalize_path_for_key(inc, build_root));
    }
    std::sort(norm_includes.begin(), norm_includes.end());
    
    std::vector<std::string> norm_flags = cmd.flags;
    std::sort(norm_flags.begin(), norm_flags.end());
    
    return compute_key(cmd.source, norm_includes, norm_flags, cmd.compiler_hash, 
                       detect_compiler(cmd.compiler_path).type);
}

// Generate unique temporary file path
static std::filesystem::path make_temp_path(const std::filesystem::path& base_dir, const std::string& prefix) {
    std::ostringstream oss;
    oss << prefix << "_" << std::this_thread::get_id() << "_" << std::chrono::steady_clock::now().time_since_epoch().count() << ".tmp";
    return base_dir / oss.str();
}

bool Cache::put(const CacheKey& key, const std::vector<std::filesystem::path>& outputs,
                const std::vector<std::pair<std::filesystem::path, SidecarType>>& sidecars) {
    int64_t total_size = 0;
    int64_t max_cache_size = config_.max_cache_size_bytes;
    bool success = false;

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        std::filesystem::path key_dir = root_ / key.hash.substr(0, 2) / key.hash.substr(2, 2) / key.hash;
        std::filesystem::path lock_file = key_dir / ".lock";
        
        FileLock file_lock(lock_file);
        FileLock::ScopedLock scoped_lock(file_lock, FileLock::LockType::Exclusive);
        if (!scoped_lock) return false;

        std::error_code ec;
        std::filesystem::create_directories(key_dir, ec);
        if (ec) return false;

        std::vector<std::pair<std::filesystem::path, SidecarType>> stored_sidecars;
        
        #if REMAKE_HAVE_ZSTD
        // Store primary outputs (with compression)
        std::vector<std::pair<std::filesystem::path, bool>> stored_outputs; // {path, is_compressed}
        for (const auto& output : outputs) {
            if (!std::filesystem::exists(output)) continue;
            
            std::filesystem::path dest = key_dir / (output.filename().string() + ".zst");
            std::filesystem::path temp_dest = make_temp_path(key_dir, "cache_" + output.filename().string() + ".zst");
            
            // Compress file
            if (!compress_file(output, temp_dest)) {
                return false;
            }
            
            // Atomic replace
            if (!atomic_replace_file(temp_dest, dest)) {
                std::error_code ec2;
                std::filesystem::remove(temp_dest, ec2);
                return false;
            }
            
            total_size += std::filesystem::file_size(dest, ec);
            stored_outputs.emplace_back(dest, true);
        }
        
        // Store sidecar files (with compression)
        for (const auto& [sidecar_path, sidecar_type] : sidecars) {
            if (!std::filesystem::exists(sidecar_path)) continue;
            
            std::filesystem::path dest = key_dir / (sidecar_path.filename().string() + ".zst");
            std::filesystem::path temp_dest = make_temp_path(key_dir, "cache_" + sidecar_path.filename().string() + ".zst");
            
            // Compress file
            if (!compress_file(sidecar_path, temp_dest)) {
                return false;
            }
            
            // Atomic replace
            if (!atomic_replace_file(temp_dest, dest)) {
                std::error_code ec2;
                std::filesystem::remove(temp_dest, ec2);
                return false;
            }
            
            total_size += std::filesystem::file_size(dest, ec);
            stored_sidecars.emplace_back(dest, sidecar_type);
        }
        #else
        // Store primary outputs (without compression)
        for (const auto& output : outputs) {
            if (!std::filesystem::exists(output)) continue;
            
            std::filesystem::path dest = key_dir / output.filename();
            std::filesystem::path temp_dest = make_temp_path(key_dir, "cache_" + output.filename().string());
            
            // Copy to temp file first
            std::filesystem::copy_file(output, temp_dest, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) return false;
            
            // Atomic replace
            if (!atomic_replace_file(temp_dest, dest)) {
                std::error_code ec2;
                std::filesystem::remove(temp_dest, ec2);
                return false;
            }
            
            total_size += std::filesystem::file_size(output, ec);
        }
        
        // Store sidecar files
        for (const auto& [sidecar_path, sidecar_type] : sidecars) {
            if (!std::filesystem::exists(sidecar_path)) continue;
            
            std::filesystem::path dest = key_dir / sidecar_path.filename();
            std::filesystem::path temp_dest = make_temp_path(key_dir, "cache_" + sidecar_path.filename().string());
            
            // Copy to temp file first
            std::filesystem::copy_file(sidecar_path, temp_dest, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) return false;
            
            // Atomic replace
            if (!atomic_replace_file(temp_dest, dest)) {
                std::error_code ec2;
                std::filesystem::remove(temp_dest, ec2);
                return false;
            }
            
            total_size += std::filesystem::file_size(sidecar_path, ec);
            stored_sidecars.emplace_back(dest, sidecar_type);
        }
        #endif
        
        // Write meta.json atomically with sidecar info and compression info
        std::filesystem::path meta_file = key_dir / "meta.json";
        std::filesystem::path meta_temp = make_temp_path(key_dir, "meta");
        if (!write_meta_json(meta_file, meta_temp, key.toolchain_id, total_size, stored_sidecars)) {
            return false;
        }
        
        success = true;
    }
    
    // Update cache size stats after releasing all locks
    if (success) {
        stats_tracker_->update_cache_size(1, total_size, max_cache_size);
    }
    
    return success;
}

std::optional<CacheEntry> Cache::safe_get(const CacheKey& key) const {
    // First try local cache
    std::optional<CacheEntry> local_entry;
    try {
        local_entry = get(key);
        if (local_entry) return local_entry;
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[build-cache warning] Cache read failed: " << e.what() << ". Falling back to native compilation.\n";
        try {
            std::filesystem::path key_dir = root_ / key.hash.substr(0, 2) / key.hash.substr(2, 2) / key.hash;
            std::error_code ec;
            std::filesystem::remove_all(key_dir, ec);
        } catch (...) {}
    } catch (const std::exception& e) {
        std::cerr << "[build-cache warning] Cache read failed: " << e.what() << ". Falling back to native compilation.\n";
        try {
            std::filesystem::path key_dir = root_ / key.hash.substr(0, 2) / key.hash.substr(2, 2) / key.hash;
            std::error_code ec;
            std::filesystem::remove_all(key_dir, ec);
        } catch (...) {}
    } catch (...) {
        std::cerr << "[build-cache warning] Cache read failed: unknown error. Falling back to native compilation.\n";
        try {
            std::filesystem::path key_dir = root_ / key.hash.substr(0, 2) / key.hash.substr(2, 2) / key.hash;
            std::error_code ec;
            std::filesystem::remove_all(key_dir, ec);
        } catch (...) {}
    }
    
    // Local miss - try remote cache
    if (remote_cache_) {
        try {
            auto remote_result = remote_cache_->get(key.hash, key.toolchain_id);
            if (remote_result.success && !remote_result.primary_data.empty()) {
                // Create local cache directory for this key
                fs::path key_dir = root_ / key.hash.substr(0, 2) / key.hash.substr(2, 2) / key.hash;
                fs::create_directories(key_dir);
                
                // Decompress primary artifact
                std::vector<uint8_t> primary_decompressed;
                if (!decompress_data(remote_result.primary_data, primary_decompressed)) {
                    std::cerr << "[build-cache warning] Failed to decompress primary artifact from remote cache.\n";
                } else {
                    // Save primary artifact
                    fs::path primary_path = key_dir / (key.hash + ".obj");
                    if (write_file(primary_path, primary_decompressed)) {
                        // Create CacheEntry
                        CacheEntry entry;
                        entry.key = key;
                        entry.timestamp = std::time(nullptr);
                        entry.size_bytes = primary_decompressed.size();
                        entry.outputs.push_back(primary_path);
                        
                        // Decompress and save sidecars
                        for (const auto& [name, compressed_data] : remote_result.sidecars) {
                            std::vector<uint8_t> decompressed;
                            if (decompress_data(compressed_data, decompressed)) {
                                fs::path sidecar_path = key_dir / name;
                                if (write_file(sidecar_path, decompressed)) {
                                    SidecarInfo si;
                                    si.relative_path = sidecar_path.filename();
                                    si.hash = compute_sha256(decompressed);
                                    
                                    // Determine sidecar type from extension
                                    std::string ext = name;
                                    if (ext.size() > 4 && ext.substr(ext.size() - 4) == ".zst") {
                                        ext = ext.substr(0, ext.size() - 4);
                                    }
                                    if (ext.size() > 4 && (ext.substr(ext.size() - 4) == ".pdb" || ext.substr(ext.size() - 4) == ".idb")) {
                                        si.type = (ext.substr(ext.size() - 4) == ".pdb") ? SidecarType::PDB : SidecarType::IDB;
                                    } else if (ext.size() > 4 && ext.substr(ext.size() - 4) == ".dwo") {
                                        si.type = SidecarType::DWO;
                                    } else {
                                        si.type = SidecarType::Other;
                                    }
                                    
                                    entry.sidecars.push_back(std::move(si));
                                    entry.size_bytes += decompressed.size();
                                }
                            }
                        }
                        
                        return entry;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[build-cache warning] Remote cache read failed: " << e.what() << ". Falling back to native compilation.\n";
        } catch (...) {
            std::cerr << "[build-cache warning] Remote cache read failed: unknown error. Falling back to native compilation.\n";
        }
    }
    
    stats_tracker_->record_miss();
    return std::nullopt;
}

bool Cache::safe_put(const CacheKey& key, const std::vector<std::filesystem::path>& outputs,
                     const std::vector<std::pair<std::filesystem::path, SidecarType>>& sidecars) {
    bool local_ok = false;
    try {
        local_ok = put(key, outputs, sidecars);
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[build-cache warning] Cache write failed: " << e.what() << ". Build output preserved.\n";
    } catch (const std::exception& e) {
        std::cerr << "[build-cache warning] Cache write failed: " << e.what() << ". Build output preserved.\n";
    } catch (...) {
        std::cerr << "[build-cache warning] Cache write failed: unknown error. Build output preserved.\n";
    }
    
    // Also upload to remote cache if configured and not read-only
    if (remote_cache_ && !config_.read_only) {
        try {
            // Compress primary outputs
            std::vector<uint8_t> primary_data;
            for (const auto& output : outputs) {
                if (!fs::exists(output)) continue;
                std::vector<uint8_t> file_data;
                if (!read_file(output, file_data)) continue;
                
                std::vector<uint8_t> compressed;
                if (compress_data(file_data, compressed)) {
                    primary_data = std::move(compressed);
                    break; // Use first valid output
                }
            }
            
            // Compress sidecars
            std::vector<std::pair<std::string, std::vector<uint8_t>>> compressed_sidecars;
            for (const auto& [path, type] : sidecars) {
                if (!fs::exists(path)) continue;
                std::vector<uint8_t> file_data;
                if (!read_file(path, file_data)) continue;
                
                std::vector<uint8_t> compressed;
                if (compress_data(file_data, compressed)) {
                    // Determine remote filename
                    std::string ext = path.extension().string();
                    std::string remote_name = key.hash + ext + ".zst";
                    compressed_sidecars.emplace_back(remote_name, std::move(compressed));
                }
            }
            
            if (!primary_data.empty() || !compressed_sidecars.empty()) {
                remote_cache_->put(key.hash, key.toolchain_id, primary_data, compressed_sidecars);
            }
        } catch (const std::exception& e) {
            std::cerr << "[build-cache warning] Remote cache write failed: " << e.what() << ". Build output preserved locally.\n";
        } catch (...) {
            std::cerr << "[build-cache warning] Remote cache write failed: unknown error. Build output preserved locally.\n";
        }
    }
    
    return local_ok;
}

std::optional<CacheEntry> Cache::get(const CacheKey& key) const {
    // First, acquire the cache mutex to do the lookup
    std::optional<CacheEntry> result;
    bool hit = false;
    int64_t entry_size = 0;
    
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        std::filesystem::path key_dir = root_ / key.hash.substr(0, 2) / key.hash.substr(2, 2) / key.hash;
        std::filesystem::path lock_file = key_dir / ".lock";
        
        FileLock file_lock(lock_file);
        FileLock::ScopedLock scoped_lock(file_lock, FileLock::LockType::Shared);
        if (!scoped_lock) {
            // Cache mutex released here (lock goes out of scope)
        } else {
            std::filesystem::path meta_file = key_dir / "meta.json";

            if (!std::filesystem::exists(meta_file)) {
                // Cache mutex released here
            } else {
                std::error_code ec;
                // Validate meta.json has expected content
                std::ifstream meta_check(meta_file);
                std::string meta_content((std::istreambuf_iterator<char>(meta_check)), std::istreambuf_iterator<char>());
                if (meta_content.find("\"toolchain\"") == std::string::npos) {
                    // Corrupted meta.json - purge and return nullopt
                    FileLock file_lock_excl(lock_file);
                    FileLock::ScopedLock excl_lock(file_lock_excl, FileLock::LockType::Exclusive);
                    if (excl_lock) {
                        std::filesystem::remove_all(key_dir, ec);
                    }
                    // Cache mutex released here
                } else {
                    CacheEntry entry;
                    entry.key = key;
                    entry.timestamp = std::time(nullptr);
                    entry.size_bytes = 0;

                    bool has_corrupted = false;
                    
                    for (auto& f : std::filesystem::directory_iterator(key_dir, ec)) {
                        if (f.path().filename() != "meta.json" && f.path().filename() != ".lock") {
                            auto size = std::filesystem::file_size(f.path(), ec);
                            if (size == 0) {
                                has_corrupted = true;
                                break;
                            }
                            // Check if it's a compressed file (.zst)
                            std::string ext = f.path().extension().string();
                            #if REMAKE_HAVE_ZSTD
                            if (ext == ".zst") {
                                // Decompress to cache directory (remove .zst extension)
                                // This overwrites any previously decompressed file
                                std::filesystem::path dest_path = key_dir / f.path().stem();
                                if (!decompress_file(f.path(), dest_path)) {
                                    has_corrupted = true;
                                    break;
                                }
                                
                                // Check if it's a sidecar file
                                std::string stem_ext = dest_path.extension().string();
                                if (stem_ext == ".pdb" || stem_ext == ".idb" || stem_ext == ".dwo") {
                                    SidecarInfo si;
                                    si.relative_path = dest_path.filename();
                                    si.hash = compute_file_hash(dest_path);
                                    
                                    if (stem_ext == ".pdb") si.type = SidecarType::PDB;
                                    else if (stem_ext == ".idb") si.type = SidecarType::IDB;
                                    else if (stem_ext == ".dwo") si.type = SidecarType::DWO;
                                    else si.type = SidecarType::Other;
                                    
                                    entry.sidecars.push_back(si);
                                } else {
                                    entry.outputs.push_back(dest_path);
                                }
                                entry.size_bytes += std::filesystem::file_size(dest_path, ec);
                            }
                            // Ignore non-.zst files (they may be previously decompressed files)
                            #else
                            {
                                // Uncompressed file (legacy)
                                // Check if it's a sidecar file by checking extension
                                if (ext == ".pdb" || ext == ".idb" || ext == ".dwo") {
                                    SidecarInfo si;
                                    si.relative_path = f.path().filename();
                                    si.hash = compute_file_hash(f.path());
                                    
                                    if (ext == ".pdb") si.type = SidecarType::PDB;
                                    else if (ext == ".idb") si.type = SidecarType::IDB;
                                    else if (ext == ".dwo") si.type = SidecarType::DWO;
                                    else si.type = SidecarType::Other;
                                    
                                    entry.sidecars.push_back(si);
                                } else {
                                    entry.outputs.push_back(f.path());
                                }
                                entry.size_bytes += size;
                            }
                            #endif
                        }
                    }

                    // Parse sidecars from meta.json for additional metadata (hashes, types)
                    auto meta_sidecars = parse_sidecars_from_meta(meta_file);
                    for (const auto& [name, type, hash] : meta_sidecars) {
                        // Find matching sidecar in entry
                        for (auto& si : entry.sidecars) {
                            if (si.relative_path.filename().string() == name) {
                                si.hash = hash;
                                si.type = type;
                                break;
                            }
                        }
                    }

                    // Check for zero-byte outputs or missing outputs
                    if (entry.outputs.empty() || has_corrupted) {
                        // Release shared lock (via scoped_lock destructor) and acquire exclusive lock to purge
                    }
                    
                    if (entry.outputs.empty() || has_corrupted) {
                        // scoped_lock goes out of scope here, releasing the shared lock
                        FileLock file_lock_excl(lock_file);
                        FileLock::ScopedLock excl_lock(file_lock_excl, FileLock::LockType::Exclusive);
                        if (excl_lock) {
                            std::filesystem::remove_all(key_dir, ec);
                        }
                        // Cache mutex released here
                    } else {
                        update_access_time(key.hash);
                        hit = true;
                        entry_size = entry.size_bytes;
                        result = std::move(entry);
                    }
                }
            }
        }
    }
    
    // Record stats OUTSIDE the cache mutex
    if (hit) {
        stats_tracker_->record_hit(1000, entry_size); // 1000ms estimated compile time saved
    } else {
        stats_tracker_->record_miss();
    }
    
    return result;
}

void Cache::update_access_time(const std::string& hash) const {
    std::filesystem::path key_dir = root_ / hash.substr(0, 2) / hash.substr(2, 2) / hash;
    std::filesystem::path lock_file = key_dir / ".lock";
    
    FileLock file_lock(lock_file);
    FileLock::ScopedLock scoped_lock(file_lock, FileLock::LockType::Shared);
    if (!scoped_lock) return;
    
    std::filesystem::path meta_file = key_dir / "meta.json";
    if (std::filesystem::exists(meta_file)) {
        std::error_code ec;
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::filesystem::last_write_time(meta_file, std::filesystem::file_time_type::clock::now(), ec);
    }
}

std::vector<Cache::EntryMeta> Cache::collect_entries() const {
    std::vector<EntryMeta> entries;
    std::error_code ec;

    if (!std::filesystem::exists(root_)) return entries;

    for (auto& d1 : std::filesystem::directory_iterator(root_, ec)) {
        for (auto& d2 : std::filesystem::directory_iterator(d1.path(), ec)) {
            for (auto& d3 : std::filesystem::directory_iterator(d2.path(), ec)) {
                std::filesystem::path meta_file = d3.path() / "meta.json";
                if (std::filesystem::exists(meta_file)) {
                    EntryMeta meta;
                    meta.path = d3.path();
                    meta.timestamp = std::filesystem::last_write_time(meta_file, ec).time_since_epoch().count();
                    meta.size_bytes = 0;
                    meta.last_access = meta.timestamp;
                    for (auto& f : std::filesystem::directory_iterator(d3.path(), ec)) {
                        if (f.path().filename() != "meta.json" && f.path().filename() != ".lock") {
                            meta.size_bytes += std::filesystem::file_size(f.path(), ec);
                        }
                    }
                    entries.push_back(std::move(meta));
                }
            }
        }
    }
    return entries;
}

void Cache::purge_corrupted_entry(const EntryMeta& entry) const {
    std::filesystem::path lock_file = entry.path / ".lock";
    FileLock file_lock(lock_file);
    FileLock::ScopedLock scoped_lock(file_lock, FileLock::LockType::Exclusive);
    if (!scoped_lock) return;
    
    std::error_code ec;
    std::filesystem::remove_all(entry.path, ec);
}

void Cache::prune(int64_t max_size_bytes, int max_age_days) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // Use config defaults if not explicitly provided
    if (max_size_bytes < 0) max_size_bytes = config_.max_cache_size_bytes;
    if (max_age_days < 0) max_age_days = config_.max_age_days;
    double target_ratio = config_.eviction_target_ratio;
    
    auto entries = collect_entries();
    if (entries.empty()) return;
    
    // First pass: purge corrupted/zero-byte entries
    std::vector<EntryMeta> valid_entries;
    for (const auto& e : entries) {
        bool corrupted = false;
        std::error_code ec;
        
        // Check if meta.json exists
        std::filesystem::path meta_file = e.path / "meta.json";
        if (!std::filesystem::exists(meta_file)) {
            corrupted = true;
        } else {
            // Check for zero-byte or missing object files
            bool has_valid_output = false;
            for (auto& f : std::filesystem::directory_iterator(e.path, ec)) {
                if (f.path().filename() != "meta.json" && f.path().filename() != ".lock") {
                    auto size = std::filesystem::file_size(f.path(), ec);
                    if (size > 0) {
                        has_valid_output = true;
                        break;
                    }
                }
            }
            if (!has_valid_output) {
                corrupted = true;
            }
        }
        
        if (corrupted) {
            purge_corrupted_entry(e);
        } else {
            valid_entries.push_back(e);
        }
    }
    
    if (valid_entries.empty()) return;
    
    // Sort by last access time (oldest first) for LRU
    std::sort(valid_entries.begin(), valid_entries.end(), [](const EntryMeta& a, const EntryMeta& b) {
        return a.last_access < b.last_access;
    });
    
    // Age-based eviction
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(24 * max_age_days);
    
    int64_t current_size = 0;
    for (const auto& e : valid_entries) current_size += e.size_bytes;
    
    // First, evict entries older than max_age_days
    for (const auto& e : valid_entries) {
        if (current_size <= max_size_bytes) break;
        if (std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>(std::chrono::nanoseconds(e.last_access)) > cutoff) continue;
        
        // Mutex protects us in single-process case; file lock is for inter-process
        // Skip file lock here since we need to delete the lock file itself
        std::error_code ec;
        std::filesystem::remove_all(e.path, ec);
        if (!ec) {
            current_size -= e.size_bytes;
        }
    }
    
    // Then, LRU eviction until we reach target_ratio of max_size_bytes
    int64_t target_size = static_cast<int64_t>(max_size_bytes * target_ratio);
    if (current_size > target_size) {
        for (const auto& e : valid_entries) {
            if (current_size <= target_size) break;
            
            // Mutex protects us in single-process case; file lock is for inter-process
            // Skip file lock here since we need to delete the lock file itself
            std::error_code ec;
            std::filesystem::remove_all(e.path, ec);
            if (!ec) {
                current_size -= e.size_bytes;
            }
        }
    }
}

Cache::Stats Cache::get_stats() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    Stats stats;
    auto entries = collect_entries();
    stats.entries = entries.size();
    for (const auto& e : entries) {
        stats.total_size += e.size_bytes;
    }
    return stats;
}

#ifdef _WIN32
std::string to_long_path(const std::string& path) {
    if (path.size() >= 260 && path.find("\\\\?\\") != 0) {
        return "\\\\?\\" + path;
    }
    return path;
}

std::string from_long_path(const std::string& path) {
    if (path.find("\\\\?\\") == 0) {
        return path.substr(4);
    }
    return path;
}

std::filesystem::path normalize_path(const std::filesystem::path& path) {
    std::string p = to_long_path(path.string());
    return std::filesystem::path(p);
}
#endif

CompilerInfo detect_compiler(const std::string& compiler_path) {
    CompilerInfo info;
    info.path = compiler_path;
    
    std::string name = std::filesystem::path(compiler_path).filename().string();
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    
    if (name.find("cl.exe") != std::string::npos || name.find("clang-cl") != std::string::npos) {
        info.type = CompilerType::MSVC;
    } else if (name.find("clang") != std::string::npos) {
        info.type = CompilerType::Clang;
    } else if (name.find("gcc") != std::string::npos || name.find("g++") != std::string::npos) {
        info.type = CompilerType::GCC;
    } else {
        info.type = CompilerType::Unknown;
    }
    
    info.version_string = "unknown";
    info.hash = get_compiler_hash(compiler_path);
    return info;
}

std::optional<CompileCommand> parse_compile_command(int argc, char* argv[]) {
    CompileCommand cmd;
    bool is_compile = false;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-c" || arg == "/c") {
            is_compile = true;
            cmd.args.push_back(arg);
        } else if ((arg == "-o" || arg == "/Fo") && i + 1 < argc) {
            cmd.output = argv[++i];
            cmd.args.push_back(arg);
            cmd.args.push_back(argv[i]);
        } else if (arg.rfind("-I", 0) == 0 || arg.rfind("/I", 0) == 0) {
            std::string prefix = arg.rfind("-I", 0) == 0 ? "-I" : "/I";
            cmd.includes.push_back(arg.substr(prefix.size()));
            cmd.args.push_back(arg);
        } else if ((arg == "-I" || arg == "/I") && i + 1 < argc) {
            cmd.includes.push_back(argv[++i]);
            cmd.args.push_back(arg);
            cmd.args.push_back(argv[i]);
        } else if (arg.rfind("-std=", 0) == 0 || arg.rfind("-O", 0) == 0 || arg.rfind("-g", 0) == 0 || arg.rfind("-Wall", 0) == 0 || arg.rfind("-W", 0) == 0) {
            cmd.flags.push_back(arg);
            cmd.args.push_back(arg);
        } else if (arg == "/Zi" || arg == "/ZI") {
            // MSVC debug info flags
            cmd.flags.push_back(arg);
            cmd.args.push_back(arg);
            cmd.has_debug_info = true;
        } else if (arg.rfind("/Fo", 0) == 0) {
            // MSVC output file: /Fo<path> or /Fo <path>
            cmd.flags.push_back(arg);
            cmd.args.push_back(arg);
            std::string output_path = arg.substr(3);
            if (!output_path.empty()) {
                cmd.output = output_path;
            } else if (i + 1 < argc) {
                cmd.output = argv[++i];
                cmd.args.push_back(cmd.output.string());
            }
        } else if (arg == "-gsplit-dwarf") {
            // GCC/Clang Split DWARF
            cmd.flags.push_back(arg);
            cmd.args.push_back(arg);
            cmd.has_debug_info = true;
            // Expected .dwo file is <output_name>.dwo
            if (!cmd.output.empty()) {
                fs::path dwo_path = cmd.output;
                dwo_path.replace_extension(".dwo");
                cmd.expected_sidecars.push_back(dwo_path.string());
                cmd.expected_sidecar_types.push_back(SidecarType::DWO);
            }
        } else if (arg.rfind("-g", 0) == 0) {
            // Generic debug flag (-g, -g3, etc.)
            cmd.flags.push_back(arg);
            cmd.args.push_back(arg);
            cmd.has_debug_info = true;
        } else if (arg.rfind("-std=", 0) == 0 || arg.rfind("-O", 0) == 0 || arg.rfind("-Wall", 0) == 0 || arg.rfind("-W", 0) == 0) {
            cmd.flags.push_back(arg);
            cmd.args.push_back(arg);
        } else if (std::filesystem::path(arg).extension() == ".cpp" || std::filesystem::path(arg).extension() == ".cc" || std::filesystem::path(arg).extension() == ".cxx" || std::filesystem::path(arg).extension() == ".c") {
            cmd.source = arg;
            cmd.args.push_back(arg);
        } else {
            cmd.args.push_back(arg);
        }
    }
    
    if (!is_compile || cmd.source.empty()) {
        return std::nullopt;
    }
    
    cmd.compiler_path = argv[0];
    auto info = detect_compiler(cmd.compiler_path);
    cmd.compiler_hash = info.hash;

    // Infer output file for MSVC if not specified (cl.exe creates .obj with same basename as source)
    if (info.type == CompilerType::MSVC && cmd.output.empty() && !cmd.source.empty()) {
        fs::path src_path(cmd.source);
        cmd.output = src_path.replace_extension(".obj");
    }

    // If debug info is enabled but no specific sidecar paths were set, infer them
    if (cmd.has_debug_info && cmd.expected_sidecars.empty()) {
        if (info.type == CompilerType::MSVC) {
            // Default MSVC PDB path: <output_dir>/vc<version>.pdb or <output_name>.pdb
            if (!cmd.output.empty()) {
                fs::path pdb_path = cmd.output;
                pdb_path.replace_extension(".pdb");
                cmd.expected_sidecars.push_back(pdb_path.string());
                cmd.expected_sidecar_types.push_back(SidecarType::PDB);
            }
        } else if (info.type == CompilerType::GCC || info.type == CompilerType::Clang) {
            // Default GCC/Clang: debug info in object file unless -gsplit-dwarf
            // For -g, debug info is embedded in .o, no separate sidecar
        }
    }
    
    return cmd;
}

std::vector<std::string> get_dependencies(char** argv, int argc) {
    std::vector<std::string> deps;
    return deps;
}

LinkerType detect_linker_type(const std::string& linker_path) {
    std::string name = std::filesystem::path(linker_path).filename().string();
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    
    if (name.find("link.exe") != std::string::npos || name.find("lld-link") != std::string::npos) {
        return LinkerType::MSVC;
    } else if (name.find("ld.lld") != std::string::npos || name.find("lld") != std::string::npos) {
        return LinkerType::LLVM_LLD;
    } else if (name.find("g++") != std::string::npos || name.find("clang++") != std::string::npos) {
        return LinkerType::GCC;
    }
    return LinkerType::Unknown;
}

std::optional<LinkerCommand> parse_linker_command(int argc, char* argv[]) {
    LinkerCommand cmd;
    cmd.linker_path = argv[0];
    cmd.linker_hash = get_compiler_hash(cmd.linker_path);
    cmd.type = detect_linker_type(cmd.linker_path);
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("/OUT:", 0) == 0 || arg == "-o" || arg.rfind("-o", 0) == 0) {
            if (arg == "-o" && i + 1 < argc) {
                cmd.output = argv[++i];
            } else if (arg.rfind("-o", 0) == 0) {
                cmd.output = arg.substr(2);
            } else {
                cmd.output = arg.substr(5);
            }
        } else if (arg.rfind("/LIBPATH:", 0) == 0 || arg.rfind("-L", 0) == 0) {
            cmd.library_dirs.push_back(arg);
        } else if (arg.rfind("/DEFAULTLIB:", 0) == 0 || arg.rfind("-l", 0) == 0) {
            cmd.libraries.push_back(arg);
        } else {
            std::filesystem::path p(arg);
            if (p.extension() == ".obj" || p.extension() == ".o" || p.extension() == ".lib" || p.extension() == ".a") {
                cmd.inputs.push_back(p);
            } else if (p.extension() == ".exe" || p.extension() == ".dll" || p.extension() == ".so") {
                if (cmd.output.empty()) cmd.output = p;
            }
            cmd.args.push_back(arg);
        }
    }
    
    if (cmd.output.empty()) return std::nullopt;
    return cmd;
}

CacheKey compute_linker_key(const LinkerCommand& cmd) {
    std::ostringstream oss;
    oss << "link|";
    
    // Normalize input paths
    std::vector<std::string> norm_inputs;
    norm_inputs.reserve(cmd.inputs.size());
    for (const auto& input : cmd.inputs) {
        norm_inputs.push_back(normalize_path_for_key(input));
    }
    std::sort(norm_inputs.begin(), norm_inputs.end());
    for (const auto& input : norm_inputs) {
        oss << input << "|";
    }
    
    // Normalize library paths
    std::vector<std::string> norm_libs = cmd.libraries;
    std::sort(norm_libs.begin(), norm_libs.end());
    for (const auto& lib : norm_libs) {
        oss << lib << "|";
    }
    
    // Normalize library dirs
    std::vector<std::string> norm_dirs;
    norm_dirs.reserve(cmd.library_dirs.size());
    for (const auto& dir : cmd.library_dirs) {
        norm_dirs.push_back(normalize_path_for_key(dir));
    }
    std::sort(norm_dirs.begin(), norm_dirs.end());
    for (const auto& dir : norm_dirs) {
        oss << dir << "|";
    }
    
    // Normalize output path
    std::string norm_output = normalize_path_for_key(cmd.output);
    oss << norm_output << "|";
    oss << cmd.linker_hash << "|";
    
    // Include relevant environment hash
    std::string env_hash = get_relevant_env_hash();
    oss << env_hash;
    
    CacheKey key;
    key.hash = compute_sha256(oss.str());
    key.toolchain_id = cmd.linker_hash;
    return key;
}

// Compute compiler hash from binary content + version string + mtime
std::string get_compiler_hash(const std::string& compiler_path) {
    std::filesystem::path path(compiler_path);
    if (!std::filesystem::exists(path)) {
        return std::to_string(std::hash<std::string>{}(compiler_path));
    }
    
    std::ostringstream oss;
    
    // Hash file size and mtime
    auto ftime = std::filesystem::last_write_time(path);
    auto file_size = std::filesystem::file_size(path);
    oss << file_size << "|" << ftime.time_since_epoch().count();
    
    // Get compiler version string
    std::string version_cmd;
    #ifdef _WIN32
    version_cmd = "\"" + compiler_path + "\" --version 2>&1";
    #else
    version_cmd = compiler_path + " --version 2>&1";
    #endif
    
    std::array<char, 128> buffer;
    std::string version_output;
    #ifdef _WIN32
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(version_cmd.c_str(), "r"), _pclose);
    #else
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(version_cmd.c_str(), "r"), pclose);
    #endif
    if (pipe) {
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            version_output += buffer.data();
        }
    }
    
    // Hash first few lines of version output (to avoid build timestamps)
    std::istringstream iss(version_output);
    std::string line;
    int line_count = 0;
    while (std::getline(iss, line) && line_count < 5) {
        if (!line.empty()) {
            oss << "|" << line;
            line_count++;
        }
    }
    
    return compute_sha256(oss.str());
}

std::string get_compiler_hash_with_version(const std::string& compiler_path) {
    return get_compiler_hash(compiler_path);
}

std::string get_linker_hash(const std::string& linker_path) {
    return get_compiler_hash(linker_path);
}

// Normalize path for cache key: strip build root prefix, normalize separators, case
std::string normalize_path_for_key(const std::filesystem::path& path, const std::filesystem::path& build_root) {
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path);
    std::string path_str = normalized.string();
    
    // Normalize path separators to forward slash
    std::replace(path_str.begin(), path_str.end(), '\\', '/');
    
    // On Windows, normalize case (lowercase)
    #ifdef _WIN32
    std::transform(path_str.begin(), path_str.end(), path_str.begin(), ::tolower);
    #endif
    
    // Strip build root prefix if provided
    if (!build_root.empty()) {
        std::filesystem::path root_canon = std::filesystem::weakly_canonical(build_root);
        std::string root_str = root_canon.string();
        std::replace(root_str.begin(), root_str.end(), '\\', '/');
        #ifdef _WIN32
        std::transform(root_str.begin(), root_str.end(), root_str.begin(), ::tolower);
        #endif
        
        if (path_str.rfind(root_str, 0) == 0) {  // starts with root_str
            if (path_str.size() == root_str.size() || path_str[root_str.size()] == '/') {
                path_str = path_str.substr(root_str.size());
                if (!path_str.empty() && path_str[0] == '/') {
                    path_str = path_str.substr(1);
                }
            }
        }
    }
    
    return path_str;
}

// Find build root (common ancestor of source files, or project root with CMakeLists.txt, etc.)
std::filesystem::path find_build_root(const std::filesystem::path& source_file) {
    std::filesystem::path current = std::filesystem::weakly_canonical(source_file).parent_path();
    
    while (!current.empty() && current != current.root_path()) {
        // Check for common build root markers
        if (std::filesystem::exists(current / "CMakeLists.txt") ||
            std::filesystem::exists(current / "meson.build") ||
            std::filesystem::exists(current / "Makefile") ||
            std::filesystem::exists(current / "build.ninja") ||
            std::filesystem::exists(current / ".git") ||
            std::filesystem::exists(current / "compile_commands.json")) {
            return current;
        }
        current = current.parent_path();
    }
    
    // Fallback: use parent directory of source
    return std::filesystem::weakly_canonical(source_file).parent_path();
}

// Get hash of relevant environment variables that affect compilation
std::string get_relevant_env_hash() {
    // Environment variables that affect compiler output
    static const std::vector<std::string> relevant_vars = {
        // Include/library paths
        "INCLUDE", "LIB", "LIBPATH", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH",
        "OBJC_INCLUDE_PATH", "LIBRARY_PATH", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH",
        // Compiler flags
        "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CL", "_CL_", "CC", "CXX",
        // MSVC specific
        "VCINSTALLDIR", "WINDOWSSDKDIR", "UNIVERSALCRTSDKDIR",
        // Preprocessor defines
        "DEFINES", "DEFINES_DEBUG", "DEFINES_RELEASE",
        // Language standards
        "CSTD", "CXXSTD",
        // Optimization/debug
        "OPTIMIZE", "DEBUG"
    };
    
    std::ostringstream oss;
    for (const auto& var : relevant_vars) {
        if (const char* val = std::getenv(var.c_str())) {
            oss << var << "=" << val << "\n";
        }
    }
    
    return compute_sha256(oss.str());
}

std::vector<std::string> get_relevant_env_vars() {
    static const std::vector<std::string> relevant_vars = {
        "INCLUDE", "LIB", "LIBPATH", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH",
        "OBJC_INCLUDE_PATH", "LIBRARY_PATH", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH",
        "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CL", "_CL_", "CC", "CXX",
        "VCINSTALLDIR", "WINDOWSSDKDIR", "UNIVERSALCRTSDKDIR",
        "DEFINES", "DEFINES_DEBUG", "DEFINES_RELEASE",
        "CSTD", "CXXSTD", "OPTIMIZE", "DEBUG"
    };
    return relevant_vars;
}

// Execute native compiler directly with full stdout/stderr passthrough
FallbackResult execute_native_compiler(const std::vector<std::string>& args, const std::string& compiler_path) {
    FallbackResult result;
    result.fallback_used = true;
    
    if (args.empty()) {
        result.error_message = "No compiler arguments provided";
        result.exit_code = -1;
        return result;
    }
    
    std::string cmd_to_run = compiler_path.empty() ? args[0] : compiler_path;
    std::vector<std::string> full_args = args;
    if (!compiler_path.empty()) {
        full_args[0] = compiler_path;
    }
    
    // Build command line with proper quoting
    std::string cmdline;
    for (size_t i = 0; i < full_args.size(); ++i) {
        if (i > 0) cmdline += " ";
        if (full_args[i].find(' ') != std::string::npos || full_args[i].find('"') != std::string::npos) {
            cmdline += "\"" + full_args[i] + "\"";
        } else {
            cmdline += full_args[i];
        }
    }
    
    #ifdef _WIN32
    // Use _popen for capturing output on Windows
    std::string capture_cmd = cmdline + " 2>&1";
    FILE* pipe = _popen(capture_cmd.c_str(), "r");
    if (!pipe) {
        result.error_message = "Failed to execute compiler: " + cmd_to_run;
        result.exit_code = -1;
        return result;
    }
    
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result.stdout_output += buffer;
    }
    
    int exit_code = _pclose(pipe);
    result.exit_code = exit_code;
    #else
    // Use popen for capturing output on Unix
    std::string capture_cmd = cmdline + " 2>&1";
    FILE* pipe = popen(capture_cmd.c_str(), "r");
    if (!pipe) {
        result.error_message = "Failed to execute compiler: " + cmd_to_run;
        result.exit_code = -1;
        return result;
    }
    
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result.stdout_output += buffer;
    }
    
    int exit_code = pclose(pipe);
    result.exit_code = WEXITSTATUS(exit_code);
    #endif
    
    // If we got stderr mixed in stdout, that's fine - passthrough
    if (result.exit_code != 0) {
        if (result.error_message.empty()) {
            result.error_message = "Compiler exited with code " + std::to_string(result.exit_code);
        }
    }
    
    return result;
}

// ============================================================================
// RemoteCache Implementation (WinHTTP-based, zero-dependency)
// ============================================================================

#if defined(_WIN32)

// RAII wrapper for WinHTTP handles
struct WinHttpHandle {
    HINTERNET handle = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) : handle(h) {}
    ~WinHttpHandle() { if (handle) WinHttpCloseHandle(handle); }
    WinHttpHandle(WinHttpHandle&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (handle) WinHttpCloseHandle(handle);
        handle = other.handle;
        other.handle = nullptr;
        return *this;
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    explicit operator bool() const { return handle != nullptr; }
    operator HINTERNET() const { return handle; }
};

// Helper to convert string to wide string
static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

// URL parsing helper
struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    bool is_ssl = false;
    int default_port = 80;
};

static std::optional<ParsedUrl> parse_url(const std::string& url) {
    ParsedUrl result;
    std::string u = url;
    
    size_t scheme_end = u.find("://");
    if (scheme_end == std::string::npos) return std::nullopt;
    result.scheme = u.substr(0, scheme_end);
    result.is_ssl = (result.scheme == "https");
    result.default_port = result.is_ssl ? 443 : 80;
    u = u.substr(scheme_end + 3);
    
    size_t path_start = u.find('/');
    std::string host_port = (path_start == std::string::npos) ? u : u.substr(0, path_start);
    result.path = (path_start == std::string::npos) ? "/" : u.substr(path_start);
    
    size_t colon = host_port.find(':');
    if (colon == std::string::npos) {
        result.host = host_port;
        result.port = std::to_string(result.default_port);
    } else {
        result.host = host_port.substr(0, colon);
        result.port = host_port.substr(colon + 1);
    }
    
    return result;
}

// Build the full object URL for a cache key
static std::string build_object_url(const RemoteCache::Config& config, const std::string& key_hash, const std::string& toolchain_id) {
    std::string base = config.url;
    if (!base.empty() && base.back() != '/') base += '/';
    std::string toolchain_prefix = toolchain_id.substr(0, 8);
    std::string hash_prefix1 = key_hash.substr(0, 2);
    std::string hash_prefix2 = key_hash.substr(2, 2);
    return base + toolchain_prefix + '/' + hash_prefix1 + '/' + hash_prefix2 + '/' + key_hash;
}

// Simple SHA256 using Windows CryptoAPI
static std::string compute_sha256_winhttp(const std::vector<uint8_t>& data) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string result;
    
    if (CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
            if (CryptHashData(hHash, data.data(), static_cast<DWORD>(data.size()), 0)) {
                DWORD hash_len = 32;
                std::vector<BYTE> hash(hash_len);
                if (CryptGetHashParam(hHash, HP_HASHVAL, hash.data(), &hash_len, 0)) {
                    std::ostringstream oss;
                    for (BYTE b : hash) {
                        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
                    }
                    result = oss.str();
                }
            }
            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
    }
    return result;
}

// WinHTTP client
struct HttpResponse {
    int status_code = 0;
    std::vector<uint8_t> body;
    std::string error;
};

class WinHttpClient {
    WinHttpHandle session_;
    std::wstring user_agent_ = L"remake/0.1.0";
    int connect_timeout_ms_ = 3000;
    int read_timeout_ms_ = 3000;
    bool verify_ssl_ = true;
    std::string auth_header_;
    
public:
    WinHttpClient(int connect_timeout_ms, int read_timeout_ms, bool verify_ssl, const std::string& auth_header)
        : connect_timeout_ms_(connect_timeout_ms), read_timeout_ms_(read_timeout_ms),
          verify_ssl_(verify_ssl), auth_header_(auth_header) {
        session_.handle = WinHttpOpen(user_agent_.c_str(), 
                                       WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                       WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS,
                                       0);
    }
    
    bool is_valid() const { return static_cast<bool>(session_); }
    
    void set_timeouts(int connect_ms, int read_ms) {
        connect_timeout_ms_ = connect_ms;
        read_timeout_ms_ = read_ms;
        if (session_) {
            WinHttpSetTimeouts(session_, 0, connect_timeout_ms_, read_timeout_ms_, 0);
        }
    }
    
    HttpResponse request(const ParsedUrl& url, const std::string& method,
                         const std::vector<uint8_t>* body = nullptr,
                         const std::string& content_type = "") {
        HttpResponse response;
        
        if (!session_) {
            response.error = "WinHTTP session not initialized";
            return response;
        }
        
        WinHttpHandle connect(WinHttpConnect(session_, to_wide(url.host).c_str(),
                                              static_cast<INTERNET_PORT>(std::stoi(url.port)), 0));
        if (!connect) {
            response.error = "WinHttpConnect failed: " + std::to_string(GetLastError());
            return response;
        }
        
        WinHttpSetTimeouts(session_, 0, connect_timeout_ms_, read_timeout_ms_, 0);
        
        DWORD flags = url.is_ssl ? WINHTTP_FLAG_SECURE : 0;
        WinHttpHandle request(WinHttpOpenRequest(connect, to_wide(method).c_str(),
                                                  to_wide(url.path).c_str(),
                                                  nullptr, WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  flags));
        if (!request) {
            response.error = "WinHttpOpenRequest failed: " + std::to_string(GetLastError());
            return response;
        }
        
        if (url.is_ssl && !verify_ssl_) {
#ifndef SECURITY_FLAG_IGNORE_REVOCATION
            #define SECURITY_FLAG_IGNORE_REVOCATION 0x00000080
#endif
            DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | 
                              SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                              SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                              SECURITY_FLAG_IGNORE_REVOCATION;
            WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &sec_flags, sizeof(sec_flags));
        }
        
        if (!auth_header_.empty()) {
            std::wstring wauth = to_wide(auth_header_);
            WinHttpAddRequestHeaders(request, wauth.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
        }
        
        if (!content_type.empty()) {
            std::wstring wct = to_wide("Content-Type: " + content_type);
            WinHttpAddRequestHeaders(request, wct.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
        
        const char* body_ptr = body ? reinterpret_cast<const char*>(body->data()) : nullptr;
        DWORD body_len = body ? static_cast<DWORD>(body->size()) : 0;
        
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                const_cast<char*>(body_ptr), body_len, body_len, 0)) {
            response.error = "WinHttpSendRequest failed: " + std::to_string(GetLastError());
            return response;
        }
        
        if (!WinHttpReceiveResponse(request, nullptr)) {
            response.error = "WinHttpReceiveResponse failed: " + std::to_string(GetLastError());
            return response;
        }
        
        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size, WINHTTP_NO_HEADER_INDEX);
        response.status_code = static_cast<int>(status_code);
        
        std::vector<uint8_t> buffer;
        DWORD bytes_available = 0;
        do {
            bytes_available = 0;
            if (!WinHttpQueryDataAvailable(request, &bytes_available)) break;
            if (bytes_available == 0) break;
            
            size_t offset = buffer.size();
            buffer.resize(offset + bytes_available);
            DWORD bytes_read = 0;
            if (!WinHttpReadData(request, buffer.data() + offset, bytes_available, &bytes_read)) {
                buffer.resize(offset);
                break;
            }
            if (bytes_read < bytes_available) {
                buffer.resize(offset + bytes_read);
            }
        } while (bytes_available > 0);
        
        response.body = std::move(buffer);
        return response;
    }
    
    bool head(const ParsedUrl& url, int& status_code, std::string& error) {
        auto response = request(url, "HEAD");
        status_code = response.status_code;
        error = response.error;
        return status_code >= 200 && status_code < 300;
    }
    
    HttpResponse get(const ParsedUrl& url) {
        return request(url, "GET");
    }
    
    HttpResponse put(const ParsedUrl& url, const std::vector<uint8_t>& body, const std::string& content_type) {
        return request(url, "PUT", &body, content_type);
    }
};

#endif  // _WIN32

#ifndef _WIN32
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>

// POSIX socket HTTP client for Linux/macOS
class PosixHttpClient {
    int socket_fd_ = -1;
    std::string user_agent_ = "remake/0.1.0";
    int connect_timeout_ms_ = 3000;
    int read_timeout_ms_ = 3000;
    bool verify_ssl_ = true;  // Not used for POSIX (no SSL support in raw sockets)
    std::string auth_header_;
    std::string host_;
    std::string port_;
    bool is_ssl_ = false;
    
    // Helper to set socket timeouts
    void set_socket_timeouts(int fd, int connect_ms, int read_ms) {
        struct timeval tv_connect, tv_read;
        tv_connect.tv_sec = connect_ms / 1000;
        tv_connect.tv_usec = (connect_ms % 1000) * 1000;
        tv_read.tv_sec = read_ms / 1000;
        tv_read.tv_usec = (read_ms % 1000) * 1000;
        
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv_connect, sizeof(tv_connect));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_read, sizeof(tv_read));
    }
    
    // Connect to host:port
    bool connect_to_host(const std::string& host, const std::string& port, bool is_ssl) {
        (void)is_ssl; // SSL not supported in raw POSIX sockets
        
        struct addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
            return false;
        }
        
        for (struct addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
            socket_fd_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (socket_fd_ == -1) continue;
            
            set_socket_timeouts(socket_fd_, connect_timeout_ms_, read_timeout_ms_);
            
            if (connect(socket_fd_, rp->ai_addr, rp->ai_addrlen) == 0) {
                freeaddrinfo(res);
                return true;
            }
            
            close(socket_fd_);
            socket_fd_ = -1;
        }
        
        freeaddrinfo(res);
        return false;
    }
    
    // Send all data
    bool send_all(const void* data, size_t len) {
        const char* ptr = static_cast<const char*>(data);
        while (len > 0) {
            ssize_t sent = send(socket_fd_, ptr, len, 0);
            if (sent <= 0) return false;
            ptr += sent;
            len -= sent;
        }
        return true;
    }
    
    // Receive data until connection closes or timeout
    std::vector<uint8_t> receive_all() {
        std::vector<uint8_t> buffer;
        char temp[4096];
        
        while (true) {
            ssize_t received = recv(socket_fd_, temp, sizeof(temp), 0);
            if (received <= 0) break;
            buffer.insert(buffer.end(), temp, temp + received);
        }
        return buffer;
    }
    
    // Parse HTTP response
    HttpResponse parse_response(const std::vector<uint8_t>& raw_response) {
        HttpResponse response;
        
        // Find end of headers
        std::string raw(raw_response.begin(), raw_response.end());
        size_t header_end = raw.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            response.error = "Invalid HTTP response: no header terminator";
            return response;
        }
        
        std::string headers = raw.substr(0, header_end);
        std::string body = raw.substr(header_end + 4);
        
        // Parse status line
        size_t status_start = headers.find("HTTP/");
        if (status_start == std::string::npos) {
            response.error = "Invalid HTTP response: no status line";
            return response;
        }
        
        size_t status_code_start = headers.find(' ', status_start);
        if (status_code_start != std::string::npos) {
            size_t status_code_end = headers.find(' ', status_code_start + 1);
            if (status_code_end != std::string::npos) {
                std::string status_str = headers.substr(status_code_start + 1, status_code_end - status_code_start - 1);
                try {
                    response.status_code = std::stoi(status_str);
                } catch (...) {
                    response.status_code = 0;
                }
            }
        }
        
        response.body.assign(body.begin(), body.end());
        return response;
    }
    
public:
    PosixHttpClient(int connect_timeout_ms, int read_timeout_ms, bool verify_ssl, const std::string& auth_header)
        : connect_timeout_ms_(connect_timeout_ms), read_timeout_ms_(read_timeout_ms),
          verify_ssl_(verify_ssl), auth_header_(auth_header) {}
    
    ~PosixHttpClient() {
        if (socket_fd_ != -1) {
            close(socket_fd_);
        }
    }
    
    bool is_valid() const { return true; }  // Always valid on POSIX
    
    void set_timeouts(int connect_ms, int read_ms) {
        connect_timeout_ms_ = connect_ms;
        read_timeout_ms_ = read_ms;
    }
    
    HttpResponse request(const ParsedUrl& url, const std::string& method,
                         const std::vector<uint8_t>* body = nullptr,
                         const std::string& content_type = "") {
        HttpResponse response;
        
        // Store host/port for reconnection
        host_ = url.host;
        port_ = url.port;
        is_ssl_ = url.is_ssl;
        
        // Connect to host
        if (!connect_to_host(url.host, url.port, url.is_ssl)) {
            response.error = "Failed to connect to " + url.host + ":" + url.port;
            return response;
        }
        
        // Build HTTP request
        std::ostringstream request_stream;
        request_stream << method << " " << url.path << " HTTP/1.1\r\n";
        request_stream << "Host: " << url.host;
        if (url.port != "80" && url.port != "443") {
            request_stream << ":" << url.port;
        }
        request_stream << "\r\n";
        request_stream << "User-Agent: " << user_agent_ << "\r\n";
        request_stream << "Connection: close\r\n";
        
        if (!auth_header_.empty()) {
            request_stream << auth_header_ << "\r\n";
        }
        
        if (!content_type.empty()) {
            request_stream << "Content-Type: " << content_type << "\r\n";
        }
        
        if (body && !body->empty()) {
            request_stream << "Content-Length: " << body->size() << "\r\n";
        }
        
        request_stream << "\r\n";
        
        std::string request_str = request_stream.str();
        
        // Send request
        if (!send_all(request_str.c_str(), request_str.size())) {
            close(socket_fd_);
            socket_fd_ = -1;
            response.error = "Failed to send request";
            return response;
        }
        
        // Send body if present
        if (body && !body->empty()) {
            if (!send_all(body->data(), body->size())) {
                close(socket_fd_);
                socket_fd_ = -1;
                response.error = "Failed to send body";
                return response;
            }
        }
        
        // Receive response
        std::vector<uint8_t> raw_response = receive_all();
        close(socket_fd_);
        socket_fd_ = -1;
        
        if (raw_response.empty()) {
            response.error = "Empty response from server";
            return response;
        }
        
        return parse_response(raw_response);
    }
    
    bool head(const ParsedUrl& url, int& status_code, std::string& error) {
        auto response = request(url, "HEAD");
        status_code = response.status_code;
        error = response.error;
        return status_code >= 200 && status_code < 300;
    }
    
    HttpResponse get(const ParsedUrl& url) {
        return request(url, "GET");
    }
    
    HttpResponse put(const ParsedUrl& url, const std::vector<uint8_t>& body, const std::string& content_type) {
        return request(url, "PUT", &body, content_type);
    }
};
#endif  // _WIN32

// RemoteCache::Impl implementation
struct RemoteCache::Impl {
    Config config_;
    fs::path local_cache_;
    mutable RemoteStats stats_;
    mutable std::mutex stats_mutex_;
    
#ifdef _WIN32
    std::unique_ptr<WinHttpClient> client_;
#else
    std::unique_ptr<PosixHttpClient> client_;
#endif
    std::optional<ParsedUrl> parsed_base_url_;
    
    Impl(Config config, fs::path local_cache)
        : config_(std::move(config)), local_cache_(std::move(local_cache)) {
#ifdef _WIN32
        client_ = std::make_unique<WinHttpClient>(
            config_.connect_timeout_ms,
            config_.read_timeout_ms,
            config_.verify_ssl,
            config_.auth_header
        );
#else
        client_ = std::make_unique<PosixHttpClient>(
            config_.connect_timeout_ms,
            config_.read_timeout_ms,
            config_.verify_ssl,
            config_.auth_header
        );
#endif
        
        if (client_ && client_->is_valid()) {
            parsed_base_url_ = parse_url(config_.url);
        }
    }
    
    bool is_configured() const {
        return client_ && client_->is_valid() && parsed_base_url_.has_value();
    }
    
    void increment_stat(int64_t& stat) const {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stat;
    }
    
    RemoteStats get_stats() const {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        return stats_;
    }
    
    template<typename Func>
    auto with_retries(Func&& func) -> decltype(func()) {
        using ResultType = decltype(func());
        std::string last_error;
        
        for (int attempt = 0; attempt <= config_.max_retries; ++attempt) {
            auto result = func();
            if (!result.error.empty()) {
                last_error = result.error;
                if (attempt < config_.max_retries) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));
                    continue;
                }
            }
            return result;
        }
        
        ResultType error_result;
        error_result.error = "Max retries exceeded: " + last_error;
        return error_result;
    }
};

RemoteCache::RemoteCache(Config config, fs::path local_cache)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(local_cache))) {}

RemoteCache::~RemoteCache() = default;

RemoteCache::RemoteCache(RemoteCache&& other) noexcept = default;
RemoteCache& RemoteCache::operator=(RemoteCache&& other) noexcept = default;

bool RemoteCache::has(const std::string& key_hash) const {
    if (!impl_->is_configured()) return false;
    
    std::string toolchain_id = "unknown";
    std::string object_url = build_object_url(impl_->config_, key_hash, toolchain_id) + "/meta.json";
    auto url_opt = parse_url(object_url);
    if (!url_opt) return false;
    
    int status_code = 0;
    std::string error;
    bool exists = false;
    
    for (int attempt = 0; attempt <= impl_->config_.max_retries; ++attempt) {
        exists = impl_->client_->head(*url_opt, status_code, error);
        if (error.empty()) {
            break;
        }
        if (attempt < impl_->config_.max_retries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));
        }
    }
    
    if (!error.empty()) {
        impl_->increment_stat(impl_->stats_.errors);
    }
    
    return exists;
}

RemoteCache::GetResult RemoteCache::get(const std::string& key_hash, const std::string& toolchain_id) const {
    GetResult result;
    
    if (!impl_->is_configured()) {
        result.error = "Remote cache not configured";
        return result;
    }
    
    std::string base_url = build_object_url(impl_->config_, key_hash, toolchain_id);
    auto base_url_opt = parse_url(base_url);
    if (!base_url_opt) {
        result.error = "Invalid URL";
        return result;
    }
    
    std::string meta_url = base_url + "/meta.json";
    auto meta_url_opt = parse_url(meta_url);
    if (!meta_url_opt) {
        result.error = "Invalid meta URL";
        return result;
    }
    
    auto meta_response = impl_->with_retries([&]() {
        return impl_->client_->get(*meta_url_opt);
    });
    
    if (!meta_response.error.empty() || meta_response.status_code < 200 || meta_response.status_code >= 300) {
        impl_->increment_stat(impl_->stats_.remote_misses);
        impl_->increment_stat(impl_->stats_.errors);
        result.error = "Meta not found: " + meta_response.error;
        return result;
    }
    
    std::string primary_name = key_hash + ".zst";
    std::string file_url = base_url + "/" + primary_name;
    auto file_url_opt = parse_url(file_url);
    if (!file_url_opt) {
        result.error = "Invalid file URL";
        return result;
    }
    
    auto file_response = impl_->with_retries([&]() {
        return impl_->client_->get(*file_url_opt);
    });
    
    if (!file_response.error.empty() || file_response.status_code < 200 || file_response.status_code >= 300) {
        impl_->increment_stat(impl_->stats_.remote_misses);
        result.error = "Primary artifact not found: " + file_response.error;
        return result;
    }
    
    result.primary_data = std::move(file_response.body);
    
    std::vector<std::string> sidecar_names = {".pdb", ".idb", ".dwo"};
    for (const auto& ext : sidecar_names) {
        std::string name = key_hash + ext + ".zst";
        std::string sc_url = base_url + "/" + name;
        auto sc_url_opt = parse_url(sc_url);
        if (!sc_url_opt) continue;
        
        auto sc_response = impl_->with_retries([&]() {
            return impl_->client_->get(*sc_url_opt);
        });
        
        if (!sc_response.error.empty() || sc_response.status_code < 200 || sc_response.status_code >= 300) {
            continue;
        }
        
        result.sidecars.emplace_back(name, std::move(sc_response.body));
    }
    
    impl_->increment_stat(impl_->stats_.remote_hits);
    impl_->increment_stat(impl_->stats_.downloads);
    result.success = true;
    return result;
}

bool RemoteCache::put(const std::string& key_hash, const std::string& toolchain_id,
                      const std::vector<uint8_t>& primary_data,
                      const std::vector<std::pair<std::string, std::vector<uint8_t>>>& sidecars) {
    if (!impl_->is_configured() || impl_->config_.read_only) return false;
    
    std::string base_url = build_object_url(impl_->config_, key_hash, toolchain_id);
    auto base_url_opt = parse_url(base_url);
    if (!base_url_opt) return false;
    
    bool any_uploaded = false;
    
    std::string primary_name = key_hash + ".zst";
    std::string file_url = base_url + "/" + primary_name;
    auto file_url_opt = parse_url(file_url);
    if (!file_url_opt) return false;
    
    auto put_response = impl_->with_retries([&]() {
        return impl_->client_->put(*file_url_opt, primary_data, "application/octet-stream");
    });
    
    if (!put_response.error.empty() || put_response.status_code < 200 || put_response.status_code >= 300) {
        impl_->increment_stat(impl_->stats_.errors);
    } else {
        any_uploaded = true;
    }
    
    for (const auto& [name, data] : sidecars) {
        std::string sc_url = base_url + "/" + name;
        auto sc_url_opt = parse_url(sc_url);
        if (!sc_url_opt) continue;
        
        auto sc_response = impl_->with_retries([&]() {
            return impl_->client_->put(*sc_url_opt, data, "application/octet-stream");
        });
        
        if (!sc_response.error.empty() || sc_response.status_code < 200 || sc_response.status_code >= 300) {
            impl_->increment_stat(impl_->stats_.errors);
        } else {
            any_uploaded = true;
        }
    }
    
    if (any_uploaded) {
        std::string meta_json = "{\"toolchain\":\"" + toolchain_id + "\",\"ts\":" + std::to_string(std::time(nullptr)) + ",\"compressed\":true}";
        std::vector<uint8_t> meta_data(meta_json.begin(), meta_json.end());
        
        std::string meta_url = base_url + "/meta.json";
        auto meta_url_opt = parse_url(meta_url);
        if (meta_url_opt) {
            auto meta_response = impl_->with_retries([&]() {
                return impl_->client_->put(*meta_url_opt, meta_data, "application/json");
            });
            
            if (!meta_response.error.empty() || meta_response.status_code < 200 || meta_response.status_code >= 300) {
                impl_->increment_stat(impl_->stats_.errors);
            }
        }
        
        impl_->increment_stat(impl_->stats_.uploads);
    }
    
    return any_uploaded;
}

RemoteCache::RemoteStats RemoteCache::get_stats() const {
    return impl_->get_stats();
}

RemoteCache::Config RemoteCache::from_env() {
    Config config;
    
    if (const char* env = std::getenv("BUILDCACHE_REMOTE_URL")) {
        config.url = env;
    }
    
    if (const char* env = std::getenv("BUILDCACHE_REMOTE_READ_ONLY")) {
        std::string v = env;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        config.read_only = (v == "1" || v == "true" || v == "yes" || v == "on");
    }
    
    if (const char* env = std::getenv("BUILDCACHE_REMOTE_CONNECT_TIMEOUT")) {
        try { config.connect_timeout_ms = std::stoi(env); } catch (...) {}
    }
    
    if (const char* env = std::getenv("BUILDCACHE_REMOTE_READ_TIMEOUT")) {
        try { config.read_timeout_ms = std::stoi(env); } catch (...) {}
    }
    
    if (const char* env = std::getenv("BUILDCACHE_REMOTE_MAX_RETRIES")) {
        try { config.max_retries = std::stoi(env); } catch (...) {}
    }
    
    if (const char* env = std::getenv("BUILDCACHE_REMOTE_AUTH_HEADER")) {
        config.auth_header = env;
    }
    
    if (const char* env = std::getenv("BUILDCACHE_REMOTE_VERIFY_SSL")) {
        std::string v = env;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        config.verify_ssl = (v == "1" || v == "true" || v == "yes" || v == "on");
    }
    
    return config;
}

bool RemoteCache::test_connection() const {
    if (!impl_->is_configured()) return false;
    
    if (!impl_->parsed_base_url_) return false;
    
    int status_code = 0;
    std::string error;
    return impl_->client_->head(*impl_->parsed_base_url_, status_code, error);
}

} // namespace remake