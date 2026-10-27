#include "audio/spool.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>

#include "core/log.hpp"
#include "sys/paths.hpp"

namespace lyren::audio {

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
        s->size_ = uint64_t(st.st_size);
        s->have_.add(0, s->size_);
        return s;
    }

    std::shared_ptr<Spool> Spool::url(net::Http &http, std::string url) {
        std::shared_ptr<Spool> s(new Spool);
        s->http_ = &http;
        s->url_ = std::move(url);
        s->fd_ = ::open(paths::cache_dir().c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
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
            ssize_t n = ::pwrite(fd_, d.data() + done, d.size() - done, off_t(offset + done));
            if (n <= 0) {
                std::lock_guard lk(m_);
                error_ = std::format("cache write: {}", std::strerror(errno));
                cv_.notify_all();
                return;
            }
            done += size_t(n);
        }
        std::lock_guard lk(m_);
        have_.add(offset, offset + d.size());
        if (req == req_) {
            pos_ = offset + d.size();
        }
        if (waiting_) {
            cv_.notify_all();
        }
    }

    void Spool::on_done(uint64_t req, long status, std::string error) {
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
                ssize_t r = ::pread(fd_, buf, std::min<uint64_t>(n, a), off_t(off));
                return r > 0 ? size_t(r) : 0;
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

    std::vector<Ranges::Span> Spool::ranges() const {
        std::lock_guard lk(m_);
        return have_.spans();
    }

    std::pair<uint64_t, uint64_t> Spool::progress() const {
        std::lock_guard lk(m_);
        return {have_.total(), size_ == unknown ? 0 : size_};
    }

} // namespace lyren::audio
