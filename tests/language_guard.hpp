#pragma once
#include "core/i18n.hpp"

namespace lyren::test {
    struct LanguageGuard {
        Language previous = language();
        ~LanguageGuard() {
            set_language(previous);
        }
    };
} // namespace lyren::test
