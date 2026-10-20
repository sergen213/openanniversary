// Coverage for the mesh-pass camera key mapping (render::applyMeshKeys). The app loop only resolves SDL scancodes to
// MeshKeys, so everything worth pinning - orbit direction, the pole clamp, dolly sign, and which way WASD slides the
// look-at target for a given yaw/pitch - lives in this one pure function.

#include "render/vk/VkRenderer.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using oa::render::applyMeshKeys;
using oa::render::MeshCamera;
using oa::render::MeshKeys;

constexpr float kStep = 10.0f;   // world units per frame of pan, as the app fits it to the mesh extent

}  // namespace

TEST(MeshCamera, ArrowsOrbitYawAndPitch) {
    MeshCamera camera;
    float target[3] = {0.0f, 0.0f, 0.0f};

    MeshKeys right;
    right.right = true;
    applyMeshKeys(camera, target, right, kStep);
    EXPECT_NEAR(0.72f, camera.yaw, 1e-5f);   // default yaw 0.7 + one frame of orbit

    MeshKeys left;
    left.left = true;
    applyMeshKeys(camera, target, left, kStep);
    EXPECT_NEAR(0.70f, camera.yaw, 1e-5f);   // arrows cancel out

    MeshKeys up;
    up.up = true;
    const float pitchBefore = camera.pitch;
    applyMeshKeys(camera, target, up, kStep);
    EXPECT_NEAR(pitchBefore + 0.02f, camera.pitch, 1e-5f);
}

TEST(MeshCamera, PitchStopsShortOfThePoles) {
    MeshCamera camera;
    float target[3] = {0.0f, 0.0f, 0.0f};
    MeshKeys up, down;
    up.up = true;
    down.down = true;

    for (int i = 0; i < 200; ++i) applyMeshKeys(camera, target, up, kStep);
    EXPECT_NEAR(1.5f, camera.pitch, 1e-5f);
    for (int i = 0; i < 400; ++i) applyMeshKeys(camera, target, down, kStep);
    EXPECT_NEAR(-1.5f, camera.pitch, 1e-5f);
}

TEST(MeshCamera, DollyScalesTheFit) {
    MeshCamera camera;
    float target[3] = {0.0f, 0.0f, 0.0f};
    MeshKeys in, out;
    in.dollyIn = true;
    out.dollyOut = true;

    applyMeshKeys(camera, target, in, kStep);
    EXPECT_NEAR(1.03f, camera.zoom, 1e-6f);
    applyMeshKeys(camera, target, out, kStep);
    EXPECT_NEAR(1.0f, camera.zoom, 1e-6f);   // one frame each way is a no-op
}

TEST(MeshCamera, WasdSlidesTargetAlongTheViewBasis) {
    MeshCamera camera;              // yaw = pitch = 0 looks along +Z with right on +X
    camera.yaw = 0.0f;
    camera.pitch = 0.0f;
    float target[3] = {1.0f, 2.0f, 3.0f};

    MeshKeys forwardRight;
    forwardRight.forward = true;
    forwardRight.strafeRight = true;
    applyMeshKeys(camera, target, forwardRight, kStep);
    EXPECT_NEAR(11.0f, target[0], 1e-4f);   // right = {cos yaw, 0, sin yaw} = +X
    EXPECT_NEAR(2.0f, target[1], 1e-4f);    // a level view never changes height
    EXPECT_NEAR(13.0f, target[2], 1e-4f);   // away = {-cos p sin y, sin p, cos p cos y} = +Z

    MeshKeys backLeft;
    backLeft.back = true;
    backLeft.strafeLeft = true;
    applyMeshKeys(camera, target, backLeft, kStep);
    EXPECT_NEAR(1.0f, target[0], 1e-4f);
    EXPECT_NEAR(3.0f, target[2], 1e-4f);
}

TEST(MeshCamera, TurningYawTurnsThePanDirection) {
    MeshCamera camera;
    camera.yaw = static_cast<float>(M_PI) / 2.0f;   // right now points down +Z instead of +X
    camera.pitch = 0.0f;
    float target[3] = {0.0f, 0.0f, 0.0f};

    MeshKeys right;
    right.strafeRight = true;
    applyMeshKeys(camera, target, right, kStep);
    EXPECT_NEAR(0.0f, target[0], 1e-3f);
    EXPECT_NEAR(kStep, target[2], 1e-3f);
}

TEST(MeshCamera, PanIsMirroredIntoTheCamera) {
    MeshCamera camera;
    camera.yaw = 0.0f;
    camera.pitch = 0.0f;
    float target[3] = {4.0f, -5.0f, 6.0f};

    applyMeshKeys(camera, target, MeshKeys{}, kStep);   // no keys held: the view still tracks the target
    EXPECT_NEAR(4.0f, camera.panX, 1e-6f);
    EXPECT_NEAR(-5.0f, camera.panY, 1e-6f);
    EXPECT_NEAR(6.0f, camera.panZ, 1e-6f);
}
