#pragma once
#include "app/view/part.hpp"

namespace lyren {

    // The accounts on each platform, and signing in to one: by scanning a code, or by pasting connection settings.
    class AccountDialog : public Part {
    public:
        explicit AccountDialog(Shell &shell)
        : Part(shell) {}

        void open(ui::Ui &ui);

    private:
        void body(ui::Ui &ui, ui::Rect r);
        void list(ui::Ui &ui, ui::Rect r);
        void import(ui::Ui &ui, ui::Rect r, Source source);
        void scan(ui::Ui &ui, ui::Rect r, Source source);

        float import_h_ = 360;
    };

} // namespace lyren
