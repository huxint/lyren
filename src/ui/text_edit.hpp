#pragma once
#include <cstddef>
#include <string>
#include <string_view>

#include "wl/input.hpp"

namespace lyren::ui {

    // Byte offsets of the next / previous word boundary (spaces, ASCII punctuation, CJK and other letters are
    // separate classes).
    size_t word_left(std::string_view s, size_t i);
    size_t word_right(std::string_view s, size_t i);
    // i clamped to s and moved back to the start of its code point.
    size_t char_boundary(std::string_view s, size_t i);

    // Cursor and selection of a single-line field, as byte offsets on code point boundaries.
    struct TextEdit {
        size_t cursor = 0, anchor = 0;

        size_t lo() const {
            return cursor < anchor ? cursor : anchor;
        }
        size_t hi() const {
            return cursor < anchor ? anchor : cursor;
        }
        bool has_selection() const {
            return cursor != anchor;
        }
        std::string_view selection(std::string_view text) const {
            return text.substr(lo(), hi() - lo());
        }

        // Keeps both ends inside text after it changed underneath.
        void clamp(std::string_view text);
        void move(size_t to, bool extend) {
            cursor = to;
            if (!extend) {
                anchor = to;
            }
        }
        void select_all(std::string_view text) {
            anchor = 0, cursor = text.size();
        }
        void select_word(std::string_view text, size_t at);
        // Replaces the selection with s (newlines become spaces).
        void insert(std::string &text, std::string_view s);
        void erase(std::string &text, size_t from, size_t to);

        enum class Key {
            ignored,
            edited,
            moved,
            submit,
            cancel,
            copy,
            paste
        };
        // Applies one key press. Copy and cut put the selection into copied and return copy.
        Key apply(std::string &text, const wl::KeyEvent &k, std::string &copied);
    };

} // namespace lyren::ui
