#include "cdc/Geometry.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace oa::cdc {

bool decodeMesh(std::span<const std::byte> payload, std::vector<MeshVertex>* out, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (out == nullptr) return fail("decodeMesh needs an output vector");
    if (payload.size() < kMeshVertexBytes * 3u) {
        return fail("geometry payload is " + std::to_string(payload.size()) + " bytes, too short for a triangle ("
                    "needs at least " + std::to_string(kMeshVertexBytes * 3u) + ")");
    }
    if (payload.size() % kMeshVertexBytes != 0u) {
        return fail("geometry payload is " + std::to_string(payload.size()) + " bytes, not a whole number of "
                    + std::to_string(kMeshVertexBytes) + "-byte vertices");
    }

    // Records are read at arbitrary byte offsets inside the archive, so they are copied out one at a time rather
    // than reinterpreted in place: an odd payload offset would make every field access misaligned.
    const std::size_t count = payload.size() / kMeshVertexBytes;
    std::vector<MeshVertex> vertices(count);
    for (std::size_t index = 0; index < count; ++index) {
        std::memcpy(vertices.data() + index, payload.data() + index * kMeshVertexBytes, kMeshVertexBytes);
    }
    out->swap(vertices);
    if (error != nullptr) error->clear();
    return true;
}

MeshBounds meshBounds(const std::vector<MeshVertex>& vertices) {
    MeshBounds bounds{};
    for (int axis = 0; axis < 3; ++axis) {
        bounds.min[axis] = std::numeric_limits<float>::max();
        bounds.max[axis] = std::numeric_limits<float>::lowest();
    }
    for (const MeshVertex& vertex : vertices) {
        const float position[3] = {static_cast<float>(vertex.x), static_cast<float>(vertex.y),
                                   static_cast<float>(vertex.z)};
        for (int axis = 0; axis < 3; ++axis) {
            bounds.min[axis] = std::min(bounds.min[axis], position[axis]);
            bounds.max[axis] = std::max(bounds.max[axis], position[axis]);
        }
    }
    if (vertices.empty()) {
        for (int axis = 0; axis < 3; ++axis) {
            bounds.min[axis] = 0.0f;
            bounds.max[axis] = 0.0f;
        }
    }
    bounds.triangles = static_cast<std::uint32_t>(vertices.size() / 3u);
    return bounds;
}

namespace {

// Payloads sit at arbitrary byte offsets inside an archive record, so every field is copied out rather than
// reinterpreted in place.
std::uint32_t loadU32(const std::byte* at) {
    std::uint32_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

// Index arrays are read the same way: copied out, because payloads sit at arbitrary byte offsets.
std::uint16_t loadU16(const std::byte* at) {
    std::uint16_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}
}  // namespace



bool decodeTerrainStripChains(std::span<const std::byte> payload, std::span<const std::uint32_t> heads,
                               std::size_t vertexCount, TerrainStripChain* out, std::string* error,
                               std::span<const std::uint32_t> materialBase) {
    constexpr std::size_t kHeaderBytes = 44;   // GEOMETRY.md §10, prior art tr7/game/terrain.h:46
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (out == nullptr) return fail("decodeTerrainStripChains needs an output struct");
    out->indices.clear();
    out->materialOfTriangle.clear();
    out->heads = out->records = out->badHeads = out->brokenLinks = out->unknownMaterials = 0u;

    // A header is read wherever a link points, and the chain continues while its vertexCount is non-zero - prior art's own
    // walk, `for (strip = tsi; strip->vertexCount > 0; strip = strip->nextTexture)` (`tr7/game/pc/D3D/d3dterrain.cpp:117`).
    // A zero count is therefore the chain end marker, not a broken record: in unit `8baf540e` chains are laid out back to
    // back and each one's last link names such a header (section 79 at offset 0x360, whose following record starts four
    // bytes later - reading that marker as junk is what made raw link following look like it broke everywhere) [V-data].
    // What makes a *drawn* record real is structure: vertexCount a multiple of three within range, its index run inside
    // this payload, and every index naming a vertex of the array (§15 measured all 4,768 chain-walked records against
    // those tests).
    const auto readHeader = [&](std::size_t at, std::uint32_t* count, std::int32_t* matIdx, std::uint32_t* next) {
        if (at + kHeaderBytes > payload.size()) return false;
        *count = loadU32(payload.data() + at);
        *matIdx = static_cast<std::int32_t>(loadU32(payload.data() + at + 20u));
        *next = loadU32(payload.data() + at + 40u);
        return true;
    };
    const auto validRecord = [&](std::size_t at, std::uint32_t count, std::uint32_t base) {
        if (count < 3u || count > 8192u || count % 3u != 0u) return false;
        if (at + kHeaderBytes + 2u * static_cast<std::size_t>(count) > payload.size()) return false;
        const std::byte* run = payload.data() + at + kHeaderBytes;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (static_cast<std::size_t>(base) + loadU16(run + 2u * i) >= vertexCount) return false;
        }
        return true;
    };

