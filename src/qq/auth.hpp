#pragma once
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "net/http.hpp"

namespace lyren::qq {

    // Client version embedded in the QQ login page; empty when the page has no valid version.
    std::string qr_version(std::string_view page);

    // Parse ptuiCB's string arguments without executing the response.
    Result<std::vector<std::string>> qr_callback(std::string_view body);
    Result<net::Request> qr_confirmation(std::string_view redirect);
    std::string form(const std::map<std::string, std::string> &fields);
    std::string query_value(std::string_view url, std::string_view name);
    void receive_cookies(std::map<std::string, std::string> &jar, const std::vector<std::string> &cookies);
    std::string cookie_header(const std::map<std::string, std::string> &jar);

} // namespace lyren::qq
