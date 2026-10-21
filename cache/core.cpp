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
#include <random>
#include <cctype>

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
namespace skipbuild {

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
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!data.empty()) {
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    f.flush();
    bool ok = f.good();
    f.close();
    if (!ok || f.fail()) {
        std::error_code ec2;
        fs::remove(path, ec2);
        return false;
    }
    return true;
}

// Reduce a remote-supplied artifact name to a bare filename so that a hostile
// or compromised remote (or a MITM on plaintext HTTP) cannot write outside the
// cache entry directory via "../" or absolute paths.
static std::optional<std::string> sanitize_remote_name(const std::string& name) {
    if (name.empty()) return std::nullopt;
    fs::path p(name);
    if (p.has_root_name() || p.has_root_directory()) return std::nullopt;
    std::string fname = p.filename().string();
    if (fname.empty() || fname == "." || fname == "..") return std::nullopt;
    // Reject any remaining drive/colon or separator characters.
    if (fname.find(':') != std::string::npos ||
        fname.find('/') != std::string::npos ||
        fname.find('\\') != std::string::npos) {
        return std::nullopt;
    }
    return fname;
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

// Streaming ZSTD decompression with a hard output cap. Never trusts the
// frame header's declared content size (an attacker-controlled frame could
// claim an enormous size and exhaust memory), and bounds the total output to
// guard against decompression bombs.
static bool decompress_zstd_bounded(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
#if REMAKE_HAVE_ZSTD
    static constexpr size_t kMaxOutputBytes = static_cast<size_t>(4) * 1024 * 1024 * 1024; // 4 GiB
    if (input.empty()) return false;

    ZSTD_DStream* ds = ZSTD_createDStream();
    if (!ds) return false;
    ZSTD_initDStream(ds);

    output.clear();
    std::vector<char> outbuf(ZSTD_DStreamOutSize());
    ZSTD_inBuffer in{ input.data(), input.size(), 0 };

    bool ok = false;
    while (true) {
        ZSTD_outBuffer out{ outbuf.data(), outbuf.size(), 0 };
        size_t ret = ZSTD_decompressStream(ds, &out, &in);
        if (ZSTD_isError(ret)) break;
        if (output.size() + out.pos > kMaxOutputBytes) break;
        output.insert(output.end(), outbuf.data(), outbuf.data() + out.pos);
        if (ret == 0) { ok = true; break; }                        // frame fully decoded
        // `ret != 0` means the decoder still needs input for the current frame.
        // Running out of input here is a truncated frame, not a complete one:
        // accepting it would restore partially written artifacts.
        if (in.pos >= in.size) break;
    }

    ZSTD_freeDStream(ds);
    return ok;
#else
    output = input;
    return true;
#endif
}

static bool decompress_data(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    return decompress_zstd_bounded(input, output);
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
        DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;  // allow delete while locked
        
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
        DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        
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
            
            // Use non-blocking flock with retry loop to avoid indefinite blocking
            // on filesystems where flock() may block indefinitely (e.g., macOS tmpfs)
            int flock_op = (type == FileLock::LockType::Shared) ? (LOCK_SH | LOCK_NB) : (LOCK_EX | LOCK_NB);
            
            while (true) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed >= timeout_ms) {
                    close(fd_);
                    fd_ = -1;
                    return false;
                }
                
                if (flock(fd_, flock_op) == 0) {
                    locked_ = true;
                    return true;
                }
                
                int err = errno;
                if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK || err == EDEADLK) {
                    int sleep_ms = (50 < 10 * (attempt + 1)) ? 50 : 10 * (attempt + 1);
                    if (elapsed + sleep_ms > timeout_ms) {
                        close(fd_);
                        fd_ = -1;
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                    attempt++;
                    continue;
                }
                
                close(fd_);
                fd_ = -1;
                return false;
            }
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
    return decompress_zstd_bounded(input, output);
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
    
    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!compressed.empty()) {
        out.write(reinterpret_cast<const char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
    }
    out.flush();
    bool ok = out.good();
    out.close();
    if (!ok || out.fail()) {
        std::error_code ec;
        std::filesystem::remove(output_path, ec);
        return false;
    }
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
    
    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!decompressed.empty()) {
        out.write(reinterpret_cast<const char*>(decompressed.data()), static_cast<std::streamsize>(decompressed.size()));
    }
    out.flush();
    bool ok = out.good();
    out.close();
    if (!ok || out.fail()) {
        std::error_code ec;
        std::filesystem::remove(output_path, ec);
        return false;
    }
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
    bool ok = meta.good();
    meta.close();
    if (!ok || meta.fail()) {
        std::error_code ec;
        std::filesystem::remove(meta_temp, ec);
        return false;
    }
    
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
    
    // Normalize include paths. Include order determines header resolution, so the
    // command-line order is preserved (do NOT sort).
    std::vector<std::string> norm_includes;
    norm_includes.reserve(includes.size());
    for (const auto& inc : includes) {
        norm_includes.push_back(normalize_path_for_key(inc, build_root));
    }

    // Preserve flag order: -D/-U (and similar) are order sensitive, e.g.
    // "-DFOO -UFOO" does not behave like "-UFOO -DFOO". Do NOT sort.
    std::vector<std::string> norm_flags = flags;
    
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

    // Preserve order (include search order and -D/-U order are significant).
    std::vector<std::string> norm_flags = cmd.flags;
    
    auto base_key = compute_key(cmd.source, norm_includes, norm_flags, cmd.compiler_hash, 
                                detect_compiler(cmd.compiler_path).type);
    
    std::ostringstream extra;
    extra << base_key.hash;
    
    // Header dependencies: fold the *content* of every transitively-included
    // file into the key so that editing any header (including nested includes)
    // invalidates the entry. Only content hashes are used (not paths) so the
    // key stays machine-independent across different SDK/install locations.
    if (!cmd.dependencies.empty()) {
        std::vector<std::string> dep_hashes;
        dep_hashes.reserve(cmd.dependencies.size());
        for (const auto& dep : cmd.dependencies) {
            std::string h = compute_file_hash(dep);
            if (!h.empty()) dep_hashes.push_back(h);
        }
        std::sort(dep_hashes.begin(), dep_hashes.end());
        dep_hashes.erase(std::unique(dep_hashes.begin(), dep_hashes.end()), dep_hashes.end());
        for (const auto& h : dep_hashes) {
            extra << "|dep:" << h;
        }
    }
    
    // Include PCH source content hash in the key if using PCH
    if (cmd.pch.mode != PCHMode::None && !cmd.pch.pch_source.empty() && fs::exists(cmd.pch.pch_source)) {
        std::string pch_hash = compute_file_hash(cmd.pch.pch_source);
        if (!pch_hash.empty()) {
            extra << "|pch:" << pch_hash;
        }
    }
    
    CacheKey key;
    key.hash = compute_sha256(extra.str());
    key.toolchain_id = base_key.toolchain_id;
    return key;
}

// Generate unique temporary file path
static std::filesystem::path make_temp_path(const std::filesystem::path& base_dir, const std::string& prefix) {
    std::ostringstream oss;
    oss << prefix << "_" << std::this_thread::get_id() << "_" << std::chrono::steady_clock::now().time_since_epoch().count() << ".tmp";
    return base_dir / oss.str();
}

