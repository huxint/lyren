#include "sys/mpris.hpp"

#include <systemd/sd-bus.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/log.hpp"

namespace lyren {

static constexpr const char* name = "org.mpris.MediaPlayer2.lyren";
static constexpr const char* path = "/org/mpris/MediaPlayer2";
static constexpr const char* root_iface = "org.mpris.MediaPlayer2";
static constexpr const char* player_iface = "org.mpris.MediaPlayer2.Player";
static constexpr const char* instance_iface = "lyren.Instance";

// sd-bus callbacks. Methods reply right away; whatever they change comes back through Mpris::update.
struct MprisCalls {
    static Mpris& self(void* u) { return *static_cast<Mpris*>(u); }

    template <auto Handler>
    static int call(sd_bus_message* m, void* u, sd_bus_error*) {
        (self(u).h_.*Handler)();
        return sd_bus_reply_method_return(m, "");
    }

    static int raise(sd_bus_message* m, void* u, sd_bus_error*) {
        self(u).h_.raise("");
        return sd_bus_reply_method_return(m, "");
    }

    static int activate(sd_bus_message* m, void* u, sd_bus_error*) {
        const char* token;
        if (int r = sd_bus_message_read(m, "s", &token); r < 0) return r;
        self(u).h_.raise(token);
        return sd_bus_reply_method_return(m, "");
    }

    // Relative; past the end acts like Next.
    static int seek(sd_bus_message* m, void* u, sd_bus_error*) {
        int64_t offset;
        if (int r = sd_bus_message_read(m, "x", &offset); r < 0) return r;
        Mpris& s = self(u);
        if (s.state_.can_seek) {
            double to = s.h_.position() + double(offset) / 1e6;
            if (to >= s.state_.length) s.h_.next();
            else s.h_.seek(std::max(to, 0.0));
        }
        return sd_bus_reply_method_return(m, "");
    }

    // Absolute; ignored when it names another track (stale) or lies outside this one.
    static int set_position(sd_bus_message* m, void* u, sd_bus_error*) {
        const char* track;
        int64_t pos;
        if (int r = sd_bus_message_read(m, "ox", &track, &pos); r < 0) return r;
        Mpris& s = self(u);
        double to = double(pos) / 1e6;
        if (s.state_.can_seek && s.state_.track == track && to >= 0 && to <= s.state_.length) s.h_.seek(to);
        return sd_bus_reply_method_return(m, "");
    }

    static int open_uri(sd_bus_message*, void*, sd_bus_error* e) {
        return sd_bus_error_set(e, SD_BUS_ERROR_NOT_SUPPORTED, "Lyren does not open URIs");
    }

    static int metadata(sd_bus_message* m, const MprisState& s) {
        int r = sd_bus_message_open_container(m, 'a', "{sv}");
        auto put = [&](const char* key, const char* type, auto... value) {
            if (r >= 0) r = sd_bus_message_append(m, "{sv}", key, type, value...);
        };
        put("mpris:trackid", "o", s.track.empty() ? "/org/mpris/MediaPlayer2/TrackList/NoTrack" : s.track.c_str());
        if (!s.track.empty()) {
            put("mpris:length", "x", int64_t(s.length * 1e6));
            put("xesam:title", "s", s.title.c_str());
            put("xesam:artist", "as", 1, s.artist.c_str());
            put("xesam:album", "s", s.album.c_str());
            if (!s.art.empty()) put("mpris:artUrl", "s", s.art.c_str());
        }
        return r < 0 ? r : sd_bus_message_close_container(m);
    }

