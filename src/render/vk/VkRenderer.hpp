#pragma once
// Vulkan renderer for the native front end: instance, surface from SDL, device selection, swapchain and a frame
// loop. The first milestone renders a clear pass only; geometry arrives with the DRM unit parser.
//
// Failure convention matches the rest of the tree: bool return plus std::string* error, no exceptions.

#include "platform/Window.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oa::render {

struct RendererConfig {
    std::string applicationName = "OpenAnniversary";
    bool validation = true;            // VK_LAYER_KHRONOS_validation when the loader offers it
    std::array<float, 4> clearColour{{0.10f, 0.35f, 0.62f, 1.0f}};
    std::string shaderDirectory = "shaders";   // resolved against the executable's own directory by the caller
};

// One texture ready for upload: a parsed payload's packed mip chain plus the Vulkan format that matches its PCD9
// tag. The level list exists so the uploader can issue one copy per mip without learning anything about PCD9.
struct TextureUpload {
    struct Level {
        std::uint64_t offset = 0;   // bytes from `bytes`
        uint32_t bytes = 0;
    };

    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    std::vector<Level> levels;
    const void* bytes = nullptr;
    std::size_t byteSize = 0;
};
class VkRenderer {
public:
    ~VkRenderer();

    bool init(platform::Window& window, const RendererConfig& config, std::string* error = nullptr);
    void shutdown();

    // Called when the drawable size changes; recreation is deferred to the next frame like the game does it.
    void notifyResize(int widthPixels, int heightPixels);

    // Acquires an image and records+submits a clear pass. Returns false when this frame has nothing to present
    // (resize pending, timeout); the caller then simply skips presentation for one iteration.
    bool renderFrame(std::string* error = nullptr);

    // Proves device work end-to-end without depending on what is visible: clears an offscreen image, copies it back
    // and checks every pixel against the clear colour. Runs on any display state, so CI/Xvfb can assert on it.
    bool selfCheck(std::string* error = nullptr);

    // Uploads a texture and makes it the one drawn by subsequent frames. Mip levels beyond the first are uploaded
    // too, so minification stays sharp instead of showing the top level shrunken.
    bool uploadTexture(const TextureUpload& texture, std::string* error = nullptr);
    bool hasTexture() const { return texture_ != VK_NULL_HANDLE; }

    const std::string& deviceName() const { return deviceName_; }
    VkDevice device() const { return device_; }
    VkFormat swapchainFormat() const { return format_; }

private:
    struct SwapchainImage {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
    };

    bool createInstance(const RendererConfig& config, std::string* error);
    bool createSurface(platform::Window& window, std::string* error);
    bool pickDevice(std::string* error);
    bool createSwapchain(int widthPixels, int heightPixels, std::string* error);
    bool createRenderTargets(std::string* error);
    bool createPipeline(std::string* error);
    void destroyPipeline();
    void destroyTexture();
    bool sampleInto(VkImage colourAttachment, VkExtent2D size, std::vector<std::byte>* outRgba, std::string* error);
    void destroySwapchain(bool waitForIdle);

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugMessenger(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                                         const VkDebugUtilsMessengerCallbackDataEXT* data,
                                         void* userData);

    RendererConfig config_;
    std::string deviceName_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    bool hasQueueFamily_ = false;
    VkQueue queue_ = nullptr;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<SwapchainImage> images_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;

    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkImage texture_ = VK_NULL_HANDLE;
    VkDeviceMemory textureMemory_ = VK_NULL_HANDLE;
    VkImageView textureView_ = VK_NULL_HANDLE;
    uint32_t textureMipLevels_ = 1;
    float textureAspect_ = 1.0f;
    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory_ = VK_NULL_HANDLE;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commands_;
    std::vector<VkFence> frameFences_;
    std::vector<VkSemaphore> acquireSemaphores_;  // one per swapchain image, signalled by acquisition
    std::size_t frameIndex_ = 0;

    bool recreatePending_ = false;
};

}  // namespace oa::render
