#include "render/vk/VkRenderer.hpp"

#include <core/Log.hpp>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>

#include <cstring>
#include <filesystem>

namespace oa::render {
namespace {

constexpr std::size_t kSelfCheckSize = 64;
// Must match the colour format the pipeline's render pass was built against (the swapchain prefers sRGB), otherwise
// the offscreen pass is not render-pass-compatible with that pipeline.
constexpr VkFormat kSelfCheckFormat = VK_FORMAT_B8G8R8A8_SRGB;
// Phase one clears and reads back a bare image with no pipeline involved, so it keeps a plain UNORM format whose
// stored bytes are the clear floats times 255.
constexpr VkFormat kClearCheckFormat = VK_FORMAT_R8G8B8A8_UNORM;
// Depth for the level's triangle runs. D32_SFLOAT is the one depth format every desktop driver offers, and no
// stencil is read yet.
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;


// SPIR-V words of a compiled shader module. The build compiles shaders/ to build/shaders (CMake rule), so the
// repository stores GLSL and the loader only has to read 32-bit words.
std::vector<std::uint32_t> readSpv(const std::filesystem::path& path, std::string* error) {
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        if (error != nullptr) *error = "cannot open shader " + path.string();
        return {};
    }
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::rewind(file);
    std::vector<unsigned char> raw(length > 0 ? static_cast<std::size_t>(length) : 0);
    const std::size_t readCount = std::fread(raw.data(), 1, raw.size(), file);
    std::fclose(file);
    if (readCount != raw.size() || raw.size() % 4u != 0u) {
        if (error != nullptr) *error = "shader " + path.string() + " has a truncated SPIR-V body";
        return {};
    }
    std::vector<std::uint32_t> words(raw.size() / 4u);
    std::memcpy(words.data(), raw.data(), raw.size());
    if (words.empty() || words[0] != 0x07230203u) {
        if (error != nullptr) *error = "shader " + path.string() + " is not a SPIR-V module";
        return {};
    }
    return words;
}

uint32_t findMemoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
        if ((bits & (1u << index)) != 0u &&
            (memory.memoryTypes[index].propertyFlags & wanted) == wanted) {
            return index;
        }
    }
    return 0xFFFFFFFFu;
}

bool check(VkResult result, const char* what, std::string* error) {
    if (result == VK_SUCCESS) return true;
    if (error != nullptr) *error = std::string(what) + " failed: vkResult=" + std::to_string(static_cast<int>(result));
    return false;
}

}  // namespace

VKAPI_ATTR VkBool32 VKAPI_CALL VkRenderer::debugMessenger(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                                         const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                         void*) {
    LogLevel level = LogLevel::Info;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) level = LogLevel::Error;
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) level = LogLevel::Warn;
    logMsg(level, "vk-validation", std::string("type=") + std::to_string(type) + " " + data->pMessage);
    return VK_FALSE;
}

VkRenderer::~VkRenderer() { shutdown(); }

bool VkRenderer::init(platform::Window& window, const RendererConfig& config, std::string* error) {
    config_ = config;
    if (!createInstance(config, error)) return false;
    if (!createSurface(window, error)) return false;
    if (!pickDevice(error)) return false;

    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    const float priority = 1.0f;
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    if (!check(vkCreateDevice(physical_, &deviceInfo, nullptr, &device_), "vkCreateDevice", error)) return false;
    hasQueueFamily_ = true;
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_, &properties);
    deviceName_ = properties.deviceName;
    OA_LOG(LogLevel::Info, "vk", "device '{}' type={} apiVersion={}.{}", deviceName_,
           static_cast<int>(properties.deviceType), VK_VERSION_MAJOR(properties.apiVersion),
           VK_VERSION_MINOR(properties.apiVersion));

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    if (!check(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "vkCreateCommandPool", error)) {
        return false;
    }

    // The command pool has to exist first: createRenderTargets allocates one command buffer per swapchain image.
    presentMode_ = config.presentMode;
    if (!createSwapchain(window.widthPixels(), window.heightPixels(), error)) return false;
    if (!createRenderTargets(error)) return false;
    if (!createPipeline(error)) return false;
    if (!createMeshPipeline(error)) return false;

    return true;
}

bool VkRenderer::createInstance(const RendererConfig& config, std::string* error) {
    uint32_t sdlCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlCount);
    if (sdlExtensions == nullptr || sdlCount == 0) {
        if (error != nullptr) *error = "SDL offered no Vulkan instance extensions";
        return false;
    }
    std::vector<std::string> extensions;
    for (uint32_t i = 0; i < sdlCount; ++i) extensions.emplace_back(sdlExtensions[i]);

    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    const bool validationLayer = std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& l) {
        return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
    });
    uint32_t availableCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
    std::vector<VkExtensionProperties> available(availableCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available.data());
    const bool debugUtilsAvailable = std::any_of(available.begin(), available.end(), [](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
    });
    // A messenger in the pNext chain is only legal with VK_EXT_debug_utils enabled, so both are required.
    const bool useValidation = config.validation && validationLayer && debugUtilsAvailable;
    if (config.validation && !useValidation) {
        OA_LOG(LogLevel::Warn, "vk", "validation requested but unavailable (layer={}, debugUtils={})",
               validationLayer, debugUtilsAvailable);
    }
    if (useValidation) extensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkDebugUtilsMessengerCreateInfoEXT messengerInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    if (useValidation) {
        messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                       VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = &VkRenderer::debugMessenger;
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = config.applicationName.c_str();
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "OpenAnniversary";
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pNext = useValidation ? &messengerInfo : nullptr;
    info.pApplicationInfo = &appInfo;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    std::vector<const char*> extensionNames;
    for (const std::string& name : extensions) extensionNames.push_back(name.c_str());
    info.ppEnabledExtensionNames = extensionNames.data();
    if (useValidation) {
        const char* only[] = {"VK_LAYER_KHRONOS_validation"};
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = only;
    }
    if (!check(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance", error)) return false;

    if (useValidation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create != nullptr) {
            // The instance-level messenger catches validation messages that the pNext chain above already covers;
            // keeping a handle lets shutdown destroy it cleanly instead of leaking it.
            create(instance_, &messengerInfo, nullptr, &messenger_);
        }
    }
    OA_LOG(LogLevel::Info, "vk", "instance ready, {} instance extensions, validation={}", extensionNames.size(),
           useValidation ? "on" : "off");
    return true;
}

bool VkRenderer::createSurface(platform::Window& window, std::string* error) {
    if (!SDL_Vulkan_CreateSurface(window.sdl(), instance_, nullptr, &surface_)) {
        if (error != nullptr) *error = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
        return false;
    }
    return true;
}

bool VkRenderer::pickDevice(std::string* error) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        if (error != nullptr) *error = "no Vulkan-capable physical device";
        return false;
    }
    std::vector<VkPhysicalDevice> candidates(count);
    vkEnumeratePhysicalDevices(instance_, &count, candidates.data());

    int bestScore = -1;
    for (VkPhysicalDevice candidate : candidates) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());

        bool swapchainSupported = false;
        uint32_t layerCount = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &layerCount, nullptr);
        std::vector<VkExtensionProperties> deviceExtensions(layerCount);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &layerCount, deviceExtensions.data());
        for (const VkExtensionProperties& e : deviceExtensions) {
            if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) swapchainSupported = true;
        }
        if (!swapchainSupported) continue;

        for (uint32_t family = 0; family < familyCount; ++family) {
            if (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 presents = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface_, &presents) != VK_SUCCESS) continue;
            if (!presents) continue;

            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > bestScore) {
                bestScore = score;
                physical_ = candidate;
                queueFamily_ = family;
            }
        }
    }
    if (physical_ == VK_NULL_HANDLE) {
        if (error != nullptr) *error = "no graphics+present queue with a swapchain-capable device";
        return false;
    }

    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
    OA_LOG(LogLevel::Info, "vk", "selected physical device with queue family {} ({} heap(s), present-capable)",
           queueFamily_, memory.memoryHeapCount);
    return true;
}

bool VkRenderer::createSwapchain(int widthPixels, int heightPixels, std::string* error) {
    VkSurfaceCapabilitiesKHR capabilities{};
    if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &capabilities), "surface caps", error)) {
        return false;
    }

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &formatCount, formats.data());
    if (formats.empty()) {
        if (error != nullptr) *error = "surface reports no formats";
        return false;
    }
    const VkSurfaceFormatKHR chosen = [&]() {
        for (const VkSurfaceFormatKHR& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
        }
        return formats.front();
    }();

    uint32_t presentCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &presentCount, nullptr);
    std::vector<VkPresentModeKHR> modes(presentCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &presentCount, modes.data());
    // The default is the wait-for-vertical-blend swap (the steady, tear-free rhythm); presentMode 1 in
    // RendererConfig asks for the no-wait swap instead — same image loop, just no pacing, which is what
    // measures the frame-rate independence. Unavailable modes fall back to the default and say so.
    const VkPresentModeKHR wanted =
        presentMode_ == 1 ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    const VkPresentModeKHR presentMode = std::find(modes.begin(), modes.end(), wanted) != modes.end()
                                             ? wanted
                                             : (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_FIFO_KHR) != modes.end()
                                                    ? VK_PRESENT_MODE_FIFO_KHR
                                                    : modes.front());
    if (presentMode != wanted)
        OA_LOG(LogLevel::Warn, "vk", "present mode {} requested but not offered by this surface, going with the wait", static_cast<int>(wanted));

    extent_ = capabilities.currentExtent.width != 0xFFFFFFFFu
                  ? capabilities.currentExtent
                  : VkExtent2D{static_cast<uint32_t>(std::max(1, widthPixels)),
                               static_cast<uint32_t>(std::max(1, heightPixels))};
    extent_.width = std::clamp(extent_.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent_.height = std::clamp(extent_.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = imageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    if (!check(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_), "vkCreateSwapchainKHR", error)) {
        return false;
    }
    format_ = chosen.format;

    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    std::vector<VkImage> raw(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, raw.data());
    images_.clear();
    for (VkImage image : raw) images_.push_back(SwapchainImage{image, VK_NULL_HANDLE, VK_NULL_HANDLE});

    OA_LOG(LogLevel::Info, "vk", "swapchain {}x{} images={} format={} presentMode={}", extent_.width, extent_.height,
           images_.size(), static_cast<int>(format_), static_cast<int>(presentMode));

    commands_.assign(images_.size(), nullptr);
    frameFences_.assign(images_.size(), VK_NULL_HANDLE);
    acquireSemaphores_.assign(images_.size(), VK_NULL_HANDLE);
    for (std::size_t i = 0; i < images_.size(); ++i) {
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!check(vkCreateFence(device_, &fenceInfo, nullptr, &frameFences_[i]), "vkCreateFence", error)) {
            return false;
        }
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &acquireSemaphores_[i]), "vkCreateSemaphore", error)) {
            return false;
        }
    }
    return true;
}

