#pragma once
#include <map>
#include <string>

#include "core/result.hpp"
#include "core/task.hpp"

namespace lyren::creds {

// ~/.local/state/lyren/credentials holds the plaintext encrypted by systemd-creds (user scope, over varlink; asking
// for withKey=auto silently yields a system-scope credential that needs polkit to decrypt), so
// only this user on this machine can read it. load() returns "" when there is no file yet.
Task<Result<std::string>> load(Loop& loop);
Task<Result<void>> save(Loop& loop, std::string plaintext);

// What the plaintext holds: each service's cookie session.
using Credentials = std::map<std::string, std::map<std::string, std::string>>;
std::string to_json(const Credentials& c);
Credentials from_json(std::string_view json);

}  // namespace lyren::creds
