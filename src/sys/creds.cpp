#include "sys/creds.hpp"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <glaze/glaze.hpp>

#include <cctype>
#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <fstream>

#include "core/log.hpp"
#include "core/encoding.hpp"
#include "sys/paths.hpp"

namespace lyren::creds {

    namespace {

        struct Params {
            std::optional<std::string> blob, data;
        };
        struct VarlinkReply {
            std::optional<std::string> error;
            std::optional<Params> parameters;
        };

        // One varlink call on the loop: the request goes out at once, the reply ends with a NUL byte.
        struct Call {
            Loop &loop;
            std::string request;
            std::string reply, error;

            bool await_ready() const noexcept {
                return false;
            }
            void await_suspend(std::coroutine_handle<> h) {
                int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
                sockaddr_un addr{.sun_family = AF_UNIX, .sun_path = "/run/systemd/io.systemd.Credentials"};
                request += '\0';
                if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0 || write(fd, request.data(), request.size()) != static_cast<ssize_t>(request.size())) {
                    error = std::format("systemd-creds: {}", std::strerror(errno));
                    close(fd);
                    loop.defer([h] {
                        h.resume();
                    });
                    return;
                }
                fcntl(fd, F_SETFL, O_NONBLOCK);
                loop.watch(fd, EPOLLIN, [this, h, fd](uint32_t) {
                    std::array<char, 4096> buf;
                    ssize_t n;
                    while ((n = read(fd, buf.data(), buf.size())) > 0) {
                        reply.append(buf.data(), static_cast<size_t>(n));
                    }
                    bool complete = !reply.empty() && reply.back() == '\0';
                    if (!complete && n < 0 && errno == EAGAIN) {
                        return;
                    }
                    if (!complete) {
                        error = "systemd-creds closed the connection";
                    } else {
                        reply.pop_back();
                    }
                    loop.unwatch(fd);
                    close(fd);
                    h.resume();
                });
            }
            void await_resume() const noexcept {}
        };

        Result<Params> parse_reply(std::string_view reply) {
            VarlinkReply r;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(r, reply)) {
                return std::unexpected("systemd-creds: bad reply");
            }
            if (r.error) {
                return std::unexpected(std::format("systemd-creds: {}", *r.error));
            }
            return r.parameters.value_or(Params{});
        }

        Task<Result<Params>> call(Loop &loop, std::string method, std::string params) {
            Call c{loop, std::format(R"({{"method":"io.systemd.Credentials.{}","parameters":{}}})", method, params)};
            co_await c;
            if (!c.error.empty()) {
                co_return std::unexpected(c.error);
            }
            co_return parse_reply(c.reply);
        }

        Result<Params> call_sync(std::string_view method, std::string_view params) {
            int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0) {
                return std::unexpected(std::format("systemd-creds: {}", std::strerror(errno)));
            }
            timeval tv{.tv_sec = 2, .tv_usec = 0};
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            sockaddr_un addr{.sun_family = AF_UNIX, .sun_path = "/run/systemd/io.systemd.Credentials"};
            std::string request = std::format(R"({{"method":"io.systemd.Credentials.{}","parameters":{}}})", method, params);
            request += '\0';
            if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0 || write(fd, request.data(), request.size()) != static_cast<ssize_t>(request.size())) {
                const int err = errno;
                close(fd);
                return std::unexpected(std::format("systemd-creds: {}", std::strerror(err)));
            }
            std::string reply;
            std::array<char, 4096> buf;
            while (reply.empty() || reply.back() != '\0') {
                const ssize_t n = read(fd, buf.data(), buf.size());
                if (n <= 0) {
                    const int err = n < 0 ? errno : 0;
                    close(fd);
                    return std::unexpected(err ? std::format("systemd-creds: {}", std::strerror(err)) : "systemd-creds closed the connection");
                }
                reply.append(buf.data(), static_cast<size_t>(n));
            }
            close(fd);
            reply.pop_back();
            return parse_reply(reply);
        }

        Result<void> write_blob(const Params &r) {
            auto path = paths::state_dir() / "credentials", tmp = path;
            tmp += ".tmp";
            int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            std::string blob = r.blob.value_or("") + "\n";
            bool ok = fd >= 0 && write(fd, blob.data(), blob.size()) == static_cast<ssize_t>(blob.size()) && fsync(fd) == 0;
            if (fd >= 0) {
                close(fd);
            }
            if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
                return std::unexpected(std::format("Could not save credentials: {}", std::strerror(errno)));
            }
            return {};
        }

    } // namespace

    Task<Result<std::string>> load(Loop &loop) {
        std::ifstream in(paths::state_dir() / "credentials");
        if (!in) {
            co_return std::string{};
        }
        std::string blob{std::istreambuf_iterator<char>(in), {}};
        while (!blob.empty() && std::isspace(static_cast<unsigned char>(blob.back()))) {
            blob.pop_back();
        }
        co_return (co_await call(loop, "Decrypt", std::format(R"({{"name":"credentials","blob":"{}","scope":"user"}})", blob))).transform([](const Params &r) {
            return encoding::unbase64(r.data.value_or(""));
        });
    }

    Task<Result<void>> save(Loop &loop, std::string plaintext) {
        co_return (co_await call(loop, "Encrypt", std::format(R"({{"name":"credentials","data":"{}","scope":"user"}})", encoding::base64(plaintext)))).and_then(write_blob);
    }

    Result<void> save_sync(std::string_view plaintext) {
        return call_sync("Encrypt", std::format(R"({{"name":"credentials","data":"{}","scope":"user"}})", encoding::base64(plaintext))).and_then(write_blob);
    }

    std::string to_json(const Credentials &c) {
        std::string out;
        static_cast<void>(glz::write_json(c, out));
        return out;
    }

    Credentials from_json(std::string_view json) {
        Credentials c;
        if (!json.empty() && glz::read<glz::opts{.error_on_unknown_keys = false}>(c, json)) {
            log::warn("credentials: unreadable, starting over");
            return {};
        }
        return c;
    }

} // namespace lyren::creds
