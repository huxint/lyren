#include "net/http.hpp"

#include <curl/curl.h>

#include <cctype>
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string_view>

#include "core/log.hpp"

namespace lyren::net {

    namespace {

        using CookieFields = std::array<std::string_view, 7>;

        CookieFields cookie_fields(std::string_view entry) {
            if (entry.starts_with("#HttpOnly_")) {
                entry.remove_prefix(10);
            }
            CookieFields fields;
            for (size_t i = 0; i < 6; ++i) {
                auto tab = entry.find('\t');
                fields[i] = entry.substr(0, tab);
                entry.remove_prefix(tab + 1);
            }
            fields.back() = entry;
            return fields;
        }

        bool matches(const CookieFields &fields, std::string_view host, std::string_view path, bool secure, int64_t now) {
            auto domain = fields[0], cookie_path = fields[2];
            domain.remove_prefix(domain.starts_with('.'));
            if (host.size() < domain.size() ||
                !std::ranges::equal(host.substr(host.size() - domain.size()), domain,
                                    [](unsigned char a, unsigned char b) {
                                        return std::tolower(a) == std::tolower(b);
                                    }) ||
                (host.size() != domain.size() && (fields[1] != "TRUE" || host[host.size() - domain.size() - 1] != '.'))) {
                return false;
            }
            if (!path.starts_with(cookie_path) || (path.size() != cookie_path.size() && !cookie_path.ends_with('/') && path[cookie_path.size()] != '/')) {
                return false;
            }
            int64_t expires = 0;
            std::from_chars(fields[4].data(), fields[4].data() + fields[4].size(), expires);
            return (!expires || expires > now) && (secure || fields[3] != "TRUE");
        }

    } // namespace

