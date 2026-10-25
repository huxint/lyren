#include "ui/text_edit.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>

#include "core/utf8.hpp"

namespace lyren::ui {

static int char_class(char32_t c) {
    if (c == ' ' || c == '\t' || c == 0x3000) return 0;
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ? 2 : 1;
    if (c >= 0x3000 && c <= 0x303f) return 1;  // CJK punctuation
    if (c >= 0xff00 && c <= 0xff0f) return 1;  // fullwidth punctuation
    if (c >= 0x2e80) return 3;
    return 2;
}

static int class_at(std::string_view s, size_t i) { return char_class(utf8::decode(s, i)); }

size_t word_left(std::string_view s, size_t i) {
    while (i > 0 && class_at(s, utf8::prev(s, i)) == 0) i = utf8::prev(s, i);
    if (i == 0) return 0;
    int c = class_at(s, utf8::prev(s, i));
    while (i > 0 && class_at(s, utf8::prev(s, i)) == c) i = utf8::prev(s, i);
    return i;
}

size_t word_right(std::string_view s, size_t i) {
    if (i < s.size()) {
        int c = class_at(s, i);
        if (c != 0)
            while (i < s.size() && class_at(s, i) == c) i = utf8::next(s, i);
    }
    while (i < s.size() && class_at(s, i) == 0) i = utf8::next(s, i);
    return i;
}

size_t char_boundary(std::string_view s, size_t i) {
    i = std::min(i, s.size());
    while (i > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80) --i;
    return i;
}

void TextEdit::clamp(std::string_view text) {
    cursor = char_boundary(text, cursor);
    anchor = char_boundary(text, anchor);
}

void TextEdit::select_word(std::string_view text, size_t at) {
    if (text.empty()) return;
    at = char_boundary(text, at);
    if (at == text.size()) at = utf8::prev(text, at);
    int c = class_at(text, at);
    size_t a = at, b = utf8::next(text, at);
    while (a > 0 && class_at(text, utf8::prev(text, a)) == c) a = utf8::prev(text, a);
    while (b < text.size() && class_at(text, b) == c) b = utf8::next(text, b);
    anchor = a, cursor = b;
}

void TextEdit::insert(std::string& text, std::string_view s) {
    std::string clean(s);
    std::ranges::replace_if(clean, [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
    size_t at = lo();
    text.replace(at, hi() - at, clean);
    cursor = anchor = at + clean.size();
}

void TextEdit::erase(std::string& text, size_t from, size_t to) {
    text.erase(from, to - from);
    cursor = anchor = from;
}

TextEdit::Key TextEdit::apply(std::string& text, const wl::KeyEvent& k, std::string& copied) {
    bool ctrl = k.mods & wl::mod_ctrl, shift = k.mods & wl::mod_shift;
    switch (k.keysym) {
        case XKB_KEY_Left:
        case XKB_KEY_KP_Left:
            if (has_selection() && !shift) move(lo(), false);
            else move(ctrl ? word_left(text, cursor) : utf8::prev(text, cursor), shift);
            return Key::moved;
        case XKB_KEY_Right:
        case XKB_KEY_KP_Right:
            if (has_selection() && !shift) move(hi(), false);
            else move(ctrl ? word_right(text, cursor) : utf8::next(text, cursor), shift);
            return Key::moved;
        case XKB_KEY_Home:
        case XKB_KEY_KP_Home: move(0, shift); return Key::moved;
        case XKB_KEY_End:
        case XKB_KEY_KP_End: move(text.size(), shift); return Key::moved;
        case XKB_KEY_BackSpace:
            if (has_selection()) erase(text, lo(), hi());
            else if (cursor > 0) erase(text, ctrl ? word_left(text, cursor) : utf8::prev(text, cursor), cursor);
            return Key::edited;
        case XKB_KEY_Delete:
        case XKB_KEY_KP_Delete:
            if (has_selection()) erase(text, lo(), hi());
            else if (cursor < text.size()) erase(text, cursor, ctrl ? word_right(text, cursor) : utf8::next(text, cursor));
            return Key::edited;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter: return Key::submit;
        case XKB_KEY_Escape: return Key::cancel;
    }
    if (ctrl) {
        switch (k.keysym) {
            case XKB_KEY_a:
            case XKB_KEY_A: select_all(text); return Key::moved;
            case XKB_KEY_c:
            case XKB_KEY_C:
            case XKB_KEY_x:
            case XKB_KEY_X:
                if (!has_selection()) return Key::moved;
                copied = selection(text);
                if (k.keysym == XKB_KEY_x || k.keysym == XKB_KEY_X) erase(text, lo(), hi());
                return Key::copy;
            case XKB_KEY_v:
            case XKB_KEY_V: return Key::paste;
        }
        return Key::ignored;
    }
    if (k.text.empty() || (k.mods & wl::mod_alt)) return Key::ignored;
    insert(text, k.text);
    return Key::edited;
}

}  // namespace lyren::ui
