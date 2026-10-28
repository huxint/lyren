#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "gfx/context.hpp"

namespace lyren::gfx {

    enum class Kind : uint32_t {
        rect = 0,
        shadow = 1,
        glyph = 2,
        image = 3,
        arc = 4,
        wave = 5
    };
    enum Flag : uint32_t {
        flag_gradient = 1,
        flag_cutout = 2,
        flag_hgradient = 4
    };

    // One instanced quad; layout matches shaders/prim.glsl. Coordinates are buffer pixels, colours premultiplied.
    struct Prim {
        std::array<float, 4> rect;
        std::array<float, 4> clip;
        std::array<float, 4> uv; // texture coordinates; wave: x played width
        std::array<float, 4> color;
        std::array<float, 4> color2;
        std::array<float, 4> radii;
        // rect: border width; shadow: _, sigma; arc: thickness, start, sweep (radians, clockwise from 12 o'clock);
        // wave: thickness, amplitude, wavelength, phase; glyph with flag_gradient: wipe x, half its width
        std::array<float, 4> params;
        std::array<uint32_t, 4> info; // kind, texture, flags
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
            void *prims_map = nullptr;
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
        std::array<Slot, 2> slots;
        int slot = 0;
        bool stale = true;
    };

    class Renderer {
    public:
        explicit Renderer(Context &ctx);
        ~Renderer();
        Renderer(const Renderer &) = delete;
        Renderer &operator=(const Renderer &) = delete;

        Target *create_target(wl_display *display, wl_surface *surface);
        void destroy_target(Target *t);
        // Takes effect at the next draw.
        void resize(Target &t, uint32_t width, uint32_t height);
        // Renders and presents. before_present runs between submit and present (the present commits the surface).
        void draw(Target &t, std::span<const Prim> prims, const std::array<float, 4> &clear, std::function_ref<void()> before_present);

        // Bindless textures. Uploads are staged now and copied at the start of the next draw.
        TextureId create_texture(uint32_t width, uint32_t height, VkFormat format, bool smooth);
        void upload(TextureId tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *pixels, uint32_t pitch);
        void destroy_texture(TextureId tex);
        uint64_t texture_version() const {
            return texture_version_;
        }

        void wait_idle() {
            vkDeviceWaitIdle(ctx_.device);
            completed_ = submitted_;
            poll_completed();
        }

        // The next draw of t also reads the image back and hands fn premultiplied BGRA rows, tightly packed.
        void capture_next(const Target &t, std::move_only_function<void(const uint8_t *bgra, uint32_t width, uint32_t height)> fn) {
            capture_target_ = &t;
            capture_ = std::move(fn);
        }

    private:
        struct Texture {
            VkImage image = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            uint32_t texel = 1;
        };
        struct Copy {
            TextureId tex;
            VkBufferImageCopy region;
        };
        struct Staging {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            uint8_t *map = nullptr;
            size_t size = 0, used = 0;
        };
        struct Retired {
            uint64_t serial;
            std::move_only_function<void()> release;
        };

        void recreate(Target &t);
        void release_swapchain(Target &t);
        void ensure_capacity(Target::Slot &s, size_t count);
        void record_uploads(VkCommandBuffer cmd);
        void poll_completed();
        void retire(std::move_only_function<void()> release);
        VkPipeline pipeline_for(VkFormat format);

        Context &ctx_;
        VkCommandPool pool_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
        VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
        VkDescriptorSet set_ = VK_NULL_HANDLE;
        VkPipelineLayout layout_ = VK_NULL_HANDLE;
        VkSampler nearest_ = VK_NULL_HANDLE, linear_ = VK_NULL_HANDLE;
        std::vector<std::pair<VkFormat, VkPipeline>> pipelines_;
        std::vector<Target *> targets_;

        std::vector<Texture> textures_;
        std::vector<TextureId> free_slots_;
        uint64_t texture_version_ = 0;
        std::vector<Copy> copies_;
        Staging staging_;
        std::vector<Retired> retired_;
        uint64_t submitted_ = 0, completed_ = 0;
        std::move_only_function<void(const uint8_t *, uint32_t, uint32_t)> capture_;
        const Target *capture_target_ = nullptr;
    };

} // namespace lyren::gfx
