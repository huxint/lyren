#include "gfx/renderer.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

#include "core/log.hpp"

namespace lyren::gfx {

    alignas(4) static const unsigned char vert_spv[] = {
#embed "ui.vert.spv"
    };
    alignas(4) static const unsigned char frag_spv[] = {
#embed "ui.frag.spv"
    };

    constexpr uint32_t max_textures = 1024;

    struct PushConstants {
        VkDeviceAddress prims;
        std::array<float, 2> screen;
    };

    static void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                              VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
        VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = src_stage,
                                .srcAccessMask = src_access,
                                .dstStageMask = dst_stage,
                                .dstAccessMask = dst_access,
                                .oldLayout = from,
                                .newLayout = to,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = image,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    static uint32_t texel_size(VkFormat f) {
        return f == VK_FORMAT_R8_UNORM ? 1 : 4;
    }

    Renderer::Renderer(Context &ctx)
    : ctx_(ctx) {
        VkDevice dev = ctx_.device;
        VkCommandPoolCreateInfo pci{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = ctx_.queue_family};
        VK_CHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool_));

        VkSamplerCreateInfo sci{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_NEAREST,
                                .minFilter = VK_FILTER_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &nearest_));
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &linear_));

        VkDescriptorBindingFlags bflags =
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = 1, .pBindingFlags = &bflags};
        VkDescriptorSetLayoutBinding binding{
            .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = max_textures, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
        VkDescriptorSetLayoutCreateInfo dli{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .pNext = &bfi,
                                            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                                            .bindingCount = 1,
                                            .pBindings = &binding};
        VK_CHECK(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &set_layout_));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, max_textures};
        VkDescriptorPoolCreateInfo dpi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
                                       .maxSets = 1,
                                       .poolSizeCount = 1,
                                       .pPoolSizes = &size};
        VK_CHECK(vkCreateDescriptorPool(dev, &dpi, nullptr, &desc_pool_));
        VkDescriptorSetAllocateInfo dai{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = desc_pool_, .descriptorSetCount = 1, .pSetLayouts = &set_layout_};
        VK_CHECK(vkAllocateDescriptorSets(dev, &dai, &set_));

        VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
        VkPipelineLayoutCreateInfo pli{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout_, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
        VK_CHECK(vkCreatePipelineLayout(dev, &pli, nullptr, &layout_));
    }

    Renderer::~Renderer() {
        wait_idle();
        VkDevice dev = ctx_.device;
        while (!targets_.empty()) {
            destroy_target(targets_.back());
        }
        completed_ = submitted_;
        for (auto &r : retired_) {
            r.release();
        }
        retired_.clear();
        for (TextureId id = 0; id < textures_.size(); ++id) {
            if (textures_[id].image) {
                vkDestroyImageView(dev, textures_[id].view, nullptr);
                vkDestroyImage(dev, textures_[id].image, nullptr);
                vkFreeMemory(dev, textures_[id].memory, nullptr);
            }
        }
        if (staging_.buffer) {
            vkDestroyBuffer(dev, staging_.buffer, nullptr);
            vkFreeMemory(dev, staging_.memory, nullptr);
        }
        for (auto &[f, p] : pipelines_) {
            vkDestroyPipeline(dev, p, nullptr);
        }
        vkDestroyPipelineLayout(dev, layout_, nullptr);
        vkDestroyDescriptorPool(dev, desc_pool_, nullptr);
        vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
        vkDestroySampler(dev, nearest_, nullptr);
        vkDestroySampler(dev, linear_, nullptr);
        vkDestroyCommandPool(dev, pool_, nullptr);
    }

    VkPipeline Renderer::pipeline_for(VkFormat format) {
        for (auto &[f, p] : pipelines_) {
            if (f == format) {
                return p;
            }
        }
        VkDevice dev = ctx_.device;
        auto module = [&](const unsigned char *code, size_t size) {
            VkShaderModuleCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = reinterpret_cast<const uint32_t *>(code)};
            VkShaderModule m;
            VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
            return m;
        };
        VkShaderModule vs = module(vert_spv, sizeof vert_spv), fs = module(frag_spv, sizeof frag_spv);
        auto stages = std::to_array<VkPipelineShaderStageCreateInfo>({
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
        });
        VkPipelineVertexInputStateCreateInfo vi{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
        VkPipelineViewportStateCreateInfo vp{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo rs{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo ms{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState blend{.blendEnable = VK_TRUE,
                                                  .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                                  .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                  .colorBlendOp = VK_BLEND_OP_ADD,
                                                  .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                  .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                  .alphaBlendOp = VK_BLEND_OP_ADD,
                                                  .colorWriteMask = 0xf};
        VkPipelineColorBlendStateCreateInfo cb{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend};
        std::array dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = static_cast<uint32_t>(dyn.size()), .pDynamicStates = dyn.data()};
        VkPipelineRenderingCreateInfo ri{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
        VkGraphicsPipelineCreateInfo gp{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                        .pNext = &ri,
                                        .stageCount = static_cast<uint32_t>(stages.size()),
                                        .pStages = stages.data(),
                                        .pVertexInputState = &vi,
                                        .pInputAssemblyState = &ia,
                                        .pViewportState = &vp,
                                        .pRasterizationState = &rs,
                                        .pMultisampleState = &ms,
                                        .pColorBlendState = &cb,
                                        .pDynamicState = &ds,
                                        .layout = layout_};
        VkPipeline p;
        VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &p));
        vkDestroyShaderModule(dev, vs, nullptr);
        vkDestroyShaderModule(dev, fs, nullptr);
        pipelines_.emplace_back(format, p);
        return p;
    }

    Target *Renderer::create_target(wl_display *display, wl_surface *surface) {
        auto *t = new Target;
        VkWaylandSurfaceCreateInfoKHR ci{.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, .display = display, .surface = surface};
        VK_CHECK(vkCreateWaylandSurfaceKHR(ctx_.instance, &ci, nullptr, &t->surface));
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(ctx_.physical, ctx_.queue_family, t->surface, &supported);
        VkCommandBufferAllocateInfo ai{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool_, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        for (auto &s : t->slots) {
            VK_CHECK(vkAllocateCommandBuffers(ctx_.device, &ai, &s.cmd));
            VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
            VK_CHECK(vkCreateFence(ctx_.device, &fci, nullptr, &s.fence));
            VkSemaphoreCreateInfo sci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vkCreateSemaphore(ctx_.device, &sci, nullptr, &s.acquired));
        }
        targets_.push_back(t);
        return t;
    }

    void Renderer::release_swapchain(Target &t) {
        for (auto v : t.views) {
            vkDestroyImageView(ctx_.device, v, nullptr);
        }
        for (auto s : t.rendered) {
            vkDestroySemaphore(ctx_.device, s, nullptr);
        }
        t.views.clear();
        t.rendered.clear();
        t.images.clear();
    }

    void Renderer::destroy_target(Target *t) {
        wait_idle();
        VkDevice dev = ctx_.device;
        release_swapchain(*t);
        if (t->swapchain) {
            vkDestroySwapchainKHR(dev, t->swapchain, nullptr);
        }
        vkDestroySurfaceKHR(ctx_.instance, t->surface, nullptr);
        for (auto &s : t->slots) {
            vkFreeCommandBuffers(dev, pool_, 1, &s.cmd);
            vkDestroyFence(dev, s.fence, nullptr);
            vkDestroySemaphore(dev, s.acquired, nullptr);
            if (s.prims) {
                vkDestroyBuffer(dev, s.prims, nullptr);
                vkFreeMemory(dev, s.prims_mem, nullptr);
            }
        }
        std::erase(targets_, t);
        delete t;
    }

    void Renderer::resize(Target &t, uint32_t width, uint32_t height) {
        if (width == t.want_width && height == t.want_height) {
            return;
        }
        t.want_width = width, t.want_height = height;
        t.stale = true;
    }

    void Renderer::recreate(Target &t) {
        VkDevice dev = ctx_.device;
        vkQueueWaitIdle(ctx_.queue);
        completed_ = submitted_;
        poll_completed();

        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physical, t.surface, &caps);
        if (t.format == VK_FORMAT_UNDEFINED) {
            uint32_t n = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical, t.surface, &n, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(n);
            vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical, t.surface, &n, formats.data());
            t.format = formats[0].format;
            for (auto &f : formats) {
                if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    t.format = f.format;
                    break;
                }
            }
        }
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent = {t.want_width, t.want_height};
        }
        extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);

        VkSwapchainCreateInfoKHR ci{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = t.surface,
            .minImageCount = std::max(caps.minImageCount, 2u),
            .imageFormat = t.format,
            .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
            .imageExtent = extent,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = caps.currentTransform,
            .compositeAlpha =
                (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = VK_PRESENT_MODE_FIFO_KHR,
            .clipped = VK_TRUE,
            .oldSwapchain = t.swapchain,
        };
        if (caps.maxImageCount) {
            ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
        }
        VkSwapchainKHR swapchain;
        VK_CHECK(vkCreateSwapchainKHR(dev, &ci, nullptr, &swapchain));
        release_swapchain(t);
        if (t.swapchain) {
            vkDestroySwapchainKHR(dev, t.swapchain, nullptr);
        }
        t.swapchain = swapchain;
        t.width = extent.width, t.height = extent.height;

        uint32_t n = 0;
        vkGetSwapchainImagesKHR(dev, swapchain, &n, nullptr);
        t.images.resize(n);
        vkGetSwapchainImagesKHR(dev, swapchain, &n, t.images.data());
        for (VkImage img : t.images) {
            VkImageViewCreateInfo vci{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                      .image = img,
                                      .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                      .format = t.format,
                                      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            VK_CHECK(vkCreateImageView(dev, &vci, nullptr, &t.views.emplace_back()));
            VkSemaphoreCreateInfo sci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vkCreateSemaphore(dev, &sci, nullptr, &t.rendered.emplace_back()));
        }
        t.stale = false;
    }

    void Renderer::ensure_capacity(Target::Slot &s, size_t count) {
        if (count <= s.capacity) {
            return;
        }
        VkDevice dev = ctx_.device;
        if (s.prims) {
            vkDestroyBuffer(dev, s.prims, nullptr);
            vkFreeMemory(dev, s.prims_mem, nullptr);
        }
        s.capacity = std::max<size_t>(256, std::bit_ceil(count));
        VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = s.capacity * sizeof(Prim),
                               .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
        VK_CHECK(vkCreateBuffer(dev, &bci, nullptr, &s.prims));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, s.prims, &req);
        s.prims_mem = ctx_.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        vkBindBufferMemory(dev, s.prims, s.prims_mem, 0);
        vkMapMemory(dev, s.prims_mem, 0, VK_WHOLE_SIZE, 0, &s.prims_map);
        VkBufferDeviceAddressInfo bai{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = s.prims};
        s.prims_addr = vkGetBufferDeviceAddress(dev, &bai);
    }

    void Renderer::draw(Target &t, std::span<const Prim> prims, const std::array<float, 4> &clear, std::function_ref<void()> before_present) {
        VkDevice dev = ctx_.device;
        auto &s = t.slots[t.slot];
        t.slot ^= 1;
        VK_CHECK(vkWaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX));
        completed_ = std::max(completed_, s.serial);
        poll_completed();

        uint32_t index = 0;
        for (int attempt = 0;; ++attempt) {
            if (t.stale) {
                recreate(t);
            }
            VkResult r = vkAcquireNextImageKHR(dev, t.swapchain, UINT64_MAX, s.acquired, VK_NULL_HANDLE, &index);
            if (r == VK_ERROR_OUT_OF_DATE_KHR && attempt == 0) {
                t.stale = true;
                continue;
            }
            VK_CHECK(r);
            if (r == VK_SUBOPTIMAL_KHR) {
                t.stale = true;
            }
            break;
        }
        VK_CHECK(vkResetFences(dev, 1, &s.fence));

        ensure_capacity(s, prims.size());
        std::memcpy(s.prims_map, prims.data(), prims.size_bytes());

        VkCommandBuffer cmd = s.cmd;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cmd, &bi);
        bool uploaded = !copies_.empty();
        record_uploads(cmd);
        image_barrier(cmd, t.images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo att{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                      .imageView = t.views[index],
                                      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                      .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        std::ranges::copy(clear, att.clearValue.color.float32);
        VkRenderingInfo ri{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {t.width, t.height}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att};
        vkCmdBeginRendering(cmd, &ri);
        if (!prims.empty()) {
            VkViewport viewport{0, 0, static_cast<float>(t.width), static_cast<float>(t.height), 0, 1};
            VkRect2D scissor{{0, 0}, {t.width, t.height}};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_for(t.format));
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
            PushConstants pc{s.prims_addr, {static_cast<float>(t.width), static_cast<float>(t.height)}};
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
            vkCmdDraw(cmd, 4, static_cast<uint32_t>(prims.size()), 0, 0);
        }
        vkCmdEndRendering(cmd);
        VkBuffer readback = VK_NULL_HANDLE;
        VkDeviceMemory readback_mem = VK_NULL_HANDLE;
        if (capture_ && capture_target_ == &t) {
            VkBufferCreateInfo bci{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = static_cast<VkDeviceSize>(t.width) * t.height * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            VK_CHECK(vkCreateBuffer(dev, &bci, nullptr, &readback));
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(dev, readback, &req);
            readback_mem = ctx_.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkBindBufferMemory(dev, readback, readback_mem, 0);
            image_barrier(cmd, t.images[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy region{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {t.width, t.height, 1}};
            vkCmdCopyImageToBuffer(cmd, t.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
            image_barrier(cmd, t.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_2_NONE, 0);
        } else {
            image_barrier(cmd, t.images[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
        }
        vkEndCommandBuffer(cmd);

        VkSemaphoreSubmitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = s.acquired, .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
        VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = t.rendered[index], .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        VkCommandBufferSubmitInfo cbi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cmd};
        VkSubmitInfo2 si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                         .waitSemaphoreInfoCount = 1,
                         .pWaitSemaphoreInfos = &wait,
                         .commandBufferInfoCount = 1,
                         .pCommandBufferInfos = &cbi,
                         .signalSemaphoreInfoCount = 1,
                         .pSignalSemaphoreInfos = &signal};
        VK_CHECK(vkQueueSubmit2(ctx_.queue, 1, &si, s.fence));
        s.serial = ++submitted_;
        if (uploaded) {
            // The staging buffer now belongs to this submission.
            retire([this, st = staging_] {
                vkDestroyBuffer(ctx_.device, st.buffer, nullptr);
                vkFreeMemory(ctx_.device, st.memory, nullptr);
            });
            staging_ = {};
        }

        before_present();
        VkPresentInfoKHR pi{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                            .waitSemaphoreCount = 1,
                            .pWaitSemaphores = &t.rendered[index],
                            .swapchainCount = 1,
                            .pSwapchains = &t.swapchain,
                            .pImageIndices = &index};
        VkResult r = vkQueuePresentKHR(ctx_.queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            t.stale = true;
        } else {
            VK_CHECK(r);
        }
        if (readback) {
            VK_CHECK(vkWaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX));
            void *map;
            vkMapMemory(dev, readback_mem, 0, VK_WHOLE_SIZE, 0, &map);
            auto *pixels = static_cast<uint8_t *>(map);
            if (t.format == VK_FORMAT_R8G8B8A8_UNORM) {
                for (size_t i = 0; i < static_cast<size_t>(t.width) * t.height * 4; i += 4) {
                    std::swap(pixels[i], pixels[i + 2]);
                }
            }
            std::exchange(capture_, nullptr)(pixels, t.width, t.height);
            vkUnmapMemory(dev, readback_mem);
            vkDestroyBuffer(dev, readback, nullptr);
            vkFreeMemory(dev, readback_mem, nullptr);
        }
    }

    TextureId Renderer::create_texture(uint32_t width, uint32_t height, VkFormat format, bool smooth) {
        VkDevice dev = ctx_.device;
        if (free_slots_.empty()) {
            poll_completed();
        }
        TextureId id;
        if (!free_slots_.empty()) {
            id = free_slots_.back();
            free_slots_.pop_back();
        } else {
            id = TextureId(textures_.size());
            textures_.emplace_back();
        }
        Texture &t = textures_[id];
        t = {.texel = texel_size(format)};
        VkImageCreateInfo ici{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {width, height, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VK_CHECK(vkCreateImage(dev, &ici, nullptr, &t.image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, t.image, &req);
        t.memory = ctx_.allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkBindImageMemory(dev, t.image, t.memory, 0);
        VkImageViewCreateInfo vci{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .image = t.image,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                  .format = format,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(dev, &vci, nullptr, &t.view));
        VkDescriptorImageInfo dii{smooth ? linear_ : nearest_, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = set_,
                               .dstArrayElement = id,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &dii};
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        return id;
    }

    void Renderer::upload(TextureId tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *pixels, uint32_t pitch) {
        const Texture &t = textures_[tex];
        size_t row = static_cast<size_t>(w) * t.texel, bytes = row * h;
        size_t offset = (staging_.used + 15) & ~static_cast<size_t>(15);
        if (offset + bytes > staging_.size) {
            // Grow, keeping what is already staged for this frame.
            size_t size = std::max({static_cast<size_t>(256) << 10, staging_.size * 2, offset + bytes});
            Staging grown;
            VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
            VK_CHECK(vkCreateBuffer(ctx_.device, &bci, nullptr, &grown.buffer));
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(ctx_.device, grown.buffer, &req);
            grown.memory = ctx_.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkBindBufferMemory(ctx_.device, grown.buffer, grown.memory, 0);
            vkMapMemory(ctx_.device, grown.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void **>(&grown.map));
            grown.size = size;
            grown.used = staging_.used;
            if (staging_.buffer) {
                std::memcpy(grown.map, staging_.map, staging_.used);
                vkDestroyBuffer(ctx_.device, staging_.buffer, nullptr);
                vkFreeMemory(ctx_.device, staging_.memory, nullptr);
            }
            staging_ = grown;
        }
        auto *src = static_cast<const uint8_t *>(pixels);
        for (uint32_t r = 0; r < h; ++r) {
            std::memcpy(staging_.map + offset + r * row, src + static_cast<size_t>(r) * pitch, row);
        }
        staging_.used = offset + bytes;
        copies_.push_back({tex,
                           {.bufferOffset = offset,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0},
                            .imageExtent = {w, h, 1}}});
        ++texture_version_;
    }

    void Renderer::record_uploads(VkCommandBuffer cmd) {
        if (copies_.empty()) {
            return;
        }
        std::ranges::stable_sort(copies_, {}, &Copy::tex);
        for (size_t i = 0; i < copies_.size();) {
            TextureId id = copies_[i].tex;
            size_t j = i;
            std::vector<VkBufferImageCopy> regions;
            while (j < copies_.size() && copies_[j].tex == id) {
                regions.push_back(copies_[j++].region);
            }
            Texture &t = textures_[id];
            image_barrier(cmd, t.image, t.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(cmd, staging_.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
            image_barrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            i = j;
        }
        copies_.clear();
    }

    void Renderer::destroy_texture(TextureId id) {
        Texture t = textures_[id];
        std::erase_if(copies_, [id](const Copy &c) {
            return c.tex == id;
        });
        textures_[id] = {};
        retire([this, t, id] {
            vkDestroyImageView(ctx_.device, t.view, nullptr);
            vkDestroyImage(ctx_.device, t.image, nullptr);
            vkFreeMemory(ctx_.device, t.memory, nullptr);
            free_slots_.push_back(id);
        });
    }

    void Renderer::retire(std::move_only_function<void()> release) {
        if (submitted_ == completed_) {
            release();
        } else {
            retired_.push_back({submitted_, std::move(release)});
        }
    }

    void Renderer::poll_completed() {
        for (Target *t : targets_) {
            for (auto &s : t->slots) {
                if (s.serial > completed_ && vkGetFenceStatus(ctx_.device, s.fence) == VK_SUCCESS) {
                    completed_ = s.serial;
                }
            }
        }
        std::erase_if(retired_, [this](Retired &r) {
            if (r.serial > completed_) {
                return false;
            }
            r.release();
            return true;
        });
    }

} // namespace lyren::gfx