bool Cache::put(const CacheKey& key, const std::vector<std::filesystem::path>& outputs,
                const std::vector<std::pair<std::filesystem::path, SidecarType>>& sidecars) {
    // Read-only caches may still be read, but must never create or modify
    // entries (local or remote).
    if (config_.read_only) return false;

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
            // A zero-byte object file means the compile failed (or produced
            // nothing); caching it would poison the key, so skip it.
            if (std::filesystem::file_size(output, ec) == 0) continue;
            
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
            // A zero-byte object file means the compile failed (or produced
            // nothing); caching it would poison the key, so skip it.
            if (std::filesystem::file_size(output, ec) == 0) continue;
            
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
    
    // Update cache-wide stats after releasing all locks. Previously this stored
    // the literal count 1 and just this entry's size, so the reported entry count
    // was always 1 and the reported size only ever reflected the last entry.
    // Recompute the totals from the actual on-disk entries instead.
    if (success) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto entries = collect_entries();
        int64_t usage = 0;
        for (const auto& e : entries) usage += e.size_bytes;

        // Auto-clear: once the cache reaches auto_clear_ratio of the configured
        // limit, purge everything (free the space back to zero) rather than
        // waiting for the periodic prune. <= 0 disables the behaviour.
        if (config_.max_cache_size_bytes > 0 && config_.auto_clear_ratio > 0.0) {
            int64_t trigger = static_cast<int64_t>(config_.max_cache_size_bytes * config_.auto_clear_ratio);
            if (usage >= trigger) {
                for (const auto& e : entries) {
                    purge_corrupted_entry(e);
                }
                entries.clear();
                usage = 0;
            }
        }

        stats_tracker_->update_cache_size(static_cast<int64_t>(entries.size()), usage, max_cache_size);
    }

    return success;
}

namespace { std::filesystem::path make_stage_dir(const std::filesystem::path& fallback_root); }

std::optional<CacheEntry> Cache::safe_get(const CacheKey& key,
    const std::filesystem::path& expected_output) const {
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
            if (!remote_result.success || remote_result.artifacts.empty()) return std::nullopt;

            // Pick the primary output name (first non-sidecar artifact). New
            // entries carry the real filename; legacy entries store it only by
            // hash and fall back to the caller's expected output.
            std::string primary_name;
            for (const auto& artifact : remote_result.artifacts) {
                if (!artifact.is_sidecar && !artifact.name.empty()) {
                    primary_name = artifact.name;
                    break;
                }
            }
            if (primary_name.empty()) primary_name = expected_output.filename().string();
            if (primary_name.empty()) primary_name = key.hash + ".obj";
            auto safe_primary = sanitize_remote_name(primary_name);
            if (!safe_primary) return std::nullopt;
            primary_name = *safe_primary;
            const std::string primary_stem = fs::path(primary_name).stem().string();

            fs::path key_dir = root_ / key.hash.substr(0, 2) / key.hash.substr(2, 2) / key.hash;
            std::error_code ec;
            fs::create_directories(key_dir, ec);
            if (ec) return std::nullopt;

            // Install into the local cache under the entry's exclusive lock so
            // concurrent readers see either no entry or the complete one.
            FileLock file_lock(key_dir / ".lock");
            FileLock::ScopedLock scoped_lock(file_lock, FileLock::LockType::Exclusive);
            if (!scoped_lock) return std::nullopt;

            // Another process may have installed the entry while we downloaded.
            if (fs::exists(key_dir / "meta.json")) {
                scoped_lock.release();
                return get(key);
            }

            fs::path stage_dir = make_stage_dir(root_);
            if (stage_dir.empty()) return std::nullopt;
            auto drop_stage = [&stage_dir]() {
                std::error_code sec;
                fs::remove_all(stage_dir, sec);
            };

            CacheEntry entry;
            entry.key = key;
            entry.timestamp = std::time(nullptr);
            entry.size_bytes = 0;
            std::vector<std::pair<fs::path, SidecarType>> stored_sidecars;
            std::vector<fs::path> staged_files;

            for (const auto& artifact : remote_result.artifacts) {
                std::string name = artifact.name;
                if (name.empty()) name = primary_name;  // legacy primary
                // Legacy sidecars are named "<hash>.pdb"; rewrite them to the
                // real "<output stem>.pdb".
                if (artifact.is_sidecar && name.rfind(key.hash, 0) == 0) {
                    auto dot = name.rfind('.');
                    if (dot != std::string::npos) name = primary_stem + name.substr(dot);
                }
                auto safe_name = sanitize_remote_name(name);
                if (!safe_name) continue;

                std::vector<uint8_t> decompressed;
                if (!decompress_data(artifact.data, decompressed)) {
                    drop_stage();
                    return std::nullopt;
                }

                fs::path staged = stage_dir / *safe_name;
                if (!write_file(staged, decompressed)) {
                    drop_stage();
                    return std::nullopt;
                }

                // Persist the (already compressed) blob under its canonical name.
                #if REMAKE_HAVE_ZSTD
                fs::path dest = key_dir / (*safe_name + ".zst");
                if (!write_file(dest, artifact.data)) {
                    drop_stage();
                    return std::nullopt;
                }
                #else
                fs::path dest = key_dir / *safe_name;
                if (!write_file(dest, decompressed)) {
                    drop_stage();
                    return std::nullopt;
                }
                #endif

                entry.size_bytes += static_cast<int64_t>(decompressed.size());
                staged_files.push_back(staged);

                if (artifact.is_sidecar) {
                    SidecarInfo si;
                    si.relative_path = staged;
                    si.hash = compute_sha256(decompressed);
                    std::string ext = fs::path(*safe_name).extension().string();
                    si.type = (ext == ".pdb") ? SidecarType::PDB
                            : (ext == ".idb") ? SidecarType::IDB
                            : (ext == ".dwo") ? SidecarType::DWO
                            : SidecarType::Other;
                    entry.sidecars.push_back(si);
                    stored_sidecars.emplace_back(dest, si.type);
                } else {
                    entry.outputs.push_back(staged);
                }
            }

            if (entry.outputs.empty()) {
                drop_stage();
                return std::nullopt;
            }

            // Write meta.json atomically so later lookups hit the local cache
            // and collectors/pruners see a complete entry.
            fs::path meta_file = key_dir / "meta.json";
            fs::path meta_temp = make_temp_path(key_dir, "meta.json");
            if (!write_meta_json(meta_file, meta_temp, key.toolchain_id, entry.size_bytes, stored_sidecars)) {
                drop_stage();
                return std::nullopt;
            }

            entry.transient_files = std::move(staged_files);
            entry.transient_files.push_back(stage_dir);
            return entry;
        } catch (const std::exception& e) {
            std::cerr << "[build-cache warning] Remote cache read failed: " << e.what() << ". Falling back to native compilation.\n";
        } catch (...) {
            std::cerr << "[build-cache warning] Remote cache read failed: unknown error. Falling back to native compilation.\n";
        }
    }
    
    // NOTE: Cache::get() above already recorded the hit/miss; do not record a
    // second miss here or a single lookup is counted twice.
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
    if (remote_cache_ && !config_.read_only && !config_.remote_read_only) {
        try {
            std::vector<RemoteCache::Artifact> artifacts;

            // Every output (object, PCH, DLL, import library, ...)
            for (const auto& output : outputs) {
                if (!fs::exists(output)) continue;
                std::error_code ec;
                if (fs::file_size(output, ec) == 0) continue;
                std::vector<uint8_t> file_data;
                if (!read_file(output, file_data)) continue;

                std::vector<uint8_t> compressed;
                if (!compress_data(file_data, compressed)) continue;

                RemoteCache::Artifact artifact;
                artifact.name = output.filename().string();
                artifact.is_sidecar = false;
                artifact.data = std::move(compressed);
                artifacts.push_back(std::move(artifact));
            }

            // Debug sidecars travel as named artifacts too, so their original
            // names survive the round trip.
            for (const auto& [path, type] : sidecars) {
                (void)type;
                if (!fs::exists(path)) continue;
                std::vector<uint8_t> file_data;
                if (!read_file(path, file_data)) continue;

                std::vector<uint8_t> compressed;
                if (!compress_data(file_data, compressed)) continue;

                RemoteCache::Artifact artifact;
                artifact.name = path.filename().string();
                artifact.is_sidecar = true;
                artifact.data = std::move(compressed);
                artifacts.push_back(std::move(artifact));
            }

            if (!artifacts.empty()) {
                remote_cache_->put(key.hash, key.toolchain_id, artifacts);
            }
        } catch (const std::exception& e) {
            std::cerr << "[build-cache warning] Remote cache write failed: " << e.what() << ". Build output preserved locally.\n";
        } catch (...) {
            std::cerr << "[build-cache warning] Remote cache write failed: unknown error. Build output preserved locally.\n";
        }
    }
    
    return local_ok;
}

