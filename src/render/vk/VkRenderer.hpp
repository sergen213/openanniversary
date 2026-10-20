#pragma once
// Vulkan renderer for the native front end: instance, surface from SDL, device selection, swapchain, a frame loop
// and offscreen read-back. It draws two things today: one textured quad (docs/formats/TEXTURES.md) and one
// unindexed triangle run with vertex colours (docs/formats/GEOMETRY.md §2, §6).
//
// Failure convention matches the rest of the tree: bool return plus std::string* error, no exceptions.

#include "platform/Window.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cmath>
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

// One geometry payload ready for upload: the record bytes of a general section, as many whole 20-byte vertices
// as they hold, plus the bounds the caller measured so the camera can be fitted without walking them again here.
// `indices`, when present, draws those vertices as a triangle list (the terrain strips of
// docs/formats/GEOMETRY.md §9); without it the run is drawn straight, three records per triangle.
struct MeshUpload {
    const void* bytes = nullptr;
    std::uint32_t vertexCount = 0;
    const std::vector<std::uint32_t>* indices = nullptr;
    float boundsMin[3] = {0.0f, 0.0f, 0.0f};
    float boundsMax[3] = {0.0f, 0.0f, 0.0f};
};

// Camera for the mesh pass: yaw and pitch in radians around a target, `zoom` scales the fit, `panX/panY/panZ` move that
// target through world units (the archive's own axis convention, see pushMeshMatrix), and `fovDegrees` switches between
// the orthographic fit used by the deterministic probes (0) and a perspective frustum for looking around a level.
struct MeshCamera {
    float yaw = 0.7f;
    float pitch = 0.4f;
    float zoom = 1.0f;
    float panX = 0.0f;
    float panY = 0.0f;
    float panZ = 0.0f;
    float fovDegrees = 0.0f;   // 0 keeps the orthographic fit
};

// The keys the mesh pass listens for, resolved to booleans by the app so this mapping needs no window.
struct MeshKeys {
    bool left = false, right = false, up = false, down = false;
    bool forward = false, back = false, strafeLeft = false, strafeRight = false;
    bool dollyIn = false, dollyOut = false;
};

// One frame of camera motion: arrows orbit (pitch clamped short of the poles), Q/E dolly the fit, WASD slide the
// look-at target along the view's own basis vectors. panTarget accumulates in world units and is mirrored into
// camera.pan*, so a caller that rebuilds MeshCamera every frame keeps its place. camStep is fitted to the mesh extent.
inline void applyMeshKeys(MeshCamera& camera, float panTarget[3], const MeshKeys& keys, float camStep) {
    const float cy = std::cos(camera.yaw), sy = std::sin(camera.yaw);
    const float cp = std::cos(camera.pitch), sp = std::sin(camera.pitch);
    const float right[3] = {cy, 0.0f, sy};
    const float away[3] = {-cp * sy, sp, cp * cy};
    if (keys.left) camera.yaw -= 0.02f;
    if (keys.right) camera.yaw += 0.02f;
    if (keys.up) camera.pitch = std::min(camera.pitch + 0.02f, 1.5f);
    if (keys.down) camera.pitch = std::max(camera.pitch - 0.02f, -1.5f);
    const float pan[3][2] = {{right[0], away[0]}, {right[1], away[1]}, {right[2], away[2]}};
    for (int axis = 0; axis < 3; ++axis) {
        if (keys.forward) panTarget[axis] += camStep * pan[axis][1];
        if (keys.back) panTarget[axis] -= camStep * pan[axis][1];
        if (keys.strafeRight) panTarget[axis] += camStep * pan[axis][0];
        if (keys.strafeLeft) panTarget[axis] -= camStep * pan[axis][0];
    }
    if (keys.dollyIn) camera.zoom *= 1.03f;
    if (keys.dollyOut) camera.zoom /= 1.03f;
    camera.panX = panTarget[0];
    camera.panY = panTarget[1];
    camera.panZ = panTarget[2];
}

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

    // Uploads a triangle run and makes it the geometry drawn by subsequent frames, depth-tested against itself.
    bool uploadMesh(const MeshUpload& mesh, std::string* error = nullptr);
    bool hasMesh() const { return meshVertexCount_ != 0; }
    void setMeshCamera(const MeshCamera& camera) { camera_ = camera; }

    // Draws the uploaded mesh into an offscreen RGBA8 image and copies it back, so a headless run (Xvfb, CI) can
    // assert that shipped vertex positions and colours really reach pixels. `size` is square in pixels.
    bool renderMeshOffscreen(std::vector<std::byte>* outRgba, uint32_t size, std::string* error = nullptr);


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
    void destroySwapchain(bool waitForIdle);
    bool createRenderTargets(std::string* error);
    bool createPipeline(std::string* error);
    void destroyPipeline();
    void destroyTexture();
    // `drawMesh` picks the mesh pipeline and its vertex buffer instead of the textured quad; either way the pass
    // gets a depth attachment when there is geometry to sort against.
    bool sampleInto(VkImage colourAttachment, VkExtent2D size, std::vector<std::byte>* outRgba, std::string* error,
                    bool drawMesh = false);
    bool createDepthImage(std::string* error);
    void destroyDepthImage();
    bool createMeshPipeline(std::string* error);
    void destroyMeshPipeline();
    void pushMeshMatrix(VkCommandBuffer command) const;
    // Binds the mesh pipeline plus its vertex (and index, when there is one) buffers and issues the draw; both the
    // swapchain pass and the offscreen probe call it.
    void drawMeshGeometry(VkCommandBuffer command) const;

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
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;

    VkPipeline meshPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout meshLayout_ = VK_NULL_HANDLE;
    VkBuffer meshBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory meshMemory_ = VK_NULL_HANDLE;
    std::uint32_t meshVertexCount_ = 0;
    VkBuffer meshIndexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory meshIndexMemory_ = VK_NULL_HANDLE;
    std::uint32_t meshIndexCount_ = 0;
    MeshCamera camera_;
    float meshCentre_[3] = {0.0f, 0.0f, 0.0f};
    float meshRadius_ = 1.0f;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commands_;
    std::vector<VkFence> frameFences_;
    std::vector<VkSemaphore> acquireSemaphores_;  // one per swapchain image, signalled by acquisition
    std::size_t frameIndex_ = 0;

    bool recreatePending_ = false;
};

}  // namespace oa::render