bool VkRenderer::createRenderTargets(std::string* error) {
    VkAttachmentDescription color{};
    color.format = format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;  // the render pass ends in the layout presentation wants

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    // Depth belongs to the pass rather than to one pipeline: a triangle run has to sort against itself instead of
    // drawing in archive order. The textured quad still declares depth off, so it is unaffected.
    VkAttachmentDescription depth{};
    depth.format = kDepthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const VkAttachmentDescription attachments[2] = {color, depth};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    const VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = 2;
    passInfo.pAttachments = attachments;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 1;
    passInfo.pDependencies = &dependency;
    if (!check(vkCreateRenderPass(device_, &passInfo, nullptr, &renderPass_), "vkCreateRenderPass", error)) {
        return false;
    }

    if (!createDepthImage(error)) return false;

    for (SwapchainImage& target : images_) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = target.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format_;
        viewInfo.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!check(vkCreateImageView(device_, &viewInfo, nullptr, &target.view), "vkCreateImageView", error)) {
            return false;
        }
        // Both attachments of the pass have to be present, so every swapchain image pairs with the shared depth
        // buffer (recreated with the extent, exactly like the images themselves).
        const VkImageView views[2] = {target.view, depthView_};
        VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = renderPass_;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = views;
        fbInfo.width = extent_.width;
        fbInfo.height = extent_.height;
        fbInfo.layers = 1;
        if (!check(vkCreateFramebuffer(device_, &fbInfo, nullptr, &target.framebuffer), "vkCreateFramebuffer", error)) {
            return false;
        }
    }

    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = commandPool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = static_cast<uint32_t>(images_.size());
    if (!check(vkAllocateCommandBuffers(device_, &allocate, commands_.data()), "vkAllocateCommandBuffers", error)) {
        return false;
    }
    return true;
}

void VkRenderer::notifyResize(int widthPixels, int heightPixels) {
    if (widthPixels <= 0 || heightPixels <= 0) return;
    if (static_cast<uint32_t>(widthPixels) == extent_.width && static_cast<uint32_t>(heightPixels) == extent_.height) {
        return;
    }
    recreatePending_ = true;
}

void VkRenderer::destroySwapchain(bool waitForIdle) {
    if (waitForIdle && device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
    for (SwapchainImage& target : images_) {
        if (target.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, target.framebuffer, nullptr);
        if (target.view != VK_NULL_HANDLE) vkDestroyImageView(device_, target.view, nullptr);
        target = SwapchainImage{};
    }
    images_.clear();
    commands_.clear();
    destroyPipeline();
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    destroyDepthImage();

    for (VkFence& fence : frameFences_) {
        if (fence != VK_NULL_HANDLE) vkDestroyFence(device_, fence, nullptr);
    }
    frameFences_.clear();
    for (VkSemaphore& semaphore : acquireSemaphores_) {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore, nullptr);
    }
    acquireSemaphores_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VkRenderer::renderFrame(std::string* error) {
    if (recreatePending_) {
        recreatePending_ = false;
        destroySwapchain(true);
        // The window may still be mid-resize on X11; the next iteration picks up a sane extent again.
        const uint32_t width = std::max(1u, static_cast<uint32_t>(extent_.width));
        const uint32_t height = std::max(1u, static_cast<uint32_t>(extent_.height));
        if (!createSwapchain(static_cast<int>(width), static_cast<int>(height), error)) return false;
        if (!createRenderTargets(error)) return false;
        // The pipeline is tied to the render pass it was built against, so it has to come back with its swapchain.
        if (!createPipeline(error)) return false;
        if (!createMeshPipeline(error)) return false;

    }
    if (images_.empty()) return false;

    frameIndex_ = (frameIndex_ + 1) % images_.size();
    VkFence fence = frameFences_[frameIndex_];
    if (!check(vkWaitForFences(device_, 1, &fence, VK_TRUE, ~0ull), "vkWaitForFences", error)) return false;
    if (!check(vkResetFences(device_, 1, &fence), "vkResetFences", error)) return false;

    uint32_t imageIndex = 0;
    const VkResult acquired =
        vkAcquireNextImageKHR(device_, swapchain_, ~0ull, acquireSemaphores_[frameIndex_], VK_NULL_HANDLE, &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR || acquired == VK_SUBOPTIMAL_KHR) {
        recreatePending_ = true;
        return false;
    }
    if (!check(acquired, "vkAcquireNextImageKHR", error)) return false;

    VkCommandBuffer command = commands_[frameIndex_];
    if (!check(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer", error)) return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (!check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer", error)) return false;

    VkClearValue clears[2]{};
    clears[0].color = VkClearColorValue{{config_.clearColour[0], config_.clearColour[1], config_.clearColour[2],
                                         config_.clearColour[3]}};
    clears[1].depthStencil = VkClearDepthStencilValue{1.0f, 0u};
    VkRenderPassBeginInfo passBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    passBegin.renderPass = renderPass_;
    passBegin.framebuffer = images_[imageIndex].framebuffer;
    passBegin.renderArea = VkRect2D{{0, 0}, extent_};
    passBegin.clearValueCount = 2;
    passBegin.pClearValues = clears;
    vkCmdBeginRenderPass(command, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
    // The level goes down before the textured quad so an overlay still reads on top of it. Viewport and scissor are
    // dynamic state, so each branch states them instead of depending on the other having run.
    if (meshVertexCount_ != 0 && meshPipeline_ != VK_NULL_HANDLE) {
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent_.width), static_cast<float>(extent_.height),
                                  0.0f, 1.0f};
        vkCmdSetViewport(command, 0, 1, &viewport);
        const VkRect2D scissor{{0, 0}, extent_};
        vkCmdSetScissor(command, 0, 1, &scissor);
        drawMeshGeometry(command);
    }

    if (texture_ != VK_NULL_HANDLE && pipeline_ != VK_NULL_HANDLE) {
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent_.width), static_cast<float>(extent_.height),
                                  0.0f, 1.0f};
        vkCmdSetViewport(command, 0, 1, &viewport);
        const VkRect2D scissor{{0, 0}, extent_};
        vkCmdSetScissor(command, 0, 1, &scissor);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        const VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(command, 0, 1, &vertexBuffer_, &vertexOffset);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptorSet_, 0,
                                nullptr);
        // Fit the image inside the window without distorting it: a comparatively wider window shrinks X, a
        // comparatively taller one shrinks Y (clip-space axes map to pixel extents through the viewport).
        const float windowAspect = static_cast<float>(extent_.width) / static_cast<float>(extent_.height);
        struct {
            float scale[2];
        } push{};
        if (textureAspect_ >= windowAspect) {
            push.scale[0] = 1.0f;
            push.scale[1] = windowAspect / textureAspect_;
        } else {
            push.scale[0] = textureAspect_ / windowAspect;
            push.scale[1] = 1.0f;
        }
        vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdDraw(command, 6, 1, 0, 0);
    }
    vkCmdEndRenderPass(command);
    if (!check(vkEndCommandBuffer(command), "vkEndCommandBuffer", error)) return false;

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquireSemaphores_[frameIndex_];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    if (!check(vkQueueSubmit(queue_, 1, &submit, fence), "vkQueueSubmit", error)) return false;

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    const VkResult presented = vkQueuePresentKHR(queue_, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        recreatePending_ = true;
        return true;  // the frame was recorded; only the presentation target changed
    }
    return check(presented, "vkQueuePresentKHR", error);
}

