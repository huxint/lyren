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
        void clear_qr();

        float import_h_ = 360;
        bool had_feedback_ = false;
        uint64_t qr_gen_ = 0;
        bool qr_ready_ = false;
        std::string qr_error_;
    };

} // namespace lyren
