#pragma once
#include <string>

#include "core/result.hpp"
#include "core/string_map.hpp"
#include "core/task.hpp"

namespace lyren::creds {

    using Credentials = StringMap<StringMap<std::string>>;
    enum class SaveMode {
        replace,
        back_up_previous
    };

    // ~/.local/state/lyren/credentials holds the plaintext encrypted by systemd-creds (user scope, over varlink; asking
    // for withKey=auto silently yields a system-scope credential that needs polkit to decrypt), so
    // only this user on this machine can read it. A missing file starts an empty store.
    Task<Result<Credentials>> load(Loop &loop);
    Task<Result<void>> save(Loop &loop, std::string plaintext, SaveMode mode = SaveMode::replace);
    // Blocking save for shutdown paths when the event loop is no longer running.
    Result<void> save_sync(std::string_view plaintext);

    // What the plaintext holds: each service's cookie session.
    std::string to_json(const Credentials &c);
    Result<Credentials> from_json(std::string_view json);

} // namespace lyren::creds