namespace {

// Create a private, per-lookup staging directory. Decompressed artifacts are
// materialised here - never inside the shared entry directory - so that
// concurrent cache readers/writers cannot overwrite each other's copy and so
// that release() only ever deletes files this process created.
std::filesystem::path make_stage_dir(const std::filesystem::path& fallback_root) {
    static std::atomic<uint64_t> counter{0};
    // Per-process random salt keeps directories unique across processes without
    // relying on platform-specific process-id APIs.
    static const uint64_t salt = [] {
        std::random_device rd;
        return (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
    }();
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec || base.empty()) base = fallback_root;
    std::ostringstream name;
    name << "skipbuild-stage-" << std::hex << salt << "-"
         << counter.fetch_add(1, std::memory_order_relaxed) << "-"
         << std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path dir = base / name.str();
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        ec.clear();
        std::filesystem::remove_all(dir, ec);
        return {};
    }
    return dir;
}

} // namespace

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
                    // Corrupted meta.json - purge and return nullopt. Drop the
                    // shared lock first so the exclusive acquire below cannot
                    // deadlock against our own lock.
                    scoped_lock.release();
                    FileLock file_lock_excl(lock_file);
                    FileLock::ScopedLock excl_lock(file_lock_excl, FileLock::LockType::Exclusive);
                    if (excl_lock) {
                        std::filesystem::remove_all(key_dir, ec);
                    }
                } else {
                    CacheEntry entry;
                    entry.key = key;
                    entry.timestamp = std::time(nullptr);
                    entry.size_bytes = 0;

                    bool has_corrupted = false;
                    std::filesystem::path stage_dir;  // private per-lookup staging dir

                    for (auto& f : std::filesystem::directory_iterator(key_dir, ec)) {
                        if (f.is_directory(ec)) continue;
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
                                // Materialise the artifact into a private staging
                                // directory rather than the shared entry directory.
                                // Writing a fixed path under a shared lock let two
                                // processes clobber each other's copy, and let one
                                // process's release() delete a copy another was
                                // still using. A per-lookup directory removes both
                                // hazards.
                                if (stage_dir.empty()) {
                                    stage_dir = make_stage_dir(root_);
                                    if (stage_dir.empty()) {
                                        has_corrupted = true;
                                        break;
                                    }
                                }
                                std::filesystem::path dest_path = stage_dir / f.path().stem();
                                if (!decompress_file(f.path(), dest_path)) {
                                    has_corrupted = true;
                                    break;
                                }
                                // Record it so the caller can delete the
                                // (regenerable) decompressed copy after use.
                                entry.transient_files.push_back(dest_path);
                                
                                // Check if it's a sidecar file
                                std::string stem_ext = dest_path.extension().string();
                                if (stem_ext == ".pdb" || stem_ext == ".idb" || stem_ext == ".dwo") {
                                    SidecarInfo si;
                                    si.relative_path = dest_path;
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
                                    si.relative_path = f.path();
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
                        // Nothing usable: drop the private staging directory and
                        // purge the corrupt entry under an exclusive lock. Release
                        // the shared lock first so the exclusive acquire cannot
                        // deadlock against our own lock.
                        if (!stage_dir.empty()) {
                            std::error_code sec;
                            std::filesystem::remove_all(stage_dir, sec);
                        }
                        scoped_lock.release();
                        FileLock file_lock_excl(lock_file);
                        FileLock::ScopedLock excl_lock(file_lock_excl, FileLock::LockType::Exclusive);
                        if (excl_lock) {
                            std::filesystem::remove_all(key_dir, ec);
                        }
                    } else {
                        if (!stage_dir.empty()) {
                            // release() removes this directory once the caller has
                            // copied the outputs out.
                            entry.transient_files.push_back(stage_dir);
                        }
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
        std::filesystem::last_write_time(meta_file, std::filesystem::file_time_type::clock::now(), ec);
    }
}

void Cache::release(const CacheEntry& entry) const {
    // transient_files only ever holds artifacts this process materialised in its
    // private staging directory (plus that directory itself), so removing them
    // unconditionally is safe and cannot affect another reader or writer.
    for (const auto& path : entry.transient_files) {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
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
                        auto name = f.path().filename();
                        if (name == "meta.json" || name == ".lock") continue;
                        // Skip a decompressed copy that duplicates a compressed
                        // blob already in this entry, so the on-disk accounting
                        // reflects the stored (compressed) size.
                        if (f.path().extension() != ".zst") {
                            std::filesystem::path twin = f.path();
                            twin += ".zst";
                            if (std::filesystem::exists(twin)) continue;
                        }
                        meta.size_bytes += std::filesystem::file_size(f.path(), ec);
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

// Parse response file (@file.rsp) - handles MSVC/Ninja/CMake format
// Returns expanded arguments, or empty vector on error
// depth: recursion depth for nested @file includes (max 10)
static std::vector<std::string> parse_response_file(const std::filesystem::path& rsp_path, int depth = 0) {
    constexpr int MAX_RECURSION_DEPTH = 10;
    std::vector<std::string> expanded_args;
    
    if (depth > MAX_RECURSION_DEPTH) {
        throw std::runtime_error("Response file recursion depth exceeded (max " + std::to_string(MAX_RECURSION_DEPTH) + ")");
    }
    
    try {
        std::ifstream file(rsp_path, std::ios::binary);
        if (!file) return expanded_args;
        
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (content.empty()) return expanded_args;
        
        // Limit content size to prevent issues
        if (content.size() > 1024 * 1024) {
            content = content.substr(0, 1024 * 1024);
        }
        
        // Strip UTF-8 BOM if present (0xEF, 0xBB, 0xBF)
        if (content.size() >= 3 &&
            static_cast<unsigned char>(content[0]) == 0xEF &&
            static_cast<unsigned char>(content[1]) == 0xBB &&
            static_cast<unsigned char>(content[2]) == 0xBF) {
            content = content.substr(3);
        }
        
        // Normalize line endings: replace \r\n and \r with \n
        // Then split by \n to process line by line, stripping trailing \r from each line
        std::string normalized;
        normalized.reserve(content.size());
        for (size_t i = 0; i < content.size(); ++i) {
            char c = content[i];
            if (c == '\r') {
                if (i + 1 < content.size() && content[i + 1] == '\n') {
                    // CRLF -> skip \r, let \n be processed next iteration
                    continue;
                }
                // Standalone \r -> treat as \n
                normalized += '\n';
            } else {
                normalized += c;
            }
        }
        
        std::string current_arg;
        bool in_quotes = false;
        bool escape_next = false;
        
        for (size_t i = 0; i < normalized.size(); ++i) {
            // Strict bounds check
            if (i >= normalized.size()) break;
            
            char c = normalized[i];
            
            if (escape_next) {
                current_arg += c;
                escape_next = false;
                continue;
            }
            
            if (c == '\\') {
                // Check if next char is quote (escaped quote) - strict bounds check
                if (i + 1 < normalized.size() && normalized[i + 1] == '"') {
                    escape_next = true;
                    continue;
                }
                current_arg += c;
                continue;
            }
            
            if (c == '"') {
                in_quotes = !in_quotes;
                continue;
            }
            
            if (!in_quotes && (c == ' ' || c == '\t' || c == '\n')) {
                if (!current_arg.empty()) {
                    // Strip any trailing \r that might remain (defensive)
                    while (!current_arg.empty() && current_arg.back() == '\r') {
                        current_arg.pop_back();
                    }
                    if (!current_arg.empty()) {
                        expanded_args.push_back(current_arg);
                    }
                    current_arg.clear();
                }
                continue;
            }
            
            current_arg += c;
        }
        
        if (!current_arg.empty()) {
            // Strip any trailing \r from final token
            while (!current_arg.empty() && current_arg.back() == '\r') {
                current_arg.pop_back();
            }
            if (!current_arg.empty()) {
                expanded_args.push_back(current_arg);
            }
        }
    } catch (const std::exception&) {
        // Re-throw std::exception (including our recursion depth error)
        throw;
    } catch (...) {
        // Return empty on any other error
        return {};
    }
    
    return expanded_args;
}

// Expand response files in argv recursively
static std::vector<std::string> expand_response_files(int argc, char* argv[]) {
    std::vector<std::string> expanded;
    expanded.reserve(argc * 2);
    
    // Add compiler path (argv[0])
    expanded.push_back(argv[0]);
    
    struct PendingArg {
        std::string arg;
        int depth;
    };
    
    std::vector<PendingArg> to_process;
    for (int i = 1; i < argc; ++i) {
        to_process.push_back({argv[i], 0});
    }
    
    constexpr int MAX_RECURSION_DEPTH = 10;
    
    while (!to_process.empty()) {
        PendingArg current = to_process.front();
        to_process.erase(to_process.begin());
        
        std::string arg = current.arg;
        int depth = current.depth;
        
        if (arg.size() >= 2 && arg[0] == '@') {
            // Response file argument
            try {
                std::filesystem::path rsp_path = arg.substr(1);
                if (std::filesystem::exists(rsp_path)) {
                    // Check depth before parsing
                    if (depth >= MAX_RECURSION_DEPTH) {
                        throw std::runtime_error("Response file recursion depth exceeded (max " + std::to_string(MAX_RECURSION_DEPTH) + ")");
                    }
                    auto rsp_args = parse_response_file(rsp_path, depth);
                    // Prepend to to_process for recursive expansion with incremented depth
                    for (auto it = rsp_args.rbegin(); it != rsp_args.rend(); ++it) {
                        to_process.insert(to_process.begin(), {*it, depth + 1});
                    }
                } else {
                    // File doesn't exist, pass through as-is
                    expanded.push_back(arg);
                }
            } catch (const std::exception&) {
                // Re-throw std::exception (including our recursion depth error)
                throw;
            } catch (...) {
                // Path error or filesystem error, pass through as-is
                expanded.push_back(arg);
            }
        } else {
            expanded.push_back(arg);
        }
    }
    
    return expanded;
}

std::optional<CompileCommand> parse_compile_command(int argc, char* argv[]) {
    // Expand response files first
    std::vector<std::string> expanded_argv;
    try {
        expanded_argv = expand_response_files(argc, argv);
    } catch (const std::exception&) {
        // Response file parsing error (e.g., recursion depth exceeded, malformed file)
        return std::nullopt;
    } catch (...) {
        // Unknown error during response file expansion
        return std::nullopt;
    }
    int expanded_argc = static_cast<int>(expanded_argv.size());
    
    CompileCommand cmd;
    bool is_compile = false;
    
    for (int i = 1; i < expanded_argc; ++i) {
        std::string arg = expanded_argv[i];
        if (arg == "-c" || arg == "/c") {
            is_compile = true;
            cmd.args.push_back(arg);
        } else if ((arg == "-o" || arg == "/Fo") && i + 1 < expanded_argc) {
            cmd.output = expanded_argv[++i];
            cmd.args.push_back(arg);
            cmd.args.push_back(expanded_argv[i]);
        } else if ((arg.rfind("-I", 0) == 0 || arg.rfind("/I", 0) == 0) && arg.size() > 2) {
            std::string prefix = arg.rfind("-I", 0) == 0 ? "-I" : "/I";
            cmd.includes.push_back(arg.substr(prefix.size()));
            cmd.args.push_back(arg);
        } else if ((arg == "-I" || arg == "/I") && i + 1 < expanded_argc) {
            cmd.includes.push_back(expanded_argv[++i]);
            cmd.args.push_back(arg);
            cmd.args.push_back(expanded_argv[i]);
        } else if ((arg == "-isystem" || arg == "-iquote" || arg == "-idirafter" || arg == "-isysroot") && i + 1 < expanded_argc) {
            // GCC/Clang system/quoted include directories: affect header lookup
            cmd.includes.push_back(expanded_argv[++i]);
            cmd.args.push_back(arg);
            cmd.args.push_back(expanded_argv[i]);
            cmd.flags.push_back(arg);
        } else if (((arg.rfind("-D", 0) == 0 || arg.rfind("-U", 0) == 0) && arg.size() > 2) ||
                   ((arg.rfind("/D", 0) == 0 || arg.rfind("/U", 0) == 0) && arg.size() > 2 && arg.rfind("/DEFAULTLIB:", 0) != 0)) {
            // Preprocessor defines/undefines in attached form (e.g. -DFOO=1, /DWIN32)
            cmd.flags.push_back(arg);
            cmd.args.push_back(arg);
        } else if ((arg == "-D" || arg == "-U" || arg == "/D" || arg == "/U") && i + 1 < expanded_argc) {
            // Preprocessor defines/undefines in separate form
            cmd.flags.push_back(arg);
            cmd.args.push_back(arg);
            cmd.flags.push_back(expanded_argv[i + 1]);
            cmd.args.push_back(expanded_argv[++i]);
        } else if (arg.rfind("/Fd", 0) == 0 || arg.rfind("/Fe", 0) == 0 || arg.rfind("/Fm", 0) == 0 ||
                   arg.rfind("/FR", 0) == 0 || arg.rfind("/Fa", 0) == 0 || arg.rfind("/Fi", 0) == 0) {
            // MSVC output/side-effect paths (PDB, exe, map, browse, listing, preprocessed):
            // passed through, but excluded from the key so it stays machine-independent.
            cmd.args.push_back(arg);
            if (arg.size() == 3 && i + 1 < expanded_argc) {
                cmd.args.push_back(expanded_argv[++i]);
            }
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
            } else if (i + 1 < expanded_argc) {
                cmd.output = expanded_argv[++i];
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
        } else if (arg.rfind("/Yc", 0) == 0 || arg.rfind("/Yu", 0) == 0) {
            // MSVC PCH: /Yc (create) / Yu (use), optionally with an attached
            // header name, e.g. /Yc"pch.h", /Ycpch.h or /Yu"stdafx.h".
            bool create = arg.rfind("/Yc", 0) == 0;
            cmd.flags.push_back(arg);
            cmd.args.push_back(arg);
            cmd.pch.mode = create ? PCHMode::Create : PCHMode::Use;
            std::string attached = arg.substr(3);
            if (attached.size() >= 2 && attached.front() == '"' && attached.back() == '"') {
                attached = attached.substr(1, attached.size() - 2);
            }
            if (!attached.empty()) {
                cmd.pch.pch_source = attached;
            }
        } else if (arg.rfind("/Fp", 0) == 0) {
            // MSVC PCH output/input file: /Fp<path> or /Fp <path>
            cmd.flags.push_back(arg);
            cmd.args.push_back(arg);
            std::string pch_path = arg.substr(3);
            if (!pch_path.empty()) {
                cmd.pch.pch_output = pch_path;
            } else if (i + 1 < expanded_argc) {
                cmd.pch.pch_output = expanded_argv[++i];
                cmd.args.push_back(cmd.pch.pch_output.string());
            }
        } else if (arg == "/Y-") {
            // MSVC: disable PCH for this file
            cmd.flags.push_back(arg);
            cmd.args.push_back(arg);
            cmd.pch.mode = PCHMode::None;
        } else if (arg == "-x" && i + 1 < expanded_argc) {
            // GCC/Clang: -x c++-header (create PCH)
            std::string next_arg = expanded_argv[i + 1];
            cmd.flags.push_back(arg);
            cmd.args.push_back(arg);
            if (next_arg == "c++-header" || next_arg == "c-header" || next_arg == "cpp-header") {
                cmd.pch.mode = PCHMode::Create;
                cmd.flags.push_back(next_arg);
                cmd.args.push_back(next_arg);
                ++i;
            }
        } else if (arg == "-include" && i + 1 < expanded_argc) {
            // GCC/Clang: -include <header> (use PCH)
            std::string include_file = expanded_argv[++i];
            cmd.flags.push_back(arg);
            cmd.args.push_back(arg);
            cmd.flags.push_back(include_file);
            cmd.args.push_back(include_file);
            cmd.pch.mode = PCHMode::Use;
            cmd.pch.pch_source = include_file;
        } else if (arg.rfind("-fpch-preprocess", 0) == 0) {
            // GCC/Clang: -fpch-preprocess (PCH related)
            cmd.flags.push_back(arg);
            cmd.args.push_back(arg);
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
        } else if (cmd.pch.mode == PCHMode::Create &&
                   (std::filesystem::path(arg).extension() == ".h" ||
                    std::filesystem::path(arg).extension() == ".hpp" ||
                    std::filesystem::path(arg).extension() == ".hh" ||
                    std::filesystem::path(arg).extension() == ".hxx" ||
                    std::filesystem::path(arg).extension() == ".h++")) {
            // GCC/Clang: with -x c++-header the header itself is the input file
            // being precompiled, so treat it as the source.
            cmd.source = arg;
            cmd.args.push_back(arg);
        } else {
            // Any other flag the parser did not specifically recognise must still
            // participate in the cache key, otherwise flags such as /std:c++NN,
            // /EHsc, /MD, /MT, -fPIC, -m64, -pthread ... would be ignored and
            // could produce a wrong cache hit.
            if (!arg.empty() && (arg[0] == '-' || (arg[0] == '/' && arg.size() > 1))) {
                cmd.flags.push_back(arg);
            }
            cmd.args.push_back(arg);
        }
    }
    
    if (!is_compile || cmd.source.empty()) {
        return std::nullopt;
    }
    
    cmd.compiler_path = expanded_argv[0];
    auto info = detect_compiler(cmd.compiler_path);
    cmd.compiler_hash = info.hash;

    // Infer output file for MSVC if not specified (cl.exe creates .obj with same basename as source)
    if (info.type == CompilerType::MSVC && cmd.output.empty() && !cmd.source.empty()) {
        fs::path src_path(cmd.source);
        cmd.output = src_path.replace_extension(".obj");
    }

    // Infer PCH output paths if not specified
    if (cmd.pch.mode == PCHMode::Create) {
        if (info.type == CompilerType::MSVC) {
            // MSVC default PCH output: <source_name>.pch (or via /Fp)
            if (cmd.pch.pch_output.empty() && !cmd.source.empty()) {
                fs::path src_path(cmd.source);
                cmd.pch.pch_output = src_path.replace_extension(".pch");
            }
            // For MSVC /Yc, PCH source is typically a header like stdafx.h or pch.h
            // We'll infer it from the source if not set, but ideally user specifies via /Yc"header.h"
        } else if (info.type == CompilerType::GCC || info.type == CompilerType::Clang) {
            // GCC/Clang PCH output: an explicit -o wins (that IS the .gch);
            // otherwise default to <source_name>.gch.
            if (cmd.pch.pch_output.empty() && !cmd.output.empty()) {
                cmd.pch.pch_output = cmd.output;
            } else if (cmd.pch.pch_output.empty() && !cmd.source.empty()) {
                // GCC/Clang append .gch: foo.hpp -> foo.hpp.gch
                cmd.pch.pch_output = cmd.source.string() + ".gch";
            }
            // For -x c++-header, the source IS the header being precompiled
            if (cmd.pch.pch_source.empty() && !cmd.source.empty()) {
                cmd.pch.pch_source = cmd.source;
            }
        }
    } else if (cmd.pch.mode == PCHMode::Use) {
        if (info.type == CompilerType::MSVC) {
            // MSVC /Yu: PCH input file (specified via /Fp)
            if (cmd.pch.pch_output.empty()) {
                // Default: same base as output but .pch
                if (!cmd.output.empty()) {
                    fs::path out_path(cmd.output);
                    cmd.pch.pch_output = out_path.replace_extension(".pch");
                }
            }
        } else if (info.type == CompilerType::GCC || info.type == CompilerType::Clang) {
            // GCC/Clang -include: PCH input is typically <header>.gch
            if (cmd.pch.pch_output.empty() && !cmd.pch.pch_source.empty()) {
                cmd.pch.pch_output = cmd.pch.pch_source.string() + ".gch";
            }
        }
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

PCHInfo detect_pch_mode(const CompileCommand& cmd, CompilerType type) {
    (void)type;
    PCHInfo pch_info;
    pch_info.mode = cmd.pch.mode;
    pch_info.pch_source = cmd.pch.pch_source;
    pch_info.pch_output = cmd.pch.pch_output;
    
    // Compute PCH content hash if source file exists
    if (!pch_info.pch_source.empty() && std::filesystem::exists(pch_info.pch_source)) {
        pch_info.pch_hash = compute_file_hash(pch_info.pch_source);
    }
    
    return pch_info;
}

std::string compute_pch_hash(const std::filesystem::path& pch_file) {
    if (std::filesystem::exists(pch_file)) {
        return compute_file_hash(pch_file);
    }
    return "";
}

CacheKey compute_pch_key(const CompileCommand& cmd) {
    // For PCH, we create a key based on the PCH source content + compiler flags
    // This is used when the PCH itself is the primary output (Create mode)
    auto info = detect_compiler(cmd.compiler_path);
    return Cache::compute_key(cmd.pch.pch_source, cmd.includes, cmd.flags, cmd.compiler_hash, info.type);
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
    oss << "link|" << static_cast<int>(cmd.type) << "|";

    // Inputs: keep the command-line order (it is significant for static
    // libraries / --start-group) and fold in the *content* hash of each input so
    // that recompiling or editing an object/library invalidates the entry. The
    // path itself is hashed normalized so the key stays machine-independent.
    for (const auto& input : cmd.inputs) {
        oss << normalize_path_for_key(input) << "|";
        std::string h = compute_file_hash(input);
        oss << (h.empty() ? std::string("nohash") : h) << "|";
    }

    // Libraries and search directories: order is significant, so do not sort.
    for (const auto& lib : cmd.libraries) {
        oss << lib << "|";
    }
    for (const auto& dir : cmd.library_dirs) {
        oss << dir << "|";
    }

    // Remaining flags/args (everything that is not an input or the output, both
    // of which are covered above). Order is preserved because linkers treat some
    // flags positionally (-Wl,--start-group, /ORDER:, etc.).
    for (const auto& a : cmd.args) {
        bool covered = (a == cmd.output.string());
        if (!covered) {
            for (const auto& input : cmd.inputs) {
                if (a == input.string()) {
                    covered = true;
                    break;
                }
            }
        }
        if (!covered) {
            oss << a << "|";
        }
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

// Compiler fingerprinting mode (process-wide). Initialized lazily from the
// environment; the CLI overrides it from the loaded config.
namespace {
    CompilerCheckMode g_compiler_check_mode = CompilerCheckMode::Mtime;
    std::once_flag g_compiler_check_flag;

    void init_compiler_check_mode() {
        const char* env = std::getenv("SKIPBUILD_COMPILER_CHECK");
        if (!env) env = std::getenv("BUILDCACHE_COMPILER_CHECK");
        if (env) g_compiler_check_mode = parse_compiler_check_mode(env);
    }
}

CompilerCheckMode get_compiler_check_mode() {
    std::call_once(g_compiler_check_flag, init_compiler_check_mode);
    return g_compiler_check_mode;
}

void set_compiler_check_mode(CompilerCheckMode mode) {
    std::call_once(g_compiler_check_flag, init_compiler_check_mode);
    g_compiler_check_mode = mode;
}

// Capture the first few lines of `<compiler> --version` output
// (skips build timestamps that appear later in the output).
static std::string get_compiler_version_output(const std::string& compiler_path) {
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
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
            version_output += buffer.data();
        }
    }

    std::istringstream iss(version_output);
    std::string line;
    std::string result;
    int line_count = 0;
    while (std::getline(iss, line) && line_count < 5) {
        if (!line.empty()) {
            if (!result.empty()) result += "|";
            result += line;
            line_count++;
        }
    }
    return result;
}

// Compute the compiler fingerprint based on the configured check mode:
//   Mtime   - size + mtime + version output (default; invalidates on any change)
//   Version - version output only (survives reinstalls/rebuilds of same version)
//   Content - SHA-256 of the compiler binary (only real changes invalidate)
//   None    - empty fingerprint (compiler excluded from the cache key)
std::string get_compiler_hash(const std::string& compiler_path) {
    CompilerCheckMode mode = get_compiler_check_mode();
    if (mode == CompilerCheckMode::None) {
        return "";
    }

    std::filesystem::path path(compiler_path);
    if (!std::filesystem::exists(path)) {
        // Stable SHA-256 of the path so the fingerprint has the same shape as
        // every other fingerprint (64 hex chars) instead of a varying-length
        // decimal std::hash value.
        return compute_sha256("path|" + compiler_path);
    }

    if (mode == CompilerCheckMode::Content) {
        std::string content_hash = compute_file_hash(path);
        if (!content_hash.empty()) {
            return compute_sha256("content|" + content_hash);
        }
        // Fall back to the mtime strategy if the binary could not be read
        mode = CompilerCheckMode::Mtime;
    }

    std::ostringstream oss;
    if (mode == CompilerCheckMode::Mtime) {
        std::error_code ec;
        auto file_size = std::filesystem::file_size(path, ec);
        auto ftime = std::filesystem::last_write_time(path, ec);
        if (!ec) {
            oss << file_size << "|"
                << static_cast<long long>(ftime.time_since_epoch().count());
        }
    }
    oss << "|" << get_compiler_version_output(compiler_path);

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
    // Only canonicalize absolute paths. weakly_canonical() resolves a relative
    // path against the current working directory on some platforms (e.g. macOS
    // libc++), which would bake a machine/CWD-specific absolute path into the
    // key. Relative paths are kept relative and merely lexically normalized so
    // keys stay portable.
    std::filesystem::path normalized = path.is_absolute()
        ? std::filesystem::weakly_canonical(path)
        : path.lexically_normal();
    std::string path_str = normalized.string();
    
    // Normalize path separators to forward slash
    std::replace(path_str.begin(), path_str.end(), '\\', '/');
    
    // On Windows, normalize case (lowercase)
    #ifdef _WIN32
    std::transform(path_str.begin(), path_str.end(), path_str.begin(), ::tolower);
    #endif
    
    // Strip build root prefix if provided
    if (!build_root.empty()) {
        std::filesystem::path root_canon = build_root.is_absolute()
            ? std::filesystem::weakly_canonical(build_root)
            : build_root.lexically_normal();
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

// URL parsing helper (shared across platforms)
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

// Build the full object URL for a cache key (shared across platforms)
static std::string build_object_url(const RemoteCache::Config& config, const std::string& key_hash, const std::string& toolchain_id) {
    std::string base = config.url;
    if (!base.empty() && base.back() != '/') base += '/';
    std::string toolchain_prefix = toolchain_id.substr(0, 8);
    std::string hash_prefix1 = key_hash.substr(0, 2);
    std::string hash_prefix2 = key_hash.substr(2, 2);
    return base + toolchain_prefix + '/' + hash_prefix1 + '/' + hash_prefix2 + '/' + key_hash;
}

// HTTP response structure (shared across platforms)
struct HttpResponse {
    int status_code = 0;
    std::vector<uint8_t> body;
    std::string error;
};

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

// Simple SHA256 using Windows CryptoAPI
[[maybe_unused]] static std::string compute_sha256_winhttp(const std::vector<uint8_t>& data) {
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

class WinHttpClient {
    WinHttpHandle session_;
    std::wstring user_agent_ = L"remake/" SKIPBUILD_WVERSION;
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
            WinHttpAddRequestHeaders(request, wauth.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
        }
        
        if (!content_type.empty()) {
            std::wstring wct = to_wide("Content-Type: " + content_type);
            WinHttpAddRequestHeaders(request, wct.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
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
    std::string user_agent_ = "remake/" SKIPBUILD_VERSION;
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

// --- Minimal JSON helpers for the remote meta.json artifact list -------------

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                    out += hex[static_cast<unsigned char>(c) & 0xF];
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Read a JSON string literal starting at/after `pos` (skipping whitespace).
static std::optional<std::string> json_read_string_at(const std::string& s, size_t pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) ++pos;
    if (pos >= s.size() || s[pos] != '"') return std::nullopt;
    ++pos;
    std::string out;
    while (pos < s.size()) {
        char c = s[pos++];
        if (c == '"') return out;
        if (c == '\\' && pos < s.size()) {
            char e = s[pos++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':
                    // Filenames do not normally use non-ASCII escapes; consume
                    // the 4 hex digits and keep going rather than failing.
                    pos += 4;
                    if (pos > s.size()) pos = s.size();
                    break;
                default: out += e; break;
            }
            continue;
        }
        out += c;
    }
    return std::nullopt;
}

// Find `"key"` followed by `:`; returns the offset just past the colon.
static bool json_find_key(const std::string& obj, const std::string& key, size_t& out) {
    std::string pat = "\"" + key + "\"";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return false;
    p = obj.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    out = p + 1;
    return true;
}

static size_t json_object_end(const std::string& s, size_t start) {
    bool in_str = false;
    bool esc = false;
    for (size_t i = start; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
        } else if (c == '"') {
            in_str = true;
        } else if (c == '}') {
            return i;
        }
    }
    return std::string::npos;
}

struct RemoteMetaArtifact {
    std::string name;
    std::string file;
    bool sidecar = false;
};

static std::vector<RemoteMetaArtifact> parse_meta_artifacts(const std::string& json) {
    std::vector<RemoteMetaArtifact> out;
    size_t key_pos = 0;
    if (!json_find_key(json, "artifacts", key_pos)) return out;
    size_t arr = json.find('[', key_pos);
    if (arr == std::string::npos) return out;

    size_t pos = arr + 1;
    while (pos < json.size()) {
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' ||
                json[pos] == '\n' || json[pos] == ',')) {
            ++pos;
        }
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] != '{') { ++pos; continue; }
        size_t end = json_object_end(json, pos);
        if (end == std::string::npos) break;
        std::string obj = json.substr(pos, end - pos + 1);

        RemoteMetaArtifact a;
        size_t p;
        if (json_find_key(obj, "file", p)) {
            if (auto v = json_read_string_at(obj, p)) a.file = *v;
        }
        if (json_find_key(obj, "name", p)) {
            if (auto v = json_read_string_at(obj, p)) a.name = *v;
        }
        if (json_find_key(obj, "sidecar", p)) {
            while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t' || obj[p] == '\r' || obj[p] == '\n')) ++p;
            a.sidecar = (obj.compare(p, 4, "true") == 0);
        }
        if (!a.file.empty()) out.push_back(std::move(a));
        pos = end + 1;
    }
    return out;
}

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
        // Never transmit credentials over a transport that is not actually
        // encrypted. On Windows that means plaintext HTTP; on POSIX the raw
        // socket client has no TLS at all, so every request is plaintext.
        if (!config_.url.empty() && !config_.auth_header.empty()) {
            auto parsed = parse_url(config_.url);
            bool transport_secure = false;
#ifdef _WIN32
            transport_secure = parsed.has_value() && parsed->is_ssl;
#else
            (void)parsed;
            transport_secure = false;
#endif
            if (!transport_secure) {
                if (!config_.allow_insecure) {
                    std::cerr << "[build-cache warning] Remote cache transport is not encrypted "
                                 "(plaintext HTTP, or no TLS support on this platform); "
                                 "refusing to send the auth header. Use https:// on Windows, or set "
                                 "SKIPBUILD_REMOTE_ALLOW_INSECURE=1 to override.\n";
                    config_.auth_header.clear();
                } else {
                    std::cerr << "[build-cache warning] Sending the auth header over an unencrypted "
                                 "transport (allowed by SKIPBUILD_REMOTE_ALLOW_INSECURE).\n";
                }
            }
        }
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
    
    // Current layout: meta.json lists every artifact (original name + server
    // file), so multiple outputs and sidecars round-trip with their names.
    std::string meta_body(meta_response.body.begin(), meta_response.body.end());
    auto artifacts = parse_meta_artifacts(meta_body);
    if (!artifacts.empty()) {
        for (const auto& a : artifacts) {
            auto safe_file = sanitize_remote_name(a.file);
            if (!safe_file) {
                result.error = "Unsafe remote artifact name: " + a.file;
                return result;
            }
            auto file_url_opt = parse_url(base_url + "/" + *safe_file);
            if (!file_url_opt) {
                result.error = "Invalid file URL";
                return result;
            }
            auto file_response = impl_->with_retries([&]() {
                return impl_->client_->get(*file_url_opt);
            });
            if (!file_response.error.empty() || file_response.status_code < 200 || file_response.status_code >= 300) {
                // A listed artifact that cannot be downloaded makes the entry
                // incomplete; refuse it rather than restoring a partial result.
                impl_->increment_stat(impl_->stats_.errors);
                result.error = "Artifact not found: " + *safe_file;
                return result;
            }
            Artifact artifact;
            artifact.name = a.name;
            artifact.is_sidecar = a.sidecar;
            artifact.data = std::move(file_response.body);
            result.artifacts.push_back(std::move(artifact));
        }

        impl_->increment_stat(impl_->stats_.remote_hits);
        impl_->increment_stat(impl_->stats_.downloads);
        result.success = true;
        return result;
    }

    // Legacy layout: one hash-named primary plus fixed-name sidecars.
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
    
    {
        Artifact primary;
        primary.name = "";  // recovered from the caller's expected output
        primary.is_sidecar = false;
        primary.data = std::move(file_response.body);
        result.artifacts.push_back(std::move(primary));
    }
    
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
        
        Artifact sc;
        sc.name = key_hash + ext;  // safe_get rewrites this to <output stem><ext>
        sc.is_sidecar = true;
        sc.data = std::move(sc_response.body);
        result.artifacts.push_back(std::move(sc));
    }
    
    impl_->increment_stat(impl_->stats_.remote_hits);
    impl_->increment_stat(impl_->stats_.downloads);
    result.success = true;
    return result;
}

bool RemoteCache::put(const std::string& key_hash, const std::string& toolchain_id,
                      const std::vector<Artifact>& artifacts) {
    if (!impl_->is_configured() || impl_->config_.read_only) return false;
    if (artifacts.empty()) return false;
    
    std::string base_url = build_object_url(impl_->config_, key_hash, toolchain_id);
    auto base_url_opt = parse_url(base_url);
    if (!base_url_opt) return false;
    
    // Upload every artifact first. Only advertise them in meta.json once all
    // uploads succeeded, so a reader never sees a partial entry.
    std::string artifacts_json;
    for (size_t i = 0; i < artifacts.size(); ++i) {
        const auto& artifact = artifacts[i];
        std::string file = key_hash + "." + std::to_string(i) + ".zst";
        auto file_url_opt = parse_url(base_url + "/" + file);
        if (!file_url_opt) return false;
        
        auto put_response = impl_->with_retries([&]() {
            return impl_->client_->put(*file_url_opt, artifact.data, "application/octet-stream");
        });
        
        if (!put_response.error.empty() || put_response.status_code < 200 || put_response.status_code >= 300) {
            impl_->increment_stat(impl_->stats_.errors);
            return false;
        }
        
        if (i) artifacts_json += ',';
        artifacts_json += "{\"name\":\"" + json_escape(artifact.name) +
                          "\",\"file\":\"" + json_escape(file) +
                          "\",\"sidecar\":" + (artifact.is_sidecar ? "true" : "false") + "}";
    }
    
    std::string meta_json = "{\"toolchain\":\"" + json_escape(toolchain_id) +
                            "\",\"ts\":" + std::to_string(std::time(nullptr)) +
                            ",\"compressed\":true,\"artifacts\":[" + artifacts_json + "]}";
    std::vector<uint8_t> meta_data(meta_json.begin(), meta_json.end());
    
    auto meta_url_opt = parse_url(base_url + "/meta.json");
    if (!meta_url_opt) return false;
    
    auto meta_response = impl_->with_retries([&]() {
        return impl_->client_->put(*meta_url_opt, meta_data, "application/json");
    });
    
    if (!meta_response.error.empty() || meta_response.status_code < 200 || meta_response.status_code >= 300) {
        impl_->increment_stat(impl_->stats_.errors);
        return false;
    }
    
    impl_->increment_stat(impl_->stats_.uploads);
    return true;
}

RemoteCache::RemoteStats RemoteCache::get_stats() const {
    return impl_->get_stats();
}

RemoteCache::Config RemoteCache::from_env() {
    Config config;
    
    auto get_env = [](const char* skipbuild_var, const char* buildcache_var) -> const char* {
        if (const char* v = std::getenv(skipbuild_var)) return v;
        return std::getenv(buildcache_var);
    };
    
    if (const char* env = get_env("SKIPBUILD_REMOTE_URL", "BUILDCACHE_REMOTE_URL")) {
        config.url = env;
    }
    
    if (const char* env = get_env("SKIPBUILD_REMOTE_READ_ONLY", "BUILDCACHE_REMOTE_READ_ONLY")) {
        std::string v = env;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        config.read_only = (v == "1" || v == "true" || v == "yes" || v == "on");
    }
    
    if (const char* env = get_env("SKIPBUILD_REMOTE_CONNECT_TIMEOUT", "BUILDCACHE_REMOTE_CONNECT_TIMEOUT")) {
        try { config.connect_timeout_ms = std::stoi(env); } catch (...) {}
    }
    
    if (const char* env = get_env("SKIPBUILD_REMOTE_READ_TIMEOUT", "BUILDCACHE_REMOTE_READ_TIMEOUT")) {
        try { config.read_timeout_ms = std::stoi(env); } catch (...) {}
    }
    
    if (const char* env = get_env("SKIPBUILD_REMOTE_MAX_RETRIES", "BUILDCACHE_REMOTE_MAX_RETRIES")) {
        try { config.max_retries = std::stoi(env); } catch (...) {}
    }
    
    if (const char* env = get_env("SKIPBUILD_REMOTE_AUTH_HEADER", "BUILDCACHE_REMOTE_AUTH_HEADER")) {
        config.auth_header = env;
    }
    
    if (const char* env = get_env("SKIPBUILD_REMOTE_VERIFY_SSL", "BUILDCACHE_REMOTE_VERIFY_SSL")) {
        std::string v = env;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        config.verify_ssl = (v == "1" || v == "true" || v == "yes" || v == "on");
    }
    
    if (const char* env = get_env("SKIPBUILD_REMOTE_ALLOW_INSECURE", "BUILDCACHE_REMOTE_ALLOW_INSECURE")) {
        std::string v = env;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        config.allow_insecure = (v == "1" || v == "true" || v == "yes" || v == "on");
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

// Logging implementation
namespace {
    LogLevel g_log_level = LogLevel::None;
    std::once_flag g_log_init_flag;
    bool g_is_vs_build = false;
    std::once_flag g_vs_check_flag;

    void check_vs_build() {
        // Check if running under Visual Studio / MSBuild
        g_is_vs_build = (std::getenv("BuildingInsideVisualStudio") != nullptr) ||
                        (std::getenv("MSBuildProjectDirectory") != nullptr) ||
                        (std::getenv("MSBUILD") != nullptr) ||
                        (std::getenv("VisualStudioVersion") != nullptr);
    }

    std::mutex g_log_file_mutex;

    // Append a log line to the file named by SKIPBUILD_LOG_FILE (if set).
    // This is the reliable way to observe skipbuild output under the Visual
    // Studio generator / MSBuild, where a tool's stdout/stderr is swallowed
    // on success and OutputDebugString is only captured while debugging.
    void append_to_log_file(const std::string& line) {
        const char* path = std::getenv("SKIPBUILD_LOG_FILE");
        if (!path || !*path) return;

        std::lock_guard<std::mutex> lock(g_log_file_mutex);
        std::ofstream f(path, std::ios::app);
        if (!f) return;

        std::time_t t = std::time(nullptr);
        std::tm tmv{};
        #ifdef _WIN32
        localtime_s(&tmv, &t);
        #else
        localtime_r(&t, &tmv);
        #endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
        f << buf << " " << line << "\n";
    }
} // namespace

void init_log_level() {
    const char* env = std::getenv("REMAKE_LOG");
    if (!env) {
        env = std::getenv("SKIPBUILD_DEBUG");
        if (env && (std::string(env) == "1" || std::string(env) == "true" || std::string(env) == "yes" || std::string(env) == "on")) {
            g_log_level = LogLevel::Debug;
            return;
        }
        // If a log file is configured but no level is set, default to Info so
        // that SKIPBUILD_LOG_FILE alone produces useful output (e.g. VS builds).
        if (const char* logfile = std::getenv("SKIPBUILD_LOG_FILE")) {
            if (logfile && *logfile) {
                g_log_level = LogLevel::Info;
                return;
            }
        }
        g_log_level = LogLevel::None;
        return;
    }

    std::string level = env;
    std::transform(level.begin(), level.end(), level.begin(), ::tolower);
    if (level == "debug" || level == "4") {
        g_log_level = LogLevel::Debug;
    } else if (level == "info" || level == "3") {
        g_log_level = LogLevel::Info;
    } else if (level == "warn" || level == "warning" || level == "2") {
        g_log_level = LogLevel::Warn;
    } else if (level == "error" || level == "1") {
        g_log_level = LogLevel::Error;
    } else if (level == "none" || level == "off" || level == "0") {
        g_log_level = LogLevel::None;
    } else {
        g_log_level = LogLevel::None;
    }
}

LogLevel get_log_level() {
    std::call_once(g_log_init_flag, init_log_level);
    return g_log_level;
}

bool is_vs_build() {
    std::call_once(g_vs_check_flag, check_vs_build);
    return g_is_vs_build;
}

void log_message(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) <= static_cast<int>(get_log_level())) {
        const char* prefix = "";
        switch (level) {
            case LogLevel::Error: prefix = "[skipbuild ERROR] "; break;
            case LogLevel::Warn:  prefix = "[skipbuild WARN]  "; break;
            case LogLevel::Info:  prefix = "[skipbuild INFO]  "; break;
            case LogLevel::Debug: prefix = "[skipbuild DEBUG] "; break;
            default: break;
        }
        std::string full_msg = std::string(prefix) + message;
        
        // Always output to stderr
        std::cerr << full_msg << "\n";
        
        // Also send to Visual Studio Output window when running under VS/MSBuild
        // (only captured by VS while a debugger is attached).
#ifdef _WIN32
        if (is_vs_build()) {
            std::wstring wmsg(full_msg.begin(), full_msg.end());
            wmsg += L"\n";
            OutputDebugStringW(wmsg.c_str());
        }
#endif

        // Reliable capture for build systems that swallow tool output (VS/MSBuild)
        append_to_log_file(full_msg);
    }
}

void log_debug(const std::string& message) { log_message(LogLevel::Debug, message); }
void log_info(const std::string& message) { log_message(LogLevel::Info, message); }
void log_warn(const std::string& message) { log_message(LogLevel::Warn, message); }
void log_error(const std::string& message) { log_message(LogLevel::Error, message); }

} // namespace skipbuild