    static int get(sd_bus*, const char*, const char*, const char* property, sd_bus_message* m, void* u, sd_bus_error*) {
        static constexpr const char* statuses[] = {"Stopped", "Playing", "Paused"};
        const MprisState& s = self(u).state_;
        std::string_view p = property;
        auto flag = [&](bool b) { return sd_bus_message_append(m, "b", int(b)); };
        if (p == "PlaybackStatus") return sd_bus_message_append(m, "s", statuses[int(s.status)]);
        if (p == "LoopStatus") return sd_bus_message_append(m, "s", s.loop_one ? "Track" : "Playlist");
        if (p == "Shuffle") return flag(s.shuffle);
        if (p == "Metadata") return metadata(m, s);
        if (p == "Volume") return sd_bus_message_append(m, "d", s.volume);
        if (p == "Position") return sd_bus_message_append(m, "x", int64_t(self(u).h_.position() * 1e6));
        if (p == "Rate" || p == "MinimumRate" || p == "MaximumRate") return sd_bus_message_append(m, "d", 1.0);
        if (p == "CanGoNext") return flag(s.can_next);
        if (p == "CanGoPrevious") return flag(s.can_prev);
        if (p == "CanPlay" || p == "CanPause") return flag(s.can_play);
        if (p == "CanSeek") return flag(s.can_seek);
        if (p == "CanControl" || p == "CanQuit" || p == "CanRaise") return flag(true);
        if (p == "HasTrackList") return flag(false);
        if (p == "Identity" || p == "DesktopEntry") return sd_bus_message_append(m, "s", p == "Identity" ? "Lyren" : "lyren");
        return sd_bus_message_append(m, "as", 0);  // SupportedUriSchemes, SupportedMimeTypes
    }

    static int set(sd_bus*, const char*, const char*, const char* property, sd_bus_message* m, void* u, sd_bus_error*) {
        Mpris& s = self(u);
        std::string_view p = property;
        int r;
        if (p == "Volume") {
            double v;
            if ((r = sd_bus_message_read(m, "d", &v)) < 0) return r;
            s.h_.volume(std::clamp(v, 0.0, 1.0));
        } else if (p == "LoopStatus") {
            const char* v;
            if ((r = sd_bus_message_read(m, "s", &v)) < 0) return r;
            s.h_.loop_one(std::string_view(v) == "Track");
        } else {
            int v;
            if ((r = sd_bus_message_read(m, "b", &v)) < 0) return r;
            s.h_.shuffle(v);
        }
        return 0;
    }
};

using H = Mpris::Handlers;
using C = MprisCalls;
constexpr auto emits = SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE;
constexpr auto fixed = SD_BUS_VTABLE_PROPERTY_CONST;

static const sd_bus_vtable root_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Raise", "", "", C::raise, 0),
    SD_BUS_METHOD("Quit", "", "", C::call<&H::quit>, 0),
    SD_BUS_PROPERTY("CanQuit", "b", C::get, 0, fixed),
    SD_BUS_PROPERTY("CanRaise", "b", C::get, 0, fixed),
    SD_BUS_PROPERTY("HasTrackList", "b", C::get, 0, fixed),
    SD_BUS_PROPERTY("Identity", "s", C::get, 0, fixed),
    SD_BUS_PROPERTY("DesktopEntry", "s", C::get, 0, fixed),
    SD_BUS_PROPERTY("SupportedUriSchemes", "as", C::get, 0, fixed),
    SD_BUS_PROPERTY("SupportedMimeTypes", "as", C::get, 0, fixed),
    SD_BUS_VTABLE_END,
};

