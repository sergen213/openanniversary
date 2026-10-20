// Unit tests for the startup plumbing: configuration precedence and log filtering/format. No game data involved.

#include "core/Config.hpp"
#include "core/Log.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class TempFile : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / ("oa_cfg_" + std::to_string(::getpid()) + ".ini");
    }
    void TearDown() override { std::filesystem::remove(path_); }

    void write(const std::string& text) const {
        std::ofstream out(path_);
        out << text;
    }

    std::filesystem::path path_;
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_F(TempFile, ParsesCommentsWhitespaceAndLastDefinitionWins) {
    write("# comment\n; another comment\n\n   gameDir = /tmp/install  \nvolume=0.8\n"
          "GameDir = /other\nvolume = 1.0\n");
    oa::Config config;
    std::string error;
    ASSERT_TRUE(config.loadFile(path_, &error)) << error;

    // Keys are case-insensitive, later definitions win, surrounding whitespace is stripped.
    EXPECT_EQ(config.str("gamedir"), "/other");
    EXPECT_EQ(config.str("volume"), "1.0");
    EXPECT_EQ(config.integer("volume", -1), -1);  // strict parse: a fractional value is not an int
    EXPECT_EQ(config.entries().size(), 2u);
}

TEST_F(TempFile, RejectsGarbageLine) {
    write("valid = 1\nthis is not an assignment\n");
    oa::Config config;
    std::string error;
    EXPECT_FALSE(config.loadFile(path_, &error));
    EXPECT_NE(error.find(":2:"), std::string::npos) << error;
}

TEST_F(TempFile, CommandLineOverridesFileAndDefaults) {
    write("fullscreen = false\nwidth = 1280\n");
    oa::Config config;
    config.setDefault("width", "640");
    config.setDefault("vsync", "on");
    ASSERT_TRUE(config.loadFile(path_));

    const char* argv[] = {"prog", "--fullscreen=true", "--width=1920", "positional", "--rate=60"};
    std::vector<std::string> rest;
    ASSERT_TRUE(config.applyCommandLine(5, const_cast<char**>(argv), &rest));

    EXPECT_EQ(config.flag("fullscreen", false), true);
    EXPECT_EQ(config.integer("width", 0), 1920);
    EXPECT_EQ(config.integer("rate", 0), 60);
    EXPECT_EQ(config.str("vsync"), "on");  // untouched defaults survive
    ASSERT_EQ(rest.size(), 1u);            // argv[0] is consumed, only non-option arguments remain
    EXPECT_EQ(rest[0], "positional");

    // A bare --key carries no value: it becomes the string "true", so typed getters fall back.
    const char* bare[] = {"prog", "--rate"};
    ASSERT_TRUE(config.applyCommandLine(2, const_cast<char**>(bare)));
    EXPECT_EQ(config.str("rate"), "true");
    EXPECT_EQ(config.integer("rate", -1), -1);

    // Re-applying the same option wins over the file value.
    const char* again[] = {"prog", "--width=3840"};
    ASSERT_TRUE(config.applyCommandLine(2, const_cast<char**>(again)));
    EXPECT_EQ(config.integer("width", 0), 3840);
}

TEST(ConfigFlags, TruthinessTableAndFallback) {
    oa::Config config;
    for (const char* truthy : {"1", "true", "TRUE", "yes", "on"}) {
        config.setDefault(std::string("k") + truthy, truthy);
        EXPECT_TRUE(config.flag("K" + std::string(truthy), false)) << truthy;
    }
    for (const char* falsy : {"0", "false", "No", "off"}) {
        config.setDefault(std::string("k") + falsy, falsy);
        EXPECT_FALSE(config.flag("K" + std::string(falsy), true)) << falsy;
    }
    config.setDefault("garbage", "maybe");
    EXPECT_TRUE(config.flag("garbage", true));
    EXPECT_FALSE(config.flag("missing", false));
    EXPECT_EQ(config.integer("garbage", -7), -7);
}

TEST(Logging, FiltersByLevelAndWritesSubsystemLine) {
    const auto path = std::filesystem::temp_directory_path() / ("oa_log_" + std::to_string(::getpid()) + ".log");
    std::FILE* sink = std::fopen(path.string().c_str(), "w");
    ASSERT_NE(sink, nullptr);
    oa::initLogging(oa::LogLevel::Warn, sink);
    EXPECT_EQ(oa::logLevel(), oa::LogLevel::Warn);

    OA_LOG(oa::LogLevel::Info, "cdc", "{} records in {} parts", 4304, 27);   // suppressed
    OA_LOG(oa::LogLevel::Error, "vk", "device lost");                        // written
    std::fflush(sink);
    const std::string text = readFile(path);
    std::fclose(sink);
    std::filesystem::remove(path);

    EXPECT_EQ(text.find("cdc"), std::string::npos) << text;
    EXPECT_NE(text.find("error [vk] device lost"), std::string::npos) << text;
    EXPECT_EQ(text.find('\n'), text.size() - 1) << "exactly one line expected: " << text;

    oa::initLogging(oa::LogLevel::Info, nullptr);  // back to stderr for the rest of the process
}
