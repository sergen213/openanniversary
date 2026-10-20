// oa-game: native front end. Opens a real window, initialises Vulkan, and renders frames from the shipped archive
// path if one is configured. Milestone 2 scope: window + swapchain + clear pass + read-back self-check.

#include "cdc/Bigfile.hpp"
#include "cdc/Drm.hpp"
#include "cdc/Texture.hpp"
#include "core/Config.hpp"
#include "core/Log.hpp"
#include "platform/Window.hpp"
#include "render/vk/VkRenderer.hpp"

#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <string>

#include <filesystem>
#include <span>

namespace {

oa::LogLevel levelFromName(const std::string& name) {
    if (name == "trace") return oa::LogLevel::Trace;
    if (name == "debug") return oa::LogLevel::Debug;
    if (name == "warn") return oa::LogLevel::Warn;
    if (name == "error") return oa::LogLevel::Error;
    return oa::LogLevel::Info;
}

}  // namespace

int main(int argc, char** argv) {
    oa::Config config;
    config.setDefault("title", "OpenAnniversary");
    config.setDefault("width", "1600");
    config.setDefault("height", "900");
    config.setDefault("logLevel", "info");
    config.setDefault("gameDir", "");
    config.setDefault("vk.validation", "true");
    config.setDefault("frames", "0");      // 0 = run until the window is closed
    config.setDefault("selfCheck", "false");

    std::string error;
    if (const char* ini = std::getenv("OA_CONFIG"); ini != nullptr) {
        if (!config.loadFile(ini, &error)) OA_LOG(oa::LogLevel::Warn, "config", "{}", error);
    } else {
        config.loadFile("openanniversary.ini", &error);  // absence is normal
    }
    std::vector<std::string> rest;
    if (!config.applyCommandLine(argc, argv, &rest, &error)) {
        std::fprintf(stderr, "oa-game: %s\n", error.c_str());
        return 2;
    }

    oa::initLogging(levelFromName(config.str("logLevel")));
    OA_LOG(oa::LogLevel::Info, "app", "OpenAnniversary 0.1.0 starting (gameDir='{}')", config.str("gameDir"));

    if (!config.str("gameDir").empty()) {
        oa::cdc::BigfileSet archive;
        if (archive.open(config.str("gameDir"), &error)) {
            OA_LOG(oa::LogLevel::Info, "app", "container opened: {} record(s) in {} part(s)",
                   archive.records().size(), archive.partCount());
        } else {
            OA_LOG(oa::LogLevel::Warn, "app", "container unavailable: {}", error);
        }
    }

    if (!oa::platform::Window::initVideo(&error)) {
        OA_LOG(oa::LogLevel::Error, "app", "{}", error);
        return 2;  // no display reachable: caller should use an Xvfb session or a machine with output
    }

    oa::platform::Window window;
    oa::platform::WindowConfig windowConfig;
    windowConfig.title = config.str("title");
    windowConfig.width = config.integer("width", 1600);
    windowConfig.height = config.integer("height", 900);
    if (!window.create(windowConfig, &error)) {
        OA_LOG(oa::LogLevel::Error, "app", "{}", error);
        oa::platform::Window::quitVideo();
        return 2;
    }

    oa::render::VkRenderer renderer;
    oa::render::RendererConfig renderConfig;
    renderConfig.applicationName = windowConfig.title;
    renderConfig.validation = config.flag("vk.validation", true);
    // Shaders are compiled into build/shaders next to this executable; fall back to the working directory.
    std::filesystem::path shaderDirectory = "shaders";
    if (argc > 0) {
        const auto candidate = std::filesystem::path(argv[0]).parent_path() / "shaders";
        std::error_code ignored;
        if (std::filesystem::exists(candidate, ignored)) shaderDirectory = candidate;
    }
    renderConfig.shaderDirectory = shaderDirectory.string();
    // One teardown order for every exit path: the Vulkan surface belongs to SDL's window, so the renderer releases it
    // while that window is still alive, and the video subsystem goes last.
    struct Cleanup {
        oa::render::VkRenderer& renderer;
        oa::platform::Window& window;
        ~Cleanup() { renderer.shutdown(); window.destroy(); oa::platform::Window::quitVideo(); }
    } cleanup{renderer, window};
    if (!renderer.init(window, renderConfig, &error)) {
        OA_LOG(oa::LogLevel::Error, "app", "Vulkan init failed: {}", error);
        return 2;
    }

    const bool selfCheck = config.flag("selfCheck", false);
    if (selfCheck && !renderer.selfCheck(&error)) {
        OA_LOG(oa::LogLevel::Error, "app", "Vulkan self-check failed: {}", error);
        return 1;
    }

    // --texture=<path> [--section N] draws one shipped texture instead of a bare clear colour. Like oa-asset, this
    // tool reads numbers as hexadecimal (--section 12 is unit section 18).
    const std::string texturePath = config.str("texture");
    if (!texturePath.empty()) {
        oa::cdc::BigfileSet archive;
        if (!archive.open(config.str("gameDir"), &error)) {
            OA_LOG(oa::LogLevel::Error, "app", "--texture needs a readable container: {}", error);
            return 2;
        }
        const auto record = archive.find(texturePath);
        if (!record) {
            OA_LOG(oa::LogLevel::Error, "app", "no record for '{}'", texturePath);
            return 2;
        }
        const std::vector<std::byte> unitImage = archive.read(*record, &error);
        if (unitImage.empty() && !error.empty()) {
            OA_LOG(oa::LogLevel::Error, "app", "{}", error);
            return 2;
        }
        oa::cdc::DrmFile drm;
        if (!oa::cdc::DrmFile::parse(unitImage, &drm, &error)) {
            OA_LOG(oa::LogLevel::Error, "app", "'{}' is not a DRM unit: {}", texturePath, error);
            return 2;
        }

        const std::string sectionText = config.str("section");
        std::size_t wanted = drm.sections().size();
        oa::cdc::Pcd9Info info;
        std::span<const std::byte> payload;
        if (!sectionText.empty()) {
            wanted = static_cast<std::size_t>(std::strtoul(sectionText.c_str(), nullptr, 16));
            if (wanted >= drm.sections().size()) {
                OA_LOG(oa::LogLevel::Error, "app", "section {} is out of range in '{}' ({} section(s))", sectionText,
                       texturePath, drm.sections().size());
                return 2;
            }
            payload = drm.payload(drm.sections()[wanted]);
            if (!oa::cdc::parsePcd9(payload, &info, &error)) {
                OA_LOG(oa::LogLevel::Error, "app", "section {}: {}", sectionText, error);
                return 2;
            }
        } else {
            std::string ignored;
            for (std::size_t index = 0; index < drm.sections().size(); ++index) {
                oa::cdc::Pcd9Info probe;
                const auto candidate = drm.payload(drm.sections()[index]);
                if (oa::cdc::parsePcd9(candidate, &probe, &ignored)) {
                    wanted = index;
                    info = probe;
                    payload = candidate;
                    break;
                }
            }
            if (wanted == drm.sections().size()) {
                OA_LOG(oa::LogLevel::Error, "app", "'{}' has no PCD9 section, pass --section", texturePath);
                return 2;
            }
        }

        // The stored bytes are already sRGB-encoded, and the swapchain attachment is B8G8R8A8_SRGB, so sampling has to
        // decode (SRGB formats) for the hardware encode on write to land back on the shipped colours.
        VkFormat format = VK_FORMAT_UNDEFINED;
        if (info.tag == static_cast<std::uint32_t>(oa::cdc::Pcd9Format::Dxt1)) {
            format = VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        } else if (info.tag == static_cast<std::uint32_t>(oa::cdc::Pcd9Format::Dxt5)) {
            format = VK_FORMAT_BC3_SRGB_BLOCK;
        } else if (info.tag == static_cast<std::uint32_t>(oa::cdc::Pcd9Format::Uncompressed32)) {
            // Masks are white with their shape in alpha, which the blend state then uses directly.
            format = VK_FORMAT_B8G8R8A8_SRGB;
        } else {
            OA_LOG(oa::LogLevel::Error, "app", "PCD9 tag {} has no Vulkan format yet", info.tag);
            return 2;
        }

        oa::render::TextureUpload upload;
        upload.format = format;
        upload.width = info.width;
        upload.height = info.height;
        upload.mipLevels = info.mipLevels;
        upload.bytes = payload.data() + oa::cdc::kPcd9HeaderBytes;
        upload.byteSize = info.payloadBytes;
        for (const oa::cdc::Pcd9LevelRegion& region : oa::cdc::pcd9LevelRegions(info)) {
            upload.levels.push_back(oa::render::TextureUpload::Level{region.offset, region.bytes});
        }
        if (!renderer.uploadTexture(upload, &error)) {
            OA_LOG(oa::LogLevel::Error, "app", "texture upload failed: {}", error);
            return 1;
        }
        OA_LOG(oa::LogLevel::Info, "app", "drawing section {} of '{}' ({}x{}, {} mip(s), vkFormat={})", wanted,
               texturePath, info.width, info.height, info.mipLevels, static_cast<int>(format));
    }

    const int frameLimit = config.integer("frames", 0);
    int frames = 0;
    int lastWidth = window.widthPixels();
    int lastHeight = window.heightPixels();
    const auto started = std::chrono::steady_clock::now();
    while (!window.shouldClose()) {
        window.pumpEvents();
        const int width = window.widthPixels();
        const int height = window.heightPixels();
        if (width != lastWidth || height != lastHeight) {
            renderer.notifyResize(width, height);
            lastWidth = width;
            lastHeight = height;
        }
        if (!renderer.renderFrame(&error)) {
            if (!error.empty()) {
                OA_LOG(oa::LogLevel::Warn, "app", "frame skipped: {}", error);
                error.clear();
            }
        } else {
            ++frames;
        }
        if (frameLimit > 0 && frames >= frameLimit) break;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    OA_LOG(oa::LogLevel::Info, "app", "presented {} frame(s) on '{}' in {:.3f}s ({:.1f} fps)", frames,
           renderer.deviceName(), seconds, seconds > 0.0 ? frames / seconds : 0.0);

    return frames > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
