// oa-game: native front end. Opens a real window, initialises Vulkan, and renders frames from the shipped archive
// path if one is configured. Milestone 2 scope: window + swapchain + clear pass + read-back self-check.

#include "cdc/Bigfile.hpp"
#include "cdc/Drm.hpp"
#include "cdc/Geometry.hpp"
#include "cdc/Texture.hpp"
#include "core/Config.hpp"
#include "core/Log.hpp"
#include "platform/Window.hpp"
#include "render/vk/VkRenderer.hpp"

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <map>

#include <cstdio>
#include <set>
#include <vector>
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

    // --mesh=<record> draws one shipped triangle run with vertex colours, straight out of the container: no scene
    // graph, no materials (docs/formats/GEOMETRY.md §6). --section is hexadecimal like --texture's; without it the
    // largest stored type-0 section is used, which is a convenience for level units only — §1 shows why it is not a
    // general rule. --yaw/--pitch/--zoom orbit and fit the camera so one run frames identically every time.
    const std::string meshPath = config.str("mesh");
    if (!meshPath.empty()) {
        oa::cdc::BigfileSet archive;
        if (!archive.open(config.str("gameDir"), &error)) {
            OA_LOG(oa::LogLevel::Error, "app", "--mesh needs a readable container: {}", error);
            return 2;
        }
        auto record = archive.find(meshPath);
        if (!record && meshPath.size() == 8) {
            // Most level units are unnamed: the archive only stores their path hash, so eight hexadecimal digits
            // address them directly (docs/formats/DRM.md §1).
            const auto wantedHash = static_cast<std::uint32_t>(std::strtoul(meshPath.c_str(), nullptr, 16));
            for (std::size_t i = 0; i < archive.records().size(); ++i) {
                if (archive.records()[i].hash == wantedHash) {
                    record = i;
                    break;
                }
            }
        }
        if (!record) {
            OA_LOG(oa::LogLevel::Error, "app", "no record for '{}'", meshPath);
            return 2;
        }
        const std::vector<std::byte> unitImage = archive.read(*record, &error);
        if (unitImage.empty() && !error.empty()) {
            OA_LOG(oa::LogLevel::Error, "app", "{}", error);
            return 2;
        }
        oa::cdc::DrmFile drm;
        if (!oa::cdc::DrmFile::parse(unitImage, &drm, &error)) {
            OA_LOG(oa::LogLevel::Error, "app", "'{}' is not a DRM unit: {}", meshPath, error);
            return 2;
        }

        const std::string sectionText = config.str("section");
        int chosen = -1;
        if (!sectionText.empty()) {
            const auto index = static_cast<std::size_t>(std::strtoul(sectionText.c_str(), nullptr, 16));
            if (index >= drm.sections().size()) {
                OA_LOG(oa::LogLevel::Error, "app", "section {} is out of range in '{}' ({} section(s))", sectionText,
                       meshPath, drm.sections().size());
                return 2;
            }
            chosen = static_cast<int>(index);
        } else {
            // The largest stored general section: a level unit's geometry dwarfs its sound and script blocks.
            for (std::size_t i = 0; i < drm.sections().size(); ++i) {
                const oa::cdc::DrmSection& s = drm.sections()[i];
                if (!s.stored || s.type != 0) continue;
                if (chosen < 0 || drm.sections()[static_cast<std::size_t>(chosen)].size < s.size) {
                    chosen = static_cast<int>(i);
                }
            }
        }
        if (chosen < 0) {
            OA_LOG(oa::LogLevel::Error, "app", "'{}' has no stored general section to draw", meshPath);
            return 2;
        }
        const oa::cdc::DrmSection& geometry = drm.sections()[static_cast<std::size_t>(chosen)];
        const std::span<const std::byte> payload = drm.payload(geometry);
        std::vector<oa::cdc::MeshVertex> vertices;
        if (!oa::cdc::decodeMesh(payload, &vertices, &error)) {
            OA_LOG(oa::LogLevel::Error, "app", "section {} of '{}' is not a triangle run: {}", chosen, meshPath, error);
            return 2;
        }
        const oa::cdc::MeshBounds bounds = oa::cdc::meshBounds(vertices);


        oa::render::MeshUpload mesh;
        // The buffer is the payload itself: the vertex layout is identical, so there is nothing to repack.
        mesh.bytes = payload.data();
        mesh.vertexCount = static_cast<std::uint32_t>(vertices.size());
        for (int axis = 0; axis < 3; ++axis) {
            mesh.boundsMin[axis] = bounds.min[axis];
            mesh.boundsMax[axis] = bounds.max[axis];
        }
        if (!renderer.uploadMesh(mesh, &error)) {
            OA_LOG(oa::LogLevel::Error, "app", "cannot upload the triangle run: {}", error);
            return 2;
        }
        oa::render::MeshCamera camera;
        if (const std::string yaw = config.str("yaw"); !yaw.empty()) camera.yaw = std::strtof(yaw.c_str(), nullptr);
        if (const std::string pitch = config.str("pitch"); !pitch.empty()) {
            camera.pitch = std::strtof(pitch.c_str(), nullptr);
        }
        if (const std::string zoom = config.str("zoom"); !zoom.empty()) camera.zoom = std::strtof(zoom.c_str(), nullptr);
        renderer.setMeshCamera(camera);
        OA_LOG(oa::LogLevel::Info, "app", "drawing section {} of '{}' (id {}, type {}): {} vertices, {} triangles, "
                                         "bounds {:.0f}x{:.0f}x{:.0f} world units",
               chosen, meshPath, geometry.id, static_cast<int>(geometry.type), vertices.size(), bounds.triangles,
               bounds.max[0] - bounds.min[0], bounds.max[1] - bounds.min[1], bounds.max[2] - bounds.min[2]);

        // With --self-check the same mesh is drawn offscreen and every lit pixel must be one of the shipped vertex
        // colours: that one pass proves upload, index format, matrix convention and the colour path (docs/TESTING.md).
        // --meshDump=<path> writes the same offscreen pass as a 24-bit BMP, so the picture can be looked at rather
        // than only counted. 512 rows of three bytes need no padding (512*3 divides by four).
        if (const std::string dump = config.str("meshDump"); !dump.empty()) {
            std::vector<std::byte> pixels;
            constexpr std::uint32_t kEdge = 512u;
            if (!renderer.renderMeshOffscreen(&pixels, kEdge, &error)) {
                OA_LOG(oa::LogLevel::Error, "app", "mesh dump failed: {}", error);
                return 2;
            }
            std::FILE* handle = std::fopen(dump.c_str(), "wb");
            if (handle == nullptr) {
                OA_LOG(oa::LogLevel::Error, "app", "cannot write '{}'", dump);
                return 2;
            }
            const auto put32 = [](std::uint8_t* at, std::uint32_t value) {
                for (int i = 0; i < 4; ++i) at[i] = static_cast<std::uint8_t>(value >> (8u * static_cast<unsigned>(i)));
            };
            std::uint8_t header[54]{};
            header[0] = 'B';
            header[1] = 'M';
            put32(header + 2, 54u + 3u * kEdge * kEdge);
            put32(header + 10, 54u);
            put32(header + 14, 40u);
            put32(header + 18, kEdge);
            put32(header + 22, kEdge);   // positive height: the rows below go bottom-up
            header[26] = 1;              // colour planes
            header[28] = 24;             // bits per pixel
            std::fwrite(header, 1, sizeof(header), handle);
            std::vector<char> row(3u * kEdge);
            for (std::uint32_t y = kEdge; y-- > 0;) {
                for (std::uint32_t x = 0; x < kEdge; ++x) {
                    const std::size_t at = (static_cast<std::size_t>(y) * kEdge + x) * 4u;
                    row[3u * x] = static_cast<char>(pixels[at + 2]);   // blue
                    row[3u * x + 1u] = static_cast<char>(pixels[at + 1]);
                    row[3u * x + 2u] = static_cast<char>(pixels[at]);   // red
                }
                std::fwrite(row.data(), 1, row.size(), handle);
            }
            std::fclose(handle);
            OA_LOG(oa::LogLevel::Info, "app", "mesh dump: {}x{} written to {}", kEdge, kEdge, dump);
        }
        if (config.flag("selfCheck", false)) {
            std::vector<std::byte> pixels;
            constexpr std::uint32_t kProbeSize = 96u;
            if (!renderer.renderMeshOffscreen(&pixels, kProbeSize, &error)) {
                OA_LOG(oa::LogLevel::Error, "app", "mesh probe failed: {}", error);
                return 2;
            }
            std::set<std::uint32_t> shipped;
            for (const oa::cdc::MeshVertex& v : vertices) {
                shipped.insert((static_cast<std::uint32_t>(v.red) << 16) | (static_cast<std::uint32_t>(v.green) << 8) |
                               v.blue);
            }
            std::size_t lit = 0, unexpected = 0;
            for (std::uint32_t y = 0; y < kProbeSize; ++y) {
                for (std::uint32_t x = 0; x < kProbeSize; ++x) {
                    const std::size_t at = (static_cast<std::size_t>(y) * kProbeSize + x) * 4u;
                    // B8G8R8A8_SRGB: the stored byte is the display-encoded value, so it compares against the
                    // shipped colour byte directly, up to one step of rounding.
                    const auto blue = static_cast<std::uint32_t>(pixels[at + 0]);
                    const auto green = static_cast<std::uint32_t>(pixels[at + 1]);
                    const auto red = static_cast<std::uint32_t>(pixels[at + 2]);
                    if (blue < 8u && green < 8u && red < 8u) continue;  // the transparent clear showing through
                    ++lit;
                    bool known = false;
                    for (int dr = -1; dr <= 1 && !known; ++dr) {
                        for (int dg = -1; dg <= 1 && !known; ++dg) {
                            for (int db = -1; db <= 1 && !known; ++db) {
                                if (shipped.count(((red + dr) << 16) | ((green + dg) << 8) | (blue + db))) {
                                    known = true;
                                }
                            }
                        }
                    }
                    if (!known) ++unexpected;
                }
            }
            OA_LOG(oa::LogLevel::Info, "app", "mesh probe: {}/{} pixels covered by {} distinct vertex colours, {} "
                                             "unexplained",
                   lit, kProbeSize * kProbeSize, shipped.size(), unexpected);
            if (lit == 0 || unexpected > 0) {
                OA_LOG(oa::LogLevel::Error, "app", "mesh probe failed: empty or unexplained coverage");
                return 2;
            }
        }
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
