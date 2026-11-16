#include "xr_loader.hpp"

#include "log.hpp"

#include <openxr/openxr_loader_negotiation.h>

#include <cstdio>
#include <string>
#include <vector>

namespace hsvr
{
    static std::wstring read_active_runtime_manifest()
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        {
            return {};
        }
        wchar_t value[1024]{};
        DWORD size = sizeof(value) - sizeof(wchar_t);
        DWORD type = 0;
        LSTATUS status = RegQueryValueExW(key, L"ActiveRuntime", nullptr, &type, reinterpret_cast<LPBYTE>(value), &size);
        RegCloseKey(key);
        if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
        return value;
    }

    // Pulls the string value of "library_path" out of the runtime manifest (a small JSON file).
    static std::string manifest_library_path(const std::wstring& manifest)
    {
        FILE* f = _wfopen(manifest.c_str(), L"rb");
        if (!f) return {};
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
        size_t key = text.find("\"library_path\"");
        if (key == std::string::npos) return {};
        size_t colon = text.find(':', key);
        if (colon == std::string::npos) return {};
        size_t open = text.find('"', colon);
        if (open == std::string::npos) return {};
        std::string out;
        for (size_t i = open + 1; i < text.size(); ++i)
        {
            char c = text[i];
            if (c == '"') break;
            if (c == '\\' && i + 1 < text.size())
            {
                char e = text[++i];
                out.push_back(e == '/' ? '/' : e == '\\' ? '\\' : e);
                continue;
            }
            out.push_back(c);
        }
        return out;
    }

    static std::wstring widen(const std::string& s)
    {
        if (s.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w((size_t)len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
        return w;
    }

    static std::string narrow(const std::wstring& w)
    {
        if (w.empty()) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s((size_t)len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
        return s;
    }

    bool xr_load_runtime(XrApi& api)
    {
        std::wstring manifest = read_active_runtime_manifest();
        if (manifest.empty())
        {
            HSVR_LOG("OpenXR: no active runtime in the registry (is SteamVR set as the OpenXR runtime?)");
            return false;
        }
        std::string lib = manifest_library_path(manifest);
        if (lib.empty())
        {
            HSVR_LOG("OpenXR: could not read library_path from %s", narrow(manifest).c_str());
            return false;
        }
        std::wstring lib_path = widen(lib);
        for (auto& ch : lib_path)
        {
            if (ch == L'/') ch = L'\\';
        }
        bool absolute = lib_path.size() > 2 && (lib_path[1] == L':' || (lib_path[0] == L'\\' && lib_path[1] == L'\\'));
        if (!absolute)
        {
            std::wstring dir = manifest;
            size_t cut = dir.find_last_of(L"\\/");
            dir = cut == std::wstring::npos ? L"" : dir.substr(0, cut + 1);
            lib_path = dir + lib_path;
        }
        wchar_t full[1024]{};
        if (GetFullPathNameW(lib_path.c_str(), 1023, full, nullptr) > 0) lib_path = full;
        HSVR_LOG("OpenXR: runtime library %s", narrow(lib_path).c_str());

        HMODULE module = LoadLibraryExW(lib_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module)
        {
            HSVR_LOG("OpenXR: failed to load the runtime library (error %lu)", GetLastError());
            return false;
        }
        auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
                reinterpret_cast<void*>(GetProcAddress(module, "xrNegotiateLoaderRuntimeInterface")));
        if (!negotiate)
        {
            HSVR_LOG("OpenXR: runtime has no xrNegotiateLoaderRuntimeInterface");
            return false;
        }
        XrNegotiateLoaderInfo loader_info{};
        loader_info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
        loader_info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
        loader_info.structSize = sizeof(XrNegotiateLoaderInfo);
        loader_info.minInterfaceVersion = 1;
        loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
        loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
        loader_info.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
        XrNegotiateRuntimeRequest request{};
        request.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
        request.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
        request.structSize = sizeof(XrNegotiateRuntimeRequest);
        XrResult result = negotiate(&loader_info, &request);
        if (XR_FAILED(result) || !request.getInstanceProcAddr)
        {
            HSVR_LOG("OpenXR: runtime negotiation failed (%d)", (int)result);
            return false;
        }
        api.GetInstanceProcAddr = request.getInstanceProcAddr;
        api.GetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&api.CreateInstance));
        api.GetInstanceProcAddr(XR_NULL_HANDLE,
                                "xrEnumerateInstanceExtensionProperties",
                                reinterpret_cast<PFN_xrVoidFunction*>(&api.EnumerateInstanceExtensionProperties));
        if (!api.CreateInstance)
        {
            HSVR_LOG("OpenXR: runtime did not provide xrCreateInstance");
            return false;
        }
        return true;
    }

    bool xr_resolve_functions(XrApi& api, XrInstance instance)
    {
        bool ok = true;
#define HSVR_XR_RESOLVE(name)                                                                                              \
    if (XR_FAILED(api.GetInstanceProcAddr(instance, "xr" #name, reinterpret_cast<PFN_xrVoidFunction*>(&api.name))) || !api.name) \
    {                                                                                                                      \
        HSVR_LOG("OpenXR: missing function xr" #name);                                                                     \
        ok = false;                                                                                                        \
    }
        HSVR_XR_FUNCTIONS(HSVR_XR_RESOLVE)
#undef HSVR_XR_RESOLVE
        return ok;
    }
} // namespace hsvr
