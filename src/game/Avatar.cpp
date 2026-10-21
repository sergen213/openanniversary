#include "game/Avatar.hpp"

#include <cmath>

namespace oa::game {

bool placeOnGround(AvatarState* state, float x, float z, float startY, float below, float above,
                   const oa::cdc::TerrainQuery& terrain) {
    if (state == nullptr) return false;
    oa::cdc::TerrainHit hit{};
    if (!terrain.surfaceAt(x, z, startY, above, &hit)) return false;
    if (hit.y < startY - below) return false;   // the only surface in this column is far under the spawn
    state->x = x;
    state->z = z;
    state->y = hit.y;
    state->vx = state->vy = state->vz = 0.0f;
    state->grounded = true;
    state->groundY = hit.y;
    state->groundTriangle = hit.triangle;
    return true;
}

bool advance(AvatarState* state, const AvatarInput& input, float yaw, float dt, const oa::cdc::TerrainQuery& terrain,
             const AvatarTuning& tuning, float fallMargin) {
    if (state == nullptr || dt <= 0.0f) return false;

    // Facing: yaw turns from +z towards +x, which is how the mesh camera's orbit reads on screen.
    const float forwardX = std::sin(yaw), forwardZ = std::cos(yaw);
    const float rightX = std::cos(yaw), rightZ = -std::sin(yaw);
    float axisForward = input.forward, axisRight = input.right;
    const float magnitude = std::sqrt(axisForward * axisForward + axisRight * axisRight);
    if (magnitude > 1.0f) {   // a diagonal key combo must not outrun a straight one
        axisForward /= magnitude;
        axisRight /= magnitude;
    }
    const float speed = input.run ? tuning.runSpeed : tuning.walkSpeed;
    state->vx = speed * (forwardX * axisForward + rightX * axisRight);
    state->vy = state->grounded ? 0.0f : state->vy;   // contact cancels the accumulated fall
    state->vz = speed * (forwardZ * axisForward + rightZ * axisRight);

    if (input.jump && state->grounded) {   // take-off is an edge, not a held button
        state->vy = tuning.jumpSpeed;
        state->grounded = false;
    }

    if (!state->grounded) {
        state->vy += tuning.gravity * dt;
        if (state->vy < tuning.maxFallSpeed) state->vy = tuning.maxFallSpeed;
    }

    // Horizontal move first, so the foot probe below answers for where the body ends up. Nothing blocks it sideways:
    // a column query cannot report a wall beside the body (see the header note).
    state->x += state->vx * dt;
    state->z += state->vz * dt;

    // Vertical contact. TerrainQuery::surfaceAt reports the highest surface under foot with no lower bound, so the query is
    // anchored on the HIGHER of the old and new feet positions: that is what lets a fall longer than one stride (41 units per
    // tick against a 25-unit stride in Avatar.landsOnAFloorCrossedMidFall) still meet the floor it crossed between ticks,
    // where anchoring on the lower position would let the body pass straight through the level. A body already on the ground
    // also sticks to surfaces up to one stride below its feet: without that, stepping onto anything even 3 units under the
    // feet detaches it - measured on shipped strips, where a probe answered 3.5 units below the feet one tick after
    // placement and the body then dropped into the next gap. Nothing lifts the feet more than a stride, so ceilings stay
    // ceilings; a surface more than a stride down is a drop, not ground, so walking off an edge stays free fall.
    const float nextY = state->y + state->vy * dt;
    oa::cdc::TerrainHit under{};
    const bool support = terrain.surfaceAt(state->x, state->z, state->y > nextY ? state->y : nextY, tuning.maxStepUp, &under);
    const float reachDown = state->grounded ? tuning.maxStepUp : 0.0f;   // downhill contact needs it, free fall does not
    if (support && state->vy <= 0.0f && under.y >= nextY - reachDown - 1.0e-3f && under.y <= state->y + tuning.maxStepUp) {
        state->y = under.y;
        state->vy = 0.0f;
        state->grounded = true;
        state->groundY = under.y;
        state->groundTriangle = under.triangle;
    } else {
        state->y = nextY;
        state->grounded = false;
    }

    return state->y >= terrain.boundsMin()[1] - fallMargin;
}

}  // namespace oa::game