    std::vector<std::size_t> walked;   // record offsets already emitted, so a shared tail is drawn once
    for (std::uint32_t head : heads) {
        std::size_t at = head;
        bool producedAny = false;
        while (true) {
            std::uint32_t count = 0u;
            std::uint32_t next = 0u;
            std::int32_t matIdx = 0;
            if (!readHeader(at, &count, &matIdx, &next)) {
                if (producedAny) out->brokenLinks += 1u; else out->badHeads += 1u;
                break;
            }
            if (count == 0u) break;   // the end-of-chain marker prior art tests for
            std::uint32_t base = 0u;
            if (!materialBase.empty()) {
                if (matIdx < 0 || static_cast<std::size_t>(matIdx) >= materialBase.size()) {
                    out->unknownMaterials += 1u;   // the table names no block for this record's material
                    break;
                }
                base = materialBase[static_cast<std::size_t>(matIdx)];
            }
            if (!validRecord(at, count, base)) {
                if (producedAny) out->brokenLinks += 1u; else out->badHeads += 1u;
                break;
            }
            const bool again = std::find(walked.begin(), walked.end(), at) != walked.end();
            if (!again) {
                walked.push_back(at);
                producedAny = true;
                out->records += 1u;
                const std::byte* run = payload.data() + at + kHeaderBytes;
                for (std::uint32_t i = 0; i + 2u < count; i += 3u) {
                    out->indices.push_back(base + loadU16(run + 2u * i));
                    out->indices.push_back(base + loadU16(run + 2u * (i + 1u)));
                    out->indices.push_back(base + loadU16(run + 2u * (i + 2u)));
                    out->materialOfTriangle.push_back(matIdx);
                }
            } else if (!producedAny) {
                out->heads += 1u;   // this leaf's bytes are a record, drawn by another chain
            }
            if (again || at == static_cast<std::size_t>(next)) break;   // shared tail, cycle or self link
            at = next;
        }
        if (producedAny) out->heads += 1u;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool decodeTerrainOctree(std::span<const std::byte> payload,
                         const std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>& references,
                         TerrainOctree* out, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (out == nullptr) return fail("decodeTerrainOctree needs an output struct");
    if (payload.size() < 24u) return fail("oct-tree payload is " + std::to_string(payload.size()) + " bytes, too small "
                                          "for one node (needs at least 24)");

    TerrainOctree tree;
    std::vector<std::pair<std::uint32_t, unsigned>> stack{{0u, 0u}};   // (node offset, depth)
    std::vector<bool> visited(payload.size() / 4u + 1u, false);
    while (!stack.empty()) {
        const auto [offset, depth] = stack.back();
        stack.pop_back();
        if (offset % 4u != 0u || offset / 4u >= visited.size() || visited[offset / 4u]) {
            return fail("oct-tree node at " + std::to_string(offset) + " was already visited or misaligned");
        }
        visited[offset / 4u] = true;
        if (offset + 24u > payload.size()) {
            return fail("oct-tree node at " + std::to_string(offset) + " runs past the end of the payload");
        }
        const auto childCount = static_cast<std::int32_t>(loadU32(payload.data() + offset + 20u));
        if (childCount < 0 || childCount > 8) {
            return fail("oct-tree node at " + std::to_string(offset) + " claims " + std::to_string(childCount)
                        + " children, outside the measured range 0-8");
        }
        tree.nodes += 1;
        tree.maxDepth = depth > tree.maxDepth ? depth : tree.maxDepth;
        if (childCount == 0) {
            const auto reference = references.find(offset + 16u);
            if (reference == references.end()) {
                return fail("oct-tree leaf at " + std::to_string(offset) + " has no relocation reference in its +16 slot");
            }
            tree.leaves += 1;
            tree.leafList.push_back({offset, reference->second.first, reference->second.second});
            continue;
        }
        std::uint32_t previous = offset;
        for (std::int32_t child = 0; child < childCount; ++child) {
            const std::uint32_t at = offset + 24u + 4u * static_cast<std::uint32_t>(child);
            const std::uint32_t target = loadU32(payload.data() + at);
            if (target <= previous || target + 24u > payload.size()) {
                return fail("oct-tree node at " + std::to_string(offset) + " has a child offset " 
                            + std::to_string(target) + " outside the payload or out of order");
            }
            previous = target;
            stack.emplace_back(target, depth + 1);
        }
    }
    *out = std::move(tree);
    if (error != nullptr) error->clear();
    return true;
}

bool decodeTerrainMaterialBases(std::span<const std::byte> payload, const std::vector<DrmReloc>& relocs,
                                 std::uint32_t selfSection, TerrainMaterialList* out, std::string* error) {
    constexpr std::size_t kRecordBytes = 20u;   // GEOMETRY.md §20
    constexpr std::size_t kPointerSlot = 8u;
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (out == nullptr) return fail("decodeTerrainMaterialBases needs an output struct");

    // Candidate record starts: kind-0 slots that name this very section, back-stepped over the pointer slot. A strip
    // record's own nextTexture link also names its section, but it sits at header +40 and chains are laid out at
    // variable pitch, so those never join a 20-byte run.
    std::vector<std::uint32_t> starts;
    for (const auto& reloc : relocs) {
        if (relocKind(reloc) != 0u || relocSectionIndex(reloc) != selfSection) continue;
        if (reloc.siteOffset < kPointerSlot) continue;
        const std::uint32_t at = reloc.siteOffset - static_cast<std::uint32_t>(kPointerSlot);
        if (static_cast<std::size_t>(at) + kRecordBytes > payload.size()) continue;
        starts.push_back(at);
    }
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

    std::size_t bestLength = 0u, bestIndex = 0u;
    for (std::size_t i = 0; i < starts.size(); ) {
        std::size_t j = i;
        while (j + 1u < starts.size() && starts[j + 1u] == starts[j] + kRecordBytes) ++j;
        if (j - i + 1u > bestLength) { bestLength = j - i + 1u; bestIndex = i; }
        i = j + 1u;
    }
    if (bestLength < 8u) {
        return fail("no material base table: " + std::to_string(starts.size()) + " own-section slots, longest run of "
                    "20-byte records is " + std::to_string(bestLength));
    }

    TerrainMaterialList list;
    for (std::size_t k = 0; k < bestLength; ++k) {
        const std::size_t at = starts[bestIndex + k];
        if (loadU32(payload.data() + at + 4u) != 0u) {
            return fail("material record at " + std::to_string(at) + " has a non-zero word after the base");
        }
        const std::uint32_t base = loadU32(payload.data() + at);
        if (!list.vertexBase.empty() && base < list.vertexBase.back()) {
            return fail("material bases decrease at " + std::to_string(at));
        }
        list.vertexBase.push_back(base);
        list.classValue.push_back(loadU32(payload.data() + at + 12u));   // (class << 16 | value), §21; kept raw
    }
    *out = std::move(list);
    if (error != nullptr) error->clear();
    return true;
}

bool findTerrainMaterialBases(const DrmFile& drm, std::uint32_t vertexArraySection, TerrainMaterialList* out,
                              std::size_t* sectionOut, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (out == nullptr) return fail("findTerrainMaterialBases needs an output struct");
    if (vertexArraySection >= drm.sections().size()) return fail("vertex array section index out of range");
    const auto& vertexPayload = drm.payload(drm.sections()[vertexArraySection]);
    const std::size_t vertexCount = vertexPayload.size() / kMeshVertexBytes;

    // Census walk (§10, §15): every oct-tree's leaf references entered base-free. Material indices ride in the records
    // themselves (header +20), so this pass is table-independent and gives one honest set per unit to test against.
    std::map<std::uint32_t, std::vector<std::uint32_t>> headsPerSection;
    for (std::size_t i = 1; i < drm.sections().size(); ++i) {
        const auto& nodeSection = drm.sections()[i];
        if (!nodeSection.stored || nodeSection.relocCount == 0u) continue;
        std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> references;
        for (const auto& r : drm.relocations(nodeSection)) {
            if (relocKind(r) != 0u) continue;   // node fields are patched by kind-0 entries (§9)
            references[static_cast<std::uint32_t>(r.siteOffset)] = {
                static_cast<std::uint32_t>(relocSectionIndex(r)), drm.relocAddend(nodeSection, r)};
        }
        TerrainOctree tree;
        std::string why;
        if (!decodeTerrainOctree(drm.payload(nodeSection), references, &tree, &why)) continue;
        for (const auto& leaf : tree.leafList) {
            if (leaf.targetSection >= drm.sections().size()) continue;
            headsPerSection[leaf.targetSection].push_back(leaf.offsetInTarget);
        }
    }
    if (headsPerSection.empty()) return fail("unit has no oct-tree leaf references to census");
    std::int32_t maxMat = -1;   // largest header +20 value any walked record names (§10)
    for (const auto& entry : headsPerSection) {
        const auto& strip = drm.sections()[entry.first];
        if (!strip.stored) continue;
        TerrainStripChain walked;
        std::string chainError;   // a walk that produces nothing still answers honestly (§15); census only needs mats read
        decodeTerrainStripChains(drm.payload(strip), entry.second, vertexCount, &walked, &chainError);
        for (const auto m : walked.materialOfTriangle) if (m > maxMat) maxMat = m;   // triangles share their record's matIdx
    }
    if (maxMat < 0) return fail("walked chains name no material indices to select a table against");

    // Candidate sections in file order; the first decoded table covering every named matIdx wins. §20's narrow detector
    // still refuses junk per section, and coverage over all walked records is what separates this unit's terrain table
    // from same-shaped runs serving its model arrays: sweep-bases3 measured 45 units with candidate runs, exactly one
    // covering run in every case that has a table at all (35 unique; the other 10 are multi-run where only coverage separates).
    TerrainMaterialList list;
    std::size_t found = drm.sections().size();
    for (std::size_t i = 1; i < drm.sections().size() && found == drm.sections().size(); ++i) {
        const auto& candidate = drm.sections()[i];
        if (!candidate.stored || candidate.relocCount == 0u) continue;   // §20: table sections patch their own +8 slots
        std::string why;
        decodeTerrainMaterialBases(drm.payload(candidate), drm.relocations(candidate), static_cast<std::uint32_t>(i), &list, &why);
        if (static_cast<long long>(list.vertexBase.size()) > maxMat) { found = i; break; }   // covers every named matIdx (§10 +20 < size) [V-data sweep-bases3]
    }
    if (found == drm.sections().size()) {   // no section's table covers this unit's material census (§20 open item 1)
        return fail("no base table covers the walked chains' matIdx range up to " + std::to_string(maxMat));
    }
    *out = std::move(list);
    if (sectionOut != nullptr) *sectionOut = found;
    if (error != nullptr) error->clear();
    return true;
}

// Per-oct-tree walk with each tree's own base-table choice (docs/formats/GEOMETRY.md §25). The candidate runs come
// from the same §20 detector findTerrainMaterialBases uses; only the census scope changes — one tree at a time
// instead of every tree in the unit — and `tableRule` decides how each tree picks from that pool: §21's first covering
// run, the shortest covering run, or the table its own strip payloads kind-0-relocate (§26 measured on unit 8baf540e
// that every tree names exactly one candidate this way). Nothing is asserted about which rule matches the shipped
// engine here: callers compare them on real units, and capacity/coverage failures still land in TerrainStripChain's
// counters rather than drawing junk.
// GEOMETRY.md §29 [V-exe]: tra.exe's DrawTerrainGroup (0x40d390) stores the terrain group it is drawing and DrawTerrainStrip (0x40b970) indexes *(group + 0x90) as {u32 numMaterials; 20-byte records}, handing each record's +8 slot to DrawIndexedPrimitive as the vertex base for strips whose own index at +4 reads -1. The group array is a stride-0xB0 run inside one payload, and its kind-0 relocations pair an oct-tree section at record + 0x44 with that tree's table section at + 0x90 - recorded data, not a census guess like §21/§26's rules.
bool decodeTerrainGroupLinks(const DrmFile& drm, std::vector<TerrainGroupLink>* out, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (out == nullptr) return fail("decodeTerrainGroupLinks needs an output vector");
    out->clear();
    const auto sections = drm.sections();
    for (std::size_t s = 0; s < sections.size(); ++s) {
        const auto payload = drm.payload(sections[s]);
        if (!sections[s].stored || payload.size() < 0x94u) continue;   // a group record's +0x90 slot must fit
        // Kind-0 sites of this section keyed by slot, so each group record's +0x44/+0x90 slots read as target
        // section PLUS addend - §29's table lives at *(group + 0x90) inside its section, not necessarily at byte zero.
        std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> targetAtSite;
        for (const auto& r : drm.relocations(sections[s])) {
            if (oa::cdc::relocKind(r) != 0u) continue;
            const auto slot = static_cast<std::uint32_t>(r.siteOffset);
            if (slot < payload.size()) targetAtSite[slot] = {oa::cdc::relocSectionIndex(r), drm.relocAddend(sections[s], r)};
        }
        for (std::size_t record = 0u; record + 0x94u <= payload.size(); record += 0xB0u) {
            const auto tree = targetAtSite.find(static_cast<std::uint32_t>(record + 0x44u));
            const auto table = targetAtSite.find(static_cast<std::uint32_t>(record + 0x90u));
            if (tree == targetAtSite.end() || table == targetAtSite.end()) continue;
            if (tree->second.first >= sections.size() || !sections[tree->second.first].stored) continue;
            if (table->second.first >= sections.size() || !sections[table->second.first].stored) continue;
            out->push_back(TerrainGroupLink{tree->second.first, table->second.first, table->second.second});
        }
    }
    if (out->empty()) return fail("no payload carries kind-0 relocations pairing +0x44 with +0x90");
    if (error != nullptr) error->clear();
    return true;
}


bool decodeTerrainUnit(const DrmFile& drm, std::uint32_t vertexArraySection, TerrainTableRule tableRule,
                       std::vector<TerrainTreeWalk>* out, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (out == nullptr) return fail("decodeTerrainUnit needs an output vector");
    if (vertexArraySection >= drm.sections().size()) return fail("vertex array section index out of range");
    const auto vertexPayload = drm.payload(drm.sections()[vertexArraySection]);
    if (vertexPayload.size() < kMeshVertexBytes || vertexPayload.size() % kMeshVertexBytes != 0u) {
        return fail("section " + std::to_string(vertexArraySection) + " is not a whole number of stride-20 records");
    }
    const std::size_t vertexCount = vertexPayload.size() / kMeshVertexBytes;

    // Candidate base tables in file order: every section whose own-section kind-0 pointer slots pass §20's narrow
    // detector. findTerrainMaterialBases (§21) picks among exactly these for the whole unit; here each tree picks
    // again from the same pool against its own census.
    struct Candidate { std::size_t section; TerrainMaterialList list; };
    std::vector<Candidate> candidates;
    for (std::size_t i = 1; i < drm.sections().size(); ++i) {
        const auto& candidateSection = drm.sections()[i];
        if (!candidateSection.stored || candidateSection.relocCount == 0u) continue;
        Candidate c{i, {}};
        std::string why;
        if (decodeTerrainMaterialBases(drm.payload(candidateSection), drm.relocations(candidateSection),
                                       static_cast<std::uint32_t>(i), &c.list, &why)) {
            candidates.push_back(std::move(c));
        }
    }

    // §29 [V-exe]: the shipped pairing lives in terrain GROUP records (stride-0xB0 payloads whose kind-0 relocations at
    // +0x44 name an oct-tree section and +0x90 name that tree's material table). decodeTerrainGroupLinks finds every such pair; a tree with a link draws with THAT table, no census involved.
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> groupTableForTree;   // tree section -> (table section, table offset)
    {
        std::vector<TerrainGroupLink> links;
        std::string why;
        if (decodeTerrainGroupLinks(drm, &links, &why)) {
            for (const auto& link : links) groupTableForTree[link.treeSection] = {link.tableSection, link.tableOffset};
        }
    }

    const auto walkPayload = [&](std::size_t payloadIndex, std::span<const std::uint32_t> heads,
                                 std::span<const std::uint32_t> bases, TerrainStripChain& chain) {
        decodeTerrainStripChains(drm.payload(drm.sections()[payloadIndex]), heads, vertexCount, &chain, nullptr, bases);
    };
    const auto censusMaxMaterial = [&](std::map<std::size_t, std::vector<std::uint32_t>>& headsPerPayload) {
        std::int32_t maxMat = -1;
        for (const auto& entry : headsPerPayload) {
            TerrainStripChain walked;
            walkPayload(entry.first, entry.second, {}, walked);
            for (const auto m : walked.materialOfTriangle) if (m > maxMat) maxMat = m;
        }
        return maxMat;
    };

    std::vector<TerrainTreeWalk> trees;
    for (std::size_t i = 1; i < drm.sections().size(); ++i) {
        const auto& nodeSection = drm.sections()[i];
        if (!nodeSection.stored || nodeSection.relocCount == 0u) continue;
        std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> references;
        for (const auto& r : drm.relocations(nodeSection)) {
            if (oa::cdc::relocKind(r) != 0u) continue;   // node fields are patched by kind-0 entries (§9)
            references[static_cast<std::uint32_t>(r.siteOffset)] = {
                static_cast<std::uint32_t>(oa::cdc::relocSectionIndex(r)), drm.relocAddend(nodeSection, r)};
        }
        TerrainOctree tree;
        std::string why;
        if (!decodeTerrainOctree(drm.payload(nodeSection), references, &tree, &why)) continue;

        std::map<std::size_t, std::vector<std::uint32_t>> headsPerPayload;
        for (const auto& leaf : tree.leafList) {
            if (leaf.targetSection >= drm.sections().size()) continue;
            headsPerPayload[leaf.targetSection].push_back(leaf.offsetInTarget);
        }
        if (headsPerPayload.empty()) continue;   // a tree whose leaves name no payload has nothing to walk (§9)

        TerrainTreeWalk walk;
        walk.nodeSection = i;
        // §29 [V-exe]: when asked by rule, this tree's table is whatever its terrain GROUP record points at - the shipped pairing, not a census guess. The group array names sections directly; the BASES still come from that section's §20-detected run because §29's own material records store byte offsets we have not pinned down yet [INFERENCE].
        const auto link = groupTableForTree.find(static_cast<std::uint32_t>(i));   // tree section -> (table section, table offset) from §29
        const auto maxMat = censusMaxMaterial(headsPerPayload);   // base-free census of this tree's own matIdx range (§10)
        std::size_t fallbackChoice = candidates.size();
        for (std::size_t k = 0; k < candidates.size(); ++k) {
            if (static_cast<long long>(candidates[k].list.vertexBase.size()) > maxMat) { fallbackChoice = k; break; }
        }
        std::size_t shortestChoice = candidates.size();
        for (std::size_t k = 0; k < candidates.size(); ++k) {
            if (!(static_cast<long long>(candidates[k].list.vertexBase.size()) > maxMat)) continue;
            if (shortestChoice == candidates.size() ||
                candidates[k].list.vertexBase.size() < candidates[shortestChoice].list.vertexBase.size()) {
                shortestChoice = k;
            }
        }
        std::size_t namedChoice = candidates.size();
        std::uint32_t bestCount = 0u;
        if (tableRule == TerrainTableRule::relocNamed || tableRule == TerrainTableRule::relocOrFirstCovering) {
            for (std::size_t k = 0; k < candidates.size(); ++k) {   // §26: the payloads' kind-0 relocations name a table
                std::uint32_t count = 0u;
                for (const auto& entry : headsPerPayload) {
                    for (const auto& r : drm.relocations(drm.sections()[entry.first])) {
                        if (relocKind(r) == 0u && relocSectionIndex(r) == candidates[k].section) ++count;
                    }
                }
                if (count > bestCount) { bestCount = count; namedChoice = k; }   // most-referenced table wins ties by file order
            }
        }
        std::size_t linkedChoice = candidates.size();
        if (tableRule == TerrainTableRule::groupRecord && link != groupTableForTree.end()) {
            for (std::size_t k = 0; k < candidates.size(); ++k) {   // §29: the linked section's detected run, when one exists
                if (candidates[k].section == link->second.first) { linkedChoice = k; break; }
            }
        }
        const auto choice = tableRule == TerrainTableRule::firstCovering      ? fallbackChoice
                            : tableRule == TerrainTableRule::shortestCovering ? shortestChoice
                            : tableRule == TerrainTableRule::relocNamed       ? namedChoice
                            : tableRule == TerrainTableRule::groupRecord      ? linkedChoice
                                                                              : (bestCount > 0u ? namedChoice : fallbackChoice);
        walk.tableReferences = bestCount;
        if (choice != candidates.size()) walk.tableSection = candidates[choice].section;
        for (auto& entry : headsPerPayload) {
            TerrainStripChain chain;
            walkPayload(entry.first, entry.second,
                        choice != candidates.size() ? std::span<const std::uint32_t>(candidates[choice].list.vertexBase)
                                                    : std::span<const std::uint32_t>{},
                        chain);
            walk.byPayload.emplace_back(entry.first, std::move(chain));
        }
        trees.push_back(std::move(walk));
    }
    if (trees.empty()) return fail("unit has no oct-tree with leaf references to walk");
    *out = std::move(trees);
    if (error != nullptr) error->clear();
    return true;
}

}  // namespace oa::cdc
