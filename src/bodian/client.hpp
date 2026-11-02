#pragma once
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/string_map.hpp"
#include "net/fetch.hpp"

namespace lyren::bodian {

    using Query = std::vector<std::pair<std::string, std::string>>;

    std::string form_encode(std::string_view value);
    std::string query_string(const Query &query);
    std::string signature(std::string_view path, std::string_view query, std::string_view body);

    struct Call {
        std::string path;
        Query query;
        std::optional<std::string> body;
        std::string method;
        bool sign = false;
        bool android = false;
    };

    struct Reply {
        int code = 0;
        std::string body, error;
    };

    class Client {
    public:
        Client(Loop &loop, net::Http &http, std::string base_url = "https://bd-api.kuwo.cn/api/")
        : loop_(loop),
          http_(http),
          base_url_(std::move(base_url)) {}

        StringMap<std::string> session;
        std::move_only_function<void()> on_session;

        const std::string &get(std::string_view key) const;
        bool logged_in() const;
        void ensure_device();
        void clear_login();
        net::Request request(Call call, int64_t timestamp) const;
        Task<Reply> call(Call call);
        Task<Reply> lyric(std::string id, bool words);

    private:
        Loop &loop_;
        net::Http &http_;
        std::string base_url_;
    };

} // namespace lyren::bodian
