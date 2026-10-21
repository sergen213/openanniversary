#pragma once
// Moving a body over decoded level geometry: gravity, ground contact, step-up, jump (docs/ROADMAP.md Milestone 5).
//
// The body is a point with a foot probe; the surface comes from `cdc::TerrainQuery`, which answers per vertical column, and
// that bounds what this can do. Contact only ever lifts the body by `maxStepUp` in one tick, so geometry taller than a
// stride is never climbed onto; but because a query above that height reads as a ceiling rather than ground, nothing stops
// the body walking *into* where a wall or riser stands - horizontal push-back needs the capsule sweep TerrainQuery.hpp puts
// out of scope **[INFERENCE]**. Overhangs read as floor for now too.
//
// The tuning numbers are ours, not measured from `tra.exe`: at the scale GEOMETRY.md §13 implies (median triangle edge
// 198 world units, i.e. roughly a centimetre per unit) a walk near 400 units/s and gravity near -2000 units/s^2 give a
// human-looking stride. Replacing them with the shipped values is open work; nothing here claims they are original.

#include "cdc/TerrainQuery.hpp"

namespace oa::game {

struct AvatarTuning {
    float walkSpeed = 400.0f;   // world units per second
    float runSpeed = 750.0f;
    float gravity = -2000.0f;   // world units per second squared
    float jumpSpeed = 620.0f;   // instant vertical velocity on take-off
    float maxStepUp = 25.0f;    // a foot may climb this far in one tick and still count as ground
    float maxFallSpeed = -3000.0f;
};

struct AvatarInput {
    float forward = 0.0f;   // -1..1 along the facing direction
    float right = 0.0f;     // -1..1 across it
    bool run = false;
    bool jump = false;      // taken as an edge: a jump starts only from contact
};

struct AvatarState {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    bool grounded = false;
    float groundY = 0.0f;      // the surface contact rests on, from the last supported tick
    std::uint32_t groundTriangle = 0;
};

// Advances one fixed timestep. `yaw` is the facing in radians about +y (the camera's heading), so input stays readable
// while the body turns. Returns false when the body has dropped below the mesh's lowest point by more than `fallMargin`:
// it left the level through a hole, and the caller should stop rather than integrate further.
bool advance(AvatarState* state, const AvatarInput& input, float yaw, float dt, const oa::cdc::TerrainQuery& terrain,
             const AvatarTuning& tuning, float fallMargin = 200.0f);

// Places the body on the highest surface inside [startY - below, startY + above] at (x, z), settling it into contact.
// False when nothing supports that column, which is how a caller learns its spawn point was over a hole.
bool placeOnGround(AvatarState* state, float x, float z, float startY, float below, float above,
                   const oa::cdc::TerrainQuery& terrain);

}  // namespace oa::game
