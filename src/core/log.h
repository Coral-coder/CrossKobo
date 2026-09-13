#pragma once
#include <string>

namespace ck {

enum class LogLevel { Debug = 0, Info, Warn, Error };

// Logging goes to stderr and, when a path is set, to a rotating file. The
// launcher redirects stderr into /usr/local/crosskobo/crosskobo.log on the
// device, so both sinks normally point at the same place; the explicit file
// sink exists so a crash inside the launcher still leaves a trail.
void log_init(const std::string& file_path, LogLevel level);
void log_set_level(LogLevel level);
LogLevel log_level();
void log_write(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define CK_LOGD(...) ::ck::log_write(::ck::LogLevel::Debug, __VA_ARGS__)
#define CK_LOGI(...) ::ck::log_write(::ck::LogLevel::Info, __VA_ARGS__)
#define CK_LOGW(...) ::ck::log_write(::ck::LogLevel::Warn, __VA_ARGS__)
#define CK_LOGE(...) ::ck::log_write(::ck::LogLevel::Error, __VA_ARGS__)

}  // namespace ck
