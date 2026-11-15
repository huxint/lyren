#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

#include "core/string_map.hpp"
#include "media/types.hpp"

// What the user did to comments and posts, shown at once while the write is on its way.
namespace lyren {

    // Likes the user set, shown over the service's own numbers until its pages echo them back. One write at a time
    // per item: another click waits for the first to return.
    class Likes {
    public:
        struct Mark {
            bool liked = false;
            int likes = 0;
            uint64_t pending = 0; // the write under way, or zero once it has settled
        };
        struct Write {
            bool liked = false;
            uint64_t ticket = 0;
            std::optional<Mark> previous;
        };

        // The like to show for an item the service reports as (liked, likes).
        Mark shown(Source source, std::string_view id, bool liked, int likes) const {
            auto it = marks_.find(key(source, id));
            return it == marks_.end() ? Mark{liked, likes, 0} : it->second;
        }
        // Flips an item, which shows at once. Nothing while its last write is still out.
        std::optional<Write> flip(Source source, std::string_view id, bool liked, int likes) {
            auto it = marks_.find(key(source, id));
            if (it != marks_.end() && it->second.pending) {
                return {};
            }
            Write write{.ticket = ++tickets_};
            if (it != marks_.end()) {
                write.previous = it->second;
                liked = it->second.liked, likes = it->second.likes;
            }
            write.liked = !liked;
            marks_.insert_or_assign(CacheKey(key(source, id)), Mark{write.liked, std::max(0, likes + (write.liked ? 1 : -1)), write.ticket});
            return write;
        }
        // The write returned: the mark stays, or what was there before comes back. False when the mark is no longer
        // this write's to settle.
        bool settle(Source source, std::string_view id, const Write &write, bool ok) {
            auto it = marks_.find(key(source, id));
            if (it == marks_.end() || it->second.pending != write.ticket) {
                return false;
            }
            if (ok) {
                it->second.pending = 0;
            } else if (write.previous) {
                it->second = *write.previous;
            } else {
                marks_.erase(it);
            }
            return true;
        }
        // The service shows the item again. A settled mark it agrees with has done its work: true when it was dropped,
        // and copies of the item cached from before the like should take the service's numbers.
        bool echoed(Source source, std::string_view id, bool liked) {
            auto it = marks_.find(key(source, id));
            if (it == marks_.end() || it->second.pending || it->second.liked != liked) {
                return false;
            }
            marks_.erase(it);
            return true;
        }
        void erase(Source source, std::string_view id) {
            if (auto it = marks_.find(key(source, id)); it != marks_.end()) {
                marks_.erase(it);
            }
        }
        // An account left: its marks go with it.
        void forget(Source source) {
            std::erase_if(marks_, [&](const auto &mark) {
                return mark.first.variant == static_cast<uint32_t>(source);
            });
        }

    private:
        static CacheKey::View key(Source source, std::string_view id) {
            return {id, static_cast<uint32_t>(source)};
        }
        CacheMap<Mark> marks_;
        uint64_t tickets_ = 0;
    };

    // Items being deleted, shown fading until the service answers.
    class Removals {
    public:
        bool contains(Source source, std::string_view id) const {
            return pending_.contains(key(source, id));
        }
        uint64_t begin(Source source, std::string_view id) {
            pending_.insert_or_assign(CacheKey(key(source, id)), ++tickets_);
            return tickets_;
        }
        // The answer arrived. False when a later delete of the same item has taken over.
        bool end(Source source, std::string_view id, uint64_t ticket) {
            auto it = pending_.find(key(source, id));
            if (it == pending_.end() || it->second != ticket) {
                return false;
            }
            pending_.erase(it);
            return true;
        }
        void forget(Source source) {
            std::erase_if(pending_, [&](const auto &entry) {
                return entry.first.variant == static_cast<uint32_t>(source);
            });
        }

    private:
        static CacheKey::View key(Source source, std::string_view id) {
            return {id, static_cast<uint32_t>(source)};
        }
        CacheMap<uint64_t> pending_;
        uint64_t tickets_ = 0;
    };

} // namespace lyren
