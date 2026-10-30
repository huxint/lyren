#pragma once
#include <doctest/doctest.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <charconv>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

namespace lyren::test {

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

    inline std::string ok(std::string_view body) {
        return std::format("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.size(), body);
    }

} // namespace lyren::test
