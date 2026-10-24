#include "game/SaveGame.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using oa::game::AvatarState;
using oa::game::fromText;
using oa::game::loadFromFile;
using oa::game::SaveState;
using oa::game::saveToFile;
using oa::game::toText;

// The slot format is ours (src/game/SaveGame.hpp): these tests pin the round-trip contract and the reject paths, not
// any shipped bytes. Floats ride std::to_string precision by design.

TEST(SaveGame, roundTripsThroughText) {
    SaveState state;
    state.level = "pc-w\\ma1.drm";
    state.avatar = AvatarState{-1234.5f, 67.25f, 99876.0f, 0.0f, -42.5f, 300.0f, true, 66.5f, 17u};
    state.yaw = 0.7f;
    state.pitch = 0.4f;
    state.zoom = 2.0f;
    SaveState back;
    std::string error;
    ASSERT_TRUE(fromText(toText(state), &back, &error)) << error;
    EXPECT_EQ(back.level, state.level);
    EXPECT_FLOAT_EQ(back.avatar.x, -1234.5f);
    EXPECT_FLOAT_EQ(back.avatar.y, 67.25f);     // exact at to_string precision: halves of powers of two
    EXPECT_FLOAT_EQ(back.avatar.z, 99876.0f);
    EXPECT_FLOAT_EQ(back.avatar.vy, -42.5f);
    EXPECT_TRUE(back.avatar.grounded);
    EXPECT_EQ(back.avatar.groundTriangle, 17u);
    EXPECT_FLOAT_EQ(back.zoom, 2.0f);
}

TEST(SaveGame, keepsUnwrittenOptionalFieldsAtDefaults) {
    // An older file that never heard of groundTriangle must still load; unknown keys are ignored.
    const std::string text = "level=8baf540e\nx=1\ny=2\nz=3\nvx=0\nvy=0\nvz=0\ngrounded=0\ngroundY=2\nyaw=0\npitch=0\nzoom=1\nfuture=7\n";
    SaveState back;
    std::string error;
    ASSERT_TRUE(fromText(text, &back, &error)) << error;
    EXPECT_EQ(back.level, "8baf540e");
    EXPECT_FLOAT_EQ(back.avatar.z, 3.0f);
    EXPECT_FALSE(back.avatar.grounded);
}

TEST(SaveGame, rejectsMissingFieldsAndJunk) {
    SaveState back;
    std::string error;
    EXPECT_FALSE(fromText("", &back, &error));
    EXPECT_FALSE(fromText("x=1\ny=2\n", &back, &error)) << "no level";
    EXPECT_FALSE(fromText("level=\nx=1\ny=2\nz=3\nvx=0\nvy=0\nvz=0\ngrounded=0\ngroundY=0\nyaw=0\npitch=0\nzoom=1\n", &back, &error)) << "empty level";
    EXPECT_FALSE(fromText("level=a\nx=nonsense\ny=2\nz=3\nvx=0\nvy=0\nvz=0\ngrounded=0\ngroundY=0\nyaw=0\npitch=0\nzoom=1\n", &back, &error)) << "junk float";
    EXPECT_FALSE(fromText("level=a\nx=1\ny=2\nz=3\nvx=0\nvy=0\nvz=0\ngrounded=0\ngroundY=0\nyaw=0\npitch=0\nzoom=-1\n", &back, &error)) << "inverting zoom";
    const std::string text = "level=a\nx=1\ny=2\nz=3\nvx=0\nvy=0\nvz=0\ngrounded=0\ngroundY=0\nyaw=0\npitch=0\nzoom=1\n";
    EXPECT_TRUE(fromText(text, &back, &error)) << error;   // the same file minus the one bad field parses: each veto is real
}

TEST(SaveGame, writesAndReadsTheSlotFile) {
    const auto path = std::filesystem::temp_directory_path() / "oa_savegame_test/slot.txt";
    SaveState state;
    state.level = "8baf540e";
    state.avatar.groundY = -3.5f;
    std::string error;
    ASSERT_TRUE(saveToFile(path, state, &error)) << error;   // creates the missing directory itself
    SaveState back;
    ASSERT_TRUE(loadFromFile(path, &back, &error)) << error;
    EXPECT_EQ(back.level, "8baf540e");
    EXPECT_FLOAT_EQ(back.avatar.groundY, -3.5f);
    std::filesystem::remove_all(path.parent_path());
}

TEST(SaveGame, loadReportsAMissingSlot) {
    SaveState back;
    std::string error;
    EXPECT_FALSE(loadFromFile(std::filesystem::temp_directory_path() / "oa_savegame_absent/slot.txt", &back, &error));
    EXPECT_NE(error.find("no save file"), std::string::npos) << error;
}
