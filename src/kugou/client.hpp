#pragma once
#include <map>
#include <string>
#include <string_view>

#include "core/result.hpp"
#include "core/string_map.hpp"
#include "net/fetch.hpp"

namespace lyren::kugou {

    using Params = std::map<std::string, std::string, std::less<>>;
    inline constexpr std::string_view android_salt = "OIlwieks28dk2k092lksi2UIkp";
    inline constexpr std::string_view login_salt = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt";

    std::string sign(const Params &query, std::string_view body, std::string_view salt);
    net::Request request(std::string url, const Params &query = {}, std::optional<std::string> body = {}, std::string_view salt = {}, std::vector<std::string> headers = {});

    class Client {
    public:
        Client(Loop &loop, net::Http &http)
        : loop_(loop),
          http_(http) {}

        StringMap<std::string> session;
        std::move_only_function<void()> on_session;
        uint64_t session_gen = 0;

        const std::string &get(std::string_view name) const;
        bool logged_in() const;
        Params common(std::string_view device = {}) const;
        std::vector<std::string> headers(const Params &query, std::string_view router = {}) const;
        Task<Result<std::string>> send(net::Request req, net::Retry retry = net::Retry::network);

    private:
        Loop &loop_;
        net::Http &http_;
    };

} // namespace lyren::kugou