    CookieJar CookieJar::from_netscape(std::string_view text) {
        CURL *curl = curl_easy_init();
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
        while (!text.empty()) {
            size_t end = std::min(text.find('\n'), text.size());
            std::string line(text.substr(0, end));
            curl_easy_setopt(curl, CURLOPT_COOKIELIST, line.c_str());
            text.remove_prefix(std::min(end + 1, text.size()));
        }
        curl_slist *cookies = nullptr;
        curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &cookies);
        CookieJar jar;
        for (auto *entry = cookies; entry; entry = entry->next) {
            jar.entries_.emplace_back(entry->data);
        }
        curl_slist_free_all(cookies);
        curl_easy_cleanup(curl);
        return jar;
    }

    std::string CookieJar::netscape() const {
        std::string text;
        size_t size = 0;
        for (const auto &entry : entries_) {
            size += entry.size() + 1;
        }
        text.reserve(size);
        for (const auto &entry : entries_) {
            text += entry;
            text += '\n';
        }
        return text;
    }

    std::string CookieJar::header(std::string_view host, std::string_view path, bool secure) const {
        struct Cookie {
            std::string_view name, value;
            size_t path_size;
        };
        std::vector<Cookie> cookies;
        cookies.reserve(entries_.size());
        size_t size = 0;
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        for (const auto &entry : entries_) {
            auto fields = cookie_fields(entry);
            if (matches(fields, host, path, secure, now)) {
                cookies.push_back({fields[5], fields[6], fields[2].size()});
                size += fields[5].size() + fields[6].size() + 3;
            }
        }
        std::stable_sort(cookies.begin(), cookies.end(), [](const auto &a, const auto &b) {
            return a.path_size > b.path_size;
        });
        std::string header;
        header.reserve(size);
        for (const auto &cookie : cookies) {
            if (!header.empty()) {
                header += "; ";
            }
            header += cookie.name;
            header += '=';
            header += cookie.value;
        }
        return header;
    }

    std::string CookieJar::get(std::string_view name, std::string_view host, std::string_view path) const {
        std::string_view value;
        size_t domain_size = 0, path_size = 0;
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        for (const auto &entry : entries_) {
            auto fields = cookie_fields(entry);
            if (fields[5] == name && matches(fields, host, path, true, now) &&
                (fields[2].size() > path_size || (fields[2].size() == path_size && fields[0].size() >= domain_size))) {
                value = fields[6];
                domain_size = fields[0].size();
                path_size = fields[2].size();
            }
        }
        return std::string(value);
    }

    struct Http::Transfer {
        Id id;
        CURL *easy = curl_easy_init();
        curl_slist *headers = nullptr;
        Stream cb;
        std::move_only_function<void(Response)> done; // whole-body request
        Response resp;
        std::string post;
        uint64_t offset = 0, total = 0, at = 0;
        bool started = false, keep_cookies = false;
        std::chrono::steady_clock::time_point due;
        std::array<char, CURL_ERROR_SIZE> error{};

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
            t->resp.verification = Verification::none;
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
        } else if (t->done && (has_key(line, "bdturing-verify:") || has_key(line, "x-tt-verify-passport-decision:"))) {
            auto value = line.substr(line.find(':') + 1);
            if (value.find_first_not_of(" \t\r\n") != value.npos) {
                t->resp.verification = has_key(line, "bdturing-verify:") ? Verification::captcha : Verification::passport;
            }
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
                t->offset = 0, t->total = len > 0 ? static_cast<uint64_t>(len) : 0;
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
        curl_easy_setopt(e, CURLOPT_ERRORBUFFER, t.error.data());
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
        std::string range = std::to_string(get.from) + '-';
        if (get.end) {
            range += std::to_string(get.end - 1);
        }
        curl_easy_setopt(t->easy, CURLOPT_RANGE, range.c_str());
        log::debug("http: GET {} range {}", get.url.substr(0, get.url.find('?')), range);
        return add(std::move(t), get.delay);
    }

    Http::Id Http::request(Request req, std::move_only_function<void(Response)> done) {
        auto t = std::make_unique<Transfer>();
        t->done = std::move(done);
        setup(*t, req.url, req.headers);
        curl_easy_setopt(t->easy, CURLOPT_FOLLOWLOCATION, req.follow_redirects ? 1L : 0L);
        curl_easy_setopt(t->easy, CURLOPT_COOKIEFILE, "");
        if (req.cookie_jar) {
            t->keep_cookies = true;
            for (const auto &cookie : req.cookie_jar->entries_) {
                curl_easy_setopt(t->easy, CURLOPT_COOKIELIST, cookie.c_str());
            }
        }
        curl_easy_setopt(t->easy, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(t->easy, CURLOPT_TIMEOUT, 30L);
        if (req.body) {
            t->post = std::move(*req.body);
            curl_easy_setopt(t->easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(t->post.size()));
            curl_easy_setopt(t->easy, CURLOPT_POSTFIELDS, t->post.c_str());
        }
        if (!req.method.empty()) {
            curl_easy_setopt(t->easy, CURLOPT_CUSTOMREQUEST, req.method.c_str());
        }
        log::debug("http: {} {}", req.method.empty() ? (req.body ? "POST" : "GET") : req.method, req.url.substr(0, req.url.find('?')));
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
                    err = owned->error[0] ? owned->error.data() : curl_easy_strerror(rc);
                }
                log::debug("http {}: {} {}", owned->id, status, err);
                if (owned->done) {
                    owned->resp.status = status;
                    owned->resp.error = std::move(err);
                    char *url = nullptr;
                    curl_easy_getinfo(owned->easy, CURLINFO_EFFECTIVE_URL, &url);
                    owned->resp.url = url;
                    if (owned->keep_cookies) {
                        curl_slist *list = nullptr;
                        curl_easy_getinfo(owned->easy, CURLINFO_COOKIELIST, &list);
                        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> cookies(list, curl_slist_free_all);
                        for (auto *cookie = cookies.get(); cookie; cookie = cookie->next) {
                            owned->resp.cookie_jar.entries_.emplace_back(cookie->data);
                        }
                    }
                    owned->done(std::move(owned->resp));
                } else {
                    owned->cb.done(status, std::move(err));
                }
            }
            curl_multi_poll(multi_, nullptr, 0, static_cast<int>(wait.count()), nullptr);
        }
    }

} // namespace lyren::net
