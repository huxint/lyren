#include "gfx/context.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>

#include "core/log.hpp"

namespace lyren::gfx {

void check(VkResult r, const char* what) {
    if (r >= 0) return;
    log::error("{} failed: VkResult {}", what, int(r));
    std::abort();
}

static std::string read_line(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

void select_driver(GpuChoice gpu) {
    // The integrated GPU is the one the firmware used for the boot display.
    std::string vendor;
    for (int i = 0; i < 8 && vendor.empty(); ++i) {
        std::string dev = "/sys/class/drm/card" + std::to_string(i) + "/device/";
        bool boot = read_line(dev + "boot_vga") == "1";
        if (boot == (gpu == GpuChoice::integrated) && !read_line(dev + "vendor").empty()) vendor = read_line(dev + "vendor");
    }
    const char* glob = vendor == "0x8086" ? "*intel_icd*" : vendor == "0x1002" ? "*radeon_icd*" : vendor == "0x10de" ? "*nvidia*" : nullptr;
    if (glob) setenv("VK_LOADER_DRIVERS_SELECT", glob, 0);
    setenv("VK_LOADER_LAYERS_DISABLE", "~implicit~", 0);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL on_validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) log::error("vulkan: {}", data->pMessage);
    else log::warn("vulkan: {}", data->pMessage);
    return VK_FALSE;
}

Context::Context(wl_display* display) {
#ifdef NDEBUG
    bool want_validation = std::getenv("LYREN_VALIDATION") != nullptr;
#else
    bool want_validation = true;
#endif
    if (want_validation) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> layers(n);
        vkEnumerateInstanceLayerProperties(&n, layers.data());
        for (auto& l : layers) validation |= !std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation");
        if (!validation) log::info("validation layer not installed (pacman -S vulkan-validation-layers)");
    }

    VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Lyren", .apiVersion = VK_API_VERSION_1_3};
    std::vector<const char*> exts{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (validation) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    VkInstanceCreateInfo ici{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &app,
                             .enabledLayerCount = validation ? 1u : 0u,
                             .ppEnabledLayerNames = &layer,
                             .enabledExtensionCount = uint32_t(exts.size()),
                             .ppEnabledExtensionNames = exts.data()};
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));
    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT mci{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = on_validation};
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        create(instance, &mci, nullptr, &messenger_);
    }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance, &n, devices.data());
    for (auto pd : devices) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> families(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, families.data());
        for (uint32_t i = 0; i < qn && !physical; ++i)
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && vkGetPhysicalDeviceWaylandPresentationSupportKHR(pd, i, display))
                physical = pd, queue_family = i;
        if (physical) break;
    }
    if (!physical) {
        log::error("no Vulkan device can present to this Wayland display");
        std::exit(1);
    }

    VkPhysicalDeviceDriverProperties driver{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(physical, &props);
    device_name = props.properties.deviceName;
    driver_name = driver.driverName;
    vkGetPhysicalDeviceMemoryProperties(physical, &mem_props_);

    VkPhysicalDeviceVulkan12Features have12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features have13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &have12};
    VkPhysicalDeviceFeatures2 have{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have13};
    vkGetPhysicalDeviceFeatures2(physical, &have);
    if (props.properties.apiVersion < VK_API_VERSION_1_3 || !have13.dynamicRendering || !have13.synchronization2 || !have12.bufferDeviceAddress ||
        !have12.runtimeDescriptorArray || !have12.descriptorBindingPartiallyBound || !have12.descriptorBindingSampledImageUpdateAfterBind ||
        !have12.descriptorBindingUpdateUnusedWhilePending || !have12.shaderSampledImageArrayNonUniformIndexing) {
        log::error("{} lacks Vulkan 1.3 with dynamic rendering, buffer device address and descriptor indexing", device_name);
        std::exit(1);
    }

    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> available(en);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &en, available.data());
    for (auto& e : available) memory_budget |= !std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    std::vector<const char*> dexts{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (memory_budget) dexts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    VkPhysicalDeviceVulkan12Features f12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                         .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
                                         .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
                                         .descriptorBindingUpdateUnusedWhilePending = VK_TRUE,
                                         .descriptorBindingPartiallyBound = VK_TRUE,
                                         .runtimeDescriptorArray = VK_TRUE,
                                         .bufferDeviceAddress = VK_TRUE};
    VkPhysicalDeviceVulkan13Features f13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                         .pNext = &f12,
                                         .synchronization2 = VK_TRUE,
                                         .dynamicRendering = VK_TRUE};
    float priority = 1;
    VkDeviceQueueCreateInfo qci{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                .queueFamilyIndex = queue_family,
                                .queueCount = 1,
                                .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                           .pNext = &f13,
                           .queueCreateInfoCount = 1,
                           .pQueueCreateInfos = &qci,
                           .enabledExtensionCount = uint32_t(dexts.size()),
                           .ppEnabledExtensionNames = dexts.data()};
    VK_CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    log::info("GPU: {} ({}){}", device_name, driver_name, validation ? ", validation on" : "");
}

Context::~Context() {
    vkDestroyDevice(device, nullptr);
    if (messenger_) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroy(instance, messenger_, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
}

uint32_t Context::memory_type(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mem_props_.memoryTypes[i].propertyFlags & want) == want) return i;
    log::error("no Vulkan memory type with flags {:#x}", want);
    std::abort();
}

VkDeviceMemory Context::allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags flags, bool device_address) const {
    VkMemoryAllocateFlagsInfo fi{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                            .pNext = device_address ? &fi : nullptr,
                            .allocationSize = req.size,
                            .memoryTypeIndex = memory_type(req.memoryTypeBits, flags)};
    VkDeviceMemory mem;
    VK_CHECK(vkAllocateMemory(device, &ai, nullptr, &mem));
    return mem;
}

std::vector<HeapUsage> Context::heap_usage() const {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = memory_budget ? &budget : nullptr};
    vkGetPhysicalDeviceMemoryProperties2(physical, &mp);
    std::vector<HeapUsage> out;
    for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount; ++i)
        out.push_back({budget.heapUsage[i], budget.heapBudget[i], bool(mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)});
    return out;
}

}  // namespace lyren::gfx
