#include "log.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace hsvr
{
    static std::mutex g_log_mutex;
    static FILE* g_log_file = nullptr;
    static LARGE_INTEGER g_qpc_freq{};
    static LARGE_INTEGER g_qpc_start{};

    static void anchor_function()
    {
    }

    const std::wstring& mod_dir()
    {
        static std::wstring dir = [] {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&anchor_function),
                               &self);
            wchar_t path[MAX_PATH * 2]{};
            GetModuleFileNameW(self, path, MAX_PATH * 2 - 1);
            std::wstring p(path); // ...\Mods\HalfSwordVR\dlls\main.dll
            size_t cut = p.find_last_of(L"\\/");
            if (cut != std::wstring::npos) p.resize(cut); // ...\dlls
            cut = p.find_last_of(L"\\/");
            if (cut != std::wstring::npos) p.resize(cut + 1); // ...\HalfSwordVR\ (keeps the slash)
            return p;
        }();
        return dir;
    }

    double now_seconds()
    {
        if (g_qpc_freq.QuadPart == 0)
        {
            QueryPerformanceFrequency(&g_qpc_freq);
            QueryPerformanceCounter(&g_qpc_start);
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return double(now.QuadPart - g_qpc_start.QuadPart) / double(g_qpc_freq.QuadPart);
    }

    void log_open()
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (g_log_file) return;
        now_seconds();
        std::wstring path = mod_dir() + L"HalfSwordVR.log";
        // Keep the previous run's log, so a quick restart does not lose a test.
        MoveFileExW(path.c_str(), (mod_dir() + L"HalfSwordVR.previous.log").c_str(), MOVEFILE_REPLACE_EXISTING);
        g_log_file = _wfopen(path.c_str(), L"w");
    }

    void logf(const char* fmt, ...)
    {
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        double t = now_seconds();
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (!g_log_file) return;
        fprintf(g_log_file, "[%9.3f] [t%05lu] %s\n", t, (unsigned long)GetCurrentThreadId(), buf);
        fflush(g_log_file);
    }

    bool log_throttle(const char* key, double seconds)
    {
        static std::map<std::string, double> last;
        double t = now_seconds();
        std::lock_guard<std::mutex> lock(g_log_mutex);
        auto it = last.find(key);
        if (it != last.end() && t - it->second < seconds) return false;
        last[key] = t;
        return true;
    }
} // namespace hsvr
