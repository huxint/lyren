#include "ncm/client.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <random>

#include "core/log.hpp"
#include "ncm/crypto.hpp"
#include "net/fetch.hpp"

namespace lyren::ncm {

    static constexpr const char *ua_eapi = "User-Agent: NeteaseMusic 9.0.90/5038 (iPhone; iOS 16.2; zh_CN)";
    static constexpr const char *ua_weapi =
        "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36 Edg/124.0.0.0";
    static constexpr const char *ua_osx = "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";
    static constexpr const char *form = "Content-Type: application/x-www-form-urlencoded";

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // encodeURIComponent
    static std::string uri_encode(std::string_view s) {
        std::string out;
        for (unsigned char c : s) {
            if (std::isalnum(c) || std::string_view("-_.!~*'()").contains(char(c))) {
                out += char(c);
            } else {
                out += std::format("%{:02X}", c);
            }
        }
        return out;
    }

    using Pairs = std::vector<std::pair<std::string, std::string>>;

    static std::string cookie_header(const Pairs &p) {
        std::string s = "Cookie: ";
        for (size_t i = 0; i < p.size(); ++i) {
            s += std::format("{}{}={}", i ? "; " : "", uri_encode(p[i].first), uri_encode(p[i].second));
        }
        return s;
    }

    bool Client::logged_in() const {
        return !get("MUSIC_U").empty();
    }

    const std::string &Client::get(const std::string &key) const {
        static const std::string empty;
        auto it = cookies.find(key);
        return it == cookies.end() ? empty : it->second;
    }

    void Client::ensure_fingerprint() {
        std::mt19937_64 rng{std::random_device{}()};
        auto &c = cookies;
        if (!c.contains("_ntes_nuid")) {
            c["_ntes_nuid"] = random_hex(32);
        }
        if (!c.contains("_ntes_nnid")) {
            c["_ntes_nnid"] = std::format("{},{}", c["_ntes_nuid"], now_ms());
        }
        if (!c.contains("WNMCID")) {
            std::string s;
            for (int i = 0; i < 6; ++i) {
                s += char('a' + rng() % 26);
            }
            c["WNMCID"] = std::format("{}.{}.01.0", s, now_ms());
        }
        c.try_emplace("WEVNSM", "1.0.0");
        if (!c.contains("deviceId")) {
            c["deviceId"] = random_hex(52, true);
        }
        if (!c.contains("NMTID")) {
            c["NMTID"] = "00O" + random_hex(38);
        }
        c.try_emplace("os", "pc");
        c.try_emplace("osver", "Microsoft-Windows-10-Professional-build-19045-64bit");
        c.try_emplace("appver", "3.1.17.204416");
        c.try_emplace("channel", "netease");
    }

    void Client::clear_login() {
        for (const char *k : {"MUSIC_U", "__csrf", "MUSIC_R_U", "MUSIC_SNS", "MUSIC_A_T", "MUSIC_R_T"}) {
            cookies.erase(k);
        }
    }

    void Client::apply_set_cookie(std::string_view raw) {
        std::string_view first = raw.substr(0, raw.find(';'));
        auto eq = first.find('=');
        if (eq == first.npos) {
            return;
        }
        auto trim = [](std::string_view v) {
            while (!v.empty() && v.front() == ' ') {
                v.remove_prefix(1);
            }
            while (!v.empty() && v.back() == ' ') {
                v.remove_suffix(1);
            }
            return v;
        };
        std::string key(trim(first.substr(0, eq))), value(trim(first.substr(eq + 1)));
        std::string lower(raw);
        for (auto &ch : lower) {
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
        }
        bool expired = lower.contains("max-age=0") || lower.contains("max-age=-");
        if (expired || value.empty()) {
            cookies.erase(key);
        } else {
            cookies[key] = value;
        }
    }

    namespace {
        struct Status {
            glz::raw_json code;
            std::optional<std::string> message, msg;
        };
    } // namespace

