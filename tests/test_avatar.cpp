#include "game/Avatar.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using oa::cdc::MeshVertex;
using oa::cdc::TerrainQuery;

MeshVertex vertex(std::int16_t x, std::int16_t y, std::int16_t z) {
    MeshVertex out;
    out.x = x;
    out.y = y;
    out.z = z;
    return out;
}

// One horizontal rectangle as two triangles, appended to `vertices`/`indices`.
void addQuad(std::vector<MeshVertex>* vertices, std::vector<std::uint32_t>* indices, float x0, float x1, float y,
             float z0, float z1) {
    const std::uint32_t base = static_cast<std::uint32_t>(vertices->size());
    vertices->insert(vertices->end(),
                     {vertex(static_cast<std::int16_t>(x0), static_cast<std::int16_t>(y), static_cast<std::int16_t>(z0)),
                      vertex(static_cast<std::int16_t>(x1), static_cast<std::int16_t>(y), static_cast<std::int16_t>(z0)),
                      vertex(static_cast<std::int16_t>(x1), static_cast<std::int16_t>(y), static_cast<std::int16_t>(z1)),
                      vertex(static_cast<std::int16_t>(x0), static_cast<std::int16_t>(y), static_cast<std::int16_t>(z1))});
    indices->insert(indices->end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
}

TerrainQuery build(std::vector<MeshVertex>* vertices, std::vector<std::uint32_t>* indices) {
    TerrainQuery terrain;
    std::string error;
    EXPECT_TRUE(terrain.build(*vertices, *indices, {}, &error)) << error;
    return terrain;
}

constexpr float kStep = 1.0f / 60.0f;

// yaw = pi/2 faces +x, which is the direction these layouts run along.
constexpr float kEastYaw = 1.5707963267948966f;

oa::game::AvatarInput walk(float forward) {
    oa::game::AvatarInput input;
    input.forward = forward;
    return input;
}

TEST(Avatar, walksOnFlatGroundAtTheTunedSpeed) {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    addQuad(&vertices, &indices, 0.0f, 2000.0f, 0.0f, -1000.0f, 1000.0f);   // long and wide enough for a second of walking, running and sliding
    const TerrainQuery terrain = build(&vertices, &indices);

    oa::game::AvatarTuning tuning;
    oa::game::AvatarState state;
    ASSERT_TRUE(oa::game::placeOnGround(&state, 10.0f, 50.0f, 20.0f, 40.0f, 20.0f, terrain));
    EXPECT_NEAR(state.y, 0.0f, 1.0e-3f);
    EXPECT_TRUE(state.grounded);

    std::uint32_t supported = 0;
    for (std::uint32_t tick = 0; tick < 60u; ++tick) {
        ASSERT_TRUE(oa::game::advance(&state, walk(1.0f), kEastYaw, kStep, terrain, tuning));
        supported += state.grounded ? 1u : 0u;
    }
    EXPECT_EQ(supported, 60u);
    EXPECT_NEAR(state.x - 10.0f, tuning.walkSpeed * 1.0f, 2.0f);   // one second of walking

    // Running covers more ground in the same time, and a diagonal combo must not outrun a straight one.
    oa::game::AvatarState running;
    ASSERT_TRUE(oa::game::placeOnGround(&running, 10.0f, 50.0f, 20.0f, 40.0f, 20.0f, terrain));
    oa::game::AvatarInput run = walk(1.0f);
    run.run = true;
    for (std::uint32_t tick = 0; tick < 60u; ++tick) ASSERT_TRUE(oa::game::advance(&running, run, kEastYaw, kStep, terrain, tuning));
    EXPECT_NEAR(running.x - 10.0f, tuning.runSpeed * 1.0f, 2.0f);

    oa::game::AvatarState diagonal;
    ASSERT_TRUE(oa::game::placeOnGround(&diagonal, 10.0f, 50.0f, 20.0f, 40.0f, 20.0f, terrain));
    oa::game::AvatarInput both = walk(1.0f);
    both.right = 1.0f;
    for (std::uint32_t tick = 0; tick < 60u; ++tick) ASSERT_TRUE(oa::game::advance(&diagonal, both, kEastYaw, kStep, terrain, tuning));
    EXPECT_NEAR(std::hypot(diagonal.x - 10.0f, diagonal.z - 50.0f), tuning.walkSpeed * 1.0f, 2.0f);
}

TEST(Avatar, gravityTakesTheBodyDownAPitAndItKeepsWalkingOnThePitFloor) {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    addQuad(&vertices, &indices, 0.0f, 40.0f, 0.0f, 0.0f, 100.0f);      // the level floor
    addQuad(&vertices, &indices, 40.0f, 700.0f, -40.0f, 0.0f, 100.0f); // a lower floor past the edge, long enough to keep walking on
    const TerrainQuery terrain = build(&vertices, &indices);

    oa::game::AvatarTuning tuning;
    oa::game::AvatarState state;
    ASSERT_TRUE(oa::game::placeOnGround(&state, 20.0f, 50.0f, 10.0f, 10.0f, 10.0f, terrain));

    std::uint32_t airborne = 0;
    bool landedLow = false;
    for (std::uint32_t tick = 0; tick < 90u; ++tick) {   // 1.5 s at 400 u/s carries it well past the edge
        ASSERT_TRUE(oa::game::advance(&state, walk(1.0f), kEastYaw, kStep, terrain, tuning));
        airborne += state.grounded ? 0u : 1u;
        if (state.grounded && state.groundY < -39.0f) landedLow = true;
    }
    EXPECT_GT(airborne, 0u);              // it left the upper floor under gravity rather than teleporting
    EXPECT_TRUE(landedLow);
    EXPECT_NEAR(state.y, -40.0f, 1.0e-3f);
    EXPECT_TRUE(state.grounded);
}

TEST(Avatar, oneJumpCrossesAHoleAndLandsOnTheFarFloor) {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    addQuad(&vertices, &indices, 0.0f, 40.0f, 0.0f, 0.0f, 100.0f);   // take-off floor
    addQuad(&vertices, &indices, 60.0f, 700.0f, 0.0f, 0.0f, 100.0f); // far floor; the hole between has no floor at all
    const TerrainQuery terrain = build(&vertices, &indices);

    oa::game::AvatarTuning tuning;
    oa::game::AvatarState state;
    ASSERT_TRUE(oa::game::placeOnGround(&state, 30.0f, 50.0f, 10.0f, 10.0f, 10.0f, terrain));

    float apex = state.y;
    bool crossedAirborne = false;
    bool landedAfterFlight = false;
    bool jumpedOnce = false;
    oa::game::AvatarInput input = walk(1.0f);
    for (std::uint32_t tick = 0; tick < 90u; ++tick) {
        input.jump = !jumpedOnce && state.grounded && state.x > 36.0f;   // one take-off, just before the edge
        jumpedOnce = jumpedOnce || input.jump;
        const bool held = oa::game::advance(&state, input, kEastYaw, kStep, terrain, tuning);
        ASSERT_TRUE(held) << "fell into the hole at tick " << tick;
        apex = std::max(apex, state.y);
        if (!state.grounded && state.x > 40.0f && state.x < 60.0f) crossedAirborne = true;
        if (crossedAirborne && state.grounded && state.x > 60.0f) landedAfterFlight = true;
    }
    EXPECT_TRUE(crossedAirborne);
    EXPECT_TRUE(landedAfterFlight);
    EXPECT_NEAR(state.y, 0.0f, 1.0e-3f);
    // Discrete stepping drops about one tick of gravity on the way up, so the apex sits just under v^2 / 2g.
    const float idealApex = tuning.jumpSpeed * tuning.jumpSpeed / (2.0f * -tuning.gravity);
    EXPECT_GT(apex, idealApex * 0.9f);
    EXPECT_LT(apex, idealApex * 1.05f);
}

TEST(Avatar, climbsAStepItCanReachAndNeverLiftsOntoATallerOne) {
    oa::game::AvatarTuning tuning;
    struct Case {
        float riser;
        bool expectClimb;
    };
    for (const Case& c : {Case{18.0f, true}, Case{60.0f, false}}) {
        std::vector<MeshVertex> vertices;
        std::vector<std::uint32_t> indices;
        addQuad(&vertices, &indices, 0.0f, 50.0f, 0.0f, 0.0f, 100.0f);
        addQuad(&vertices, &indices, 50.0f, 700.0f, c.riser, 0.0f, 100.0f);
        const TerrainQuery terrain = build(&vertices, &indices);

        oa::game::AvatarState state;
        ASSERT_TRUE(oa::game::placeOnGround(&state, 20.0f, 50.0f, 10.0f, 10.0f, 10.0f, terrain));
        float highest = state.y;
        bool alive = true;
        for (std::uint32_t tick = 0; tick < 90u && alive; ++tick) {
            alive = oa::game::advance(&state, walk(1.0f), kEastYaw, kStep, terrain, tuning);
            highest = std::max(highest, state.y);
        }
        if (c.expectClimb) {
            EXPECT_TRUE(alive) << "riser " << c.riser;
            EXPECT_GT(state.x, 60.0f) << "riser " << c.riser;
            EXPECT_NEAR(state.y, c.riser, 1.0e-3f);
            EXPECT_TRUE(state.grounded);
        } else {
            // A column query cannot push a body sideways, so a riser taller than a stride is not blocked horizontally -
            // that needs the capsule sweep Avatar.hpp names as out of scope. What must never happen is being lifted onto
            // it, and that is what this asserts instead of pretending the wall stops her.
            EXPECT_LE(highest, tuning.maxStepUp + 1.0e-3f) << "riser " << c.riser;
        }
    }
}

TEST(Avatar, spawnOverAHoleIsRefusedAndLeavingTheWorldStopsTheRun) {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    addQuad(&vertices, &indices, 0.0f, 40.0f, 0.0f, 0.0f, 100.0f);
    const TerrainQuery terrain = build(&vertices, &indices);

    oa::game::AvatarState state;
    EXPECT_FALSE(oa::game::placeOnGround(&state, 80.0f, 50.0f, 10.0f, 30.0f, 10.0f, terrain));   // nothing in that column
    EXPECT_TRUE(oa::game::placeOnGround(&state, 20.0f, 50.0f, 10.0f, 30.0f, 10.0f, terrain));

    // Walk east off the only floor: gravity takes it below the mesh and advance reports the body is gone.
    oa::game::AvatarTuning tuning;
    bool alive = true;
    for (std::uint32_t tick = 0; tick < 400u && alive; ++tick) alive = oa::game::advance(&state, walk(1.0f), kEastYaw, kStep, terrain, tuning);
    EXPECT_FALSE(alive);
    EXPECT_LT(state.y, terrain.boundsMin()[1]);
}

// A fall longer than one stride per tick has to meet the floor it crossed between two ticks. TerrainQuery::surfaceAt reports
// the highest surface with no lower bound, so this holds only while the foot query is anchored on the HIGHER of the old and
// new feet positions: anchoring it on the lower one puts y = -900 outside the window (this step is 41 units against a
// 25-unit stride) and the body drops through the level. The numbers are exact rather than luck-dependent.
TEST(Avatar, landsOnAFloorCrossedMidFall) {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    addQuad(&vertices, &indices, -1000.0f, 1000.0f, -900.0f, -1000.0f, 1000.0f);   // the floor it is falling towards
    const TerrainQuery terrain = build(&vertices, &indices);

    oa::game::AvatarTuning tuning;
    oa::game::AvatarState state;
    state.x = 0.0f;
    state.z = 0.0f;
    state.y = -854.0f;   // the next step ends at -894, still above the floor; the one after that crosses it
    state.vy = -2400.0f;
    state.grounded = false;

    const bool aliveFirst = oa::game::advance(&state, walk(0.0f), kEastYaw, kStep, terrain, tuning);
    EXPECT_TRUE(aliveFirst);
    EXPECT_FALSE(state.grounded) << "it landed before the feet reached the floor";
    const bool alive = oa::game::advance(&state, walk(0.0f), kEastYaw, kStep, terrain, tuning);
    EXPECT_TRUE(alive) << "the body left the world instead of landing";
    EXPECT_TRUE(state.grounded) << "it passed through the floor it crossed, y " << state.y;
    EXPECT_NEAR(state.y, -900.0f, 1.0e-3f);
    EXPECT_EQ(state.groundY, -900.0f);
}

// Shipped terrain is not a staircase of flat plates: it slopes by a few units between one triangle and the next, and a body
// that detaches on every such step never stays on a ledge long enough to cross anything. Stepping eight units down has to
// keep contact - measured on shipped strips, where a foot probe answered 3.5 units below the feet one tick after placement
// and the body then dropped into the next gap instead of running along the level.
TEST(Avatar, walksDownASmallStepWithoutLeavingTheGround) {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    addQuad(&vertices, &indices, -1000.0f, 0.0f, 0.0f, -500.0f, 500.0f);      // the higher ledge
    addQuad(&vertices, &indices, 0.0f, 2000.0f, -8.0f, -500.0f, 500.0f);      // eight units down: inside one stride
    const TerrainQuery terrain = build(&vertices, &indices);

    oa::game::AvatarTuning tuning;
    oa::game::AvatarState state;
    ASSERT_TRUE(oa::game::placeOnGround(&state, -200.0f, 0.0f, 20.0f, 40.0f, 20.0f, terrain));
    EXPECT_NEAR(state.y, 0.0f, 1.0e-3f);

    bool alive = true;
    bool everAirborne = false;
    for (int tick = 0; tick < 90 && alive; ++tick) {
        alive = oa::game::advance(&state, walk(1.0f), kEastYaw, kStep, terrain, tuning);
        everAirborne |= !state.grounded;   // eight units down is a step, not a drop: contact must hold
    }
    EXPECT_TRUE(alive);
    EXPECT_GT(state.x, 200.0f) << "it never walked past the step";
    EXPECT_NEAR(state.y, -8.0f, 1.0e-3f);
    EXPECT_FALSE(everAirborne) << "stepping down eight units put the body in free fall";
}
}  // namespace
