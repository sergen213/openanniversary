#include "core/Log.hpp"

#include <chrono>
#include <ctime>
#include <mutex>

namespace oa {
namespace {

std::mutex logMutex;  // ponytail: one global lock, per-subsystem buffers if logging ever dominates a frame
std::FILE* logSink = stderr;
LogLevel minimumLevel = LogLevel::Info;

}  // namespace

const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
    }
    return "?????";
}

void initLogging(LogLevel minimum, std::FILE* sink) {
    std::lock_guard guard(logMutex);
    minimumLevel = minimum;
    logSink = (sink != nullptr) ? sink : stderr;
}

LogLevel logLevel() {
    std::lock_guard guard(logMutex);
    return minimumLevel;
}

void logMsg(LogLevel level, std::string_view subsystem, std::string_view message) {
    std::lock_guard guard(logMutex);
    if (static_cast<int>(level) < static_cast<int>(minimumLevel)) return;

    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const int millis = static_cast<int>(
        (std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000 + 1000) % 1000);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char stamp[16];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &utc);

    std::fprintf(logSink, "%s.%03d %-5s [%.*s] %.*s\n", stamp, millis, logLevelName(level),
                 static_cast<int>(subsystem.size()), subsystem.data(), static_cast<int>(message.size()),
                 message.data());
    std::fflush(logSink);
}

}  // namespace oa
