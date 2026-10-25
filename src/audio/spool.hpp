#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "net/http.hpp"

namespace lyren::audio {

// Sorted, disjoint, non-touching [begin, end) byte ranges.
class Ranges {
public:
    using Span = std::pair<uint64_t, uint64_t>;
    void add(uint64_t b, uint64_t e);
    uint64_t available(uint64_t off) const;  // contiguous bytes present from off
    Span gap(uint64_t from, uint64_t size) const;  // first missing range in [from, size); empty if none
    uint64_t total() const;
    const std::vector<Span>& spans() const { return r_; }

private:
    std::vector<Span> r_;
};

// One track's bytes in an unlinked file under the cache directory, filled by HTTP Range requests (or a local file
// that is complete from the start) and read by the decoder thread. Reading a missing offset far from the running
// download restarts the download there; when a request finishes, the next gap is fetched.
class Spool : public std::enable_shared_from_this<Spool> {
public:
    static constexpr uint64_t unknown = UINT64_MAX;

    static std::shared_ptr<Spool> file(const std::string& path);
    static std::shared_ptr<Spool> url(net::Http& http, std::string url);
    ~Spool();

    // Blocks until bytes at off are present. Returns 0 at the end, after close() or on failure.
    size_t read(uint64_t off, void* buf, size_t n);
    bool read_exact(uint64_t off, void* buf, size_t n);
    uint64_t size();  // blocks until known; 0 on failure

    void close();  // any thread: stops the download and wakes readers for good
    std::string error() const;
    long status() const;  // HTTP status of a failed download, 0 for network errors
    bool waiting() const;  // a reader is blocked on the network
    std::pair<uint64_t, uint64_t> progress() const;  // bytes present, size

private:
    Spool() = default;
    void fetch(uint64_t from, std::chrono::milliseconds delay = {});
    void on_head(uint64_t req, uint64_t offset, uint64_t total);
    void on_data(uint64_t req, uint64_t offset, std::span<const std::byte> d);
    void on_done(uint64_t req, long status, std::string error);

    int fd_ = -1;
    net::Http* http_ = nullptr;
    std::string url_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    Ranges have_;
    uint64_t size_ = unknown;
    bool closed_ = false, waiting_ = false, no_ranges_ = false;
    std::string error_;
    net::Http::Id http_id_ = 0;  // running request, numbered req_
    uint64_t req_ = 0;
    uint64_t start_ = 0, pos_ = 0, end_ = 0;  // its first byte, next byte, end
    uint64_t read_pos_ = 0;
    int retries_ = 0;
    long status_ = 0;
};

}  // namespace lyren::audio
