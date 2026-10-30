#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "core/string_map.hpp"
#include "core/task.hpp"
#include "net/fetch.hpp"

namespace lyren::qq {

    // One part of a request; the reply answers it under req_<index>.
    struct Rpc {
        std::string module, method, param; // param: a JSON object
    };

    struct Reply {
        std::string body;  // the whole JSON; each part carries its own code
        std::string error; // empty when the request as a whole went through
        bool needs_login = false;
    };

    // The comm block: the app's for reads and playback, the Linux client's for the login exchange and its renewal.
    enum class Comm : uint8_t {
        app,
        linux
    };

    // Reads and writes use signed RPC: unsigned searches soon answer with 2001. The Linux login exchange uses musicu.fcg.
    class Client {
    public:
        Client(Loop &loop, net::Http &http)
        : loop_(loop),
          http_(http) {}

        // The login: musicid, musickey and the open-platform tokens; persisted with the credentials.
        StringMap<std::string> session;
        uint64_t session_gen = 0; // changes on login/logout, not credential renewal
        std::move_only_function<void()> on_session;
        std::move_only_function<Task<Result<void>>(bool force)> refresh_session;

        bool logged_in() const {
            return !get("musickey").empty();
        }
        const std::string &get(std::string_view key) const;

        Task<Reply> call(std::vector<Rpc> parts, Comm comm = Comm::app, net::Retry retry = net::Retry::network);

    private:
        Task<Reply> request(const std::vector<Rpc> &parts, Comm comm, net::Retry retry);
        Loop &loop_;
        net::Http &http_;
    };

} // namespace lyren::qq
