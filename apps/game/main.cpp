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
#include "cdc/Sound.hpp"
#include "platform/Audio.hpp"
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

// The one place that maps a PCD9 tag to the Vulkan format carrying its shipped bytes: shared by the
// --texture quad path and the terrain material pages of --textures (docs/formats/TEXTURES.md).
static VkFormat pcd9VkFormat(const oa::cdc::Pcd9Info& info) {
    switch (info.tag) {
        case static_cast<std::uint32_t>(oa::cdc::Pcd9Format::Dxt1): return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case static_cast<std::uint32_t>(oa::cdc::Pcd9Format::Dxt5): return VK_FORMAT_BC3_SRGB_BLOCK;
        case static_cast<std::uint32_t>(oa::cdc::Pcd9Format::Uncompressed32): return VK_FORMAT_B8G8R8A8_SRGB;
        default: return VK_FORMAT_UNDEFINED;
    }
}

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

    // --gamepad=true adopts the first connected gamepad (its identity is logged by the platform module);
    // the named keys then answer from keyboard OR pad. Without the flag nothing polls a pad, so the
    // keyboard baseline is untouched.
    if (config.flag("gamepad", false)) {
        std::string gamepadError;
        if (!window.enableGamepad(&gamepadError))
            OA_LOG(oa::LogLevel::Warn, "app", "no gamepad: {}", gamepadError);
    }

    oa::render::VkRenderer renderer;
    oa::render::RendererConfig renderConfig;
    renderConfig.applicationName = windowConfig.title;
    renderConfig.validation = config.flag("vk.validation", true);
    // --present=1 asks for the no-wait swap (uncapped presentation) instead of the default wait-for-
    // vertical-blend one; it is a settings knob, not a gameplay one — the physics ticks are wall-clock
    // paced and stay put while the picture rate changes, which is what the two runs below measure.
    renderConfig.presentMode = config.integer("present", 0);
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
        const VkFormat format = pcd9VkFormat(info);
        if (format == VK_FORMAT_UNDEFINED) {
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

    // --level=<code> selects one of the game's own level codes from the chapter table inside
    // pc-w\generalbank.drm (docs/formats/CONTAINERS.md §6.1): the code is the unit's name,
    // pc-w\<code>.drm, so an alias for --mesh through the game's own naming. Start positions are not read
    // from that table; where the body spawns and how the camera frames stay this file's behaviour.
    std::string meshPath = config.str("mesh");
    if (meshPath.empty() && !config.str("level").empty()) {
        meshPath = "pc-w\\" + config.str("level") + ".drm";
    }
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
        // --textures=true binds each walked chain's material to the texture page its +12 word names
        // (docs/formats/GEOMETRY.md §23, Milestone 4's experiment a): material indices ride the gated
        // triangles as drawMaterials, and after the walk they are grouped per material against the base
        // table's class/value words — one contiguous index run per material, one page per run.
        const bool wantTextures = config.flag("textures", false);
        std::vector<std::int32_t> drawMaterials;   // one matIdx per triangle in drawIndices
        std::vector<std::size_t> drawTable;   // which base table each drawn triangle's tree used (§27)
        std::vector<std::uint32_t> groupedIndices; // drawIndices reordered into per-material runs
        std::vector<oa::render::MeshBatch> materialBatches;
        std::vector<oa::render::TextureUpload> materialPages;
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
            // §20/§21 + §26/§27 measurement (openanniversary-work relotree and sweep-trees): a strip record's indices are
            // relative to its material's block of the unit's stride-20 vertex array, and each oct-tree owns one of the unit's
            // same-shaped base tables - the payloads' own kind-0 relocations name it (8baf540e: 77->141 x160, 86->160 x76,
            // 131->206, 140->153, 165->191, 190->188), and those picks tile the array: coverage of referenced vertices rises
            // 0.305 -> 0.983 there while §21's unit-wide census (every tree on table 141) leaves ranges overlapping. Trees
            // whose tables the terrain GROUP records name instead (§29 [V-exe]: a stride-0xB0 group array whose kind-0 sites at +0x44 name an oct-tree section and +0x90 its material table; §26's hybrid stays as the fallback rule for units with no group array). Measured on `8baf540e` under `groupRecord`: the nine trees' index ranges tile 0..149,076 with zero bad heads (§30), so tables the §20 detector never accepted - sections 218/209/215 - decode from their own record slots here; texture binding still only resolves class-32 pages through the §20-detected lists, so those three trees keep white-bound materials until question (b)'s second namespace is pinned. decodeTerrainUnit walks trees, chains and bases once so this caller cannot drift a second copy of the walk; capacity/coverage failures land in TerrainStripChain's counters instead of drawing junk (§19's edge gate keeps coarse triangles off the grid too).
            std::vector<oa::cdc::TerrainTreeWalk> walks;
            std::string walkError;   // names the refusing rule when the unit has no oct-tree at all (§15)
            if (!oa::cdc::decodeTerrainUnit(drm, 0u, oa::cdc::TerrainTableRule::groupRecord, &walks, &walkError)) {
                OA_LOG(oa::LogLevel::Warn, "app", "'{}' strips skipped: {}", meshPath, walkError);
            }
            // §23's texture binding needs each chosen table's class/value words; fetch the same §20 detector the selection
            // used so a material index is looked up in the table that actually indexed its block.
            std::map<std::size_t, oa::cdc::TerrainMaterialList> tableCache;
            const auto tableFor = [&](std::size_t section) -> oa::cdc::TerrainMaterialList* {
                if (section >= drm.sections().size()) return nullptr;
                const auto known = tableCache.find(section);
                if (known != tableCache.end()) return &known->second;
                oa::cdc::TerrainMaterialList list;
                std::string why;
                if (!oa::cdc::decodeTerrainMaterialBases(drm.payload(drm.sections()[section]), drm.relocations(drm.sections()[section]),
                                                        static_cast<std::uint32_t>(section), &list, &why)) return nullptr;
                return &tableCache.emplace(section, std::move(list)).first->second;
            };
            bool texturedNeedsTable = false;   // a tree whose matIdx no candidate covers (§13's absolute reading)
            for (const auto& walk : walks) {
                ++trees;
                if (walk.tableSection == static_cast<std::size_t>(-1)) texturedNeedsTable = true;
                for (const auto& entry : walk.byPayload) {
                    const oa::cdc::TerrainStripChain& walked = entry.second;   // bases already applied by the library (§20)
                    records += walked.records;
                    heads += walked.heads;
                    badHeads += walked.badHeads;
                    brokenLinks += walked.brokenLinks;
                    // Gate what goes to the screen, which is also what a foot probes through the terrain grid (§19):
                    // non-degenerate (shortest edge over 1 unit) and longest edge under 1,000 units. Coarse triangles
                    // stay counted (`coarseTriangles`) rather than guessed at - with §27's per-tree tables most of what
                    // used to be gated is now tight real geometry instead of wrong-block noise.
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
                        drawMaterials.push_back(t / 3u < walked.materialOfTriangle.size()   // §10: the matIdx rides each chain record
                                                ? walked.materialOfTriangle[t / 3u] : -1);
                        drawTable.push_back(walk.tableSection);
                    }
                }
            }
            OA_LOG(oa::LogLevel::Info, "app", "{} terrain tree(s): {} leaf reference(s) reached {} record(s), drawing "
                                              "{} of {} triangle(s) over {} vertex/vertices ({} gated by the edge rule, "
                                              "{} bad head(s), {} broken link(s))",
                   trees, heads, records, drawIndices.size() / 3u, stripIndices.size() / 3u,
                   vertices.size(), coarseTriangles, badHeads, brokenLinks);
            if (wantTextures) {
                // §27's tables differ per tree, so a run is keyed by (table section, matIdx): the same material index
                // means different pages under different tables, and merging them would bind the wrong texture. A tree
                // whose chains name no covering table (§13's absolute reading) keeps vertex colours alone - nothing guessed.
                std::map<std::pair<std::size_t, std::int32_t>, std::vector<std::uint32_t>> runs;
                for (std::size_t tri = 0; tri < drawMaterials.size(); ++tri) {
                    const auto material = drawMaterials[tri];
                    if (material < 0) continue;   // §10: a chain record naming no material stays untextured (§23's kNoTexture run)
                    for (int k = 0; k < 3; ++k) {
                        runs[{drawTable[tri], material}].push_back(drawIndices[3u * tri + static_cast<std::size_t>(k)]);
                    }
                }
                std::map<std::uint32_t, std::uint32_t> pageById;   // section index -> index into materialPages, deduped
                // §21 measured the id space: class 32's value matches the *id field* of this unit's type-5 (texture
                // page) sections — a different namespace from the directory index, so map it before use.
                std::map<std::uint32_t, std::size_t> pageSlot;
                for (std::size_t s = 0; s < drm.sections().size(); ++s) {
                    const oa::cdc::DrmSection& sec = drm.sections()[s];
                    if (sec.stored && sec.type == 5) pageSlot[sec.id] = s;
                }
                for (const auto& entry : runs) {
                    oa::render::MeshBatch batch;   // defaults: no page bound (white, colours only)
                    batch.indexStart = static_cast<std::uint32_t>(groupedIndices.size());
                    batch.indexCount = static_cast<std::uint32_t>(entry.second.size());
                    const oa::cdc::TerrainMaterialList* treeTable = tableFor(entry.first.first);   // §27: the table that indexed this run
                    const std::int32_t materialId = entry.first.second;
                    const bool namedByTable = treeTable != nullptr && materialId >= 0 &&
                                             static_cast<std::size_t>(materialId) < treeTable->classValue.size();
                    const std::uint32_t packed = namedByTable ? treeTable->classValue[static_cast<std::size_t>(materialId)] : 0u;
                    const uint32_t idClass = packed >> 16u, pageId = packed & 0xFFFFu;
                    if (idClass == 32u) {   // §21: class 32 names the texture page; other classes stay open
                        const auto named = pageSlot.find(pageId);
                        if (named == pageSlot.end()) {
                            OA_LOG(oa::LogLevel::Warn, "app", "--textures: material {} names page id {}, which matches no stored type-5 section of this unit", materialId, pageId);
                        } else {
                            auto known = pageById.find(static_cast<std::uint32_t>(named->second));
                            if (known == pageById.end()) {
                                oa::cdc::Pcd9Info page;
                                std::string why;
                                const std::span<const std::byte> pageBytes = drm.payload(drm.sections()[named->second]);
                                if (oa::cdc::parsePcd9(pageBytes, &page, &why)) {
                                    oa::render::TextureUpload upload;
                                    upload.format = pcd9VkFormat(page);
                                    if (upload.format == VK_FORMAT_UNDEFINED) {
                                        OA_LOG(oa::LogLevel::Warn, "app", "--textures: material {}'s page (section {}) has PCD9 tag {}, which has no Vulkan format", materialId, named->second, page.tag);
                                    } else {
                                        upload.width = page.width;
                                        upload.height = page.height;
                                        upload.mipLevels = page.mipLevels;
                                        upload.bytes = pageBytes.data() + oa::cdc::kPcd9HeaderBytes;
                                        upload.byteSize = page.payloadBytes;
                                        for (const oa::cdc::Pcd9LevelRegion& region : oa::cdc::pcd9LevelRegions(page)) {
                                            upload.levels.push_back(oa::render::TextureUpload::Level{region.offset, region.bytes});
                                        }
                                        pageById[static_cast<std::uint32_t>(named->second)] = static_cast<std::uint32_t>(materialPages.size());
                                        materialPages.push_back(std::move(upload));
                                        known = pageById.find(static_cast<std::uint32_t>(named->second));
                                    }
                                } else {
                                    OA_LOG(oa::LogLevel::Warn, "app", "--textures: material {}'s id {} resolves to section {}, which is not a decodable PCD9: {}", materialId, pageId, named->second, why);
                                }
                            }
                            if (known != pageById.end()) batch.material = known->second;
                        }
                    }
                    groupedIndices.insert(groupedIndices.end(), entry.second.begin(), entry.second.end());
                    materialBatches.push_back(batch);
                }
                OA_LOG(oa::LogLevel::Info, "app", "--textures: {} material(s) grouped into {} batch(es) over {} distinct PCD9 page(s) from named sections",
                       runs.size(), materialBatches.size(), materialPages.size());
            }
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
                // --gapEnvelope=true turns the same scan into a measurement instead of an attempt: every edge a walked
                // route meets (continuous runway + shore) is recorded with its TRUE void width - probed out to 12 cells,
                // not just to jump reach - so the level's own designed-gap envelope can be compared against what the
                // tuned model clears. Designers place a jump where the original Lara could make it, so every gap under
                // their route's edge bounds the original capability from below; voids she must go around bound related
                // moves from above. docs/ROADMAP.md Milestone 5 calls this the behavior instrument now.
                const bool envelope = config.flag("gapEnvelope", false);
                std::vector<float> gaps;
                if (envelope) gaps.reserve(4096u);
                const float probeStep = cell / 8.0f;
                const float holeCap = 12.0f * cell;   // true extent within 12 cells; beyond reads as an ocean
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
                            while (holeEnd <= holeCap && !floorAt(holeEnd)) holeEnd += probeStep;
                            const float gap = holeEnd - holeStart;
                            if (envelope && gap >= 0.5f * cell && gaps.size() < 4096u) gaps.push_back(gap);
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
                if (envelope) {
                    std::sort(gaps.begin(), gaps.end());
                    if (!gaps.empty()) {
                        const std::size_t closed = static_cast<std::size_t>(
                            std::lower_bound(gaps.begin(), gaps.end(), holeCap - 0.001f) - gaps.begin());
                        std::uint64_t clearable = 0u;
                        for (std::size_t i = 0; i < closed; ++i) if (gaps[i] + lipProbe <= reach) ++clearable;
                        OA_LOG(oa::LogLevel::Info, "app", "gap envelope of '{}': {} edges (runway+shore, >=half-cell) over the "
                                                          "gated coverage; {} voids close within the {:.0f}-unit probe cap - those "
                                                          "widths p50 {:.0f} p90 {:.0f} max {:.0f} units - {} of {} closed edges "
                                                          "are clearable in one flat-ground jump at run speed (reach {:.0f} less "
                                                          "take-off lead {:.0f}); {} uncapped edges are open water to a canyon "
                                                          "across, not designed jumps",
                                   meshPath, gaps.size(), closed, holeCap,
                                   closed ? gaps[closed / 2u] : 0.0f, closed ? gaps[closed * 9u / 10u] : 0.0f,
                                   closed ? gaps[closed - 1u] : 0.0f, clearable, closed, reach - lipProbe, lipProbe,
                                   gaps.size() - closed);
                    } else {
                        OA_LOG(oa::LogLevel::Warn, "app", "gap envelope of '{}': no edges recorded in the gated coverage", meshPath);
                    }
                }
                const int attemptCount = envelope ? 0 : bestCount;
                for (int attempt = 0; attempt < attemptCount; ++attempt) {
                    const Candidate& pick = best[attempt];
                    const Direction dir = kDirs[pick.dir];
                    // libstdc++ on ubuntu-24.04 (GCC 13.3) does not export std::atan2f; std::atan2 on two floats
                    // returns double, so cast once. Math is unchanged; the same file builds on GCC 16/Clang 23 hosts.
                    const float yawToFace = static_cast<float>(std::atan2(dir.x, dir.z));   // Avatar.cpp: forward = (sin yaw, cos yaw)
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

            // --avatarWalk=<seconds> hands the body a walker instead of the straight-line probe: it keeps its
            // current heading while the next probe point holds a foot on ground, turns at the first dead end
            // to any direction that holds, and jumps only when even turning finds nothing. It is the same
            // physics and terrain queries the keyboard drives, scripted so a headless run can report the
            // walk — "the level is playable start to finish" is measured here as a continuous supported
            // walk over the level's own decoded surface; keys stay the human instrument (docs/TESTING.md).
            if (const int walkSeconds = config.integer("avatarWalk", 0); walkSeconds > 0) {
                const float kProbe = 2.0f * cell;
                const float kWalkStep = 1.0f / 60.0f;   // same physics tick as the scripted run above
                const float kFlat = 4.0f;               // same foot tolerance the gap envelope used
                const auto holdsUnder = [&](const oa::game::AvatarState& b, float hx, float hz) {
                    oa::cdc::TerrainHit hit{};
                    if (!terrain.surfaceAt(b.x + hx * kProbe, b.z + hz * kProbe, b.y - kFlat, 2.0f * kFlat, &hit))
                        return false;
                    return hit.y >= b.y - kFlat;   // same reference height the gap scan's levelFloor used
                };
                // Start where the census placed a standing body when the scripted straight run already fell —
                // the walk measures the level, not the probe's bad start.
                oa::game::AvatarState walk{};
                if (alive) {
                    walk = avatar;
                } else if (!oa::game::placeOnGround(&walk, spawnX, spawnZ, spawn.y + 10.0f, 30.0f, 40.0f, terrain)) {
                    OA_LOG(oa::LogLevel::Error, "app", "--avatarWalk could not place the body at the census spawn");
                    return 2;
                }
                float heading = 0.0f;
                unsigned turns = 0u, jumps = 0u, ticks = 0u;
                bool walkAlive = true;
                const float walkStartX = walk.x, walkStartZ = walk.z;
                for (const unsigned walkTicks = static_cast<unsigned>(walkSeconds) * 60u; ticks < walkTicks && walkAlive; ++ticks) {
                    const auto supports = [&](float h) {
                        return holdsUnder(walk, std::cos(h), std::sin(h));
                    };
                    oa::game::AvatarInput input;
                    input.forward = 1.0f;
                    if (!supports(heading)) {
                        int turn = -1;
                        for (const float off : {1.5707963f, -1.5707963f, 0.7853982f, -0.7853982f, 3.1415927f}) {
                            if (supports(heading + off)) { heading += off; ++turns; turn = 0; break; }
                        }
                        if (turn < 0) {
                            input.jump = walk.grounded;   // no direction holds: commit to the jump the lip deserves
                            ++jumps;
                        }
                    }
                    walkAlive = oa::game::advance(&walk, input, heading, kWalkStep, terrain, tuning);
                }
                const float walked = std::hypot(walk.x - walkStartX, walk.z - walkStartZ);
                OA_LOG(oa::LogLevel::Info, "app",
                       "avatar walk over '{}': {} tick(s) ran for {} second(s), {:.1f} units walked, {} turn(s), "
                       "{} jump(s); the body {} at ({:.1f}, {:.1f}, {:.1f}) ground {:.1f}; {}",
                       meshPath, ticks, walkSeconds, walked, turns, jumps,
                       walkAlive ? "stood" : "left the level", walk.x, walk.y, walk.z, walk.groundY,
                       walkAlive && walked > 500.0f
                           ? "the surface held the body continuously across the level"
                           : (walked > 500.0f ? "the walk stopped on the surface early" : "short of a real walk"));
            }

            // --avatarDrive=true keeps the body after the scripted report and lets keys move it. It starts where the
            // scripted run finished, so what you see on screen is the same body the numbers describe.
            if (config.flag("avatarDrive", false)) {
                if (!alive) {
                    // The scripted straight-line run fell, but the census found a standing spot — start there
                    // instead of refusing, so a keyboard session still has a body on the level.
                    if (!oa::game::placeOnGround(&avatar, spawnX, spawnZ, spawn.y + 10.0f, 30.0f, 40.0f, terrain)) {
                        OA_LOG(oa::LogLevel::Warn, "app", "--avatarDrive skipped: the scripted run already left the level");
                    } else {
                        alive = true;
                        OA_LOG(oa::LogLevel::Info, "app", "--avatarDrive starts at the census spawn instead: the straight run fell");
                    }
                }
                if (alive) {
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
        if (!groupedIndices.empty()) {   // --textures: the per-material runs replace the single index draw
            mesh.indices = &groupedIndices;
            mesh.materials = std::move(materialPages);
            mesh.batches = std::move(materialBatches);
        }
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
            // With page bound the lit pixels are page-modulated shipped colours, which no offline byte set can
            // pre-enumerate: the textured probe stays coverage-only, and the exact colour assertion below stays
            // exactly the old check when nothing is bound.
            const bool textured = !groupedIndices.empty();
            std::set<std::uint32_t> shipped;
            if (!textured) {
                for (const oa::cdc::MeshVertex& v : vertices) {
                    shipped.insert((static_cast<std::uint32_t>(v.red) << 16) | (static_cast<std::uint32_t>(v.green) << 8) |
                                   v.blue);
                }
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
                    bool known = textured;   // modulated pixels are not in the shipped colour set: coverage only
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
    // --sound=true runs the engine's own decoder over the module's fixture blocks and hands the
    // decoded tone to the platform's audio device; --wavOut=<path> additionally writes the same
    // PCM as a WAV so it can be compared byte-for-byte outside the process. The record framing
    // stays open (docs/formats/AUDIO.md §4-§6), so this proves the pipeline and the decoder —
    // not record playback; the fixture's 44100 rate is our own choice, not the engine's.
    if (config.flag("sound", false)) {
        constexpr unsigned fixtureRepeats = 200u;   // 0.29 s of the fixture at 44100 Hz: audible, and short
        std::vector<std::uint8_t> fixture(fixtureRepeats * 36u);
        for (unsigned i = 0u; i < fixtureRepeats; ++i) {
            const auto block = oa::cdc::makeAdpcmBlock(1000, 5, 0x31, 0x9F);
            std::copy(block.begin(), block.end(), fixture.begin() + fixtureRepeats * 0u + i * 36u);
        }
        std::vector<std::int16_t> pcm(fixtureRepeats * 64u);
        oa::cdc::decodeAdpcmBlocks({reinterpret_cast<const std::byte*>(fixture.data()), fixture.size()}, pcm.data());
        const std::int16_t worked[4] = {1000, 1010, 991, 984};   // tests/test_sound.cpp asserts the same
        if (!std::equal(worked, worked + 4, pcm.begin())) {
            OA_LOG(oa::LogLevel::Error, "app", "sound self-check: decoded fixture differs from the hand values");
            return 2;
        }
        if (!config.str("wavOut").empty()) {
            if (FILE* f = std::fopen(config.str("wavOut").c_str(), "wb")) {
                const auto put = [&](unsigned v) {   // little-endian header, like the samples it wraps
                    const unsigned char bytes[4] = {
                        static_cast<unsigned char>(v & 0xFFu), static_cast<unsigned char>((v >> 8) & 0xFFu),
                        static_cast<unsigned char>((v >> 16) & 0xFFu), static_cast<unsigned char>(v >> 24)};
                    std::fwrite(bytes, 1u, 4u, f);
                };
                const unsigned pcmBytes = static_cast<unsigned>(pcm.size() * 2u);
                std::fwrite("RIFF", 1u, 4u, f);
                put(8u + pcmBytes);
                std::fwrite("WAVEfmt ", 1u, 8u, f);
                put(16u);
                put(1u);
                put(1u);
                put(44100u);
                put(88200u);
                put(2u);
                put(16u);
                std::fwrite("data", 1u, 4u, f);
                put(pcmBytes);
                std::fwrite(pcm.data(), 1u, pcmBytes, f);
                std::fclose(f);
                OA_LOG(oa::LogLevel::Info, "app", "wrote {} PCM bytes as WAV to '{}'", pcmBytes, config.str("wavOut"));
            } else {
                OA_LOG(oa::LogLevel::Error, "app", "--wavOut path '{}' unwritable", config.str("wavOut"));
                return 2;
            }
        }
        oa::platform::Audio audio;
        std::string audioError;
        if (!audio.create(44100, &audioError)) {
            OA_LOG(oa::LogLevel::Warn, "app", "no audio device: {}", audioError);
        } else {
            audio.push(pcm.data(), pcm.size());
            OA_LOG(oa::LogLevel::Info, "app", "sound fixture: {} samples handed to the device", pcm.size());
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
