#pragma once
#include <atomic>
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

#include "sys/config.hpp"

namespace lyren::gfx {

    // Must run before the first Vulkan call: loads only the chosen GPU's driver and no implicit layers, so the
    // other GPU is never woken and its driver never mapped. Variables already set by the user win.
    void select_driver(GpuChoice gpu);

    class Context {
    public:
        explicit Context(wl_display *display);
        ~Context();
        Context(const Context &) = delete;
        Context &operator=(const Context &) = delete;

        uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) const;
        VkDeviceMemory allocate(const VkMemoryRequirements &req, VkMemoryPropertyFlags flags, bool device_address = false) const;
        unsigned validation_errors() const {
            return validation_errors_.load();
        }

        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t queue_family = 0;
        bool validation = false;

    private:
        VkPhysicalDeviceMemoryProperties mem_props_{};
        VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
        std::atomic<unsigned> validation_errors_{0};
    };

#define VK_CHECK(expr) ::lyren::gfx::check((expr), #expr)
    void check(VkResult r, const char *what);

} // namespace lyren::gfx
