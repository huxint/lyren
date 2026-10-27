#include "net/http.hpp"

#include <curl/curl.h>

#include <cctype>
#include <algorithm>
#include <charconv>
#include <format>
#include <limits>
#include <string_view>

#include "core/log.hpp"

namespace lyren::net {

    struct Http::Transfer {
        Id id;
        CURL *easy = curl_easy_init();
        curl_slist *headers = nullptr;
        Stream cb;
        std::function<void(Response)> done; // whole-body request
        Response resp;
        std::string post;
        uint64_t offset = 0, total = 0, at = 0;
        bool started = false;
        std::chrono::steady_clock::time_point due;
        char error[CURL_ERROR_SIZE] = {};

        ~Transfer() {
            curl_easy_cleanup(easy);
            curl_slist_free_all(headers);
        }
    };

    static bool has_key(std::string_view line, std::string_view key) {
        if (line.size() < key.size()) {
            return false;
        }
        for (size_t i = 0; i < key.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(line[i])) != key[i]) {
                return false;
            }
        }
        return true;
    }

    // Keep cookies from the whole redirect chain; Location belongs to the final response.
    static size_t on_header(char *p, size_t, size_t n, void *ud) {
        auto *t = static_cast<Http::Transfer *>(ud);
        std::string_view line(p, n);
        if (line.starts_with("HTTP/")) {
            t->offset = t->total = 0;
            t->resp.location.clear();
        } else if (t->done && has_key(line, "set-cookie:")) {
            line.remove_prefix(11);
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
                line.remove_prefix(1);
            }
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                line.remove_suffix(1);
            }
            t->resp.cookies.emplace_back(line);
        } else if (t->done && has_key(line, "location:")) {
            line.remove_prefix(9);
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
                line.remove_prefix(1);
            }
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                line.remove_suffix(1);
            }
            t->resp.location = line;
        } else if (has_key(line, "content-range:")) {
            // content-range: bytes FROM-TO/TOTAL
            auto b = line.find("bytes ");
            auto dash = line.find('-', b), slash = line.find('/', dash);
            if (b != line.npos && dash != line.npos && slash != line.npos) {
                std::from_chars(line.data() + b + 6, line.data() + dash, t->offset);
                std::from_chars(line.data() + slash + 1, line.data() + line.size(), t->total);
            }
        }
        return n;
    }

    static size_t on_write(char *p, size_t, size_t n, void *ud) {
        auto *t = static_cast<Http::Transfer *>(ud);
        if (t->done) {
            t->resp.body.append(p, n);
            return n;
        }
        if (!t->started) {
            t->started = true;
            long status = 0;
            curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &status);
            if (status != 206) {
                curl_off_t len = -1;
                curl_easy_getinfo(t->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
                t->offset = 0, t->total = len > 0 ? uint64_t(len) : 0;
            }
            t->at = t->offset;
            t->cb.head(t->offset, t->total);
        }
        t->cb.data(t->at, {reinterpret_cast<const std::byte *>(p), n});
        t->at += n;
        return n;
    }

    Http::Http() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        multi_ = curl_multi_init();
        thread_ = std::thread([this] {
            run();
        });
    }

    Http::~Http() {
        {
            std::lock_guard lk(m_);
            quit_ = true;
        }
        curl_multi_wakeup(multi_);
        thread_.join();
        for (auto &[id, t] : live_) {
            curl_multi_remove_handle(multi_, t->easy);
        }
        live_.clear();
        curl_multi_cleanup(multi_);
        curl_global_cleanup();
    }

    static void setup(Http::Transfer &t, const std::string &url, const std::vector<std::string> &headers) {
        CURL *e = t.easy;
        curl_easy_setopt(e, CURLOPT_URL, url.c_str());
        curl_easy_setopt(e, CURLOPT_PRIVATE, &t);
        curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(e, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 10L);
        // A stalled connection fails instead of hanging; callers retry.
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 15L);
        curl_easy_setopt(e, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(e, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36");
        curl_easy_setopt(e, CURLOPT_ERRORBUFFER, t.error);
        curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(e, CURLOPT_HEADERDATA, &t);
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_write);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, &t);
        for (auto &h : headers) {
            t.headers = curl_slist_append(t.headers, h.c_str());
        }
        curl_easy_setopt(e, CURLOPT_HTTPHEADER, t.headers);
    }

    Http::Id Http::stream(Get get, Stream cb) {
        auto t = std::make_unique<Transfer>();
        t->cb = std::move(cb);
        setup(*t, get.url, get.headers);
        curl_easy_setopt(t->easy, CURLOPT_FAILONERROR, 1L);
        std::string range = get.end ? std::format("{}-{}", get.from, get.end - 1) : std::format("{}-", get.from);
        curl_easy_setopt(t->easy, CURLOPT_RANGE, range.c_str());
        log::debug("http: GET {} range {}", get.url.substr(0, get.url.find('?')), range);
        return add(std::move(t), get.delay);
    }

    Http::Id Http::request(Request req, std::function<void(Response)> done) {
        auto t = std::make_unique<Transfer>();
        t->done = std::move(done);
        setup(*t, req.url, req.headers);
        curl_easy_setopt(t->easy, CURLOPT_FOLLOWLOCATION, req.follow_redirects ? 1L : 0L);
        curl_easy_setopt(t->easy, CURLOPT_COOKIEFILE, "");
        curl_easy_setopt(t->easy, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(t->easy, CURLOPT_TIMEOUT, 30L);
        if (req.body) {
            t->post = std::move(*req.body);
            curl_easy_setopt(t->easy, CURLOPT_POSTFIELDSIZE, long(t->post.size()));
            curl_easy_setopt(t->easy, CURLOPT_POSTFIELDS, t->post.c_str());
        }
        log::debug("http: {} {}", req.body ? "POST" : "GET", req.url.substr(0, req.url.find('?')));
        return add(std::move(t), {});
    }

    Http::Id Http::add(std::unique_ptr<Transfer> t, std::chrono::milliseconds delay) {
        t->due = std::chrono::steady_clock::now() + delay;
        Id id;
        {
            std::lock_guard lk(m_);
            id = t->id = next_++;
            adds_.push_back(std::move(t));
        }
        curl_multi_wakeup(multi_);
        return id;
    }

    void Http::cancel(Id id) {
        {
            std::lock_guard lk(m_);
            cancels_.push_back(id);
        }
        curl_multi_wakeup(multi_);
    }

    // Returns how long the next delayed request still waits.
    std::chrono::milliseconds Http::apply() {
        auto now = std::chrono::steady_clock::now();
        // Curl supplies transfer deadlines; idle waits end when add(), cancel() or shutdown wakes the poll.
        auto wait = std::chrono::milliseconds(std::numeric_limits<int>::max());
        std::vector<std::unique_ptr<Transfer>> due;
        std::vector<Id> cancels;
        {
            std::lock_guard lk(m_);
            cancels.swap(cancels_);
            for (Id id : cancels) {
                std::erase_if(adds_, [&](const auto &t) {
                    return t->id == id;
                });
            }
            for (auto it = adds_.begin(); it != adds_.end();) {
                if ((*it)->due <= now) {
                    due.push_back(std::move(*it));
                    it = adds_.erase(it);
                } else {
                    wait = std::min(wait, std::chrono::ceil<std::chrono::milliseconds>((*it)->due - now));
                    ++it;
                }
            }
        }
        for (Id id : cancels) {
            if (auto it = live_.find(id); it != live_.end()) {
                curl_multi_remove_handle(multi_, it->second->easy);
                live_.erase(it);
            }
        }
        for (auto &t : due) {
            curl_multi_add_handle(multi_, t->easy);
            live_[t->id] = std::move(t);
        }
        return wait;
    }

    void Http::run() {
        for (;;) {
            {
                std::lock_guard lk(m_);
                if (quit_) {
                    return;
                }
            }
            auto wait = apply();
            int running = 0, left = 0;
            curl_multi_perform(multi_, &running);
            while (CURLMsg *msg = curl_multi_info_read(multi_, &left)) {
                if (msg->msg != CURLMSG_DONE) {
                    continue;
                }
                CURLcode rc = msg->data.result;
                Transfer *t = nullptr;
                curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &t);
                curl_multi_remove_handle(multi_, t->easy);
                auto owned = std::move(live_[t->id]);
                live_.erase(t->id);
                long status = 0;
                curl_easy_getinfo(owned->easy, CURLINFO_RESPONSE_CODE, &status);
                std::string err;
                if (rc != CURLE_OK) {
                    err = owned->error[0] ? owned->error : curl_easy_strerror(rc);
                }
                log::debug("http {}: {} {}", owned->id, status, err);
                if (owned->done) {
                    owned->resp.status = status;
                    owned->resp.error = std::move(err);
                    owned->done(std::move(owned->resp));
                } else {
                    owned->cb.done(status, std::move(err));
                }
            }
            curl_multi_poll(multi_, nullptr, 0, int(wait.count()), nullptr);
        }
    }

} // namespace lyren::net