bool VkRenderer::selfCheck(std::string* error) {
    if (device_ == VK_NULL_HANDLE) {
        if (error != nullptr) *error = "renderer not initialised";
        return false;
    }
    const std::array<float, 4> colour{{0.25f, 0.5f, 0.75f, 1.0f}};

    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
    const auto findMemoryType = [&](uint32_t bits, VkMemoryPropertyFlags wanted) -> uint32_t {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
        }
        return 0xFFFFFFFFu;
    };

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kClearCheckFormat;
    imageInfo.extent = VkExtent3D{static_cast<uint32_t>(kSelfCheckSize), static_cast<uint32_t>(kSelfCheckSize), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (!check(vkCreateImage(device_, &imageInfo, nullptr, &image), "selfCheck vkCreateImage", error)) return false;

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image, &requirements);
    const uint32_t deviceLocal = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = deviceLocal == 0xFFFFFFFFu ? 0 : deviceLocal;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    if (!check(vkAllocateMemory(device_, &allocate, nullptr, &imageMemory), "selfCheck vkAllocateMemory", error)) {
        vkDestroyImage(device_, image, nullptr);
        return false;
    }
    vkBindImageMemory(device_, image, imageMemory, 0);

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = kSelfCheckSize * kSelfCheckSize * 4;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging = VK_NULL_HANDLE;
    if (!check(vkCreateBuffer(device_, &bufferInfo, nullptr, &staging), "selfCheck vkCreateBuffer", error)) {
        vkFreeMemory(device_, imageMemory, nullptr);
        vkDestroyImage(device_, image, nullptr);
        return false;
    }
    vkGetBufferMemoryRequirements(device_, staging, &requirements);
    const uint32_t hostVisible = findMemoryType(requirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    allocate = VkMemoryAllocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = hostVisible == 0xFFFFFFFFu ? 0 : hostVisible;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!check(vkAllocateMemory(device_, &allocate, nullptr, &stagingMemory), "selfCheck staging memory", error)) {
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, imageMemory, nullptr);
        vkDestroyImage(device_, image, nullptr);
        return false;
    }
    vkBindBufferMemory(device_, staging, stagingMemory, 0);

    VkCommandBufferAllocateInfo commandAllocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAllocate.commandPool = commandPool_;
    commandAllocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocate.commandBufferCount = 1;
    VkCommandBuffer command = nullptr;
    if (!check(vkAllocateCommandBuffers(device_, &commandAllocate, &command), "selfCheck allocate command buffer", error)) {
        vkFreeMemory(device_, stagingMemory, nullptr);
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, imageMemory, nullptr);
        vkDestroyImage(device_, image, nullptr);
        return false;
    }

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command, &begin);
    VkClearColorValue clearValue{};
    clearValue.float32[0] = colour[0];
    clearValue.float32[1] = colour[1];
    clearValue.float32[2] = colour[2];
    clearValue.float32[3] = colour[3];
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // The image begins UNDEFINED, so the first write needs an explicit transition.
    VkImageMemoryBarrier toTransferDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransferDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransferDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransferDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransferDst.image = image;
    toTransferDst.subresourceRange = range;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toTransferDst);
    vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1, &range);

    VkImageMemoryBarrier toSource{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSource.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSource.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSource.image = image;
    toSource.subresourceRange = range;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toSource);

    VkBufferImageCopy region{};
    region.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = VkExtent3D{static_cast<uint32_t>(kSelfCheckSize), static_cast<uint32_t>(kSelfCheckSize), 1};
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
    if (!check(vkEndCommandBuffer(command), "selfCheck vkEndCommandBuffer", error)) {
        vkFreeMemory(device_, stagingMemory, nullptr);
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, imageMemory, nullptr);
        vkDestroyImage(device_, image, nullptr);
        return false;
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device_, &fenceInfo, nullptr, &fence);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VkResult result = vkQueueSubmit(queue_, 1, &submit, fence);
    if (result == VK_SUCCESS) result = vkWaitForFences(device_, 1, &fence, VK_TRUE, ~0ull);

    bool verified = false;
    int badChannelByte = -1;
    if (result == VK_SUCCESS) {
        void* mapped = nullptr;
        if (vkMapMemory(device_, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
            const auto* pixels = static_cast<const uint8_t*>(mapped);
            verified = true;
            const int expected[4] = {static_cast<int>(colour[0] * 255.0f), static_cast<int>(colour[1] * 255.0f),
                                     static_cast<int>(colour[2] * 255.0f), static_cast<int>(colour[3] * 255.0f)};
            for (std::size_t pixel = 0; pixel < kSelfCheckSize * kSelfCheckSize && verified; ++pixel) {
                for (int channel = 0; channel < 4; ++channel) {
                    const int got = pixels[pixel * 4 + channel];
                    if (std::abs(got - expected[channel]) > 1) {
                        verified = false;
                        badChannelByte = got;
                        break;
                    }
                }
            }
            vkUnmapMemory(device_, stagingMemory);
        }
    }

    vkDestroyFence(device_, fence, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);
    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, imageMemory, nullptr);
    vkDestroyImage(device_, image, nullptr);

    if (!verified) {
        if (error != nullptr) {
            *error = "offscreen read-back did not match the clear colour" +
                     (badChannelByte >= 0 ? " (first bad byte " + std::to_string(badChannelByte) + ")" : "") +
                     ", vkResult=" + std::to_string(static_cast<int>(result));
        }
        return false;
    }
    OA_LOG(LogLevel::Info, "vk", "self-check passed: {}x{} read-back matches the clear colour", kSelfCheckSize,
           kSelfCheckSize);

    // Second phase: upload a known pattern as a real texture, sample it through the pipeline into an offscreen image
    // and compare every byte. That covers staging copy, level placement within the payload, descriptor wiring, blend
    // state and shader math in one measurable path that needs no window (docs/TESTING.md §4). It also pins the UV
    // orientation: row 0 of the payload has to come back as row 0 of the attachment.
    constexpr uint32_t kProbeSize = 16;
    TextureUpload probe;
    probe.format = kSelfCheckFormat;
    probe.width = kProbeSize;
    probe.height = kProbeSize;
    probe.mipLevels = 1;
    std::vector<std::byte> pattern(static_cast<std::size_t>(kProbeSize) * kProbeSize * 4u);
    for (uint32_t y = 0; y < kProbeSize; ++y) {
        for (uint32_t x = 0; x < kProbeSize; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * kProbeSize + x) * 4u;
            pattern[at + 0] = static_cast<std::byte>(x * 16u);
            pattern[at + 1] = static_cast<std::byte>(y * 16u);
            pattern[at + 2] = static_cast<std::byte>((x + y) * 8u);
            pattern[at + 3] = static_cast<std::byte>(255u);   // opaque, so the premultiply is the identity
        }
    }
    probe.bytes = pattern.data();
    probe.byteSize = pattern.size();
    probe.levels.push_back(TextureUpload::Level{0, static_cast<uint32_t>(pattern.size())});

    std::string uploadError;
    if (!uploadTexture(probe, &uploadError)) {
        if (error != nullptr) *error = "self-check could not upload a texture: " + uploadError;
        return false;
    }

    VkImageCreateInfo attachmentInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    attachmentInfo.imageType = VK_IMAGE_TYPE_2D;
    attachmentInfo.format = kSelfCheckFormat;
    attachmentInfo.extent = VkExtent3D{kProbeSize, kProbeSize, 1};
    attachmentInfo.mipLevels = 1;
    attachmentInfo.arrayLayers = 1;
    attachmentInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    attachmentInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    attachmentInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    attachmentInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage attachment = VK_NULL_HANDLE;
    if (!check(vkCreateImage(device_, &attachmentInfo, nullptr, &attachment), "selfCheck attachment vkCreateImage",
               error)) {
        destroyTexture();
        return false;
    }
    VkMemoryRequirements probeRequirements{};
    vkGetImageMemoryRequirements(device_, attachment, &probeRequirements);
    const uint32_t probeDeviceLocal =
        findMemoryType(probeRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo probeAllocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    probeAllocate.allocationSize = probeRequirements.size;
    probeAllocate.memoryTypeIndex = probeDeviceLocal == 0xFFFFFFFFu ? 0 : probeDeviceLocal;
    VkDeviceMemory attachmentMemory = VK_NULL_HANDLE;
    if (!check(vkAllocateMemory(device_, &probeAllocate, nullptr, &attachmentMemory), "selfCheck attachment memory",
               error)) {
        vkDestroyImage(device_, attachment, nullptr);
        destroyTexture();
        return false;
    }
    if (!check(vkBindImageMemory(device_, attachment, attachmentMemory, 0), "selfCheck attachment bind", error)) {
        vkFreeMemory(device_, attachmentMemory, nullptr);
        vkDestroyImage(device_, attachment, nullptr);
        destroyTexture();
        return false;
    }

    std::vector<std::byte> drawn;
    const bool sampled = sampleInto(attachment, VkExtent2D{kProbeSize, kProbeSize}, &drawn, error);
    int mismatchAt = -1;
    if (!sampled) {
        mismatchAt = -2;   // sampleInto already filled *error
    } else if (drawn.size() != pattern.size()) {
        mismatchAt = static_cast<int>(drawn.size());
    } else {
        for (std::size_t byte = 0; byte < pattern.size(); ++byte) {
            const int got = static_cast<int>(drawn[byte]);
            const int want = static_cast<int>(pattern[byte]);
            if (std::abs(got - want) > 1) {
                mismatchAt = static_cast<int>(byte);
                break;
            }
        }
    }

    destroyTexture();
    vkFreeMemory(device_, attachmentMemory, nullptr);
    vkDestroyImage(device_, attachment, nullptr);

    if (mismatchAt != -1) {
        if (error != nullptr && mismatchAt >= 0) {
            *error = "sampled read-back did not match the uploaded texels (first differing byte index " +
                     std::to_string(mismatchAt) + ", got " +
                     std::to_string(static_cast<int>(drawn[static_cast<std::size_t>(mismatchAt)])) + ", wanted " +
                     std::to_string(static_cast<int>(pattern[static_cast<std::size_t>(mismatchAt)])) + ")";
        }
        return false;
    }
    OA_LOG(LogLevel::Info, "vk", "self-check passed: sampled {}x{} texture matches the uploaded texels byte for byte",
           kProbeSize, kProbeSize);
    return true;
}

