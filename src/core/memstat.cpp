#include "core/memstat.hpp"

#include <dirent.h>
#include <malloc.h>
#include <unistd.h>

#include <charconv>
#include <cstring>
#include <fstream>
#include <set>
#include <string_view>

namespace lyren {

static long field_kib(std::string_view line) {
    auto p = line.find_first_of("0123456789");
    long v = 0;
    if (p != std::string_view::npos) std::from_chars(line.data() + p, line.data() + line.size(), v);
    return v;
}

// DRM fdinfo sizes carry their own unit: bytes, KiB or MiB.
static long drm_kib(std::string_view line) {
    long v = field_kib(line);
    if (line.ends_with("MiB")) return v * 1024;
    if (line.ends_with("KiB")) return v;
    return v / 1024;
}

MemStat mem_sample() {
    MemStat m;
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);) {
        if (line.starts_with("VmRSS:")) m.rss = field_kib(line);
        else if (line.starts_with("RssAnon:")) m.anon = field_kib(line);
        else if (line.starts_with("RssFile:")) m.file = field_kib(line);
        else if (line.starts_with("RssShmem:")) m.shmem = field_kib(line);
    }

    // Several fds can refer to the same DRM client; count each client id once.
    std::set<std::string> seen;
    if (DIR* dir = opendir("/proc/self/fd")) {
        while (dirent* e = readdir(dir)) {
            char target[64]{};
            std::string fd = e->d_name;
            if (readlink(("/proc/self/fd/" + fd).c_str(), target, sizeof target - 1) <= 0 || std::strncmp(target, "/dev/dri/", 9)) continue;
            std::ifstream info("/proc/self/fdinfo/" + fd);
            std::string client;
            long resident = 0;
            for (std::string line; std::getline(info, line);) {
                if (line.starts_with("drm-client-id:")) client = line;
                else if (line.starts_with("drm-resident-")) resident += drm_kib(std::string_view(line).substr(line.find(':')));
            }
            if (!client.empty() && seen.insert(client).second) m.gpu += resident;
        }
        closedir(dir);
    }

    struct mallinfo2 mi = mallinfo2();
    m.heap_used = long(mi.uordblks / 1024);
    m.heap_free = long(mi.fordblks / 1024);
    return m;
}

void mem_trim() { malloc_trim(0); }

}  // namespace lyren
