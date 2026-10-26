#include "qq/auth.hpp"

#include <curl/curl.h>
#include <algorithm>

namespace lyren::qq {

Result<std::vector<std::string>> qr_callback(std::string_view body) {
    auto start = body.find("ptuiCB(");
    if (start == body.npos) return std::unexpected("QQ 登录返回了无法识别的状态");
    body.remove_prefix(start + 7);
    std::vector<std::string> args;
    while (true) {
        body.remove_prefix(std::min(body.size(), body.find_first_not_of(" \t\r\n")));
        if (body.starts_with(')')) break;
        if (body.empty() || (body.front() != '\'' && body.front() != '"'))
            return std::unexpected("QQ 登录返回了无法识别的状态");
        char quote = body.front();
        body.remove_prefix(1);
        std::string value;
        while (!body.empty() && body.front() != quote) {
            if (body.front() == '\\') {
                body.remove_prefix(1);
                if (body.empty()) return std::unexpected("QQ 登录返回了无法识别的状态");
            }
            value += body.front();
            body.remove_prefix(1);
        }
        if (body.empty()) return std::unexpected("QQ 登录返回了无法识别的状态");
        body.remove_prefix(1);
        args.push_back(std::move(value));
        body.remove_prefix(std::min(body.size(), body.find_first_not_of(" \t\r\n")));
        if (body.starts_with(')')) break;
        if (!body.starts_with(',')) return std::unexpected("QQ 登录返回了无法识别的状态");
        body.remove_prefix(1);
    }
    if (args.size() < 6) return std::unexpected("QQ 登录返回了不完整的状态");
    return args;
}

std::string form(const std::map<std::string, std::string>& fields) {
    auto encode = [](const std::string& s) {
        char* p = curl_easy_escape(nullptr, s.c_str(), int(s.size()));
        std::string out(p);
        curl_free(p);
        return out;
    };
    std::string out;
    for (const auto& [name, value] : fields) {
        if (!out.empty()) out += '&';
        out += encode(name) + '=' + encode(value);
    }
    return out;
}

std::string query_value(std::string_view url, std::string_view name) {
    auto question = url.find('?');
    if (question == url.npos) return {};
    url.remove_prefix(question + 1);
    url = url.substr(0, url.find('#'));
    while (!url.empty()) {
        auto amp = url.find('&');
        auto part = url.substr(0, amp);
        auto eq = part.find('=');
        if (part.substr(0, eq) == name && eq != part.npos) {
            std::string value(part.substr(eq + 1));
            std::ranges::replace(value, '+', ' ');
            int n = 0;
            char* p = curl_easy_unescape(nullptr, value.c_str(), int(value.size()), &n);
            std::string out(p, size_t(n));
            curl_free(p);
            return out;
        }
        if (amp == url.npos) break;
        url.remove_prefix(amp + 1);
    }
    return {};
}

void receive_cookies(std::map<std::string, std::string>& jar, const std::vector<std::string>& cookies) {
    for (std::string_view cookie : cookies) {
        cookie = cookie.substr(0, cookie.find(';'));
        auto eq = cookie.find('=');
        if (eq == cookie.npos) continue;
        jar[std::string(cookie.substr(0, eq))] = cookie.substr(eq + 1);
    }
}

std::string cookie_header(const std::map<std::string, std::string>& jar) {
    std::string out = "Cookie: ";
    for (const auto& [name, value] : jar) {
        if (out.size() > 8) out += "; ";
        out += name + '=' + value;
    }
    return out;
}

}  // namespace lyren::qq
