#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "gfx/context.hpp"

namespace lyren::gfx {

enum class Kind : uint32_t { rect = 0, shadow = 1, glyph = 2, image = 3 };
enum Flag : uint32_t { flag_gradient = 1, flag_cutout = 2 };

// One instanced quad; layout matches shaders/prim.glsl. Coordinates are buffer pixels, colours premultiplied.
struct Prim {
    float rect[4];
    float clip[4];
    float uv[4];
    float color[4];
    float color2[4];
    float radii[4];
    float params[4];  // border width, shadow sigma
    uint32_t info[4];  // kind, texture, flags
};
static_assert(sizeof(Prim) == 128);

using TextureId = uint32_t;

// Swapchain for one wl_surface plus its per-frame resources.
struct Target {
    struct Slot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkBuffer prims = VK_NULL_HANDLE;
        VkDeviceMemory prims_mem = VK_NULL_HANDLE;
        void* prims_map = nullptr;
        VkDeviceAddress prims_addr = 0;
        size_t capacity = 0;
        uint64_t serial = 0;
    };
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    uint32_t want_width = 0, want_height = 0;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> rendered;
    Slot slots[2];
    int slot = 0;
    bool stale = true;
};

class Renderer {
public:
    explicit Renderer(Context& ctx);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    Target* create_target(wl_display* display, wl_surface* surface);
    void destroy_target(Target* t);
    // Takes effect at the next draw.
    void resize(Target& t, uint32_t width, uint32_t height);
    // Renders and presents. before_present runs between submit and present (the present commits the surface).
    void draw(Target& t, std::span<const Prim> prims, const float clear[4], const std::function<void()>& before_present);

    // Bindless textures. Uploads are staged now and copied at the start of the next draw.
    TextureId create_texture(uint32_t width, uint32_t height, VkFormat format, bool smooth);
    void upload(TextureId tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* pixels, uint32_t pitch);
    void destroy_texture(TextureId tex);
    size_t texture_bytes() const { return texture_bytes_; }
    size_t texture_count() const { return texture_count_; }

    void wait_idle() { vkDeviceWaitIdle(ctx_.device); }

private:
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32_t width = 0, height = 0, texel = 1;
        bool smooth = false;
    };
    struct Copy {
        TextureId tex;
        VkBufferImageCopy region;
    };
    struct Staging {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* map = nullptr;
        size_t size = 0, used = 0;
    };
    struct Retired {
        uint64_t serial;
        std::function<void()> release;
    };

    void recreate(Target& t);
    void release_swapchain(Target& t);
    void ensure_capacity(Target::Slot& s, size_t count);
    void record_uploads(VkCommandBuffer cmd);
    void poll_completed();
    void retire(std::function<void()> release);
    VkPipeline pipeline_for(VkFormat format);

    Context& ctx_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkSampler nearest_ = VK_NULL_HANDLE, linear_ = VK_NULL_HANDLE;
    std::vector<std::pair<VkFormat, VkPipeline>> pipelines_;
    std::vector<Target*> targets_;

    std::vector<Texture> textures_;
    std::vector<TextureId> free_slots_;
    size_t texture_bytes_ = 0, texture_count_ = 0;
    std::vector<Copy> copies_;
    Staging staging_;
    std::vector<Retired> retired_;
    uint64_t submitted_ = 0, completed_ = 0;
};

}  // namespace lyren::gfx