bool VkRenderer::createPipeline(std::string* error) {
    if (pipeline_ != VK_NULL_HANDLE) return true;   // the set layout survives swapchain recreation

    const std::vector<std::uint32_t> vert = readSpv(std::filesystem::path(config_.shaderDirectory) / "quad.vert.spv",
                                                    error);
    if (vert.empty()) return false;
    const std::vector<std::uint32_t> frag = readSpv(std::filesystem::path(config_.shaderDirectory) / "quad.frag.spv",
                                                    error);
    if (frag.empty()) return false;

    VkShaderModuleCreateInfo vertInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    vertInfo.codeSize = vert.size() * 4u;
    vertInfo.pCode = vert.data();
    VkShaderModule vertModule = VK_NULL_HANDLE;
    if (!check(vkCreateShaderModule(device_, &vertInfo, nullptr, &vertModule), "vkCreateShaderModule vertex", error)) {
        return false;
    }
    VkShaderModuleCreateInfo fragInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    fragInfo.codeSize = frag.size() * 4u;
    fragInfo.pCode = frag.data();
    VkShaderModule fragModule = VK_NULL_HANDLE;
    if (!check(vkCreateShaderModule(device_, &fragInfo, nullptr, &fragModule), "vkCreateShaderModule fragment", error)) {
        vkDestroyShaderModule(device_, vertModule, nullptr);
        return false;
    }

    if (descriptorLayout_ == VK_NULL_HANDLE) {
        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo setLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setLayoutInfo.bindingCount = 1;
        setLayoutInfo.pBindings = &binding;
        if (!check(vkCreateDescriptorSetLayout(device_, &setLayoutInfo, nullptr, &descriptorLayout_),
                   "vkCreateDescriptorSetLayout", error)) {
            vkDestroyShaderModule(device_, vertModule, nullptr);
            vkDestroyShaderModule(device_, fragModule, nullptr);
            return false;
        }
    }

    const VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 2u};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (!check(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_),
               "vkCreatePipelineLayout", error)) {
        vkDestroyShaderModule(device_, vertModule, nullptr);
        vkDestroyShaderModule(device_, fragModule, nullptr);
        return false;
    }

    // Position plus texture coordinate. Clip y = -1 is the top of the framebuffer in Vulkan, so v = 0 (the payload's
    // first row) sits on that edge and the image reads upright instead of mirrored.
    static constexpr float kQuad[6][4] = {
        {-1.0f, -1.0f, 0.0f, 0.0f}, {1.0f, -1.0f, 1.0f, 0.0f}, {-1.0f, 1.0f, 0.0f, 1.0f},
        {1.0f, -1.0f, 1.0f, 0.0f}, {-1.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
    };
    if (vertexBuffer_ == VK_NULL_HANDLE) {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = sizeof(kQuad);
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check(vkCreateBuffer(device_, &bufferInfo, nullptr, &vertexBuffer_), "vertex vkCreateBuffer", error)) {
            vkDestroyShaderModule(device_, vertModule, nullptr);
            vkDestroyShaderModule(device_, fragModule, nullptr);
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, vertexBuffer_, &requirements);
        const uint32_t hostVisible = findMemoryType(physical_, requirements.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = hostVisible == 0xFFFFFFFFu ? 0 : hostVisible;
        if (!check(vkAllocateMemory(device_, &allocate, nullptr, &vertexMemory_), "vertex vkAllocateMemory", error)) {
            vkDestroyShaderModule(device_, vertModule, nullptr);
            vkDestroyShaderModule(device_, fragModule, nullptr);
            return false;
        }
        vkBindBufferMemory(device_, vertexBuffer_, vertexMemory_, 0);
        void* mapped = nullptr;
        if (!check(vkMapMemory(device_, vertexMemory_, 0, VK_WHOLE_SIZE, 0, &mapped), "vertex vkMapMemory", error)) {
            vkDestroyShaderModule(device_, vertModule, nullptr);
            vkDestroyShaderModule(device_, fragModule, nullptr);
            return false;
        }
        std::memcpy(mapped, kQuad, sizeof(kQuad));
        vkUnmapMemory(device_, vertexMemory_);
    }

    const VkPipelineShaderStageCreateInfo stages[2] = {
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                        VK_SHADER_STAGE_VERTEX_BIT, vertModule, "main", nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                        VK_SHADER_STAGE_FRAGMENT_BIT, fragModule, "main", nullptr},
    };
    const VkVertexInputBindingDescription binding{0, sizeof(float) * 4u, VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[2] = {
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 2u},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    // Nothing to cull: the quad is drawn facing whichever way the vertex order happens to fall, and back-face
    // removal would only make a resize bug look like an empty window.
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Premultiplied source over the clear colour, which is what quad.frag writes. BC1/BC3 carry their own alpha and
    // tag-0x15 masks are white with their shape in alpha (docs/formats/TEXTURES.md §3).
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = 0xFu;
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blend;

    const VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;
    const bool built = check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_),
                             "vkCreateGraphicsPipelines", error);
    vkDestroyShaderModule(device_, vertModule, nullptr);
    vkDestroyShaderModule(device_, fragModule, nullptr);
    if (!built) return false;

    OA_LOG(LogLevel::Info, "vk", "graphics pipeline ready: textured quad with premultiplied blending");
    return true;
}

