#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <charconv>
#include <atomic>
#include <format>
#include <future>
#include <memory>
#include <string_view>
#include <thread>

#include "audio/spool.hpp"
#include "core/task.hpp"
#include "net/fetch.hpp"

using namespace lyren;

namespace {

    // Real loopback connections; only the remote service is substituted.
    class Server {
    public:
        explicit Server(std::move_only_function<std::string(std::string_view)> respond)
        : respond_(std::move(respond)) {
            fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            REQUIRE(fd_ >= 0);
            sockaddr_in address{.sin_family = AF_INET, .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
            REQUIRE(bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0);
            REQUIRE(listen(fd_, 4) == 0);
            socklen_t size = sizeof address;
            REQUIRE(getsockname(fd_, reinterpret_cast<sockaddr *>(&address), &size) == 0);
            url_ = std::format("http://127.0.0.1:{}", ntohs(address.sin_port));
            thread_ = std::thread([this] {
                for (;;) {
                    int client = accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
                    if (client < 0) {
                        return;
                    }
                    std::string request;
                    char buffer[2048];
                    for (;;) {
                        ssize_t n = recv(client, buffer, sizeof buffer, 0);
                        if (n <= 0) {
                            break;
                        }
                        request.append(buffer, static_cast<size_t>(n));
                        const auto header_end = request.find("\r\n\r\n");
                        if (header_end != std::string::npos) {
                            size_t body_size = 0;
                            if (auto at = request.find("Content-Length: "); at != std::string::npos) {
                                at += 16;
                                std::from_chars(request.data() + at, request.data() + request.find("\r\n", at), body_size);
                            }
                            if (request.size() >= header_end + 4 + body_size) {
                                break;
                            }
                        }
                    }
                    std::string response = respond_(request);
                    for (size_t sent = 0; sent < response.size();) {
                        ssize_t n = send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
                        if (n <= 0) {
                            break;
                        }
                        sent += static_cast<size_t>(n);
                    }
                    close(client);
                }
            });
        }

        ~Server() {
            shutdown(fd_, SHUT_RDWR);
            thread_.join();
            close(fd_);
        }

        const std::string &url() const {
            return url_;
        }

    private:
        int fd_;
        std::string url_;
        std::move_only_function<std::string(std::string_view)> respond_;
        std::thread thread_;
    };

