#pragma once
#include <expected>
#include <string>

namespace lyren {

    template <class T>
    using Result = std::expected<T, std::string>; // error: a message for the user

} // namespace lyren
