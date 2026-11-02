#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lyren::net {

    // A libcurl cookie snapshot, preserving domain, path and expiry across requests.
    class CookieJar {
    public:
        static CookieJar from_netscape(std::string_view text);
        std::string netscape() const;
        std::string header(std::string_view host, std::string_view path = "/", bool secure = true) const;
        std::string get(std::string_view name, std::string_view host, std::string_view path = "/") const;

    private:
        friend class Http;
        std::vector<std::string> entries_;
    };

    struct Get {
        std::string url;
        uint64_t from = 0, end = 0; // byte range [from, end); end 0 = to the end of the file
        std::vector<std::string> headers;
        std::chrono::milliseconds delay{0}; // start later, for retries
    };

    // A whole-body request; an empty method selects POST with a body, GET without one.
    struct Request {
        std::string url;
        std::optional<std::string> body;
        std::vector<std::string> headers;
        bool follow_redirects = true;
        std::optional<CookieJar> cookie_jar;
        std::string method;
    };

    enum class Verification : uint8_t {
        none,
        captcha,
        passport
    };

    struct Response {
        long status = 0;                  // 0 without a response
        std::string body, error;          // error is empty when a response arrived
        std::vector<std::string> cookies; // Set-Cookie values
        std::string location;             // the final response's redirect target
        std::string url;                  // the final response's URL
        CookieJar cookie_jar;
        Verification verification = Verification::none;
    };

    // Called on the network thread; keep them short.
    struct Stream {
        // Before the first byte: where the body starts and the full resource size (0 if unknown).
        std::move_only_function<void(uint64_t offset, uint64_t total)> head;
        std::move_only_function<void(uint64_t offset, std::span<const std::byte> data)> data;
        // status is the HTTP status, 0 without a response; error is empty on success. Not called after cancel().
        std::move_only_function<void(long status, std::string error)> done;
    };

    // curl multi on its own thread.
    class Http {
    public:
        using Id = uint64_t;

        Http();
        ~Http();
        Http(const Http &) = delete;
        Http &operator=(const Http &) = delete;

        // Any thread.
        Id stream(Get get, Stream cb);
        // done runs on the network thread.
        Id request(Request req, std::move_only_function<void(Response)> done);
        void cancel(Id id);

        struct Transfer;

    private:
        void run();
        std::chrono::milliseconds apply();
        Id add(std::unique_ptr<Transfer> t, std::chrono::milliseconds delay);

        void *multi_;
        std::thread thread_;
        std::mutex m_;
        bool quit_ = false;
        Id next_ = 1;
        std::vector<std::unique_ptr<Transfer>> adds_; // waiting for the network thread, some for their start time
        std::vector<Id> cancels_;
        std::unordered_map<Id, std::unique_ptr<Transfer>> live_; // network thread only
    };

} // namespace lyren::net
