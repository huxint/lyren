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

#include "core/crypto.hpp"
#include "core/log.hpp"
#include "sys/paths.hpp"

namespace lyren::audio {

    namespace {
        std::atomic<uint64_t> max_cache_bytes{512ull << 20};
        std::mutex cache_prune_mutex;

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
    } // namespace

    void Spool::set_cache_limit(uint64_t bytes) {
        max_cache_bytes.store(bytes, std::memory_order_relaxed);
        if (bytes == 0) {
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

    bool Ranges::completes(uint64_t b, uint64_t e, uint64_t size) const {
        if (size == 0) {
            return false;
        }
        uint64_t cov = available(0);
        if (b <= cov) {
            cov = std::max(cov, e);
            cov += available(cov);
        }
        return cov >= size;
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
            const auto cached = dir / crypto::md5_hex(cache_key);
            int cfd = ::open(cached.c_str(), O_RDONLY | O_CLOEXEC);
            struct stat st{};
            if (cfd >= 0 && fstat(cfd, &st) == 0 && st.st_size > 0) {
                ::futimens(cfd, nullptr);
                s->fd_ = cfd;
                s->size_ = static_cast<uint64_t>(st.st_size);
                s->have_.add(0, s->size_);
                s->cache_path_ = cached.string();
                log::debug("audio cache hit: {} ({} bytes)", cache_key, s->size_);
                return s;
            }
            if (cfd >= 0) {
                ::close(cfd);
            }
            s->cache_path_ = cached.string();
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
        bool promote_now = false;
        {
            std::lock_guard lk(m_);
            promote_now = req == req_ && !closed_ && error_.empty() && size_ != unknown && size_ > 0 && !tmp_path_.empty() && have_.completes(offset, offset + d.size(), size_);
            if (!promote_now) {
                have_.add(offset, offset + d.size());
                if (req == req_) {
                    pos_ = offset + d.size();
                }
                if (waiting_) {
                    cv_.notify_all();
                }
                return;
            }
        }
        // Promote before exposing the final bytes so UI queries never block on fdatasync while
        // callers finishing the stream see the promoted cache file immediately.
        promote_cache();
        std::lock_guard lk(m_);
        have_.add(offset, offset + d.size());
        if (req == req_) {
            pos_ = offset + d.size();
        }
        if (waiting_) {
            cv_.notify_all();
        }
    }

    void Spool::promote_cache() {
        std::string tmp, dst;
        uint64_t limit = 0;
        {
            std::lock_guard lk(m_);
            if (cache_invalidated_ || tmp_path_.empty() || cache_path_.empty()) {
                return;
            }
            limit = cache_limit();
            if (size_ == 0 || size_ == unknown || size_ > limit) {
                return;
            }
            tmp = std::exchange(tmp_path_, {});
            dst = cache_path_;
        }
        // fd_ is immutable after construction and the caller holds a shared_ptr<Spool>.
        if (::fdatasync(fd_) != 0) {
            ::unlink(tmp.c_str());
            return;
        }
        bool promoted = false;
        {
            std::lock_guard lk(cache_prune_mutex);
            bool valid = true;
            {
                std::lock_guard slk(m_);
                valid = !cache_invalidated_ && !cache_path_.empty();
            }
            if (valid && std::rename(tmp.c_str(), dst.c_str()) == 0) {
                promoted = true;
            } else {
                ::unlink(tmp.c_str());
            }
        }
        if (promoted) {
            std::filesystem::path p(dst);
            prune_audio_cache(p.parent_path(), limit, p);
        }
    }

    void Spool::invalidate_cache() {
        std::string path;
        std::lock_guard lk(cache_prune_mutex);
        {
            std::lock_guard slk(m_);
            cache_invalidated_ = true;
            path = std::move(cache_path_);
        }
        if (!path.empty()) {
            ::unlink(path.c_str());
        }
    }

    void Spool::on_done(uint64_t req, long status, std::string error) {
        bool completed = false;
        {
            std::lock_guard lk(m_);
            if (req != req_ || closed_) {
                return;
            }
            http_id_ = 0;
            if (error.empty() && pos_ == start_) {
                error = "empty response";
            }
            if (!error.empty()) {
                if (pos_ > start_) {
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
            if (size_ == unknown) {
                size_ = pos_; // no length was given, so the body ran to the end
            }
            auto g = have_.gap(read_pos_, size_);
            if (g.first >= g.second) {
                g = have_.gap(0, size_);
            }
            if (g.first < g.second) {
                fetch(g.first);
            } else {
                log::debug("download complete: {} bytes", size_);
                completed = error_.empty() && size_ > 0 && have_.total() == size_;
            }
            cv_.notify_all();
        }
        if (completed) {
            promote_cache();
        }
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
                return r > 0 ? static_cast<size_t>(r) : 0;
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

    std::inplace_vector<Ranges::Span, 16> Spool::ranges() const {
        std::lock_guard lk(m_);
        std::inplace_vector<Ranges::Span, 16> out;
        const auto &spans = have_.spans();
        for (size_t i = 0; i < spans.size() && i < out.capacity(); ++i) {
            out.push_back(spans[i]);
        }
        return out;
    }

    std::pair<uint64_t, uint64_t> Spool::progress() const {
        std::lock_guard lk(m_);
        return {have_.total(), size_ == unknown ? 0 : size_};
    }

} // namespace lyren::audio
