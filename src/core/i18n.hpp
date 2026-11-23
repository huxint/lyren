#pragma once
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace lyren {

    enum class Language : uint8_t {
        en,
        zh
    };

    void set_language(Language language);
    Language language();
    std::string_view tr(std::string_view english);
    // Disambiguates identical English labels that translate differently by context (looked up as "english|context").
    std::string_view tr_ctx(std::string_view context, std::string_view english);

    template <class... Args>
    std::string trf(std::string_view english, Args &&...args) {
        return std::vformat(tr(english), std::make_format_args(args...));
    }

} // namespace lyren