void VkRenderer::destroyPipeline() {
    if (pipeline_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    if (pipelineLayout_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    }
    pipelineLayout_ = VK_NULL_HANDLE;
    // The mesh pipeline is built against the same render pass, so it leaves and returns with this one.
    destroyMeshPipeline();
}

void VkRenderer::destroyTexture() {
    if (device_ == VK_NULL_HANDLE) return;
    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
    if (textureView_ != VK_NULL_HANDLE) vkDestroyImageView(device_, textureView_, nullptr);
    if (descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (textureMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_, textureMemory_, nullptr);
    if (texture_ != VK_NULL_HANDLE) vkDestroyImage(device_, texture_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    textureView_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;
    textureMemory_ = VK_NULL_HANDLE;
    texture_ = VK_NULL_HANDLE;
    textureMipLevels_ = 0;
}

// Uploads one texture's whole mip chain to a fresh image + view: validation, staging copies, per-level
// vkCmdCopyBufferToImage, layout transitions, and view — exactly what both the textured-quad path (uploadTexture)
// and each terrain material page (uploadMesh) need, kept here once. The caller owns the output handles;
// on failure they are VK_NULL_HANDLE and nothing stays allocated.
bool VkRenderer::uploadSampledImage(const TextureUpload& texture, VkImage* image, VkDeviceMemory* memory,
                                     VkImageView* view, std::string* error) {
    if (device_ == VK_NULL_HANDLE) {
        if (error != nullptr) *error = "renderer not initialised";
        return false;
    }
    if (texture.format == VK_FORMAT_UNDEFINED || texture.width == 0u || texture.height == 0u ||
        texture.bytes == nullptr || texture.mipLevels == 0u || texture.levels.size() != texture.mipLevels) {
        if (error != nullptr) *error = "uploadSampledImage: incomplete texture description";
        return false;
    }
    std::uint64_t covered = 0;
    for (const TextureUpload::Level& level : texture.levels) covered += level.bytes;
    if (covered != texture.byteSize) {
        if (error != nullptr) {
            *error = "uploadSampledImage: levels cover " + std::to_string(covered) + " bytes, payload is " +
                     std::to_string(texture.byteSize);
        }
        return false;
    }

    VkImage& imageOut = *image;
    VkDeviceMemory& memOut = *memory;
    VkImageView& viewOut = *view;
    const auto releaseImage = [&]() {
        if (viewOut != VK_NULL_HANDLE) vkDestroyImageView(device_, viewOut, nullptr);
        if (memOut != VK_NULL_HANDLE) vkFreeMemory(device_, memOut, nullptr);
        if (imageOut != VK_NULL_HANDLE) vkDestroyImage(device_, imageOut, nullptr);
        viewOut = VK_NULL_HANDLE;
        memOut = VK_NULL_HANDLE;
        imageOut = VK_NULL_HANDLE;
    };

    VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    stagingInfo.size = texture.byteSize;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging = VK_NULL_HANDLE;
    if (!check(vkCreateBuffer(device_, &stagingInfo, nullptr, &staging), "texture staging vkCreateBuffer", error)) {
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, staging, &requirements);
    const uint32_t hostVisible = findMemoryType(physical_, requirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = hostVisible == 0xFFFFFFFFu ? 0 : hostVisible;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    const auto releaseStaging = [&]() {
        if (stagingMemory != VK_NULL_HANDLE) vkFreeMemory(device_, stagingMemory, nullptr);
        if (staging != VK_NULL_HANDLE) vkDestroyBuffer(device_, staging, nullptr);
    };
    if (!check(vkAllocateMemory(device_, &allocate, nullptr, &stagingMemory), "texture staging vkAllocateMemory",
               error)) {
        releaseStaging();
        return false;
    }
    if (!check(vkBindBufferMemory(device_, staging, stagingMemory, 0), "texture staging vkBindBufferMemory", error)) {
        releaseStaging();
        return false;
    }
    void* mapped = nullptr;
    if (!check(vkMapMemory(device_, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "texture vkMapMemory", error)) {
        releaseStaging();
        return false;
    }
    std::memcpy(mapped, texture.bytes, texture.byteSize);
    vkUnmapMemory(device_, stagingMemory);

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = texture.format;
    imageInfo.extent = VkExtent3D{texture.width, texture.height, 1};
    imageInfo.mipLevels = texture.mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!check(vkCreateImage(device_, &imageInfo, nullptr, &imageOut), "texture vkCreateImage", error)) {
        releaseStaging();
        return false;
    }
    vkGetImageMemoryRequirements(device_, imageOut, &requirements);
    const uint32_t deviceLocal =
        findMemoryType(physical_, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    allocate = VkMemoryAllocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = deviceLocal == 0xFFFFFFFFu ? 0 : deviceLocal;
    if (!check(vkAllocateMemory(device_, &allocate, nullptr, &memOut), "texture vkAllocateMemory", error)) {
        releaseImage();
        releaseStaging();
        return false;
    }
    if (!check(vkBindImageMemory(device_, imageOut, memOut, 0), "texture vkBindImageMemory", error)) {
        releaseImage();
        releaseStaging();
        return false;
    }

    VkCommandBufferAllocateInfo commandAllocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAllocate.commandPool = commandPool_;
    commandAllocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocate.commandBufferCount = 1;
    VkCommandBuffer command = nullptr;
    if (!check(vkAllocateCommandBuffers(device_, &commandAllocate, &command), "texture allocate command buffer",
               error)) {
        releaseImage();
        releaseStaging();
        return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check(vkBeginCommandBuffer(command, &begin), "texture vkBeginCommandBuffer", error)) {
        releaseImage();
        releaseStaging();
        return false;
    }

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.mipLevels, 0, 1};
    // The payload is already packed largest-level-first (cdc::pcd9LevelRegions), so each level copies from its own
    // buffer offset with tightly packed rows.
    VkImageMemoryBarrier toTransferDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransferDst.srcAccessMask = 0;
    toTransferDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransferDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransferDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransferDst.image = imageOut;
    toTransferDst.subresourceRange = range;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toTransferDst);
    for (uint32_t level = 0; level < texture.mipLevels; ++level) {
        const VkBufferImageCopy region{texture.levels[level].offset, 0, 0,
                                       VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                                       VkOffset3D{0, 0, 0},
                                       VkExtent3D{std::max(1u, texture.width >> level), std::max(1u, texture.height >> level), 1}};
        vkCmdCopyBufferToImage(command, staging, imageOut, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    VkImageMemoryBarrier toSampled{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSampled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toSampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSampled.image = imageOut;
    toSampled.subresourceRange = range;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toSampled);
    if (!check(vkEndCommandBuffer(command), "texture vkEndCommandBuffer", error)) {
        releaseImage();
        releaseStaging();
        return false;
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (!check(vkCreateFence(device_, &fenceInfo, nullptr, &fence), "texture vkCreateFence", error)) {
        releaseImage();
        releaseStaging();
        return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VkResult result = vkQueueSubmit(queue_, 1, &submit, fence);
    if (result == VK_SUCCESS) result = vkWaitForFences(device_, 1, &fence, VK_TRUE, ~0ull);
    vkDestroyFence(device_, fence, nullptr);
    releaseStaging();
    if (!check(result, "texture upload submit", error)) {
        releaseImage();
        return false;
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = imageOut;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = texture.format;
    viewInfo.subresourceRange = range;
    if (!check(vkCreateImageView(device_, &viewInfo, nullptr, &viewOut), "texture vkCreateImageView", error)) {
        releaseImage();
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool VkRenderer::uploadTexture(const TextureUpload& texture, std::string* error) {
    if (device_ == VK_NULL_HANDLE) {
        if (error != nullptr) *error = "renderer not initialised";
        return false;
    }
    vkDeviceWaitIdle(device_);
    destroyTexture();
    if (!uploadSampledImage(texture, &texture_, &textureMemory_, &textureView_, error)) return false;

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // Every level of the chain is real data here, so sampling may use all of it instead of only the first.
    samplerInfo.maxLod = static_cast<float>(texture.mipLevels - 1u);
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!check(vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_), "texture vkCreateSampler", error)) {
        destroyTexture();
        return false;
    }

    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (!check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_), "texture vkCreateDescriptorPool",
               error)) {
        destroyTexture();
        return false;
    }
    VkDescriptorSetAllocateInfo setAllocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setAllocate.descriptorPool = descriptorPool_;
    setAllocate.descriptorSetCount = 1;
    setAllocate.pSetLayouts = &descriptorLayout_;
    if (!check(vkAllocateDescriptorSets(device_, &setAllocate, &descriptorSet_), "texture vkAllocateDescriptorSets",
               error)) {
        destroyTexture();
        return false;
    }
    // VkDescriptorImageInfo lists the sampler first; a null sampler here makes the combined-image write invalid.
    const VkDescriptorImageInfo described{sampler_, textureView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &described;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    textureAspect_ = static_cast<float>(texture.width) / static_cast<float>(texture.height);
    textureMipLevels_ = texture.mipLevels;
    OA_LOG(LogLevel::Info, "vk", "uploaded {}x{} texture: format={} levels={} bytes={}", texture.width,
           texture.height, static_cast<int>(texture.format), texture.mipLevels,
           static_cast<unsigned long long>(texture.byteSize));
    return true;
}

// Draws the bound textured quad into an offscreen attachment and reads the pixels back. `colourAttachment` has to use
// kSelfCheckFormat, because that is what this pass declares and the pipeline it draws with was built for that format.
// upload → descriptor → pipeline → blend through real command submission, without a window in sight.
bool VkRenderer::sampleInto(VkImage colourAttachment, VkExtent2D size, std::vector<std::byte>* outRgba,
                            std::string* error, bool drawMesh) {
    if (outRgba == nullptr) return false;
    if (!drawMesh && (pipeline_ == VK_NULL_HANDLE || descriptorSet_ == VK_NULL_HANDLE)) {
        if (error != nullptr) *error = "sampleInto needs a pipeline and an uploaded texture";
        return false;
    }
    if (drawMesh && (meshPipeline_ == VK_NULL_HANDLE || meshVertexCount_ == 0)) {
        if (error != nullptr) *error = "sampleInto(drawMesh) needs an uploaded triangle run";
        return false;
    }

    // The probe pass has to be render-pass compatible with whatever pipeline it draws, and both of those were built
    // against the window pass, which carries a depth attachment. So the probe always pairs its colour attachment with
    // a temporary depth image; a textured quad ignores that (depth test off), and the byte-for-byte comparison of
    // texels is unaffected because nothing here writes to the colour attachment but the shader.
    VkImage probeDepth = VK_NULL_HANDLE;
    VkDeviceMemory probeDepthMemory = VK_NULL_HANDLE;
    VkImageView probeDepthView = VK_NULL_HANDLE;
    const auto releaseDepthProbe = [&]() {
        if (probeDepthView != VK_NULL_HANDLE) vkDestroyImageView(device_, probeDepthView, nullptr);
        if (probeDepthMemory != VK_NULL_HANDLE) vkFreeMemory(device_, probeDepthMemory, nullptr);
        if (probeDepth != VK_NULL_HANDLE) vkDestroyImage(device_, probeDepth, nullptr);
    };
    VkImageCreateInfo depthInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    depthInfo.imageType = VK_IMAGE_TYPE_2D;
    depthInfo.format = kDepthFormat;
    depthInfo.extent = VkExtent3D{size.width, size.height, 1};
    depthInfo.mipLevels = 1;
    depthInfo.arrayLayers = 1;
    depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!check(vkCreateImage(device_, &depthInfo, nullptr, &probeDepth), "sampleInto depth vkCreateImage", error)) {
        return false;
    }
    VkMemoryRequirements depthRequirements{};
    vkGetImageMemoryRequirements(device_, probeDepth, &depthRequirements);
    const uint32_t depthType =
        findMemoryType(physical_, depthRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo depthAllocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    depthAllocate.allocationSize = depthRequirements.size;
    depthAllocate.memoryTypeIndex = depthType == 0xFFFFFFFFu ? 0 : depthType;
    if (!check(vkAllocateMemory(device_, &depthAllocate, nullptr, &probeDepthMemory), "sampleInto depth memory", error)) {
        releaseDepthProbe();
        return false;
    }
    if (!check(vkBindImageMemory(device_, probeDepth, probeDepthMemory, 0), "sampleInto depth bind", error)) {
        releaseDepthProbe();
        return false;
    }
    VkImageViewCreateInfo depthViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    depthViewInfo.image = probeDepth;
    depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.format = kDepthFormat;
    depthViewInfo.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (!check(vkCreateImageView(device_, &depthViewInfo, nullptr, &probeDepthView), "sampleInto depth view", error)) {
        releaseDepthProbe();
        return false;
    }

    VkAttachmentDescription attachment{};
    attachment.format = kSelfCheckFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // The pass hands the image straight to the copy-out layout, so no extra barrier is needed here.
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentDescription depth{};
    depth.format = kDepthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const VkAttachmentDescription attachments[2] = {attachment, depth};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    const VkSubpassDependency dependency{VK_SUBPASS_EXTERNAL,
                                         0,
                                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         0,
                                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = 2;
    passInfo.pAttachments = attachments;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 1;
    passInfo.pDependencies = &dependency;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (!check(vkCreateRenderPass(device_, &passInfo, nullptr, &pass), "sampleInto vkCreateRenderPass", error)) {
        return false;
    }

    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    const auto release = [&]() {
        if (readbackMemory != VK_NULL_HANDLE) vkFreeMemory(device_, readbackMemory, nullptr);
        if (readback != VK_NULL_HANDLE) vkDestroyBuffer(device_, readback, nullptr);
        if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, framebuffer, nullptr);
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device_, view, nullptr);
        if (pass != VK_NULL_HANDLE) vkDestroyRenderPass(device_, pass, nullptr);
        releaseDepthProbe();
    };

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = colourAttachment;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kSelfCheckFormat;
    viewInfo.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!check(vkCreateImageView(device_, &viewInfo, nullptr, &view), "sampleInto vkCreateImageView", error)) {
        release();
        return false;
    }
    // Both attachments the pass declares have to be present in the framebuffer.
    const VkImageView views[2] = {view, probeDepthView};
    VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebufferInfo.renderPass = pass;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = views;
    framebufferInfo.width = size.width;
    framebufferInfo.height = size.height;
    framebufferInfo.layers = 1;
    if (!check(vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffer), "sampleInto vkCreateFramebuffer",
               error)) {
        release();
        return false;
    }

    const std::size_t pixelBytes = static_cast<std::size_t>(size.width) * size.height * 4u;
    VkBufferCreateInfo readbackInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    readbackInfo.size = pixelBytes;
    readbackInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    readbackInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateBuffer(device_, &readbackInfo, nullptr, &readback), "sampleInto vkCreateBuffer", error)) {
        release();
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, readback, &requirements);
    const uint32_t hostVisible = findMemoryType(physical_, requirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = hostVisible == 0xFFFFFFFFu ? 0 : hostVisible;
    if (!check(vkAllocateMemory(device_, &allocate, nullptr, &readbackMemory), "sampleInto vkAllocateMemory", error)) {
        release();
        return false;
    }
    if (!check(vkBindBufferMemory(device_, readback, readbackMemory, 0), "sampleInto vkBindBufferMemory", error)) {
        release();
        return false;
    }

    VkCommandBufferAllocateInfo commandAllocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAllocate.commandPool = commandPool_;
    commandAllocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocate.commandBufferCount = 1;
    VkCommandBuffer command = nullptr;
    if (!check(vkAllocateCommandBuffers(device_, &commandAllocate, &command), "sampleInto allocate command buffer",
               error)) {
        release();
        return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check(vkBeginCommandBuffer(command, &begin), "sampleInto vkBeginCommandBuffer", error)) {
        release();
        return false;
    }
    // Transparent clear: the blend is premultiplied-source-over, so what arrives in the attachment is exactly what
    // the shader produced.
    VkClearValue clears[2]{};
    clears[0].color = VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}};
    clears[1].depthStencil = VkClearDepthStencilValue{1.0f, 0u};
    VkRenderPassBeginInfo passBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    passBegin.renderPass = pass;
    passBegin.framebuffer = framebuffer;
    passBegin.renderArea = VkRect2D{{0, 0}, size};
    passBegin.clearValueCount = 2;
    passBegin.pClearValues = clears;
    vkCmdBeginRenderPass(command, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(size.width), static_cast<float>(size.height), 0.0f, 1.0f};
    vkCmdSetViewport(command, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, size};
    vkCmdSetScissor(command, 0, 1, &scissor);
    const VkDeviceSize vertexOffset = 0;
    if (drawMesh) {
        drawMeshGeometry(command);
    } else {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindVertexBuffers(command, 0, 1, &vertexBuffer_, &vertexOffset);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptorSet_, 0,
                                nullptr);
        const struct {
            float scale[2];
        } push{{1.0f, 1.0f}};
        vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdDraw(command, 6, 1, 0, 0);
    }
    vkCmdEndRenderPass(command);
    const VkBufferImageCopy region{0,
                                   0,
                                   0,
                                   VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   VkOffset3D{0, 0, 0},
                                   VkExtent3D{size.width, size.height, 1}};
    vkCmdCopyImageToBuffer(command, colourAttachment, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
    if (!check(vkEndCommandBuffer(command), "sampleInto vkEndCommandBuffer", error)) {
        release();
        return false;
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (!check(vkCreateFence(device_, &fenceInfo, nullptr, &fence), "sampleInto vkCreateFence", error)) {
        release();
        return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VkResult result = vkQueueSubmit(queue_, 1, &submit, fence);
    if (result == VK_SUCCESS) result = vkWaitForFences(device_, 1, &fence, VK_TRUE, ~0ull);
    vkDestroyFence(device_, fence, nullptr);
    if (!check(result, "sampleInto submit", error)) {
        release();
        return false;
    }

    void* mapped = nullptr;
    if (!check(vkMapMemory(device_, readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "sampleInto vkMapMemory", error)) {
        release();
        return false;
    }
    const auto* pixels = static_cast<const std::byte*>(mapped);
    outRgba->assign(pixels, pixels + pixelBytes);
    vkUnmapMemory(device_, readbackMemory);
    release();
    return true;
}

bool VkRenderer::createDepthImage(std::string* error) {
    if (depthImage_ != VK_NULL_HANDLE) return true;   // survives nothing: it is sized with the extent below
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kDepthFormat;
    imageInfo.extent = VkExtent3D{extent_.width, extent_.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!check(vkCreateImage(device_, &imageInfo, nullptr, &depthImage_), "depth vkCreateImage", error)) {
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, depthImage_, &requirements);
    const uint32_t deviceLocal =
        findMemoryType(physical_, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = deviceLocal == 0xFFFFFFFFu ? 0 : deviceLocal;
    if (!check(vkAllocateMemory(device_, &allocate, nullptr, &depthMemory_), "depth vkAllocateMemory", error)) {
        return false;
    }
    if (!check(vkBindImageMemory(device_, depthImage_, depthMemory_, 0), "depth vkBindImageMemory", error)) {
        return false;
    }
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = depthImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kDepthFormat;
    viewInfo.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    return check(vkCreateImageView(device_, &viewInfo, nullptr, &depthView_), "depth vkCreateImageView", error);
}

void VkRenderer::destroyDepthImage() {
    if (device_ == VK_NULL_HANDLE) return;
    if (depthView_ != VK_NULL_HANDLE) vkDestroyImageView(device_, depthView_, nullptr);
    if (depthMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_, depthMemory_, nullptr);
    if (depthImage_ != VK_NULL_HANDLE) vkDestroyImage(device_, depthImage_, nullptr);
    depthView_ = VK_NULL_HANDLE;
    depthMemory_ = VK_NULL_HANDLE;
    depthImage_ = VK_NULL_HANDLE;
}

bool VkRenderer::createMeshPipeline(std::string* error) {
    if (meshPipeline_ != VK_NULL_HANDLE) return true;   // the layout survives swapchain recreation

    const std::vector<std::uint32_t> vert = readSpv(std::filesystem::path(config_.shaderDirectory) / "mesh.vert.spv",
                                                    error);
    if (vert.empty()) return false;
    const std::vector<std::uint32_t> frag = readSpv(std::filesystem::path(config_.shaderDirectory) / "mesh.frag.spv",
                                                    error);
    if (frag.empty()) return false;

    VkShaderModuleCreateInfo vertInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    vertInfo.codeSize = vert.size() * 4u;
    vertInfo.pCode = vert.data();
    VkShaderModule vertModule = VK_NULL_HANDLE;
    if (!check(vkCreateShaderModule(device_, &vertInfo, nullptr, &vertModule), "vkCreateShaderModule mesh vertex",
               error)) {
        return false;
    }
    VkShaderModuleCreateInfo fragInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    fragInfo.codeSize = frag.size() * 4u;
    fragInfo.pCode = frag.data();
    VkShaderModule fragModule = VK_NULL_HANDLE;
    if (!check(vkCreateShaderModule(device_, &fragInfo, nullptr, &fragModule), "vkCreateShaderModule mesh fragment",
               error)) {
        vkDestroyShaderModule(device_, vertModule, nullptr);
        return false;
    }

    // The interface is a 4x4 matrix plus one page per draw: a combined image sampler at binding 0, so each
    // material run can bind its own page (docs/formats/GEOMETRY.md §23; white when a material names none).
    if (meshSetLayout_ == VK_NULL_HANDLE) {
        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo setLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setLayoutInfo.bindingCount = 1;
        setLayoutInfo.pBindings = &binding;
        if (!check(vkCreateDescriptorSetLayout(device_, &setLayoutInfo, nullptr, &meshSetLayout_),
                   "vkCreateDescriptorSetLayout mesh", error)) {
            return false;
        }
    }
    const VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 16u};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &meshSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (!check(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &meshLayout_), "vkCreatePipelineLayout mesh",
               error)) {
        vkDestroyShaderModule(device_, vertModule, nullptr);
        vkDestroyShaderModule(device_, fragModule, nullptr);
        return false;
    }

    const VkPipelineShaderStageCreateInfo stages[2] = {
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                        VK_SHADER_STAGE_VERTEX_BIT, vertModule, "main", nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                        VK_SHADER_STAGE_FRAGMENT_BIT, fragModule, "main", nullptr},
    };
    // The record layout the archive stores (docs/formats/GEOMETRY.md §2): position i16x3 at +0, four colour bytes at
    // +8, and the two UV shorts at +12 that §2's community-reader note puts at 1/4096 fixed point.
    constexpr std::size_t kRecordBytes = 20;
    const VkVertexInputBindingDescription binding{0, static_cast<std::uint32_t>(kRecordBytes),
                                                  VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[3] = {
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R16G16B16_SINT, 0},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R8G8B8A8_UNORM, 8},
        VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R16G16_SINT, 12},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    // Winding has not been established from the archive yet, so nothing may be culled: a wrong guess would delete
    // half of every level rather than expose the order.
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // Vertex colours are written straight through: the shipped modulate colour is already display-encoded and the
    // attachment re-encodes it, so no blend state is wanted here.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_FALSE;
    blend.colorWriteMask = 0xFu;
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blend;

    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.layout = meshLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;
    const bool built = check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                                       &meshPipeline_),
                             "vkCreateGraphicsPipelines mesh", error);
    vkDestroyShaderModule(device_, vertModule, nullptr);
    vkDestroyShaderModule(device_, fragModule, nullptr);
    if (!built) return false;

    OA_LOG(LogLevel::Info, "vk", "graphics pipeline ready: triangle runs with vertex colours, depth-tested");
    return true;
}

