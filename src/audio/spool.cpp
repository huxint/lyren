#include "audio/spool.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <numeric>
#include <thread>

#include "core/crypto.hpp"
#include "core/log.hpp"
#include "core/string_map.hpp"
#include "sys/paths.hpp"

namespace lyren::audio {

    namespace {
        std::atomic<uint64_t> max_cache_bytes{512ull << 20};
        std::mutex cache_prune_mutex;
        std::mutex cache_files_mutex;
        StringMap<int> pending_files; // completed downloads can be reopened while the worker syncs them

        std::filesystem::path audio_cache_dir() {
            auto dir = paths::cache_dir() / "audio";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return dir;
        }

        int open_temp_spool(const std::filesystem::path &dir, bool keep_path, std::string &out_path) {
            if (!keep_path) {
                int fd = ::open(dir.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
                if (fd >= 0) {
                    return fd;
                }
            }
            std::string tmpl = (dir / ".tmp.XXXXXX").string();
            int fd = ::mkostemp(tmpl.data(), O_CLOEXEC);
            if (fd < 0) {
                return -1;
            }
            if (keep_path) {
                out_path = std::move(tmpl);
            } else {
                ::unlink(tmpl.c_str());
            }
            return fd;
        }

        void prune_audio_cache(const std::filesystem::path &dir, uint64_t limit, const std::filesystem::path &keep = {}) {
            std::lock_guard lk(cache_prune_mutex);
            std::error_code ec;
            struct Entry {
                std::filesystem::file_time_type mtime;
                uint64_t size;
                std::filesystem::path path;
            };
            std::vector<Entry> files;
            uint64_t total = 0;
            const auto now = std::filesystem::file_time_type::clock::now();
            for (const auto &de : std::filesystem::directory_iterator(dir, ec)) {
                if (!de.is_regular_file(ec)) {
                    continue;
                }
                const auto name = de.path().filename().string();
                const auto mtime = de.last_write_time(ec);
                if (name.starts_with(".tmp.")) {
                    if (!ec && now - mtime > std::chrono::hours(1)) {
                        std::filesystem::remove(de.path(), ec);
                    }
                    continue;
                }
                const uint64_t sz = de.file_size(ec);
                if (ec) {
                    continue;
                }
                total += sz;
                if (de.path() != keep) {
                    files.push_back({mtime, sz, de.path()});
                }
            }
            if (total <= limit) {
                return;
            }
            std::ranges::sort(files, {}, &Entry::mtime);
            const uint64_t target = std::max(keep.empty() ? uint64_t{0}
                                                          : total - std::accumulate(files.begin(), files.end(), uint64_t{0},
                                                                                    [](uint64_t a, const Entry &e) {
                                                                                        return a + e.size;
                                                                                    }),
                                             limit * 7 / 8);
            for (const auto &f : files) {
                if (total <= target) {
                    break;
                }
                if (std::filesystem::remove(f.path, ec)) {
                    total -= std::min(total, f.size);
                }
            }
        }

        int open_cached(const std::string &path) {
            {
                std::lock_guard lock(cache_files_mutex);
                if (auto it = pending_files.find(path); it != pending_files.end()) {
                    return ::fcntl(it->second, F_DUPFD_CLOEXEC, 0);
                }
            }
            return ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        }

        struct CacheWrite {
            std::string tmp, dst;
            uint64_t size;
            int fd;

            CacheWrite(const std::string &path, uint64_t bytes, int source)
            : dst(path),
              size(bytes),
              fd(::fcntl(source, F_DUPFD_CLOEXEC, 0)) {}
            CacheWrite(const CacheWrite &) = delete;
            CacheWrite &operator=(const CacheWrite &) = delete;
            ~CacheWrite() {
                if (fd < 0) {
                    return;
                }
                if (!tmp.empty()) {
                    {
                        std::lock_guard lock(cache_files_mutex);
                        if (auto it = pending_files.find(dst); it != pending_files.end() && it->second == fd) {
                            pending_files.erase(it);
                        }
                    }
                    ::unlink(tmp.c_str());
                }
                ::close(fd);
            }

            void persist() {
                if (::fdatasync(fd) != 0) {
                    return;
                }
                uint64_t limit;
                {
                    std::lock_guard lock(cache_files_mutex);
                    auto it = pending_files.find(dst);
                    limit = Spool::cache_limit();
                    if (it == pending_files.end() || it->second != fd || size > limit || std::rename(tmp.c_str(), dst.c_str()) != 0) {
                        return;
                    }
                    pending_files.erase(it);
                    tmp.clear();
                }
                prune_audio_cache(std::filesystem::path(dst).parent_path(), limit, dst);
            }
        };

        class CacheWriter {
        public:
            void submit(std::unique_ptr<CacheWrite> write) {
                {
                    std::lock_guard lock(mutex_);
                    queue_.push_back(std::move(write));
                }
                ready_.notify_one();
            }

        private:
            void run(std::stop_token stop) {
                std::vector<std::unique_ptr<CacheWrite>> batch;
                for (;;) {
                    {
                        std::unique_lock lock(mutex_);
                        if (!ready_.wait(lock, stop, [this] {
                                return !queue_.empty();
                            })) {
                            return;
                        }
                        batch.swap(queue_);
                    }
                    for (auto &write : batch) {
                        try {
                            write->persist();
                        } catch (const std::exception &e) {
                            log::warn("audio cache: {}", e.what());
                        }
                        write.reset();
                    }
                    batch.clear();
                }
            }

            std::mutex mutex_;
            std::condition_variable_any ready_;
            std::vector<std::unique_ptr<CacheWrite>> queue_;
            std::jthread worker_{[this](std::stop_token stop) {
                run(stop);
            }};
        };
    } // namespace

