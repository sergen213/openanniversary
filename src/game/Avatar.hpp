#pragma once
// Moving a body over decoded level geometry: gravity, ground contact, step-up, jump (docs/ROADMAP.md Milestone 5).
//
// The body is a point with a foot probe; the surface comes from `cdc::TerrainQuery`, which answers per vertical column, and
// that bounds what this can do. Contact only ever lifts the body by `maxStepUp` in one tick, so geometry taller than a
// stride is never climbed onto; but because a query above that height reads as a ceiling rather than ground, nothing stops
// the body walking *into* where a wall or riser stands - horizontal push-back needs the capsule sweep TerrainQuery.hpp puts
// out of scope **[INFERENCE]**. Overhangs read as floor for now too.
//
// The tuning numbers are ours, not measured from `tra.exe`: a full-image IEEE-754 byte search of the exe (Ghidra scout,
// RESEARCH.md "Shipped runtime constants") found ZERO encodings for gravity or jump magnitudes - vertical motion is
// data-driven, read per-frame as stride-12 records from animation/motion data through `FUN_004e38c0`
// (`return *(float *)(*(int *)(in_ECX + 0x14) + param_1 * 0xc)`), so the shipped values live in level/anim payloads, not
// code. What IS proven from the exe: integration is `pos += vel*dt` with dt in seconds (so velocities are units/s,
// `FUN_0043cf00` lines 42473-88; player entity at line 245392), and dt itself is real time clamped to [0.001, 4.0] s with a
// nominal 30 Hz tick (`DAT_00838458`/`DAT_00838430 = 0x21`, `FUN_004534e0` lines 56272-90). At GEOMETRY.md §13's scale
// (median triangle edge ~198 units, roughly a centimetre per unit) a walk near 400 units/s and gravity near -2000 units/s^2
// give a human-looking stride; nothing here claims the magnitudes are original.

#include "cdc/TerrainQuery.hpp"

namespace oa::game {

struct AvatarTuning {
    float walkSpeed = 400.0f;   // world units per second [V-exe: unverified - the .rdata 400 at 0x652b9c is a camera offset]
    float runSpeed = 750.0f;    // [V-exe: unverified - the u16 750 in the player defaults (FUN_0047f4a0 +0x1288) scales a
                                // turn rate inside FUN_00482650, not locomotion]
    float gravity = -2000.0f;   // world units per second squared [V-exe: absent from the image; anim-record driven]
    float jumpSpeed = 620.0f;   // instant vertical velocity on take-off [V-exe: absent; animation records launch jumps]
    float maxStepUp = 25.0f;    // a foot may climb this far in one tick and still count as ground; the shipped ground snap
                                // gates by angle instead - pi/256 (0.703 deg) and 0.2454 rad tolerances [V-exe, FUN_00482510]
    float maxFallSpeed = -3000.0f;  // [-3000.0 appears only as code immediates (16 sites), never as a data constant]
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