void VkRenderer::destroyMeshMaterials() {
    if (device_ == VK_NULL_HANDLE) return;
    for (MeshMaterial& material : meshMaterials_) {
        if (material.image != VK_NULL_HANDLE) vkDestroyImage(device_, material.image, nullptr);
        if (material.view != VK_NULL_HANDLE) vkDestroyImageView(device_, material.view, nullptr);
        if (material.memory != VK_NULL_HANDLE) vkFreeMemory(device_, material.memory, nullptr);
    }
    meshMaterials_.clear();
    if (meshWhite_.image != VK_NULL_HANDLE) vkDestroyImage(device_, meshWhite_.image, nullptr);
    if (meshWhite_.view != VK_NULL_HANDLE) vkDestroyImageView(device_, meshWhite_.view, nullptr);
    if (meshWhite_.memory != VK_NULL_HANDLE) vkFreeMemory(device_, meshWhite_.memory, nullptr);
    meshWhite_ = MeshMaterial{};
    if (meshPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, meshPool_, nullptr);   // frees the sets too
    meshPool_ = VK_NULL_HANDLE;
    if (meshSetLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, meshSetLayout_, nullptr);
    meshSetLayout_ = VK_NULL_HANDLE;
    if (meshSampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, meshSampler_, nullptr);
    meshSampler_ = VK_NULL_HANDLE;
    meshBatches_.clear();
}

void VkRenderer::destroyMeshPipeline() {
    destroyMeshMaterials();   // pages, sampler, and the set pool leave with the pipeline they belong to
    if (meshPipeline_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, meshPipeline_, nullptr);
    meshPipeline_ = VK_NULL_HANDLE;
    if (meshLayout_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, meshLayout_, nullptr);
    }
    meshLayout_ = VK_NULL_HANDLE;
}

