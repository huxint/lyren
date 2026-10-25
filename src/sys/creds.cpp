#include "sys/creds.hpp"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <glaze/glaze.hpp>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <format>
#include <fstream>

#include "core/log.hpp"
#include "ncm/crypto.hpp"
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
    Loop& loop;
    std::string request;
    std::string reply, error;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr{.sun_family = AF_UNIX};
        std::strcpy(addr.sun_path, "/run/systemd/io.systemd.Credentials");
        request += '\0';
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || write(fd, request.data(), request.size()) != ssize_t(request.size())) {
            error = std::format("systemd-creds: {}", std::strerror(errno));
            close(fd);
            loop.defer([h] { h.resume(); });
            return;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        loop.watch(fd, EPOLLIN, [this, h, fd](uint32_t) {
            char buf[4096];
            ssize_t n;
            while ((n = read(fd, buf, sizeof buf)) > 0) reply.append(buf, size_t(n));
            bool complete = !reply.empty() && reply.back() == '\0';
            if (!complete && n < 0 && errno == EAGAIN) return;
            if (!complete) error = "systemd-creds closed the connection";
            else reply.pop_back();
            loop.unwatch(fd);
            close(fd);
            h.resume();
        });
    }
    void await_resume() const noexcept {}
};

Task<Result<Params>> call(Loop& loop, std::string method, std::string params) {
    Call c{loop, std::format(R"({{"method":"io.systemd.Credentials.{}","parameters":{}}})", method, params)};
    co_await c;
    if (!c.error.empty()) co_return std::unexpected(c.error);
    VarlinkReply r;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(r, c.reply)) co_return std::unexpected("systemd-creds: bad reply");
    if (r.error) co_return std::unexpected(std::format("systemd-creds: {}", *r.error));
    co_return r.parameters.value_or(Params{});
}

}  // namespace

Task<Result<std::string>> load(Loop& loop) {
    std::ifstream in(paths::state_dir() / "credentials");
    if (!in) co_return std::string{};
    std::string blob{std::istreambuf_iterator<char>(in), {}};
    while (!blob.empty() && std::isspace(static_cast<unsigned char>(blob.back()))) blob.pop_back();
    auto r = co_await call(loop, "Decrypt", std::format(R"({{"name":"credentials","blob":"{}","scope":"user"}})", blob));
    if (!r) co_return std::unexpected(r.error());
    co_return ncm::unbase64(r->data.value_or(""));
}

Task<Result<void>> save(Loop& loop, std::string plaintext) {
    auto r = co_await call(loop, "Encrypt",
                           std::format(R"({{"name":"credentials","data":"{}","scope":"user"}})", ncm::base64(plaintext)));
    if (!r) co_return std::unexpected(r.error());
    auto path = paths::state_dir() / "credentials", tmp = path;
    tmp += ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    std::string blob = r->blob.value_or("") + "\n";
    bool ok = fd >= 0 && write(fd, blob.data(), blob.size()) == ssize_t(blob.size()) && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) co_return std::unexpected(std::format("写入凭据失败：{}", std::strerror(errno)));
    co_return Result<void>{};
}

std::string to_json(const Credentials& c) {
    std::string out;
    (void)glz::write_json(c, out);
    return out;
}

Credentials from_json(std::string_view json) {
    Credentials c;
    if (!json.empty() && glz::read<glz::opts{.error_on_unknown_keys = false}>(c, json)) log::warn("credentials: unreadable, starting over");
    return c;
}

}  // namespace lyren::creds
