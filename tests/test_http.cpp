#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <charconv>
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
        explicit Server(std::function<std::string(std::string_view)> respond)
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
                    while (request.find("\r\n\r\n") == std::string::npos) {
                        ssize_t n = recv(client, buffer, sizeof buffer, 0);
                        if (n <= 0) {
                            break;
                        }
                        request.append(buffer, size_t(n));
                    }
                    std::string response = respond_(request);
                    for (size_t sent = 0; sent < response.size();) {
                        ssize_t n = send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
                        if (n <= 0) {
                            break;
                        }
                        sent += size_t(n);
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
        std::function<std::string(std::string_view)> respond_;
        std::thread thread_;
    };

    std::string ok(std::string_view body) {
        return std::format("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.size(), body);
    }

} // namespace

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

TEST_CASE("chunked audio downloads expose the exact bytes and final size") {
    std::string body(4097, '\0');
    for (size_t i = 0; i < body.size(); ++i) {
        body[i] = char(i % 251);
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
        body[i] = char(i % 251);
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