void VkRenderer::drawMeshGeometry(VkCommandBuffer command) const {
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline_);
    const VkDeviceSize bufferOffset = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &meshBuffer_, &bufferOffset);
    pushMeshMatrix(command);
    // The shader always samples a page, so every mesh draw binds one: a material's own page for its run, the
    // white 1x1 otherwise (multiplying by white keeps the shipped vertex colours byte-identical).
    const auto bindPage = [&](const MeshBatch& batch) {
        const VkDescriptorSet set = batch.material == kNoTexture || static_cast<std::size_t>(batch.material) >= meshMaterials_.size()
                                      ? meshWhite_.set
                                      : meshMaterials_[batch.material].set;
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout_, 0, 1, &set, 0, nullptr);
    };
    if (meshIndexCount_ != 0) {
        vkCmdBindIndexBuffer(command, meshIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);
        if (!meshBatches_.empty()) {
            // One draw per material run, each bound to that material's page: the first experiment in
            // docs/formats/GEOMETRY.md §23. §31 adds the shipped visibility test on top (docs GEOMETRY.md §29 [V-exe]:
            // sphere-versus-frustum ONLY - no distance, depth or LOD swap anywhere in tr7's terrain walk), so a batch
            // whose bounding sphere lies outside any plane of THIS frame's view-projection matrix never issues its
            // draw call. The planes are read straight off buildMeshMatrix()'s matrix, the very one pushed as the vertex
            // constant, so culling and shading cannot drift apart; a batch built without a bound (negative radius)
            // always draws, which keeps every older path byte-identical.
            const auto matrix = buildMeshMatrix();
            float planes[6][4];   // Gribbush rows over this column-major matrix: inside means dot(plane, p_h) >= -radius
            for (int i = 0; i < 4; ++i) {
                const float r0 = matrix[i], r1 = matrix[4 + i], r2 = matrix[8 + i], r3 = matrix[12 + i];   // element(row, col)
                planes[0][i] = r3 + r0;   // left:   w + x >= 0
                planes[1][i] = r3 - r0;   // right:  w - x >= 0
                planes[2][i] = r3 + r1;   // bottom: w + y >= 0
                planes[3][i] = r3 - r1;   // top:    w - y >= 0
                planes[4][i] = r2;        // near:   z >= 0 (this pass maps depth into [0,1], not [-w,w])
                planes[5][i] = r3 - r2;   // far:    w - z >= 0
            }
            for (auto& plane : planes) {
                const float len = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
                if (len == 0.0f) { plane[0] = plane[1] = plane[2] = plane[3] = 0.0f; continue; }   // a degenerate row vetoes nothing
                for (float& coefficient : plane) coefficient /= len;                                // radius then compares in world units
            }
            const auto outside = [&](const MeshBatch& batch) {
                if (!(batch.bound[3] >= 0.0f)) return false;   // unbuilt bound: always draw (ponytail: no per-batch recompute here, uploadMesh fills bounds once)
                for (const auto& plane : planes) {
                    const float planeOffset = plane[0] * batch.bound[0] + plane[1] * batch.bound[1] + plane[2] * batch.bound[2] + plane[3];
                    if (planeOffset < -batch.bound[3]) return true;   // whole sphere beyond this plane
                }
                return false;
            };
            meshDrawn_ = 0;
            meshCulled_ = 0;
            const bool cullThisFrame = camera_.fovDegrees > 0.0f;   // §31: only a perspective view frustum-culls; the orthographic fit IS the probe framing, so --meshDump and the offscreen pictures §6/§23 document stay byte-identical when it is off
            for (const MeshBatch& batch : meshBatches_) {
                if (cullThisFrame && outside(batch)) { ++meshCulled_; continue; }
                bindPage(batch);
                vkCmdDrawIndexed(command, batch.indexCount, 1, 0, batch.indexStart, 0);
                ++meshDrawn_;
            }
            return;
        }
    }
    // The unbatched fallbacks bind white: indexed runs (the old single draw) and unindexed runs alike.
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout_, 0, 1, &meshWhite_.set,
                            0, nullptr);
    if (meshIndexCount_ != 0) {
        vkCmdDrawIndexed(command, meshIndexCount_, 1, 0, 0, 0);
    } else {
        vkCmdDraw(command, meshVertexCount_, 1, 0, 0);
    }
}

bool VkRenderer::uploadMesh(const MeshUpload& mesh, std::string* error) {
    if (device_ == VK_NULL_HANDLE) {
        if (error != nullptr) *error = "renderer not initialised";
        return false;
    }
    if (mesh.bytes == nullptr || mesh.vertexCount < 3u) {
        if (error != nullptr) *error = "a triangle run needs at least three vertices";
        return false;
    }
    // Per-material runs (docs/formats/GEOMETRY.md §23): the ranges must be whole triangles, ascending and
    // non-overlapping, inside the index list, and name a material entry or kNoTexture. Uncovered indices are
    // simply not drawn — the caller is expected to group all of them, and this stays honest about what it does.
    std::uint32_t coveredByBatches = 0u;
    if (!mesh.batches.empty()) {
        if (mesh.indices == nullptr || mesh.indices->empty()) {
            if (error != nullptr) *error = "material batches need an index list to range over";
            return false;
        }
        for (const MeshBatch& batch : mesh.batches) {
            if (batch.indexCount == 0u || batch.indexCount % 3u != 0u) {
                if (error != nullptr) *error = "a material run holds whole triangles";
                return false;
            }
            if (batch.indexStart < coveredByBatches ||
                static_cast<std::uint64_t>(batch.indexStart) + batch.indexCount > mesh.indices->size()) {
                if (error != nullptr) *error = "material batches must be ascending, non-overlapping, inside the index list";
                return false;
            }
            coveredByBatches = batch.indexStart + batch.indexCount;
            if (batch.material != kNoTexture && static_cast<std::size_t>(batch.material) >= mesh.materials.size()) {
                if (error != nullptr) *error = "a batch names no material entry";
                return false;
            }
        }
    }
    const bool indexed = mesh.indices != nullptr && !mesh.indices->empty();
    if (indexed) {
        if (mesh.indices->size() % 3u != 0u) {
            if (error != nullptr) *error = "a triangle list needs whole triangles";
            return false;
        }
        for (const std::uint32_t index : *mesh.indices) {
            if (index >= mesh.vertexCount) {
                if (error != nullptr) *error = "an index falls outside the uploaded vertices";
                return false;
            }
        }
    }
    constexpr std::size_t kRecordBytes = 20;   // docs/formats/GEOMETRY.md §2
    const std::size_t bytes = kRecordBytes * static_cast<std::size_t>(mesh.vertexCount);

    if (meshMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_, meshMemory_, nullptr);
    if (meshBuffer_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, meshBuffer_, nullptr);
    meshMemory_ = VK_NULL_HANDLE;
    meshBuffer_ = VK_NULL_HANDLE;
    meshVertexCount_ = 0;
    if (meshIndexMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_, meshIndexMemory_, nullptr);
    if (meshIndexBuffer_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, meshIndexBuffer_, nullptr);
    meshIndexMemory_ = VK_NULL_HANDLE;
    meshIndexBuffer_ = VK_NULL_HANDLE;
    meshIndexCount_ = 0;

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateBuffer(device_, &bufferInfo, nullptr, &meshBuffer_), "mesh vkCreateBuffer", error)) {
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, meshBuffer_, &requirements);
    const uint32_t hostVisible = findMemoryType(physical_, requirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = hostVisible == 0xFFFFFFFFu ? 0 : hostVisible;
    if (!check(vkAllocateMemory(device_, &allocate, nullptr, &meshMemory_), "mesh vkAllocateMemory", error)) {
        return false;
    }
    if (!check(vkBindBufferMemory(device_, meshBuffer_, meshMemory_, 0), "mesh vkBindBufferMemory", error)) {
        return false;
    }
    void* mapped = nullptr;
    if (!check(vkMapMemory(device_, meshMemory_, 0, VK_WHOLE_SIZE, 0, &mapped), "mesh vkMapMemory", error)) {
        return false;
    }
    std::memcpy(mapped, mesh.bytes, bytes);
    vkUnmapMemory(device_, meshMemory_);

    if (indexed) {
        const std::size_t indexBytes = mesh.indices->size() * sizeof(std::uint32_t);
        VkBufferCreateInfo indexInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        indexInfo.size = indexBytes;
        indexInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        indexInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check(vkCreateBuffer(device_, &indexInfo, nullptr, &meshIndexBuffer_), "index vkCreateBuffer", error)) {
            return false;
        }
        vkGetBufferMemoryRequirements(device_, meshIndexBuffer_, &requirements);
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = hostVisible == 0xFFFFFFFFu ? 0 : hostVisible;
        if (!check(vkAllocateMemory(device_, &allocate, nullptr, &meshIndexMemory_), "index vkAllocateMemory", error)) {
            return false;
        }
        if (!check(vkBindBufferMemory(device_, meshIndexBuffer_, meshIndexMemory_, 0), "index vkBindBufferMemory", error)) {
            return false;
        }
        void* indexMapped = nullptr;
        if (!check(vkMapMemory(device_, meshIndexMemory_, 0, VK_WHOLE_SIZE, 0, &indexMapped), "index vkMapMemory", error)) {
            return false;
        }
        std::memcpy(indexMapped, mesh.indices->data(), indexBytes);
        vkUnmapMemory(device_, meshIndexMemory_);
    }

    // Every mesh draw samples a page, so give it one: a white 1x1 bound whenever a batch names no page
    // (multiplying by linear white keeps the shipped vertex colours byte-identical), plus one real page per
    // material entry — a page that fails to upload fails the whole upload rather than quietly going colour-only
    // (docs/formats/GEOMETRY.md §23).
    {
        VkSamplerCreateInfo meshSamplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        meshSamplerInfo.magFilter = VK_FILTER_LINEAR;
        meshSamplerInfo.minFilter = VK_FILTER_LINEAR;
        meshSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        // Terrain UVs tile past one page: a boundary is where the next tile of the same page continues, so
        // wrap, not the clamp the full-image quad path wants.
        meshSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        meshSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (!check(vkCreateSampler(device_, &meshSamplerInfo, nullptr, &meshSampler_), "mesh vkCreateSampler", error))
            return false;
    }
    const VkDescriptorPoolSize meshPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                            static_cast<uint32_t>(mesh.materials.size() + 2u)};   // descriptorCount is the pool total, not per set
    VkDescriptorPoolCreateInfo meshPoolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    meshPoolInfo.maxSets = static_cast<uint32_t>(mesh.materials.size() + 2u);   // one set per page plus white, plus headroom — the driver refuses pools sized exactly at the boundary
    meshPoolInfo.poolSizeCount = 1;
    meshPoolInfo.pPoolSizes = &meshPoolSize;
    if (!check(vkCreateDescriptorPool(device_, &meshPoolInfo, nullptr, &meshPool_), "mesh vkCreateDescriptorPool",
               error)) {
        return false;
    }
    const auto addPage = [&](const TextureUpload& page, MeshMaterial& material) -> bool {
        if (!uploadSampledImage(page, &material.image, &material.memory, &material.view, error)) return false;
        VkDescriptorSetAllocateInfo setAllocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAllocate.descriptorPool = meshPool_;
        setAllocate.descriptorSetCount = 1;
        setAllocate.pSetLayouts = &meshSetLayout_;
        if (!check(vkAllocateDescriptorSets(device_, &setAllocate, &material.set), "mesh vkAllocateDescriptorSets",
                   error)) {
            return false;
        }
        const VkDescriptorImageInfo described{meshSampler_, material.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = material.set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &described;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        return true;
    };
    const std::array<std::uint8_t, 4> kWhiteByte{255u, 255u, 255u, 255u};   // linear white: multiplying changes nothing
    TextureUpload white;
    white.format = VK_FORMAT_B8G8R8A8_SRGB;   // every byte 0xFF: the order is moot and 0xFF decodes to exactly 1.0
    white.width = white.height = 1u;
    white.mipLevels = 1u;
    white.levels.push_back(TextureUpload::Level{0u, 4u});
    white.bytes = kWhiteByte.data();
    white.byteSize = 4u;
    if (!addPage(white, meshWhite_)) return false;
    meshMaterials_.clear();
    meshMaterials_.reserve(mesh.materials.size());
    for (const TextureUpload& page : mesh.materials) {
        MeshMaterial material;
        if (!addPage(page, material)) return false;
        meshMaterials_.push_back(material);
    }
    meshBatches_ = mesh.batches;
    // §31's per-batch spheres, computed here so a caller cannot forget them and every path shares one definition: the
    // shipped engine tests these against the frustum only (§29 [V-exe]); the sphere is the bounding box of the run's
    // vertices with the radius out to a corner, because over-approximating beats culling visible geometry.
    if (!meshBatches_.empty() && indexed) {
        const auto* records = static_cast<const std::int16_t*>(mesh.bytes);   // stride 20: i16 x, y, z then the rest (§2)
        for (auto& batch : meshBatches_) {
            int lo[3] = {32767, 32767, 32767};
            int hi[3] = {-32768, -32768, -32768};
            for (std::uint32_t slot = batch.indexStart; slot < batch.indexStart + batch.indexCount; ++slot) {
                const std::int16_t* record = records + 10 * static_cast<std::size_t>((*mesh.indices)[slot]);
                for (int axis = 0; axis < 3; ++axis) {
                    const int value = record[axis];
                    if (value < lo[axis]) lo[axis] = value;
                    if (value > hi[axis]) hi[axis] = value;
                }
            }
            if (hi[0] >= lo[0]) {   // an empty run keeps the default bound and always draws
                float radius = 0.0f;
                for (int axis = 0; axis < 3; ++axis) {
                    batch.bound[axis] = 0.5f * static_cast<float>(lo[axis] + hi[axis]);
                    const float half = 0.5f * static_cast<float>(hi[axis] - lo[axis]);
                    radius += half * half;
                }
                batch.bound[3] = std::sqrt(radius);   // to the box corner, not the mean edge
            }
        }
    }
    if (!meshBatches_.empty()) {
        OA_LOG(LogLevel::Info, "vk", "mesh materials uploaded: {} page(s) for {} batch(es), white-bound batches draw colours alone",
               meshMaterials_.size(), meshBatches_.size());
    }

    // The camera fits itself to these, so the archive's own numbers are what decide how big the level looks.
    for (int axis = 0; axis < 3; ++axis) {
        meshCentre_[axis] = 0.5f * (mesh.boundsMin[axis] + mesh.boundsMax[axis]);
        const float half = 0.5f * (mesh.boundsMax[axis] - mesh.boundsMin[axis]);
        meshRadius_ = axis == 0 ? half : std::max(meshRadius_, half);
    }
    if (!(meshRadius_ > 1e-3f)) meshRadius_ = 1.0f;   // a flat or point-like run still needs a finite scale
    meshVertexCount_ = mesh.vertexCount;
    meshIndexCount_ = indexed ? static_cast<std::uint32_t>(mesh.indices->size()) : 0u;
    if (error != nullptr) error->clear();
    return true;
}

