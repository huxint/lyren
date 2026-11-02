#pragma once
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "core/string_map.hpp"
#include "net/fetch.hpp"

namespace lyren::qishui {

    using Json = glz::generic_u64;
    using Query = std::vector<std::pair<std::string, std::string>>;
    enum class Operation : uint8_t {
        read,
        write
    };

    const Json &field(const Json &value, std::string_view name);
    std::span<const Json> array(const Json &value);
    std::string_view text(const Json &value);
    std::string string(const Json &value);
    double number(const Json &value);
    bool boolean(const Json &value);
    std::string encode(std::string_view value);
    std::string query_string(const Query &query);
    Result<Json> parse_response(std::string_view body, bool *session_expired = nullptr);

    class Client {
    public:
        Client(Loop &loop, net::Http &http)
        : loop_(loop),
          http_(http) {}

        StringMap<std::string> session;
        std::move_only_function<void()> on_session;
        uint64_t generation = 0;

        const std::string &get(std::string_view name) const;
        bool logged_in() const;
        Task<Result<void>> initialize();
        Task<Result<Json>> call(std::string path, Query query = {}, std::optional<std::string> body = {}, Operation operation = Operation::read);
        Task<Result<Json>> android_search(std::string type, std::string query, std::string cursor, int count);
        Task<Result<Json>> public_get(std::string url);

        // The endpoint override is used by protocol tests, never persisted with account data.
        std::string api_base = "https://api.qishui.com";
        std::string seo_base = "https://beta-luna.douyin.com";

    private:
        friend class Qishui;
        Task<Result<Json>> signer(std::string path, std::optional<std::string> body = {});
        Task<Result<Json>> send(net::Request request, net::Retry retry);
        net::CookieJar cookies_;
        Loop &loop_;
        net::Http &http_;
    };

} // namespace lyren::qishui
