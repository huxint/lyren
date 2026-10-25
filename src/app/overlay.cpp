#include "app/overlay.hpp"

#include <algorithm>

namespace lyren {

void Overlay::draw(ui::Painter& p, const ui::Theme& theme, float right, float top) {
    if (!visible) return;
    ui::TextStyle label{.size = theme.fs(12), .color = theme.text_dim};
    ui::TextStyle value{.size = theme.fs(12), .color = theme.text, .features = text::tabular_nums};
    float row_h = theme.fs(12) * 1.6f, pad = 12;
    float label_w = 0, value_w = 0;
    for (auto& [k, v] : rows) {
        label_w = std::max(label_w, p.measure(k, label));
        value_w = std::max(value_w, p.measure(v, value));
    }
    float w = pad * 3 + label_w + value_w, h = pad * 2 + row_h * float(rows.size());
    ui::Rect panel{right - w, top, w, h};
    p.shadow(panel, theme.radius, 24, theme.shadow);
    p.rect(panel, theme.elevated.with_alpha(0.96f), theme.radius, theme.border, 1);
    float y = top + pad;
    for (auto& [k, v] : rows) {
        p.text_in({panel.x + pad, y, label_w, row_h}, k, label);
        float vw = p.measure(v, value);
        p.text_in({panel.right() - pad - vw, y, vw, row_h}, v, value);
        y += row_h;
    }
}

}  // namespace lyren
