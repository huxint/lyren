#include "app/state.hpp"

#include <glaze/glaze.hpp>

#include <cstdio>
#include <fstream>

#include "core/log.hpp"
#include "sys/paths.hpp"

namespace lyren {

SavedState load_state() {
    SavedState s;
    std::ifstream in(paths::state_dir() / "state.json");
    if (!in) return s;
    std::string text{std::istreambuf_iterator<char>(in), {}};
    if (auto e = glz::read<glz::opts{.error_on_unknown_keys = false}>(s, text)) {
        log::warn("state.json: {}", glz::format_error(e, text));
        return {};
    }
    if (s.current >= int(s.queue.size())) s.current = -1;
    return s;
}

void save_state(const SavedState& s) {
    std::string text;
    (void)glz::write_json(s, text);
    auto path = paths::state_dir() / "state.json", tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << text;
        if (!out) return log::warn("cannot write {}", tmp.string());
    }
    std::rename(tmp.c_str(), path.c_str());
}

}  // namespace lyren