std::array<float, 16> VkRenderer::buildMeshMatrix() const {
    // World to view: yaw about Z? The archive stores levels in its own axis convention, and which way is up has not
    // been established from the executable yet [INFERENCE], so the camera orbits and lets the picture say.
    const float cy = std::cos(camera_.yaw);
    const float sy = std::sin(camera_.yaw);
    const float cp = std::cos(camera_.pitch);
    const float sp = std::sin(camera_.pitch);
    // A = Rx(pitch) * Ry(yaw), rows of the world-to-view rotation. v = A * (p - target) puts the camera's look-at
    // point at the origin; +v.z runs away from the viewer, which is the direction the orthographic path below has
    // always mapped to greater depth.
    const float a[3][3] = {{cy, 0.0f, sy}, {sp * sy, cp, -sp * cy}, {-cp * sy, sp, cp * cy}};
    const float target[3] = {meshCentre_[0] + camera_.panX, meshCentre_[1] + camera_.panY, meshCentre_[2] + camera_.panZ};
    float m[4][3];   // rows of the linear part, applied to p - target; k[] holds the constant term of each row
    float k[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (camera_.fovDegrees > 0.0f) {
        // Perspective: the camera sits `distance` behind the target along -v.z, and the sphere that fits the mesh is
        // framed at zoom 1. A point's distance from the camera is u = v.z + distance, so w = u and depth is linear in u:
        // z_clip/w = aDepth + bDepth/u with aDepth + bDepth/nearPlane = 0 and aDepth + bDepth/farPlane = 1. The planes sit
        // outside the mesh at any zoom, so panning into geometry does not clip the subject away.
        const float fov = camera_.fovDegrees * 3.14159265358979323846f / 180.0f;
        const float distance = meshRadius_ * camera_.zoom / std::tan(0.5f * fov);
        const float nearPlane = std::max(1.0f, distance - 1.5f * meshRadius_);
        const float farPlane = distance + 1.5f * meshRadius_;
        const float scaleXY = distance / meshRadius_;   // the fitting sphere spans the frame at zoom 1
        const float aDepth = farPlane / (farPlane - nearPlane);
        const float bDepth = -nearPlane * farPlane / (farPlane - nearPlane);
        for (int column = 0; column < 3; ++column) {
            m[0][column] = scaleXY * a[0][column];
            m[1][column] = -scaleXY * a[1][column];
            m[2][column] = aDepth * a[2][column];
            m[3][column] = a[2][column];
        }
        k[2] = aDepth * distance + bDepth;
        k[3] = distance;
    } else {
        // Uniform scale keeps proportions; y flips because clip space has its origin at the top left in Vulkan, and z is
        // halved into the [0,1] depth range the attachment uses.
        const float s = camera_.zoom / meshRadius_;
        const float rowScale[3] = {s, -s, 0.5f * s};
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 3; ++row) m[row][column] = rowScale[row] * a[row][column];
            m[3][column] = 0.0f;
        }
        k[2] = 0.5f;   // centre the orthographic depth range on the target
        k[3] = 1.0f;
    }
    std::array<float, 16> matrix{};
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 4; ++row) matrix[column * 4 + row] = m[row][column];
    }
    // Translation: -M*target plus the row's constant.
    for (int row = 0; row < 4; ++row) {
        float sum = k[row];
        for (int column = 0; column < 3; ++column) sum -= m[row][column] * target[column];
        matrix[12 + row] = sum;
    }
    // matrix[15] is the constant of the w row, which the loop above already wrote (the orthographic path's k[3] is 1.0
    // and its m[3] is zero, so it stays 1 there; perspective needs `distance`, not a hardcoded 1).
    return matrix;
}

void VkRenderer::pushMeshMatrix(VkCommandBuffer command) const {
    const auto matrix = buildMeshMatrix();   // §31's cull planes come out of this same matrix so the two paths cannot drift apart
    vkCmdPushConstants(command, meshLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(matrix), matrix.data());
}

bool VkRenderer::renderMeshOffscreen(std::vector<std::byte>* outRgba, uint32_t size, std::string* error) {
    if (device_ == VK_NULL_HANDLE || meshVertexCount_ == 0) {
        if (error != nullptr) *error = "renderMeshOffscreen needs an initialised renderer and an uploaded mesh";
        return false;
    }
    // The attachment starts undefined: the pass redefines it, so no barrier is needed before recording.
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kSelfCheckFormat;
    imageInfo.extent = VkExtent3D{size, size, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage attachment = VK_NULL_HANDLE;
    if (!check(vkCreateImage(device_, &imageInfo, nullptr, &attachment), "mesh offscreen vkCreateImage", error)) {
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, attachment, &requirements);
    const uint32_t deviceLocal =
        findMemoryType(physical_, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = deviceLocal == 0xFFFFFFFFu ? 0 : deviceLocal;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (!check(vkAllocateMemory(device_, &allocate, nullptr, &memory), "mesh offscreen vkAllocateMemory", error)) {
        vkDestroyImage(device_, attachment, nullptr);
        return false;
    }
    if (!check(vkBindImageMemory(device_, attachment, memory, 0), "mesh offscreen vkBindImageMemory", error)) {
        vkFreeMemory(device_, memory, nullptr);
        vkDestroyImage(device_, attachment, nullptr);
        return false;
    }
    const bool drawn = sampleInto(attachment, VkExtent2D{size, size}, outRgba, error, true);
    vkFreeMemory(device_, memory, nullptr);
    vkDestroyImage(device_, attachment, nullptr);
    return drawn;
}

void VkRenderer::shutdown() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
    destroySwapchain(false);
    destroyPipeline();
    destroyTexture();
    if (descriptorLayout_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
        descriptorLayout_ = VK_NULL_HANDLE;
    }
    if (vertexMemory_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) vkFreeMemory(device_, vertexMemory_, nullptr);
    if (vertexBuffer_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, vertexBuffer_, nullptr);
    vertexMemory_ = VK_NULL_HANDLE;
    vertexBuffer_ = VK_NULL_HANDLE;
    if (meshMemory_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) vkFreeMemory(device_, meshMemory_, nullptr);
    if (meshBuffer_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, meshBuffer_, nullptr);
    meshMemory_ = VK_NULL_HANDLE;
    meshBuffer_ = VK_NULL_HANDLE;
    meshVertexCount_ = 0;
    if (meshIndexMemory_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) vkFreeMemory(device_, meshIndexMemory_, nullptr);
    if (meshIndexBuffer_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, meshIndexBuffer_, nullptr);
    meshIndexMemory_ = VK_NULL_HANDLE;
    meshIndexBuffer_ = VK_NULL_HANDLE;
    meshIndexCount_ = 0;
    if (commandPool_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    hasQueueFamily_ = false;
    physical_ = VK_NULL_HANDLE;
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        // SDL3 exposes this as a void call; getting the ordering right (surface before its window dies,
        // instance last) is what keeps validation from reporting a leaked VkSurfaceKHR.
        SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (messenger_ != VK_NULL_HANDLE) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy != nullptr) destroy(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

}  // namespace oa::render
