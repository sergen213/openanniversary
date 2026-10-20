#include "render/vk/VkRenderer.hpp"

#include <core/Log.hpp>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>

namespace oa::render {
namespace {

constexpr std::size_t kSelfCheckSize = 64;
constexpr VkFormat kSelfCheckFormat = VK_FORMAT_R8G8B8A8_UNORM;

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
    if (!createSwapchain(window.widthPixels(), window.heightPixels(), error)) return false;
    if (!createRenderTargets(error)) return false;
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
    const VkPresentModeKHR presentMode = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_FIFO_KHR) != modes.end()
                                             ? VK_PRESENT_MODE_FIFO_KHR
                                             : modes.front();

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
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &color;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 1;
    passInfo.pDependencies = &dependency;
    if (!check(vkCreateRenderPass(device_, &passInfo, nullptr, &renderPass_), "vkCreateRenderPass", error)) {
        return false;
    }

    for (SwapchainImage& target : images_) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = target.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format_;
        viewInfo.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!check(vkCreateImageView(device_, &viewInfo, nullptr, &target.view), "vkCreateImageView", error)) {
            return false;
        }
        VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = renderPass_;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &target.view;
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
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
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

    VkClearValue clear{};
    clear.color = VkClearColorValue{{config_.clearColour[0], config_.clearColour[1], config_.clearColour[2],
                                     config_.clearColour[3]}};
    VkRenderPassBeginInfo passBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    passBegin.renderPass = renderPass_;
    passBegin.framebuffer = images_[imageIndex].framebuffer;
    passBegin.renderArea = VkRect2D{{0, 0}, extent_};
    passBegin.clearValueCount = 1;
    passBegin.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
    // No draw calls yet: the clear pass is Milestone 2's proof that a swapchain image really goes through a
    // render pass. Geometry arrives when the DRM v14 parser lands (Milestone 4).
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
    imageInfo.format = kSelfCheckFormat;
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
    return true;
}

void VkRenderer::shutdown() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
    destroySwapchain(false);
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
