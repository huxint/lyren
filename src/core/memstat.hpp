#pragma once
#include <cstddef>
#include <string>

namespace lyren {

// Process memory in KiB, from /proc/self/status, DRM fdinfo and mallinfo2.
struct MemStat {
    long rss = 0, anon = 0, file = 0, shmem = 0;
    long gpu = 0;  // DRM resident (integrated GPU memory lives in system RAM)
    long heap_used = 0, heap_free = 0;
};

MemStat mem_sample();
// Gives freed heap pages back to the kernel.
void mem_trim();

}  // namespace lyren
