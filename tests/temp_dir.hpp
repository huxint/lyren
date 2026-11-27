#pragma once
#include <doctest/doctest.h>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace lyren::test {
    struct TempDir {
        std::filesystem::path path;
        TempDir() {
            auto pattern = (std::filesystem::temp_directory_path() / "lyren-test-XXXXXX").string();
            REQUIRE(::mkdtemp(pattern.data()) != nullptr);
            path = std::move(pattern);
        }
        ~TempDir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };
} // namespace lyren::test
