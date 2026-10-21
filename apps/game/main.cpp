// oa-game: native front end. Opens a real window, initialises Vulkan, and renders frames from the shipped archive
// path if one is configured. Milestone 2 scope: window + swapchain + clear pass + read-back self-check.

#include "cdc/Bigfile.hpp"
#include "cdc/Drm.hpp"
#include "cdc/Geometry.hpp"
#include "cdc/TerrainQuery.hpp"
#include "game/Avatar.hpp"
#include "cdc/Texture.hpp"
#include "core/Config.hpp"
#include "core/Log.hpp"
#include "platform/Window.hpp"
#include "render/vk/VkRenderer.hpp"

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <map>
#include <cmath>

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
    // The mesh camera lives outside the --mesh block so the frame loop can move it with keys; panTarget is its look-at
    // offset in world units and camStep is fitted to whatever mesh gets drawn.
    oa::render::MeshCamera camera;
    float panTarget[3] = {0.0f, 0.0f, 0.0f};
    float camStep = 1.0f;
    // --avatarDrive=true hands the body to the keyboard in the frame loop. These live outside the --mesh block for the
    // same reason the camera does, and driveTerrain is moved out of the scripted run below once that has reported.
    oa::cdc::TerrainQuery driveTerrain;
    oa::game::AvatarState driven;
    bool driveAvatar = false;
    const oa::game::AvatarTuning driveTuning;   // the same defaults the scripted run reports
    bool drivenAlive = true;
    std::uint64_t driveTicks = 0u;   // physics ticks since the keyboard took over, for the periodic report
    // Wall-clock pacing for the drive loop: the swapchain is uncapped, so one tick per presented frame would move the
    // body faster than the tuning says it should. The clock starts here; the gap covering container loading and upload is
    // dropped by the quarter-second cap below rather than run as catch-up ticks.
    std::chrono::steady_clock::time_point driveLast = std::chrono::steady_clock::now();
    double driveAccum = 0.0;
    // The mesh's framing centre, filled when the body is handed to the keyboard. camera.pan* is an offset from that
    // centre (VkRenderer.cpp builds the look-at as meshCentre + pan), so following a body needs the subtraction.
    float viewCentre[3] = {0.0f, 0.0f, 0.0f};
    // --avatarFollow=true asks for the look-at to sit on the body; the pan itself can only be applied once the framing
    // centre exists, so this records the world point and lets the framing code below do the subtraction.
    bool followBody = false;
    float followAt[3] = {0.0f, 0.0f, 0.0f};

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


        // --strips=true draws the shipped terrain topology rather than reading the vertex array as one run. Each oct-tree
        // in the unit hands its leaves' references to decodeTerrainStripChains, which follows nextTexture from a leaf's
        // record instead of guessing where records start: with the section numbering Drm.hpp documents, all 1,620 leaf
        // references of this unit land on a record header (docs/formats/GEOMETRY.md §15). Culling is not applied yet, so
        // whole chains are drawn rather than the cells a camera would see.
        std::vector<std::uint32_t> stripIndices;
        std::vector<std::uint32_t> drawIndices;   // the gated copy handed to the GPU
        std::uint32_t coarseTriangles = 0;
        constexpr double kMaxEdgeSquared = 1000.0 * 1000.0;   // world units squared, see the gate below
        if (config.flag("strips", false)) {
            std::uint32_t trees = 0;
            std::uint32_t records = 0;
            std::uint32_t heads = 0;
            std::uint32_t badHeads = 0;
            std::uint32_t brokenLinks = 0;
            // §20 + sweep-bases3 (GEOMETRY.md §21): a strip record's indices are relative to its material's block of this vertex
            // array, and the unit stores each block's base in a section of its own. findTerrainMaterialBases walks every tree once
            // without bases first — which same-shaped run is THE table answers from the matIdx values (header +20) those chains
            // name: take the first decoded table whose length covers them all. Units with no covering table stay absolute (§13).
            oa::cdc::TerrainMaterialList materials;
            bool haveMaterials = false;
            std::string tableError;   // names the refusing rule when no section's run covers this unit's chains (§21)
            if (oa::cdc::findTerrainMaterialBases(drm, 0u, &materials, nullptr, &tableError)) {   // §21 census rule
                haveMaterials = true;
            }   // no else needed; §13's absolute reading stands when the census finds nothing to cover
            for (std::size_t i = 0; i < drm.sections().size(); ++i) {
                const oa::cdc::DrmSection& nodeSection = drm.sections()[i];
                if (!nodeSection.stored || nodeSection.relocCount == 0) continue;
                std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> references;
                for (const auto& r : drm.relocations(nodeSection)) {
                    if (oa::cdc::relocKind(r) != 0) continue;   // node fields are patched by kind-0 entries (§9)
                    references[static_cast<std::uint32_t>(r.siteOffset)] = {
                        static_cast<std::uint32_t>(oa::cdc::relocSectionIndex(r)), drm.relocAddend(nodeSection, r)};
                }
                oa::cdc::TerrainOctree tree;
                std::string why;
                if (!oa::cdc::decodeTerrainOctree(drm.payload(nodeSection), references, &tree, &why)) continue;
                ++trees;
                std::map<std::uint32_t, std::vector<std::uint32_t>> headsPerSection;
                for (const auto& leaf : tree.leafList) {
                    if (leaf.targetSection >= drm.sections().size()) continue;
                    headsPerSection[leaf.targetSection].push_back(leaf.offsetInTarget);
                }
                for (const auto& entry : headsPerSection) {
                    const oa::cdc::DrmSection& strip = drm.sections()[entry.first];
                    if (!strip.stored) continue;
                    oa::cdc::TerrainStripChain walked;
                    std::string chainError;
                    if (!oa::cdc::decodeTerrainStripChains(
                            drm.payload(strip), entry.second, vertices.size(), &walked, &chainError,
                            haveMaterials ? std::span<const std::uint32_t>(materials.vertexBase)
                                          : std::span<const std::uint32_t>{})) {
                        OA_LOG(oa::LogLevel::Warn, "app", "section {} of '{}' chains skipped: {}", entry.first, meshPath,
                               chainError);
                        continue;
                    }
                    records += walked.records;
                    heads += walked.heads;
                    badHeads += walked.badHeads;
                    brokenLinks += walked.brokenLinks;
                    // Gate what goes to the screen, which is also what a foot probes through the terrain grid (§19). With
                    // §20's bases in place almost every triangle is tight (unit 4956BEA0: p50 edge 153 units); a residual
                    // population still lands at thousands of units, so an edge gate like the one §18 tried keeps it off -
                    // non-degenerate (shortest edge over 1 unit) and longest edge under 1,000 units. Those triangles are
                    // counted in `coarseTriangles` and left undrawn, not guessed at.
                    for (std::size_t t = 0; t + 2 < walked.indices.size(); t += 3u) {
                        const auto& a = vertices[walked.indices[t]];
                        const auto& b = vertices[walked.indices[t + 1u]];
                        const auto& c = vertices[walked.indices[t + 2u]];
                        const double e[3] = {(double)(a.x - b.x) * (a.x - b.x) + (double)(a.y - b.y) * (a.y - b.y) + (double)(a.z - b.z) * (a.z - b.z),
                                             (double)(b.x - c.x) * (b.x - c.x) + (double)(b.y - c.y) * (b.y - c.y) + (double)(b.z - c.z) * (b.z - c.z),
                                             (double)(c.x - a.x) * (c.x - a.x) + (double)(c.y - a.y) * (c.y - a.y) + (double)(c.z - a.z) * (c.z - a.z)};
                        const double low = std::min({e[0], e[1], e[2]});
                        const double high = std::max({e[0], e[1], e[2]});
                        stripIndices.insert(stripIndices.end(), {walked.indices[t], walked.indices[t + 1u], walked.indices[t + 2u]});
                        if (!(low > 1.0 && high < kMaxEdgeSquared)) { ++coarseTriangles; continue; }
                        drawIndices.insert(drawIndices.end(), {walked.indices[t], walked.indices[t + 1u], walked.indices[t + 2u]});
                    }
                }
            }
            OA_LOG(oa::LogLevel::Info, "app", "{} terrain tree(s): {} leaf reference(s) reached {} record(s), drawing "
                                              "{} of {} triangle(s) over {} vertex/vertices ({} gated by the edge rule, "
                                              "{} bad head(s), {} broken link(s))",
                   trees, heads, records, drawIndices.size() / 3u, stripIndices.size() / 3u,
                   vertices.size(), coarseTriangles, badHeads, brokenLinks);
        }

        // --avatar=true answers for the foot with a column query over the strip records that survived the gate. The older
        // `--walk=true` probe is gone: it walked a fixed straight line from the mesh centre, which on the gated set starts
        // over a hole and exits with an error (§19), and --avatar=true measures the same query with support counts, a jump
        // and level-change accounting. GEOMETRY.md §16 measured that some chains land far away because no vertex base has
        // been established for them, which is why the app keeps saying these heights measure the decode rather than shipped
        // ground.
        const bool wantAvatar = config.flag("avatar", false);
        oa::cdc::TerrainQuery terrain;
        if (wantAvatar) {
            if (drawIndices.empty()) {
                OA_LOG(oa::LogLevel::Error, "app", "--avatar needs --strips=true: it reads the strip index lists");
                return 2;
            }
            std::string queryError;
            // The grid is built over what actually gets drawn, not every chained triangle: §18 measured that the ungated
            // list carries triangles tens of thousands of units across, and a column query answers with the highest one,
            // so an agent would walk on those slivers instead of the floor.
            if (!terrain.build(vertices, drawIndices, {}, &queryError)) {
                OA_LOG(oa::LogLevel::Error, "app", "cannot build the terrain query: {}", queryError);
                return 2;
            }
            OA_LOG(oa::LogLevel::Info, "app", "terrain grid: {} triangle(s), cell {:.1f} units, {}x{} cells, y {:.0f}..{:.0f}",
                   terrain.triangleCount(), terrain.cellSize(), terrain.cellsX(), terrain.cellsZ(),
                   terrain.boundsMin()[1], terrain.boundsMax()[1]);
            OA_LOG(oa::LogLevel::Warn, "app", "the surface under foot comes from strip index lists whose vertex base is not "
                                              "established (GEOMETRY.md §16): these heights measure the decode, not shipped ground");
        }

        // --avatar=true drives the movement core over shipped geometry with a fixed script - walk one way, take one jump,
        // keep walking - so the run repeats exactly and reports what gravity, ground contact and step-up did against this
        // level's own triangles (docs/ROADMAP.md Milestone 5). --avatarDrive=true then hands that same body to the keyboard;
        // nothing here claims the tuning numbers are the shipped ones (see src/game/Avatar.hpp).
        if (wantAvatar) {
            const oa::game::AvatarTuning tuning;   // defaults, logged below so a run can be reproduced from the log alone
            constexpr float kStep = 1.0f / 60.0f;
            const std::uint32_t ticks = static_cast<std::uint32_t>(std::max(1, config.integer("avatarTicks", 300)));
            const float yaw = config.str("avatarYaw").empty() ? 1.5707963267948966f
                                                              : std::strtof(config.str("avatarYaw").c_str(), nullptr);

            // Spawn where the decode is actually walkable: score each sampled column by how many of its eight neighbours
            // one cell away are supported too, and take the densest. §15-§16 measured that these chains address only a
            // compact block of the vertex array (16,338 of 149,077 vertices here), so coverage - not the physics - is what
            // limits where an agent can stand; the census prints that instead of hiding it.
            oa::cdc::TerrainHit spawn{};
            float spawnX = 0.0f, spawnZ = 0.0f;
            int bestScore = -1;
            bool foundSpawn = false;
            constexpr std::uint32_t kSamples = 48u;
            const float cell = terrain.cellSize();
            std::uint32_t probes = 0u, supportedProbes = 0u;
            for (std::uint32_t iz = 0u; iz < kSamples; ++iz) {
                for (std::uint32_t ix = 0u; ix < kSamples; ++ix) {
                    const float sx = terrain.boundsMin()[0] +
                                     (terrain.boundsMax()[0] - terrain.boundsMin()[0]) * (static_cast<float>(ix) + 0.5f) / static_cast<float>(kSamples);
                    const float sz = terrain.boundsMin()[2] +
                                     (terrain.boundsMax()[2] - terrain.boundsMin()[2]) * (static_cast<float>(iz) + 0.5f) / static_cast<float>(kSamples);
                    oa::cdc::TerrainHit here{};
                    ++probes;
                    if (!terrain.surfaceAt(sx, sz, terrain.boundsMax()[1], 4.0f * cell, &here)) continue;
                    ++supportedProbes;
                    int score = 0;
                    for (int dz = -1; dz <= 1; ++dz) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 && dz == 0) continue;
                            oa::cdc::TerrainHit neighbour{};
                            if (terrain.surfaceAt(sx + static_cast<float>(dx) * cell, sz + static_cast<float>(dz) * cell,
                                                  here.y + cell, 4.0f * cell, &neighbour)) {
                                ++score;
                            }
                        }
                    }
                    if (!foundSpawn || score > bestScore) {
                        foundSpawn = true;
                        bestScore = score;
                        spawn = here;
                        spawnX = sx;
                        spawnZ = sz;
                    }
                }
            }
            OA_LOG(oa::LogLevel::Info, "app", "avatar census: {} of {} sampled column(s) supported across {:.0f}x{:.0f} "
                                              "units; densest spot scores {}/8 at ({:.1f}, {:.1f}) ground {:.1f}",
                   supportedProbes, probes, terrain.boundsMax()[0] - terrain.boundsMin()[0],
                   terrain.boundsMax()[2] - terrain.boundsMin()[2], bestScore, spawnX, spawnZ, spawn.y);
            if (!foundSpawn) {
                OA_LOG(oa::LogLevel::Error, "app", "no supported column anywhere in the gated terrain: cannot place an avatar");
                return 2;
            }
            oa::game::AvatarState avatar;
            if (!oa::game::placeOnGround(&avatar, spawnX, spawnZ, spawn.y + 10.0f, 30.0f, 40.0f, terrain)) {
                OA_LOG(oa::LogLevel::Error, "app", "avatar could not be placed at ({:.1f}, {:.1f})", spawnX, spawnZ);
                return 2;
            }

            std::uint32_t supported = 0u, airborne = 0u, jumps = 0u, levelChanges = 0u;
            float apex = avatar.y, travelled = 0.0f, previousGroundY = avatar.groundY;
            bool alive = true;
            std::uint32_t tick = 0u;
            for (; tick < ticks && alive; ++tick) {
                oa::game::AvatarInput input;
                input.forward = 1.0f;
                input.run = config.flag("avatarRun", false);
                input.jump = (tick == ticks / 3u && avatar.grounded);   // one take-off per run, at a fixed tick
                jumps += input.jump ? 1u : 0u;
                const float beforeX = avatar.x, beforeZ = avatar.z;
                alive = oa::game::advance(&avatar, input, yaw, kStep, terrain, tuning);
                travelled += std::abs(avatar.x - beforeX) + std::abs(avatar.z - beforeZ);
                supported += avatar.grounded ? 1u : 0u;
                airborne += avatar.grounded ? 0u : 1u;
                apex = std::max(apex, avatar.y);
                if (avatar.grounded && std::abs(avatar.groundY - previousGroundY) > 1.0f) {
                    ++levelChanges;
                    previousGroundY = avatar.groundY;
                }
                if (tick % 60u == 0u || tick + 1u == ticks) {
                    OA_LOG(oa::LogLevel::Info, "app", "avatar tick {}: ({:.1f}, {:.1f}, {:.1f}) {} ground {:.1f} triangle {}",
                           tick, avatar.x, avatar.y, avatar.z, avatar.grounded ? "on" : "in the air off", avatar.groundY,
                           avatar.groundTriangle);
                }
            }
            OA_LOG(oa::LogLevel::Info, "app", "avatar run over '{}': tuning walk {:.0f} run {:.0f} gravity {:.0f} jump "
                                              "{:.0f} step {:.0f}; {} tick(s) ran, {} supported, {} airborne, {} jump(s), "
                                              "apex {:.1f} above spawn ground {:.1f}, path length {:.1f} units, {} level "
                                              "change(s): {}",
                   meshPath, tuning.walkSpeed, tuning.runSpeed, tuning.gravity, tuning.jumpSpeed, tuning.maxStepUp, tick,
                   supported, airborne, jumps, apex - previousGroundY, previousGroundY, travelled, levelChanges,
                   alive ? "still standing on the level" : "left the level through a hole");

            // --avatarTraversal=true asks the question the fixed script cannot answer: can one take-off clear a hole the
            // shipped strips actually leave? It scans the census grid for a supported column with an unsupported span of
            // jumpable width straight ahead, takes the widest such span, then walks the body at run speed and jumps only when
            // the next cell forward holds no floor - the rule a player uses looking at a hole. Deterministic, and it says so
            // out loud if the gated coverage holds nothing jumpable rather than inventing one.
            if (config.flag("avatarTraversal", false)) {
                struct Direction { float x, z; };
                constexpr Direction kDirs[8] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1},
                                                {0.7071068f, 0.7071068f}, {-0.7071068f, 0.7071068f},
                                                {-0.7071068f, -0.7071068f}, {0.7071068f, -0.7071068f}};
                // A flat-ground jump stays airborne for 2 * jumpSpeed / |gravity| seconds, so the gap it clears is that long
                // times the speed it crossed at: run speed, because nobody walks into a hole they can see. Take-off happens a
                // quarter cell before the edge (the lip probe below), so that lead comes off the reach before a candidate counts.
                const float reach = tuning.runSpeed * (2.0f * tuning.jumpSpeed / -tuning.gravity);
                const float lipProbe = 0.25f * cell;
                // TerrainQuery::surfaceAt answers with the highest surface at or below (fromY + above) and has no lower
                // bound, so both ends of a foot probe have to be set by hand. And "within one stride" is not flat enough to
                // cross on: at run speed a shore that drops 18 units per tick (a ~27 degree ramp, still inside a 25-unit
                // tolerance sampled every eighth of a cell) falls away faster than gravity brings the body down, so contact
                // never closes and the jump ends in the void. These probes want real plateaus: three units of sag per eighth
                // of a cell, about a 3 degree surface, which is what shipped floors look like between holes.
                constexpr float kFlat = 3.0f;
                const auto levelFloor = [&](float px, float pz, float refY) {
                    oa::cdc::TerrainHit h{};
                    if (!terrain.surfaceAt(px, pz, refY - kFlat, 2.0f * kFlat, &h)) return false;
                    return h.y >= refY - kFlat;   // the call caps at refY + kFlat already
                };
                struct Candidate {
                    float gap, x, z, ground;
                    float holeStart, holeEnd;   // distances ahead of the start sample where the void begins and ends
                    int dir;
                };
                // The decoded patches are a lattice of isolated triangles, so probing at the census grid spacing can call a
                // line "supported" while nothing connects two samples. This scan walks each candidate line at an eighth of a
                // cell and accepts only holes whose runway and shore are continuous at that resolution: without that rule the
                // body took off from a sliver, sailed over ground it never touched and fell out of the world, which is what
                // every attempt on sixteen units did before this change. Top three, widest first.
                const float probeStep = cell / 8.0f;
                Candidate best[3] = {};
                int bestCount = 0;
                for (std::uint32_t iz = 0u; iz < kSamples; ++iz) {
                    for (std::uint32_t ix = 0u; ix < kSamples; ++ix) {
                        const float sx = terrain.boundsMin()[0] + (terrain.boundsMax()[0] - terrain.boundsMin()[0]) *
                                                                  (static_cast<float>(ix) + 0.5f) / static_cast<float>(kSamples);
                        const float sz = terrain.boundsMin()[2] + (terrain.boundsMax()[2] - terrain.boundsMin()[2]) *
                                                                  (static_cast<float>(iz) + 0.5f) / static_cast<float>(kSamples);
                        oa::cdc::TerrainHit here{};
                        if (!terrain.surfaceAt(sx, sz, terrain.boundsMax()[1], 4.0f * cell, &here)) continue;
                        for (int d = 0; d < 8; ++d) {
                            const float dx = kDirs[d].x, dz = kDirs[d].z;
                            const auto floorAt = [&](float dist) { return levelFloor(sx + dx * dist, sz + dz * dist, here.y); };
                            // A solid run-up: a body that starts on an island has nothing to build speed along.
                            bool runway = true;
                            for (float dist = -cell; dist < 0.0f && runway; dist += probeStep) runway = floorAt(dist);
                            if (!runway || !floorAt(0.0f)) continue;
                            float holeStart = probeStep;
                            while (holeStart <= reach && floorAt(holeStart)) holeStart += probeStep;
                            if (holeStart > reach) continue;   // this line holds no edge within jump range
                            float holeEnd = holeStart;
                            while (holeEnd <= reach && !floorAt(holeEnd)) holeEnd += probeStep;
                            const float gap = holeEnd - holeStart;
                            if (gap < 0.5f * cell || gap + lipProbe > reach) continue;   // a crack is not a jump
                            // The far side has to stay solid for another cell and a half, not just answer the first probe.
                            bool shore = true;
                            for (float dist = holeEnd; dist <= holeEnd + 1.5f * cell && shore; dist += probeStep) {
                                shore = floorAt(dist);
                            }
                            if (!shore) continue;
                            int slot = bestCount < 3 ? bestCount : 2;
                            for (; slot > 0 && best[slot - 1].gap < gap; --slot) {
                                if (slot < 3) best[slot] = best[slot - 1];
                            }
                            if (slot < 3 || best[2].gap < gap) {
                                const int at = slot < 3 ? slot : 2;
                                best[at] = Candidate{gap, sx, sz, here.y, holeStart, holeEnd, d};
                                if (bestCount < 3) ++bestCount;
                            }
                        }
                    }
                }
                if (bestCount == 0) {
                    OA_LOG(oa::LogLevel::Warn, "app", "--avatarTraversal found no hole of half a cell to {:.1f} cells with a "
                                                      "continuous runway and shore in the gated coverage of '{}'",
                           reach / cell, meshPath);
                }
                for (int attempt = 0; attempt < bestCount; ++attempt) {
                    const Candidate& pick = best[attempt];
                    const Direction dir = kDirs[pick.dir];
                    const float yawToFace = std::atan2f(dir.x, dir.z);   // src/game/Avatar.cpp: forward = (sin yaw, cos yaw)
                    oa::game::AvatarState trav;
                    if (!oa::game::placeOnGround(&trav, pick.x, pick.z, pick.ground + 10.0f, 30.0f, 40.0f, terrain)) {
                        OA_LOG(oa::LogLevel::Warn, "app", "--avatarTraversal could not place the body at ({:.1f}, {:.1f})",
                                   pick.x, pick.z);
                        continue;
                    }
                    OA_LOG(oa::LogLevel::Info, "app", "traversal attempt {} in '{}': a {:.0f}-unit hole ({:.2f} cell(s)) facing "
                                                      "{} from ({:.1f}, {:.1f}) ground {:.1f}; a jump at run speed reaches "
                                                      "{:.0f} units less the {:.0f}-unit take-off lead; void runs from {:.0f} "
                                                      "to {:.0f} ahead", attempt + 1, meshPath, pick.gap, pick.gap / cell,
                           pick.dir, pick.x, pick.z, pick.ground, reach, lipProbe, pick.holeStart, pick.holeEnd);
                    std::uint32_t travTicks = 0u, airborneTicks = 0u, jumpsUsed = 0u;
                    bool travAlive = true, crossed = false;
                    const float startX = trav.x, startZ = trav.z, startY = trav.y;
                    constexpr std::uint32_t kBudget = 400u;   // about seven seconds: one jump and a stride to settle
                    for (; travTicks < kBudget && travAlive; ++travTicks) {
                        oa::game::AvatarInput input;
                        input.forward = 1.0f;
                        input.run = true;
                        const bool lip = !levelFloor(trav.x + dir.x * lipProbe, trav.z + dir.z * lipProbe, trav.y);
                        input.jump = trav.grounded && lip;   // take off at the edge, not a cell before it
                        jumpsUsed += input.jump ? 1u : 0u;
                        airborneTicks += trav.grounded ? 0u : 1u;
                        // Stop as soon as the body is standing past the far edge. Further along these decoded fragments
                        // there is more void, and a later fall would say nothing about whether this hole was cleared.
                        if (trav.grounded && std::hypot(trav.x - pick.x, trav.z - pick.z) > pick.holeEnd + 0.5f * cell) {
                            crossed = true;
                            break;
                        }
                        travAlive = oa::game::advance(&trav, input, yawToFace, kStep, terrain, tuning);
                    }
                    const float ran = std::hypot(trav.x - startX, trav.z - startZ);
                    // A crossing has to end on the level it left: a body that drops onto a lower deck went into the hole.
                    const bool sameLevel = std::fabs(trav.y - startY) <= tuning.maxStepUp;
                    // A crossing without a take-off is not evidence of one: the body would simply be walking around a hole
                    // the scan mis-certified because its reference height differs from the placed body's.
                    const bool over = crossed && travAlive && trav.grounded && sameLevel && jumpsUsed >= 1u;
                    OA_LOG(oa::LogLevel::Info, "app", "traversal attempt {} over '{}': {} tick(s), {} jump(s), {} airborne "
                                                      "tick(s), {:.1f} units travelled, ending at ({:.1f}, {:.1f}, {:.1f}) on "
                                                      "ground {:.1f}: {}", attempt + 1, meshPath, travTicks, jumpsUsed,
                           airborneTicks, ran, trav.x, trav.y, trav.z, trav.groundY,
                           over ? "crossed the hole and stands on the far side within one stride of the take-off level"
                                : (trav.grounded && sameLevel ? "stopped short of the hole"
                                                             : "fell into the hole or onto a lower deck"));
                    if (over) {
                        avatar = trav;   // --avatarFollow / --avatarDrive below pick up the crossing body
                        break;
                    }
                }
            }

            // --avatarFollow=true moves the look-at to where the run ended, so --meshDump shows the level from her
            // vantage. It stays off by default because panning away from the mesh centre also moves the byte-exact
            // --selfCheck probe's framing (docs/TESTING.md), and that check must keep passing on the plain run.
            if (config.flag("avatarFollow", false)) {
                followBody = true;
                followAt[0] = avatar.x;
                followAt[1] = avatar.y;
                followAt[2] = avatar.z;
            }

            // --avatarDrive=true keeps the body after the scripted report and lets keys move it. It starts where the
            // scripted run finished, so what you see on screen is the same body the numbers describe.
            if (config.flag("avatarDrive", false)) {
                if (!alive) {
                    OA_LOG(oa::LogLevel::Warn, "app", "--avatarDrive skipped: the scripted run already left the level");
                } else {
                    driveTerrain = std::move(terrain);
                    driven = avatar;
                    driveAvatar = true;
                    OA_LOG(oa::LogLevel::Info, "app",
                           "interactive body at ({:.1f}, {:.1f}, {:.1f}) on ground {:.1f}: W/S walk, A/D sidestep, left "
                           "shift run, space jump; arrows orbit the camera and Q/E dolly. The tuning stays ours "
                           "(src/game/Avatar.hpp) and these heights still carry GEOMETRY.md §16's caveat.", driven.x,
                           driven.y, driven.z, driven.groundY);
                }
            }
        }

        oa::render::MeshUpload mesh;
        // The buffer is the payload itself: the vertex layout is identical, so there is nothing to repack.
        mesh.bytes = payload.data();
        mesh.vertexCount = static_cast<std::uint32_t>(vertices.size());
        // Frame the camera on what is actually drawn. With strips gated, most of the shipped vertex array carries no
        // triangle at all, and fitting to the whole array leaves the drawn patch a few pixels wide.
        float frameMin[3], frameMax[3];
        for (int axis = 0; axis < 3; ++axis) {
            frameMin[axis] = bounds.min[axis];
            frameMax[axis] = bounds.max[axis];
        }
        if (!drawIndices.empty()) {
            // Fit the camera to where the drawn vertices actually cluster, not to their extremes: a handful of records
            // reach across tens of thousands of units (the coarse ones §16-§17 measured), and fitting to those leaves the
            // bulk of the geometry a few pixels wide. p2-p98 per axis is our own framing choice, stated here so the
            // picture can be compared run to run.
            std::vector<float> coordinate[3];
            for (const auto& index : drawIndices) {
                const auto& vertex = vertices[index];
                coordinate[0].push_back(static_cast<float>(vertex.x));
                coordinate[1].push_back(static_cast<float>(vertex.y));
                coordinate[2].push_back(static_cast<float>(vertex.z));
            }
            for (int axis = 0; axis < 3; ++axis) {
                std::sort(coordinate[axis].begin(), coordinate[axis].end());
                const auto pick = [&](float fraction) {
                    return coordinate[axis][static_cast<std::size_t>(fraction * static_cast<float>(coordinate[axis].size() - 1u))];
                };
                frameMin[axis] = pick(0.02f);
                frameMax[axis] = pick(0.98f);
            }
        }
        for (int axis = 0; axis < 3; ++axis) {
            mesh.boundsMin[axis] = frameMin[axis];
            mesh.boundsMax[axis] = frameMax[axis];
        }
        // The look-at can only be placed relative to the framing centre, so a follow request waits for this point.
        viewCentre[0] = (frameMin[0] + frameMax[0]) * 0.5f;
        viewCentre[1] = (frameMin[1] + frameMax[1]) * 0.5f;
        viewCentre[2] = (frameMin[2] + frameMax[2]) * 0.5f;
        if (followBody) {
            panTarget[0] = followAt[0] - viewCentre[0];
            panTarget[1] = followAt[1] - viewCentre[1];
            panTarget[2] = followAt[2] - viewCentre[2];
            camera.panX = panTarget[0];
            camera.panY = panTarget[1];
            camera.panZ = panTarget[2];
        }
        // The gated copy is what goes to the GPU and into the terrain grid --avatar=true probes (see above).
        mesh.indices = drawIndices.empty() ? nullptr : &drawIndices;
        if (!renderer.uploadMesh(mesh, &error)) {
            OA_LOG(oa::LogLevel::Error, "app", "cannot upload the triangle run: {}", error);
            return 2;
        }
        if (const std::string yaw = config.str("yaw"); !yaw.empty()) camera.yaw = std::strtof(yaw.c_str(), nullptr);
        if (const std::string pitch = config.str("pitch"); !pitch.empty()) {
            camera.pitch = std::strtof(pitch.c_str(), nullptr);
        }
        if (const std::string zoom = config.str("zoom"); !zoom.empty()) camera.zoom = std::strtof(zoom.c_str(), nullptr);
        // --fov=<degrees> switches the mesh pass to a perspective frustum; 0 keeps the orthographic fit that the
        // offscreen probe and the dumps depend on.
        if (const std::string fov = config.str("fov"); !fov.empty()) camera.fovDegrees = std::strtof(fov.c_str(), nullptr);
        camStep = 0.02f * std::max({frameMax[0] - frameMin[0], frameMax[1] - frameMin[1], frameMax[2] - frameMin[2]});
        renderer.setMeshCamera(camera);
        OA_LOG(oa::LogLevel::Info, "app", "camera keys: arrows orbit, WASD slide the look-at target, Q/E dolly; "
                                         "{} projection at yaw {:.2f} pitch {:.2f} zoom {:.2f}",
               camera.fovDegrees > 0.0f ? "perspective" : "orthographic", camera.yaw, camera.pitch, camera.zoom);
        OA_LOG(oa::LogLevel::Info, "app", "drawing section {} of '{}' (id {}, type {}): {} vertices, {} triangles, "
                                         "bounds {:.0f}x{:.0f}x{:.0f} world units",
               chosen, meshPath, geometry.id, static_cast<int>(geometry.type), vertices.size(), bounds.triangles,
               frameMax[0] - frameMin[0], frameMax[1] - frameMin[1], frameMax[2] - frameMin[2]);

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
        if (renderer.hasMesh()) {
            // Keys move the mesh camera every frame. The mapping itself lives in render::applyMeshKeys so it is
            // covered without a window (tests/test_meshcamera.cpp); arrows orbit, Q/E dolly, and WASD slides the look-at
            // target - unless --avatarDrive=true handed the body to the keyboard, in which case WASD moves her instead.
            oa::render::MeshKeys keys;
            keys.left = window.keyDown(oa::platform::Key::Left);
            keys.right = window.keyDown(oa::platform::Key::Right);
            keys.up = window.keyDown(oa::platform::Key::Up);
            keys.down = window.keyDown(oa::platform::Key::Down);
            keys.dollyIn = window.keyDown(oa::platform::Key::E);
            keys.dollyOut = window.keyDown(oa::platform::Key::Q);
            if (driveAvatar) {
                // Fixed 1/60 s ticks taken from a wall-clock accumulator, so real seconds match the tuning while every
                // tick stays identical; more than a quarter second of gap is dropped instead of catching up on a frame
                // the window spent hidden. Forward is where the camera looks flattened onto XZ: applyMeshKeys uses
                // right = (cos yaw, sin yaw) and away = (-sin yaw, cos yaw) while src/game/Avatar.cpp builds forward =
                // (sin facing, cos facing), so facing = -camera.yaw makes the two bases agree.
                const auto now = std::chrono::steady_clock::now();
                driveAccum += std::min(0.25, std::chrono::duration<double>(now - driveLast).count());
                driveLast = now;
                constexpr double kDriveStep = 1.0 / 60.0;
                const auto held = [&](oa::platform::Key key) { return window.keyDown(key) ? 1.0f : 0.0f; };
                oa::game::AvatarInput input;
                input.forward = held(oa::platform::Key::W) - held(oa::platform::Key::S);
                input.right = held(oa::platform::Key::D) - held(oa::platform::Key::A);
                input.run = window.keyDown(oa::platform::Key::Run);
                input.jump = window.keyDown(oa::platform::Key::Jump);  // held space hops again on each contact tick
                while (driveAccum >= kDriveStep) {
                    driveAccum -= kDriveStep;
                    if (drivenAlive && !oa::game::advance(&driven, input, -camera.yaw, static_cast<float>(kDriveStep),
                                                          driveTerrain, driveTuning)) {
                        drivenAlive = false;
                        OA_LOG(oa::LogLevel::Warn, "app", "the driven body left the level through a hole at ({:.1f}, "
                                                          "{:.1f}, {:.1f}); keys stop moving it - re-run for a fresh spawn",
                               driven.x, driven.y, driven.z);
                    }
                    if (++driveTicks % 120u == 0u) {
                        OA_LOG(oa::LogLevel::Info, "app", "driven body at tick {}: ({:.1f}, {:.1f}, {:.1f}) ground "
                                                          "{:.1f} {}", driveTicks, driven.x, driven.y, driven.z,
                               driven.groundY, driven.grounded ? "supported" : "airborne");
                    }
                }
                // Look slightly above the body so she is not framed at knee height, relative to the mesh's centre.
                panTarget[0] = driven.x - viewCentre[0];
                panTarget[1] = driven.y + 60.0f - viewCentre[1];
                panTarget[2] = driven.z - viewCentre[2];
            } else {
                keys.forward = window.keyDown(oa::platform::Key::W);
                keys.back = window.keyDown(oa::platform::Key::S);
                keys.strafeRight = window.keyDown(oa::platform::Key::D);
                keys.strafeLeft = window.keyDown(oa::platform::Key::A);
            }
            oa::render::applyMeshKeys(camera, panTarget, keys, camStep);
            renderer.setMeshCamera(camera);
        }
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
