#pragma once
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

#include <string>
#include <vector>

#include "sys/config.hpp"

namespace lyren::gfx {

// Must run before the first Vulkan call: loads only the chosen GPU's driver and no implicit layers, so the
// other GPU is never woken and its driver never mapped. Variables already set by the user win.
void select_driver(GpuChoice gpu);

struct HeapUsage {
    uint64_t used = 0, budget = 0;
    bool device_local = false;
};

class Context {
public:
    explicit Context(wl_display* display);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) const;
    VkDeviceMemory allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags flags, bool device_address = false) const;
    std::vector<HeapUsage> heap_usage() const;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    std::string device_name, driver_name;
    bool memory_budget = false;
    bool validation = false;

private:
    VkPhysicalDeviceMemoryProperties mem_props_{};
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
};

#define VK_CHECK(expr) ::lyren::gfx::check((expr), #expr)
void check(VkResult r, const char* what);

}  // namespace lyren::gfx