    void Spool::set_cache_limit(uint64_t bytes) {
        max_cache_bytes.store(bytes, std::memory_order_relaxed);
        if (bytes == 0) {
            {
                std::lock_guard lock(cache_files_mutex);
                pending_files.clear();
            }
            prune_audio_cache(audio_cache_dir(), 0);
        }
    }

    uint64_t Spool::cache_limit() {
        return max_cache_bytes.load(std::memory_order_relaxed);
    }

    void Ranges::add(uint64_t b, uint64_t e) {
        if (b >= e) {
            return;
        }
        auto it = std::lower_bound(r_.begin(), r_.end(), b, [](const Span &s, uint64_t v) {
            return s.second < v;
        });
        auto last = it;
        for (; last != r_.end() && last->first <= e; ++last) {
            b = std::min(b, last->first), e = std::max(e, last->second);
        }
        r_.insert(r_.erase(it, last), {b, e});
    }

    uint64_t Ranges::available(uint64_t off) const {
        auto it = std::upper_bound(r_.begin(), r_.end(), off, [](uint64_t v, const Span &s) {
            return v < s.first;
        });
        if (it == r_.begin()) {
            return 0;
        }
        --it;
        return off < it->second ? it->second - off : 0;
    }

    Ranges::Span Ranges::gap(uint64_t from, uint64_t size) const {
        auto it = std::upper_bound(r_.begin(), r_.end(), from, [](uint64_t v, const Span &s) {
            return v < s.first;
        });
        uint64_t b = from;
        if (it != r_.begin() && std::prev(it)->second > b) {
            b = std::prev(it)->second;
        }
        if (b >= size) {
            return {size, size};
        }
        return {b, it != r_.end() ? std::min(it->first, size) : size};
    }

    uint64_t Ranges::total() const {
        uint64_t n = 0;
        for (auto [b, e] : r_) {
            n += e - b;
        }
        return n;
    }

    std::shared_ptr<Spool> Spool::file(const std::string &path) {
        std::shared_ptr<Spool> s(new Spool);
        struct stat st;
        s->fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (s->fd_ < 0 || fstat(s->fd_, &st) < 0) {
            s->error_ = std::format("{}: {}", path, std::strerror(errno));
            s->size_ = 0;
            return s;
        }
        s->size_ = static_cast<uint64_t>(st.st_size);
        s->have_.add(0, s->size_);
        return s;
    }

    std::shared_ptr<Spool> Spool::url(net::Http &http, std::string url, std::string cache_key) {
        std::shared_ptr<Spool> s(new Spool);
        const uint64_t limit = cache_limit();
        const bool persist = !cache_key.empty() && limit > 0;
        const auto dir = persist ? audio_cache_dir() : paths::cache_dir();
        if (persist) {
            s->cache_path_ = (dir / crypto::md5_hex(cache_key)).string();
            int cfd = open_cached(s->cache_path_);
            struct stat st{};
            if (cfd >= 0 && fstat(cfd, &st) == 0 && st.st_size > 0) {
                ::futimens(cfd, nullptr);
                s->fd_ = cfd;
                s->size_ = static_cast<uint64_t>(st.st_size);
                s->have_.add(0, s->size_);
                log::debug("audio cache hit: {} ({} bytes)", cache_key, s->size_);
                return s;
            }
            if (cfd >= 0) {
                ::close(cfd);
            }
        }
        s->http_ = &http;
        s->url_ = std::move(url);
        s->fd_ = open_temp_spool(dir, persist, s->tmp_path_);
        if (s->fd_ < 0) {
            s->error_ = std::format("cache file: {}", std::strerror(errno));
            s->size_ = 0;
            return s;
        }
        std::lock_guard lk(s->m_);
        s->fetch(0);
        return s;
    }

