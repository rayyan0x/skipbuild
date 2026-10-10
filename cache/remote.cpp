#include "remote.hpp"
#include <windows.h>
#include <winhttp.h>
#include <vector>
#include <string>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdint>
#include <fstream>

#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

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
static std::string compute_sha256(const std::vector<uint8_t>& data) {
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

// Read file into vector
static bool read_file(const fs::path& path, std::vector<uint8_t>& data) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    data.resize(size);
    f.read(reinterpret_cast<char*>(data.data()), size);
    return f.good();
}

// Write vector to file
static bool write_file(const fs::path& path, const std::vector<uint8_t>& data) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    return f.good();
}

// ZSTD compression (optional)
static bool compress_data(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
#if REMAKE_HAVE_ZSTD
    #include <zstd.h>
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
    #include <zstd.h>
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

// Implementation class
struct RemoteCache::Impl {
    Config config_;
    fs::path local_cache_;
    mutable RemoteStats stats_;
    mutable std::mutex stats_mutex_;
    std::unique_ptr<WinHttpClient> client_;
    std::optional<ParsedUrl> parsed_base_url_;
    
    Impl(Config config, fs::path local_cache)
        : config_(std::move(config)), local_cache_(std::move(local_cache)) {
        client_ = std::make_unique<WinHttpClient>(
            config_.connect_timeout_ms,
            config_.read_timeout_ms,
            config_.verify_ssl,
            config_.auth_header
        );
        
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
    
    std::string toolchain_id = "unknown"; // We don't have toolchain_id for has()
    std::string object_url = build_object_url(impl_->config_, key_hash, toolchain_id) + "/meta.json";
    auto url_opt = parse_url(object_url);
    if (!url_opt) return false;
    
    int status_code = 0;
    std::string error;
    bool exists = false;
    
    // Manual retry loop for HEAD request
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
    
    // Get meta.json first
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
    
    // Download primary artifact
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
    
    // Download sidecar files
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
    
    // Upload primary artifact
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
    
    // Upload sidecars
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
    
    // Upload meta.json
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