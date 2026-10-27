#pragma once
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "core/json.hpp"
#include "core/task.hpp"
#include "net/http.hpp"

namespace lyren::ncm {

    // clientlog: eapi to the client-log host as the macOS client, which is how listening reports are sent.
    enum class Api {
        eapi,
        weapi,
        clientlog
    };

    struct Reply {
        long code = 0; // business code, or the HTTP status when the body has none
        std::string body;
        std::string error; // empty on success
    };

    // A cookie session and the request encodings. Behaviour follows api-enhanced util/request.js.
    class Client {
    public:
        Client(Loop &loop, net::Http &http)
        : loop_(loop),
          http_(http) {}

        // The whole session including the device fingerprint; persisted with the credentials.
        std::map<std::string, std::string> cookies;
        std::function<void()> on_cookies; // a response changed the session

        bool logged_in() const;
        void ensure_fingerprint();
        void clear_login();
        const std::string &get(const std::string &key) const;

        // fields: JSON object members without the braces, e.g. R"("id":1)". uri is the /api/... path.
        Task<Reply> call(std::string uri, std::string fields, Api api = Api::eapi);

    private:
        void apply_set_cookie(std::string_view raw);

        Loop &loop_;
        net::Http &http_;
    };

} // namespace lyren::ncm