    Spool::~Spool() {
        if (http_id_) {
            http_->cancel(http_id_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
        if (!tmp_path_.empty()) {
            ::unlink(tmp_path_.c_str());
        }
    }

    void Spool::fetch(uint64_t from, std::chrono::milliseconds delay) {
        if (http_id_) {
            http_->cancel(http_id_);
        }
        http_id_ = 0;
        auto [b, e] = have_.gap(from, size_);
        if (b >= e) {
            return;
        }
        start_ = pos_ = b, end_ = e;
        progressed_ = false;
        uint64_t req = ++req_;
        std::weak_ptr<Spool> w = weak_from_this();
        http_id_ = http_->stream({.url = url_, .from = b, .end = e >= size_ ? 0 : e, .delay = delay}, {.head =
                                                                                                           [w, req](uint64_t off, uint64_t total) {
                                                                                                               if (auto s = w.lock()) {
                                                                                                                   s->on_head(req, off, total);
                                                                                                               }
                                                                                                           },
                                                                                                       .data =
                                                                                                           [w, req](uint64_t off, std::span<const std::byte> d) {
                                                                                                               if (auto s = w.lock()) {
                                                                                                                   s->on_data(req, off, d);
                                                                                                               }
                                                                                                           },
                                                                                                       .done =
                                                                                                           [w, req](long status, std::string err) {
                                                                                                               if (auto s = w.lock()) {
                                                                                                                   s->on_done(req, status, std::move(err));
                                                                                                               }
                                                                                                           }});
    }

    void Spool::on_head(uint64_t req, uint64_t offset, uint64_t total) {
        std::lock_guard lk(m_);
        if (total && size_ == unknown) {
            size_ = total;
        }
        if (req == req_ && offset != start_) {
            // The server ignored the Range header and sends the whole file; take it as it comes.
            no_ranges_ = true;
            start_ = pos_ = offset;
        }
        cv_.notify_all();
    }

    void Spool::on_data(uint64_t req, uint64_t offset, std::span<const std::byte> d) {
        // Bytes from a cancelled request are still the right bytes.
        for (size_t done = 0; done < d.size();) {
            ssize_t n = ::pwrite(fd_, d.data() + done, d.size() - done, static_cast<off_t>(offset + done));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                std::lock_guard lk(m_);
                error_ = std::format("cache write: {}", std::strerror(errno));
                cv_.notify_all();
                return;
            }
            done += static_cast<size_t>(n);
        }
        std::unique_lock lk(m_);
        const bool new_bytes = have_.available(offset) < d.size();
        have_.add(offset, offset + d.size());
        if (req == req_) {
            progressed_ |= new_bytes;
            pos_ = offset + d.size();
            if (!closed_ && error_.empty() && !tmp_path_.empty() && size_ != unknown && have_.available(0) == size_) {
                return finish_cached_download(std::move(lk));
            }
        }
        if (waiting_) {
            cv_.notify_all();
        }
    }

    void Spool::finish_cached_download(std::unique_lock<std::mutex> lock) {
        cv_.notify_all();
        if (cache_path_.empty() || size_ > cache_limit()) {
            return;
        }
        try {
            auto write = std::make_unique<CacheWrite>(cache_path_, size_, fd_);
            if (write->fd < 0) {
                return;
            }
            {
                // A reader that sees the last byte can immediately reopen the finished download.
                std::lock_guard files(cache_files_mutex);
                if (size_ > cache_limit()) {
                    return;
                }
                pending_files.insert_or_assign(cache_path_, write->fd);
                write->tmp.swap(tmp_path_);
            }
            lock.unlock();
            static CacheWriter writer;
            writer.submit(std::move(write));
        } catch (const std::exception &e) {
            log::warn("audio cache: {}", e.what());
        }
    }

    void Spool::invalidate_cache() {
        std::string path;
        {
            std::lock_guard slk(m_);
            if (closed_ || !error_.empty()) {
                return;
            }
            path = std::exchange(cache_path_, {});
        }
        if (!path.empty()) {
            std::lock_guard lock(cache_files_mutex);
            pending_files.erase(path);
            ::unlink(path.c_str());
        }
    }

    void Spool::on_done(uint64_t req, long status, std::string error) {
        std::unique_lock lk(m_);
        if (req != req_ || closed_) {
            return;
        }
        http_id_ = 0;
        if (error.empty() && pos_ == start_) {
            error = "empty response";
        }
        if (!error.empty()) {
            if (progressed_) {
                retries_ = 0;
            }
            log::warn("download at {}: {}", pos_, error);
            // Network trouble is worth retrying, after 0.5, 1 and 2 s; an answer like 403 (an expired link) is not.
            if (status >= 400 || ++retries_ > 3) {
                status_ = status;
                error_ = std::move(error);
                cv_.notify_all();
                return;
            }
            fetch(pos_, std::chrono::milliseconds(250 << retries_));
            return;
        }
        retries_ = 0;
        if (size_ == unknown && status != 206 && start_ == 0 && end_ == unknown) {
            size_ = pos_; // a full non-range response without Content-Length ran to the end
        }
        auto g = have_.gap(read_pos_, size_);
        if (g.first >= g.second) {
            g = have_.gap(0, size_);
        }
        if (g.first < g.second) {
            fetch(g.first);
        } else {
            log::debug("download complete: {} bytes", size_);
            if (!tmp_path_.empty() && error_.empty() && size_ > 0 && size_ != unknown && have_.available(0) == size_) {
                return finish_cached_download(std::move(lk));
            }
        }
        cv_.notify_all();
    }

    size_t Spool::read(uint64_t off, void *buf, size_t n) {
        constexpr uint64_t near = 256 << 10;
        std::unique_lock lk(m_);
        for (;;) {
            if (closed_ || off >= size_) {
                return 0;
            }
            if (uint64_t a = have_.available(off)) {
                lk.unlock();
                ssize_t r;
                do {
                    r = ::pread(fd_, buf, std::min<uint64_t>(n, a), static_cast<off_t>(off));
                } while (r < 0 && errno == EINTR);
                if (r < 0) {
                    std::lock_guard err_lk(m_);
                    if (error_.empty()) {
                        error_ = std::format("cache read: {}", std::strerror(errno));
                    }
                    cv_.notify_all();
                    return 0;
                }
                return static_cast<size_t>(r);
            }
            if (!error_.empty()) {
                return 0;
            }
            read_pos_ = off;
            bool coming = http_id_ && pos_ <= off && off < end_ && (off < pos_ + near || no_ranges_);
            if (!coming) {
                fetch(off);
            }
            waiting_ = true;
            cv_.wait(lk);
            waiting_ = false;
        }
    }

    bool Spool::read_exact(uint64_t off, void *buf, size_t n) {
        auto *p = static_cast<char *>(buf);
        while (n) {
            size_t k = read(off, p, n);
            if (!k) {
                return false;
            }
            off += k, p += k, n -= k;
        }
        return true;
    }

    uint64_t Spool::size() {
        std::unique_lock lk(m_);
        cv_.wait(lk, [&] {
            return size_ != unknown || closed_ || !error_.empty();
        });
        return size_ == unknown || closed_ ? 0 : size_;
    }

    void Spool::close() {
        std::lock_guard lk(m_);
        closed_ = true;
        if (http_id_) {
            http_->cancel(http_id_);
        }
        http_id_ = 0;
        cv_.notify_all();
    }

    bool Spool::closed() const {
        std::lock_guard lk(m_);
        return closed_;
    }

    std::string Spool::error() const {
        std::lock_guard lk(m_);
        return error_;
    }

    long Spool::status() const {
        std::lock_guard lk(m_);
        return status_;
    }

    bool Spool::waiting() const {
        std::lock_guard lk(m_);
        return waiting_;
    }

    std::pair<uint64_t, uint64_t> Spool::progress(std::vector<Ranges::Span> *partial) const {
        std::lock_guard lk(m_);
        const auto bytes = have_.total(), size = size_ == unknown ? 0 : size_;
        if (partial) {
            if (bytes && bytes < size) {
                *partial = have_.spans();
            } else {
                partial->clear();
            }
        }
        return {bytes, size};
    }

} // namespace lyren::audio
