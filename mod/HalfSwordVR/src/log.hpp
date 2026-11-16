// Thread-safe log file for the VR mod: <game>\...\ue4ss\Mods\HalfSwordVR\HalfSwordVR.log
#pragma once
#include <string>

namespace hsvr
{
    // Folder that holds the mod (the parent of "dlls"), with a trailing backslash.
    const std::wstring& mod_dir();
    void log_open();
    void logf(const char* fmt, ...);
    // Logs at most once every `seconds` per call site key.
    bool log_throttle(const char* key, double seconds);
    double now_seconds();
} // namespace hsvr

#define HSVR_LOG(...) ::hsvr::logf(__VA_ARGS__)
#define HSVR_LOG_EVERY(seconds, ...)                          \
    do                                                        \
    {                                                         \
        if (::hsvr::log_throttle(__FILE__ ":" HSVR_STR(__LINE__), (seconds))) ::hsvr::logf(__VA_ARGS__); \
    } while (0)
#define HSVR_STR2(x) #x
#define HSVR_STR(x) HSVR_STR2(x)
