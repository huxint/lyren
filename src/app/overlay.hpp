#pragma once
#include <string>
#include <utility>
#include <vector>

#include "core/memstat.hpp"
#include "ui/painter.hpp"

namespace lyren {

// F12 debug panel: memory breakdown and cache sizes. The app samples it twice a second while it is open.
class Overlay {
public:
    bool visible = false;
    std::vector<std::pair<std::string, std::string>> rows;

    void set(std::vector<std::pair<std::string, std::string>> r) { rows = std::move(r); }
    void draw(ui::Painter& p, const ui::Theme& theme, float right, float top);
};

}  // namespace lyren
