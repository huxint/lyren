#include "sys/creds.hpp"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <glaze/glaze.hpp>

#include <cctype>
#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

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

        bool write_all(int fd, std::string_view data) {
            size_t off = 0;
            while (off < data.size()) {
                const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
                if (n > 0) {
                    off += static_cast<size_t>(n);
                    continue;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                return false;
            }
            return true;
        }

        // One non-blocking varlink call on the loop with a bounded timeout; the reply ends with a NUL byte.
        struct Call {
            struct State {
                Loop &loop;
                int fd = -1;
                Loop::TimerId timer = 0;
                size_t written = 0;
                bool done = false;
                std::string request, reply, error;
                std::coroutine_handle<> h;

                void finish(std::string err = {}) {
                    if (done) {
                        return;
                    }
                    done = true;
                    if (!err.empty()) {
                        error = std::move(err);
                    }
                    loop.cancel(timer);
                    timer = 0;
                    if (fd >= 0) {
                        loop.unwatch(fd);
                        close(fd);
                        fd = -1;
                    }
                    h.resume();
                }
            };

            Loop &loop;
            std::string request;
            std::shared_ptr<State> state;

            bool await_ready() const noexcept {
                return false;
            }
            void await_suspend(std::coroutine_handle<> h) {
                state = std::make_shared<State>(State{.loop = loop, .request = std::move(request), .h = h});
                state->request += '\0';
                state->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
                if (state->fd < 0) {
                    state->error = std::format("systemd-creds: {}", std::strerror(errno));
                    state->done = true;
                    loop.defer([h] {
                        h.resume();
                    });
                    return;
                }
                sockaddr_un addr{.sun_family = AF_UNIX, .sun_path = "/run/systemd/io.systemd.Credentials"};
                if (connect(state->fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0 && errno != EINPROGRESS && errno != EAGAIN) {
                    state->error = std::format("systemd-creds: {}", std::strerror(errno));
                    close(state->fd);
                    state->fd = -1;
                    state->done = true;
                    loop.defer([h] {
                        h.resume();
                    });
                    return;
                }
                state->timer = loop.after(2s, [s = state] {
                    s->finish("systemd-creds: timed out");
                });
                auto pump_read = [](const std::shared_ptr<State> &s) {
                    std::array<char, 4096> buf;
                    ssize_t n = 0;
                    while ((n = read(s->fd, buf.data(), buf.size())) > 0) {
                        s->reply.append(buf.data(), static_cast<size_t>(n));
                    }
                    if (!s->reply.empty() && s->reply.back() == '\0') {
                        s->reply.pop_back();
                        s->finish();
                        return;
                    }
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                        return;
                    }
                    s->finish(n < 0 ? std::format("systemd-creds: {}", std::strerror(errno)) : "systemd-creds closed the connection");
                };
                loop.watch(state->fd, EPOLLIN | EPOLLOUT, [s = state, pump_read](uint32_t events) {
                    if (s->done) {
                        return;
                    }
                    if (s->written < s->request.size() && (events & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
                        while (s->written < s->request.size()) {
                            const ssize_t n = send(s->fd, s->request.data() + s->written, s->request.size() - s->written, MSG_NOSIGNAL);
                            if (n > 0) {
                                s->written += static_cast<size_t>(n);
                                continue;
                            }
                            if (n < 0 && errno == EINTR) {
                                continue;
                            }
                            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                                return;
                            }
                            s->finish(std::format("systemd-creds: {}", std::strerror(errno)));
                            return;
                        }
                        s->loop.watch(s->fd, EPOLLIN, [s, pump_read](uint32_t) {
                            if (!s->done) {
                                pump_read(s);
                            }
                        });
                    }
                    if (s->written == s->request.size() && (events & (EPOLLIN | EPOLLERR | EPOLLHUP))) {
                        pump_read(s);
                    }
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
            Call c{loop, std::format(R"({{"method":"io.systemd.Credentials.{}","parameters":{}}})", method, params), nullptr};
            co_await c;
            if (!c.state->error.empty()) {
                co_return std::unexpected(c.state->error);
            }
            co_return parse_reply(c.state->reply);
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
            if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0 || !write_all(fd, request)) {
                const int err = errno;
                close(fd);
                return std::unexpected(std::format("systemd-creds: {}", std::strerror(err)));
            }
            std::string reply;
            std::array<char, 4096> buf;
            while (reply.empty() || reply.back() != '\0') {
                const ssize_t n = read(fd, buf.data(), buf.size());
                if (n < 0 && errno == EINTR) {
                    continue;
                }
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

        // Persistent background writer offloading fsync + rename from the UI loop thread; keeps only the latest blob.
        struct BlobWriter {
            struct Completion {
                int efd = -1;
                Result<void> result{};
                bool done = false;
                ~Completion() {
                    if (efd >= 0) {
                        close(efd);
                    }
                }
            };

            std::mutex mu;
            std::condition_variable_any cv;
            std::condition_variable done_cv;
            uint64_t next_seq = 0;
            uint64_t written_seq = 0;
            std::optional<Params> pending;
            uint64_t pending_seq = 0;
            std::vector<std::shared_ptr<Completion>> waiters;
            bool writing = false;
            std::jthread worker;

            uint64_t claim_seq() {
                std::lock_guard lock(mu);
                return ++next_seq;
            }

            static Result<void> write_file(const Params &r) {
                auto path = paths::state_dir() / "credentials", tmp = path;
                tmp += ".tmp";
                int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
                std::string blob = r.blob.value_or("") + "\n";
                bool ok = fd >= 0 && write_all(fd, blob) && fsync(fd) == 0;
                if (fd >= 0) {
                    close(fd);
                }
                if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
                    return std::unexpected(std::format("Could not save credentials: {}", std::strerror(errno)));
                }
                return {};
            }

            static void signal(const std::shared_ptr<Completion> &c, const Result<void> &res) {
                c->result = res;
                const uint64_t one = 1;
                static_cast<void>(::write(c->efd, &one, sizeof one));
            }

            void submit(Params params, uint64_t seq, std::shared_ptr<Completion> done) {
                std::unique_lock lock(mu);
                if (seq < written_seq || (pending && seq < pending_seq)) {
                    lock.unlock();
                    signal(done, {});
                    return;
                }
                pending = std::move(params);
                pending_seq = seq;
                waiters.push_back(std::move(done));
                if (!worker.joinable()) {
                    worker = std::jthread([this](std::stop_token stop) {
                        run(std::move(stop));
                    });
                }
                cv.notify_one();
            }

            Result<void> write_sync(const Params &params, uint64_t seq) {
                std::vector<std::shared_ptr<Completion>> superseded;
                {
                    std::unique_lock lock(mu);
                    done_cv.wait(lock, [this] {
                        return !writing;
                    });
                    if (seq < written_seq) {
                        return {};
                    }
                    if (pending && pending_seq <= seq) {
                        pending.reset();
                        superseded = std::move(waiters);
                    }
                    writing = true;
                }
                auto res = write_file(params);
                {
                    std::lock_guard lock(mu);
                    if (res) {
                        written_seq = std::max(written_seq, seq);
                    }
                    writing = false;
                }
                done_cv.notify_all();
                for (const auto &w : superseded) {
                    signal(w, res);
                }
                return res;
            }

            void run(std::stop_token stop) {
                for (;;) {
                    Params params;
                    uint64_t seq = 0;
                    bool stale = false;
                    std::vector<std::shared_ptr<Completion>> batch;
                    {
                        std::unique_lock lock(mu);
                        cv.wait(lock, stop, [this] {
                            return pending.has_value();
                        });
                        if (!pending) {
                            return;
                        }
                        done_cv.wait(lock, [this] {
                            return !writing;
                        });
                        if (!pending) {
                            continue;
                        }
                        params = std::move(*pending);
                        pending.reset();
                        seq = pending_seq;
                        stale = seq < written_seq;
                        batch = std::move(waiters);
                        writing = true;
                    }
                    const Result<void> res = stale ? Result<void>{} : write_file(params);
                    {
                        std::lock_guard lock(mu);
                        if (res) {
                            written_seq = std::max(written_seq, seq);
                        }
                        writing = false;
                    }
                    done_cv.notify_all();
                    for (const auto &w : batch) {
                        signal(w, res);
                    }
                }
            }
        };

        BlobWriter &blob_writer() {
            static BlobWriter writer;
            return writer;
        }

        // Awaits BlobWriter completion over an eventfd; destroying the coroutine cancels wakeup cleanly.
        struct SaveBlob {
            Loop &loop;
            Params params;
            uint64_t seq;
            std::shared_ptr<BlobWriter::Completion> shared;

            ~SaveBlob() {
                if (shared && !shared->done && shared->efd >= 0) {
                    shared->done = true;
                    loop.unwatch(shared->efd);
                }
            }
            bool await_ready() const noexcept {
                return false;
            }
            void await_suspend(std::coroutine_handle<> h) {
                shared = std::make_shared<BlobWriter::Completion>();
                shared->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
                if (shared->efd < 0) {
                    shared->result = blob_writer().write_sync(params, seq);
                    shared->done = true;
                    loop.defer([h] {
                        h.resume();
                    });
                    return;
                }
                loop.watch(shared->efd, EPOLLIN, [&loop = loop, s = shared, h](uint32_t) {
                    uint64_t v = 0;
                    static_cast<void>(read(s->efd, &v, sizeof v));
                    loop.unwatch(s->efd);
                    if (!s->done) {
                        s->done = true;
                        h.resume();
                    }
                });
                blob_writer().submit(std::move(params), seq, shared);
            }
            Result<void> await_resume() noexcept {
                return std::move(shared->result);
            }
        };

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
        const uint64_t seq = blob_writer().claim_seq();
        auto encrypted = co_await call(loop, "Encrypt", std::format(R"({{"name":"credentials","data":"{}","scope":"user"}})", encoding::base64(plaintext)));
        if (!encrypted) {
            co_return std::unexpected(encrypted.error());
        }
        co_return co_await SaveBlob{loop, std::move(*encrypted), seq, nullptr};
    }

    Result<void> save_sync(std::string_view plaintext) {
        const uint64_t seq = blob_writer().claim_seq();
        auto encrypted = call_sync("Encrypt", std::format(R"({{"name":"credentials","data":"{}","scope":"user"}})", encoding::base64(plaintext)));
        if (!encrypted) {
            return std::unexpected(encrypted.error());
        }
        return blob_writer().write_sync(*encrypted, seq);
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
