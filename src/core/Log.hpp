#pragma once
// Process-wide logging. One line per record, thread-safe, no dependencies beyond the standard library.

#include <cstdio>
#include <format>
#include <string_view>

namespace oa {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error };

// sink defaults to stderr; messages below `minimum` are dropped before formatting cost is paid elsewhere.
void initLogging(LogLevel minimum, std::FILE* sink = nullptr);
LogLevel logLevel();
const char* logLevelName(LogLevel level);

void logMsg(LogLevel level, std::string_view subsystem, std::string_view message);

}  // namespace oa

// Usage: OA_LOG(oa::LogLevel::Info, "cdc", "{} records in {} parts", count, parts);
#define OA_LOG(level, subsystem, ...) ::oa::logMsg((level), (subsystem), std::format(__VA_ARGS__))