static const sd_bus_vtable player_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Next", "", "", C::call<&H::next>, 0),
    SD_BUS_METHOD("Previous", "", "", C::call<&H::previous>, 0),
    SD_BUS_METHOD("Pause", "", "", C::call<&H::pause>, 0),
    SD_BUS_METHOD("PlayPause", "", "", C::call<&H::toggle>, 0),
    SD_BUS_METHOD("Stop", "", "", C::call<&H::stop>, 0),
    SD_BUS_METHOD("Play", "", "", C::call<&H::play>, 0),
    SD_BUS_METHOD_WITH_NAMES("Seek", "x", SD_BUS_PARAM(Offset), "", , C::seek, 0),
    SD_BUS_METHOD_WITH_NAMES("SetPosition", "ox", SD_BUS_PARAM(TrackId) SD_BUS_PARAM(Position), "", , C::set_position, 0),
    SD_BUS_METHOD_WITH_NAMES("OpenUri", "s", SD_BUS_PARAM(Uri), "", , C::open_uri, 0),
    SD_BUS_SIGNAL_WITH_NAMES("Seeked", "x", SD_BUS_PARAM(Position), 0),
    SD_BUS_PROPERTY("PlaybackStatus", "s", C::get, 0, emits),
    SD_BUS_WRITABLE_PROPERTY("LoopStatus", "s", C::get, C::set, 0, emits),
    SD_BUS_PROPERTY("Rate", "d", C::get, 0, fixed),
    SD_BUS_WRITABLE_PROPERTY("Shuffle", "b", C::get, C::set, 0, emits),
    SD_BUS_PROPERTY("Metadata", "a{sv}", C::get, 0, emits),
    SD_BUS_WRITABLE_PROPERTY("Volume", "d", C::get, C::set, 0, emits),
    SD_BUS_PROPERTY("Position", "x", C::get, 0, 0),
    SD_BUS_PROPERTY("MinimumRate", "d", C::get, 0, fixed),
    SD_BUS_PROPERTY("MaximumRate", "d", C::get, 0, fixed),
    SD_BUS_PROPERTY("CanGoNext", "b", C::get, 0, emits),
    SD_BUS_PROPERTY("CanGoPrevious", "b", C::get, 0, emits),
    SD_BUS_PROPERTY("CanPlay", "b", C::get, 0, emits),
    SD_BUS_PROPERTY("CanPause", "b", C::get, 0, emits),
    SD_BUS_PROPERTY("CanSeek", "b", C::get, 0, emits),
    SD_BUS_PROPERTY("CanControl", "b", C::get, 0, fixed),
    SD_BUS_VTABLE_END,
};

static const sd_bus_vtable instance_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD_WITH_NAMES("Activate", "s", SD_BUS_PARAM(Token), "", , C::activate, 0),
    SD_BUS_VTABLE_END,
};

std::unique_ptr<Mpris> Mpris::create(Bus& bus, Handlers h) {
    std::unique_ptr<Mpris> m(new Mpris(bus, std::move(h)));
    sd_bus* b = bus.get();
    sd_bus_add_object_vtable(b, &m->slots_[0], path, root_iface, root_vtable, m.get());
    sd_bus_add_object_vtable(b, &m->slots_[1], path, player_iface, player_vtable, m.get());
    sd_bus_add_object_vtable(b, &m->slots_[2], path, instance_iface, instance_vtable, m.get());
    // The object exists before the name appears, so controls that react to the name find it at once.
    if (int r = sd_bus_request_name(b, name, 0); r < 0) {
        log::warn("{} is taken ({}); running without media controls", name, std::strerror(-r));
        return nullptr;
    }
    bus.arm();
    return m;
}

Mpris::~Mpris() {
    for (sd_bus_slot* s : slots_) sd_bus_slot_unref(s);
}

void Mpris::update(const MprisState& s) {
    std::vector<const char*> changed;
    auto check = [&](bool differs, const char* property) {
        if (differs) changed.push_back(property);
    };
    const MprisState& o = state_;
    check(s.status != o.status, "PlaybackStatus");
    check(s.track != o.track || s.title != o.title || s.artist != o.artist || s.album != o.album || s.art != o.art || s.length != o.length,
          "Metadata");
    check(s.volume != o.volume, "Volume");
    check(s.loop_one != o.loop_one, "LoopStatus");
    check(s.shuffle != o.shuffle, "Shuffle");
    check(s.can_play != o.can_play, "CanPlay");
    check(s.can_play != o.can_play, "CanPause");
    check(s.can_next != o.can_next, "CanGoNext");
    check(s.can_prev != o.can_prev, "CanGoPrevious");
    check(s.can_seek != o.can_seek, "CanSeek");
    if (changed.empty()) return;
    state_ = s;
    changed.push_back(nullptr);
    sd_bus_emit_properties_changed_strv(bus_.get(), path, player_iface, const_cast<char**>(changed.data()));
    bus_.arm();
}

void Mpris::seeked(double position) {
    sd_bus_emit_signal(bus_.get(), path, player_iface, "Seeked", "x", int64_t(position * 1e6));
    bus_.arm();
}

bool hand_over_to_running(Bus& bus) {
    const char* token = std::getenv("XDG_ACTIVATION_TOKEN");
    sd_bus_set_method_call_timeout(bus.get(), 2'000'000);
    return sd_bus_call_method(bus.get(), name, path, instance_iface, "Activate", nullptr, nullptr, "s", token ? token : "") >= 0;
}

}  // namespace lyren