    Task<Reply> Client::call(std::string uri, std::string fields, Api api) {
        std::string sep = fields.empty() ? "" : ",";
        net::Request req;
        if (api != Api::weapi) {
            bool log = api == Api::clientlog;
            Pairs header{{"osver", log ? "15.5" : get("osver")},
                         {"deviceId", get("deviceId")},
                         {"os", log ? "osx" : get("os")},
                         {"appver", log ? "3.1.10.5100" : get("appver")},
                         {"versioncode", "140"},
                         {"mobilename", ""},
                         {"buildver", std::to_string(now_ms() / 1000)},
                         {"resolution", "1920x1080"},
                         {"__csrf", get("__csrf")},
                         {"channel", get("channel")},
                         {"requestId", std::format("{}_{:04}", now_ms(), std::random_device{}() % 1000)}};
            if (logged_in()) {
                header.emplace_back("MUSIC_U", get("MUSIC_U"));
            } else if (!get("MUSIC_A").empty()) {
                header.emplace_back("MUSIC_A", get("MUSIC_A"));
            }
            header.emplace_back("NMTID", get("NMTID"));
            std::string h;
            for (auto &[k, v] : header) {
                h += std::format("{}{}:{}", h.empty() ? "" : ",", json_string(k), json_string(v));
            }
            std::string text = std::format("{{{}{}\"header\":{{{}}},\"e_r\":false}}", fields, sep, h);
            req.url = std::format("https://{}/eapi/{}", log ? "clientlog.music.163.com" : "interfacepc.music.163.com", uri.substr(5));
            req.body = "params=" + eapi(uri, text);
            req.headers = {log ? ua_osx : ua_eapi, cookie_header(header), form};
        } else {
            std::string text = std::format("{{{}{}\"csrf_token\":{}}}", fields, sep, json_string(get("__csrf")));
            auto w = weapi(text);
            Pairs all(cookies.begin(), cookies.end());
            if (!logged_in()) {
                std::erase_if(all, [](auto &p) {
                    return p.first == "MUSIC_U";
                });
            }
            all.emplace_back("__remember_me", "true");
            all.emplace_back("ntes_kaola_ad", "1");
            req.url = "https://music.163.com/weapi/" + uri.substr(5);
            req.body = std::format("params={}&encSecKey={}", uri_encode(w.params), w.enc_sec_key);
            req.headers = {ua_weapi, "Referer: https://music.163.com", cookie_header(all), form};
        }

        net::Response res;
        // Connection hiccups get two retries.
        for (int attempt = 0;; ++attempt) {
            res = co_await net::fetch{loop_, http_, req};
            if (res.error.empty() || attempt == 2) {
                break;
            }
            log::warn("ncm {}: {}, retrying", uri, res.error);
            co_await sleep{loop_, std::chrono::milliseconds(300 * (attempt ? 3 : 1))};
        }
        if (!res.cookies.empty()) {
            auto before = cookies;
            for (auto &c : res.cookies) {
                apply_set_cookie(c);
            }
            if (cookies != before && on_cookies) {
                on_cookies();
            }
        }
        Reply r;
        if (!res.error.empty()) {
            r.error = "网络连接失败";
            log::warn("ncm {}: {}", uri, res.error);
            co_return r;
        }
        Status st;
        if (auto e = glz::read<glz::opts{.error_on_unknown_keys = false}>(st, res.body)) {
            r.code = res.status;
            r.error = std::format("服务器返回了无法识别的内容（HTTP {}）", res.status);
            log::warn("ncm {}: HTTP {}, {} bytes not JSON", uri, res.status, res.body.size());
            co_return r;
        }
        std::string_view code = st.code.str;
        if (code.starts_with('"')) {
            code = code.substr(1, code.size() - 2);
        }
        if (code.empty()) {
            r.code = res.status;
        } else {
            std::from_chars(code.data(), code.data() + code.size(), r.code);
        }
        static constexpr std::array ok = {200, 201, 302, 400, 502, 800, 801, 802, 803};
        if (!std::ranges::contains(ok, r.code)) {
            r.error = st.message.value_or(st.msg.value_or(std::format("请求失败（{}）", r.code)));
            if (r.code == 301) {
                r.error = "登录已失效，请重新扫码登录";
            }
            log::warn("ncm {}: code {} {}", uri, r.code, r.error);
        }
        r.body = std::move(res.body);
        co_return r;
    }

} // namespace lyren::ncm
