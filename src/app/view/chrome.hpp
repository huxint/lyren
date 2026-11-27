#pragma once
#include <string>

#include "app/view/part.hpp"

namespace lyren {

    // What frames the pages: the bar across the top (the way back, the platform, search, the account) and the sidebar.
    class Chrome : public Part {
    public:
        explicit Chrome(Shell &shell)
        : Part(shell) {}

        // bar is the top strip beside the window controls; the search box centres on center_x.
        void top_bar(ui::Ui &ui, ui::Rect bar, float center_x);
        void sidebar(ui::Ui &ui, ui::Rect r);

    private:
        int search_sel_ = -1;
        std::string last_search_query_;
        double search_pop_since_ = -10;
        bool search_was_open_ = false;
    };

} // namespace lyren