    std::string ok(std::string_view body) {
        return std::format("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.size(), body);
    }

    net::Response request_with_retry(net::Request req, net::Retry retry) {
        Loop loop;
        net::Http http;
        std::optional<net::Response> result;
        loop.after(5s, [&] {
            loop.quit();
        });
        spawn([](Loop *loop, net::Http *http, net::Request req, net::Retry retry, std::optional<net::Response> *result) -> Task<void> {
            *result = co_await net::fetch_retry(*loop, *http, std::move(req), retry);
            loop->quit();
        }(&loop, &http, std::move(req), retry, &result));
        loop.run();
        REQUIRE(result);
        return std::move(*result);
    }

} // namespace

TEST_CASE("network reads retry a dropped response with the original request body") {
    std::atomic<size_t> requests{0};
    const std::string body = R"({"query":"周杰伦"})";
    Server server([&](std::string_view request) {
        if (++requests == 1) {
            return std::string{};
        }
        return ok(request.substr(request.find("\r\n\r\n") + 4));
    });
    const auto response = request_with_retry({.url = server.url(), .body = body}, net::Retry::network);
    CHECK(requests == 2);
    CHECK(response.status == 200);
    CHECK(response.body == body);
    CHECK(response.error.empty());
}

TEST_CASE("writes with an uncertain result are never replayed") {
    std::atomic<size_t> requests{0};
    Server server([&](std::string_view) {
        ++requests;
        return std::string{};
    });
    const auto response = request_with_retry({.url = server.url(), .body = "{}"}, net::Retry::never);
    CHECK(requests == 1);
    CHECK_FALSE(response.error.empty());
}

TEST_CASE("HTTP failures remain visible without retrying a completed request") {
    std::atomic<size_t> requests{0};
    Server server([&](std::string_view) {
        ++requests;
        return std::string("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 6\r\nConnection: close\r\n\r\nfailed");
    });
    const auto response = request_with_retry({.url = server.url()}, net::Retry::network);
    CHECK(requests == 1);
    CHECK(response.status == 500);
    CHECK(response.body == "failed");
    CHECK(response.error.empty());
}

TEST_CASE("network reads stop after two retries") {
    std::atomic<size_t> requests{0};
    Server server([&](std::string_view) {
        ++requests;
        return std::string{};
    });
    const auto response = request_with_retry({.url = server.url()}, net::Retry::network);
    CHECK(requests == 3);
    CHECK_FALSE(response.error.empty());
}

TEST_CASE("fetch resumes on the loop thread and preserves cookies across redirects") {
    const std::string body = "歌词已就绪";
    Server server([&](std::string_view request) {
        if (request.starts_with("GET /redirect ")) {
            return std::string("HTTP/1.1 302 Found\r\nLocation: /final\r\nSet-Cookie: first=one; Path=/\r\n"
                               "Content-Length: 0\r\nConnection: close\r\n\r\n");
        }
        return std::format("HTTP/1.1 200 OK\r\nSet-Cookie: second=two; Path=/\r\nContent-Length: {}\r\n"
                           "Connection: close\r\n\r\n{}",
                           body.size(), body);
    });
    Loop loop;
    std::optional<net::Response> result;
    std::thread::id resumed_on;
    net::Http http;
    loop.after(2s, [&] {
        loop.quit();
    });
    spawn([](Loop *loop, net::Http *http, std::string url, std::optional<net::Response> *result, std::thread::id *resumed_on) -> Task<void> {
        *result = co_await net::fetch{*loop, *http, {.url = std::move(url)}};
        *resumed_on = std::this_thread::get_id();
        loop->quit();
    }(&loop, &http, server.url() + "/redirect", &result, &resumed_on));
    loop.run();
    REQUIRE(result.has_value());
    CHECK(result->status == 200);
    CHECK(result->body == body);
    CHECK(result->error.empty());
    CHECK(result->cookies == std::vector<std::string>{"first=one; Path=/", "second=two; Path=/"});
    CHECK(result->location.empty());
    CHECK(resumed_on == std::this_thread::get_id());
}

TEST_CASE("an HTTP client wakes for a new request after the previous one finishes") {
    Server server([](std::string_view) {
        return ok("ready");
    });
    net::Http http;
    for (int i = 0; i < 2; ++i) {
        CAPTURE(i);
        auto response = std::make_shared<std::promise<net::Response>>();
        auto ready = response->get_future();
        http.request({.url = server.url()}, [response](net::Response r) {
            response->set_value(std::move(r));
        });
        REQUIRE(ready.wait_for(2s) == std::future_status::ready);
        auto r = ready.get();
        CHECK(r.status == 200);
        CHECK(r.body == "ready");
        CHECK(r.error.empty());
    }
}

TEST_CASE("cookie sessions preserve domain and path scopes across separate requests") {
    Server server([](std::string_view request) {
        auto response = ok(request);
        std::string cookies;
        if (request.starts_with("GET /qr ")) {
            cookies = "Set-Cookie: qrsig=QR; Domain=login.example.test; Path=/; HttpOnly\r\n"
                      "Set-Cookie: common=shared; Domain=example.test; Path=/\r\n"
                      "Set-Cookie: p_skey=old; Domain=example.test; Path=/\r\n";
        } else if (request.starts_with("GET /confirm ")) {
            // Clearing the parent domain's cookie must not erase the new auth-domain cookie.
            cookies = "Set-Cookie: p_skey=new; Domain=auth.example.test; Path=/\r\n"
                      "Set-Cookie: p_skey=; Domain=example.test; Path=/; Max-Age=0\r\n"
                      "Set-Cookie: path_only=private; Domain=auth.example.test; Path=/private\r\n"
                      "Set-Cookie: foreign=blocked; Domain=unrelated.test; Path=/\r\n";
        }
        response.insert(response.find("\r\n") + 2, cookies);
        return response;
    });
    net::Http http;
    auto fetch = [&](std::string path, std::string host, net::CookieJar cookies) {
        auto result = std::make_shared<std::promise<net::Response>>();
        auto ready = result->get_future();
        // Host selects the cookie origin; the connection always stays on 127.0.0.1.
        http.request({.url = server.url() + path, .headers = {"Host: " + host}, .cookie_jar = std::move(cookies)}, [result](net::Response r) {
            result->set_value(std::move(r));
        });
        REQUIRE(ready.wait_for(2s) == std::future_status::ready);
        auto response = ready.get();
        REQUIRE(response.error.empty());
        REQUIRE(response.status == 200);
        return response;
    };
    auto qr = fetch("/qr", "login.example.test", {});
    CHECK(qr.cookie_jar.get("qrsig", "login.example.test") == "QR");
    auto confirmed = fetch("/confirm", "auth.example.test", qr.cookie_jar);
    CHECK(confirmed.body.contains("common=shared"));
    CHECK_FALSE(confirmed.body.contains("qrsig=QR"));
    CHECK(confirmed.cookie_jar.get("p_skey", "auth.example.test") == "new");
    CHECK(confirmed.cookie_jar.get("p_skey", "login.example.test").empty());
    CHECK(confirmed.cookie_jar.get("path_only", "auth.example.test", "/private/item") == "private");
    CHECK(confirmed.cookie_jar.get("path_only", "auth.example.test", "/privately").empty());
    auto authorized = fetch("/oauth", "auth.example.test", confirmed.cookie_jar);
    CHECK(authorized.body.contains("p_skey=new"));
    CHECK_FALSE(authorized.body.contains("p_skey=old"));
    CHECK_FALSE(authorized.body.contains("qrsig=QR"));
    CHECK_FALSE(authorized.body.contains("path_only="));
    CHECK_FALSE(authorized.body.contains("foreign="));
    auto login = fetch("/done", "login.example.test", authorized.cookie_jar);
    CHECK(login.body.contains("qrsig=QR"));
    CHECK_FALSE(login.body.contains("p_skey="));
}

TEST_CASE("chunked audio downloads expose the exact bytes and final size") {
    std::string body(4097, '\0');
    for (size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<char>(i % 251);
    }
    Server server([&](std::string_view) {
        return std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n") +
               std::format("{:x}\r\n{}\r\n{:x}\r\n{}\r\n0\r\n\r\n", 17, body.substr(0, 17), body.size() - 17, body.substr(17));
    });
    net::Http http;
    auto spool = audio::Spool::url(http, server.url());
    auto read = std::async(std::launch::async, [&] {
        std::string bytes(body.size(), '\0');
        bool complete = spool->read_exact(0, bytes.data(), bytes.size());
        return std::pair{complete, std::move(bytes)};
    });
    auto status = read.wait_for(2s);
    if (status != std::future_status::ready) {
        spool->close();
    }
    REQUIRE(status == std::future_status::ready);
    auto [complete, bytes] = read.get();
    CHECK(complete);
    CHECK(bytes == body);
    CHECK(spool->size() == 4097);
    CHECK(spool->progress() == std::pair<uint64_t, uint64_t>{4097, 4097});
    CHECK(spool->error().empty());
}

TEST_CASE("HTTP byte ranges retain the response offset and resource size") {
    const std::string body = "range data";
    Server server([&](std::string_view) {
        return std::format("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 73-82/4097\r\n"
                           "Content-Length: {}\r\nConnection: close\r\n\r\n{}",
                           body.size(), body);
    });
    uint64_t first = 0, total = 0;
    std::vector<uint64_t> offsets;
    std::string bytes;
    std::promise<std::pair<long, std::string>> response;
    auto ready = response.get_future();
    net::Http http;
    http.stream({.url = server.url(), .from = 73, .end = 83}, {.head =
                                                                   [&](uint64_t off, uint64_t size) {
                                                                       first = off, total = size;
                                                                   },
                                                               .data =
                                                                   [&](uint64_t off, std::span<const std::byte> data) {
                                                                       offsets.push_back(off);
                                                                       bytes.append(reinterpret_cast<const char *>(data.data()), data.size());
                                                                   },
                                                               .done =
                                                                   [&](long status, std::string error) {
                                                                       response.set_value({status, std::move(error)});
                                                                   }});
    REQUIRE(ready.wait_for(2s) == std::future_status::ready);
    auto [status, error] = ready.get();
    CHECK(status == 206);
    CHECK(error.empty());
    CHECK(first == 73);
    CHECK(total == 4097);
    REQUIRE_FALSE(offsets.empty());
    CHECK(offsets.front() == 73);
    CHECK(bytes == body);
}

TEST_CASE("partial audio responses fill subsequent gaps without overwriting earlier bytes") {
    std::string body(6145, '\0');
    for (size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<char>(i % 251);
    }
    Server server([&](std::string_view request) {
        auto start = request.find("Range: bytes=") + 13;
        uint64_t from = 0;
        std::from_chars(request.data() + start, request.data() + request.find('-', start), from);
        size_t end = std::min<size_t>(from + 2048, body.size());
        return std::format("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes {}-{}/{}\r\n"
                           "Content-Length: {}\r\nConnection: close\r\n\r\n{}",
                           from, end - 1, body.size(), end - from, body.substr(from, end - from));
    });
    net::Http http;
    auto spool = audio::Spool::url(http, server.url());
    auto read = std::async(std::launch::async, [&] {
        std::string bytes(body.size(), '\0');
        bool complete = spool->read_exact(0, bytes.data(), bytes.size());
        return std::pair{complete, std::move(bytes)};
    });
    auto status = read.wait_for(2s);
    if (status != std::future_status::ready) {
        spool->close();
    }
    REQUIRE(status == std::future_status::ready);
    auto [complete, bytes] = read.get();
    CHECK(complete);
    CHECK(bytes == body);
    CHECK(spool->progress() == std::pair<uint64_t, uint64_t>{6145, 6145});
}
