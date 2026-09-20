// platform_win32.cpp -- load-time proof-of-life and path resolution.
//
// WHY THIS FILE EXISTS
//
// The whole investigation has been stuck on an ambiguity the old code could
// not resolve: "no log file" was being read as "the DLL never loaded", but
// the old logger opened a RELATIVE path ("smaa_layer.log") against the
// process working directory and never checked whether the open succeeded.
// So "no log file" actually meant one of two completely different things:
//
//   (a) the Vulkan loader never LoadLibrary'd us  -> a discovery problem
//   (b) we loaded fine but the CWD wasn't writable/wasn't where you looked
//       -> a logging problem, and the layer may well have been running
//
// Those need opposite fixes, so first we make the signal unambiguous.
// breadcrumb() below writes to an ABSOLUTE path under %LOCALAPPDATA% using
// nothing but kernel32 calls -- no CRT, no iostreams, no locale, no heap.
// It is called from DllMain(DLL_PROCESS_ATTACH) and again from the Logger
// constructor. If vkbchoom_load.log does not appear, the DLL is genuinely
// not in the process and no amount of render-side fixing will help.
//
// Raw Win32 rather than the CRT is deliberate: DllMain runs under the
// loader lock, and with a static CRT the C++ static initialisers have
// already run by then -- if one of them had thrown or crashed we would
// never reach DllMain via the normal logging path. CreateFile/WriteFile
// on kernel32 (always mapped, always initialised) is safe here.

#include "platform_win32.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

namespace vkbChoom
{
    namespace
    {
        // Address anchor: taking the address of a function defined in this
        // TU gives us a pointer that is guaranteed to live inside
        // smaa_layer.dll, which is how we find our own module without
        // needing the module name to be hardcoded.
        void moduleAnchor() {}

        std::string envVarA(const char* name)
        {
            char  buf[32768];
            DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
            if (n == 0 || n >= sizeof(buf))
                return std::string();
            return std::string(buf, n);
        }

        std::string dirOf(const std::string& fullPath)
        {
            size_t slash = fullPath.find_last_of("\\/");
            if (slash == std::string::npos)
                return std::string();
            return fullPath.substr(0, slash + 1); // keep trailing separator
        }

        std::string timestampA()
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char buf[64];
            // dd/mm/yy hh:mm:ss.mmm
            std::snprintf(buf,
                          sizeof(buf),
                          "%02u/%02u/%02u %02u:%02u:%02u.%03u",
                          st.wDay,
                          st.wMonth,
                          static_cast<unsigned>(st.wYear % 100),
                          st.wHour,
                          st.wMinute,
                          st.wSecond,
                          st.wMilliseconds);
            return std::string(buf);
        }
    } // namespace

    std::string moduleDir()
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&moduleAnchor),
                                &self))
        {
            return std::string();
        }

        char  buf[MAX_PATH * 4];
        DWORD n = GetModuleFileNameA(self, buf, static_cast<DWORD>(sizeof(buf)));
        if (n == 0 || n >= sizeof(buf))
            return std::string();

        return dirOf(std::string(buf, n));
    }

    std::string modulePath()
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&moduleAnchor),
                                &self))
        {
            return std::string();
        }
        char  buf[MAX_PATH * 4];
        DWORD n = GetModuleFileNameA(self, buf, static_cast<DWORD>(sizeof(buf)));
        if (n == 0 || n >= sizeof(buf))
            return std::string();
        return std::string(buf, n);
    }

    std::string exeDir()
    {
        char  buf[MAX_PATH * 4];
        DWORD n = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
        if (n == 0 || n >= sizeof(buf))
            return std::string();
        return dirOf(std::string(buf, n));
    }

    std::string exeName()
    {
        char  buf[MAX_PATH * 4];
        DWORD n = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
        if (n == 0 || n >= sizeof(buf))
            return std::string("<unknown>");
        std::string full(buf, n);
        size_t      slash = full.find_last_of("\\/");
        return slash == std::string::npos ? full : full.substr(slash + 1);
    }

    std::string currentDir()
    {
        char  buf[MAX_PATH * 4];
        DWORD n = GetCurrentDirectoryA(static_cast<DWORD>(sizeof(buf)), buf);
        if (n == 0 || n >= sizeof(buf))
            return std::string();
        std::string s(buf, n);
        if (!s.empty() && s.back() != '\\')
            s += '\\';
        return s;
    }

    std::string fallbackLogDir()
    {
        // %LOCALAPPDATA%\vkbchoom\ is writable by the game process under any
        // integrity level Steam will realistically launch it at, and is
        // outside Program Files so it never hits virtualisation/UAC.
        std::string base = envVarA("LOCALAPPDATA");
        if (base.empty())
            base = envVarA("TEMP");
        if (base.empty())
            base = envVarA("TMP");
        if (base.empty())
            return std::string("C:\\Windows\\Temp\\");

        if (base.back() != '\\')
            base += '\\';
        base += "vkbchoom\\";
        CreateDirectoryA(base.c_str(), nullptr); // ignore "already exists"
        return base;
    }

    // -----------------------------------------------------------------
    // The process gate. See the long comment in platform_win32.hpp.
    // -----------------------------------------------------------------

    namespace
    {
        bool equalsNoCaseA(const std::string& a, const std::string& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); i++)
            {
                unsigned char ca = static_cast<unsigned char>(a[i]);
                unsigned char cb = static_cast<unsigned char>(b[i]);
                if (ca >= 'A' && ca <= 'Z')
                    ca = static_cast<unsigned char>(ca - 'A' + 'a');
                if (cb >= 'A' && cb <= 'Z')
                    cb = static_cast<unsigned char>(cb - 'A' + 'a');
                if (ca != cb)
                    return false;
            }
            return true;
        }

        std::string trimA(const std::string& s)
        {
            size_t b = s.find_first_not_of(" \t\r\n\"");
            if (b == std::string::npos)
                return std::string();
            size_t e = s.find_last_not_of(" \t\r\n\"");
            return s.substr(b, e - b + 1);
        }

        // Raw-Win32 read of a small text file. No ifstream: this runs before
        // (and independently of) any CRT/iostream state we can rely on, and
        // may be called from DllMain.
        std::string slurpSmallFileA(const std::string& path)
        {
            HANDLE h = CreateFileA(path.c_str(),
                                   GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
            if (h == INVALID_HANDLE_VALUE)
                return std::string();

            LARGE_INTEGER size{};
            if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 20))
            {
                CloseHandle(h);
                return std::string();
            }

            std::string buf(static_cast<size_t>(size.QuadPart), '\0');
            DWORD       read = 0;
            BOOL        good = ReadFile(h, &buf[0], static_cast<DWORD>(buf.size()), &read, nullptr);
            CloseHandle(h);

            if (!good)
                return std::string();
            buf.resize(read);
            return buf;
        }

        // Pulls "targetExecutable = foo.exe" out of a conf file's text.
        // Comment-aware in the same way Config::readConfigLine is: '#' ends
        // the line. Returns "" if the key is absent.
        std::string targetFromConfText(const std::string& text)
        {
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t      eol  = text.find_first_of("\r\n", pos);
                std::string line = text.substr(pos, (eol == std::string::npos ? text.size() : eol) - pos);
                pos              = (eol == std::string::npos) ? text.size() : eol + 1;

                size_t hash = line.find('#');
                if (hash != std::string::npos)
                    line = line.substr(0, hash);

                size_t eq = line.find('=');
                if (eq == std::string::npos)
                    continue;

                if (!equalsNoCaseA(trimA(line.substr(0, eq)), "targetExecutable"))
                    continue;

                std::string value = trimA(line.substr(eq + 1));
                if (!value.empty())
                    return value;
            }
            return std::string();
        }

        // -1 = not yet decided, 0 = inert here, 1 = this is the target.
        // Plain int with a constant initialiser on purpose: a function-local
        // static with a non-constant initialiser compiles to a guarded,
        // thread-safe init, and taking that guard under the loader lock in
        // DllMain is a deadlock risk. Worst case here is that two threads
        // both compute the same deterministic answer and write the same
        // value, which is harmless.
        int g_gateState  = -1;
        int g_forceLog   = -1;
    } // namespace

    std::string targetExecutableName()
    {
        std::string fromEnv = envVarA("VKBCHOOM_TARGET_EXE");
        if (!fromEnv.empty())
            return fromEnv;

        if (std::string dir = moduleDir(); !dir.empty())
        {
            std::string t = targetFromConfText(slurpSmallFileA(dir + "vkbchoom.conf"));
            if (!t.empty())
                return t;
        }

        if (std::string dir = exeDir(); !dir.empty())
        {
            std::string t = targetFromConfText(slurpSmallFileA(dir + "vkbchoom.conf"));
            if (!t.empty())
                return t;
        }

        return std::string("mgsvtpp.exe");
    }

    bool layerShouldRunHere()
    {
        if (g_gateState >= 0)
            return g_gateState == 1;

        const std::string self    = exeName();
        const std::string targets = targetExecutableName();

        bool   match = false;
        size_t pos   = 0;
        while (pos <= targets.size())
        {
            size_t      sep  = targets.find(';', pos);
            std::string one  = trimA(targets.substr(pos, (sep == std::string::npos ? targets.size() : sep) - pos));
            if (!one.empty() && equalsNoCaseA(self, one))
            {
                match = true;
                break;
            }
            if (sep == std::string::npos)
                break;
            pos = sep + 1;
        }

        g_gateState = match ? 1 : 0;
        return match;
    }

    void breadcrumb(const std::string& message)
    {
        // THE GATE, applied to our noisiest side effect. Without this the
        // layer creates %LOCALAPPDATA%\vkbchoom\vkbchoom_load.log and spams
        // OutputDebugString inside every Vulkan process on the machine --
        // which is how vkbchoom's own diagnostics ended up reporting
        // citron.exe and pcsx2-qt.exe as "loaded". A layer that is supposed
        // to be inert outside MGSV must not leave traces outside MGSV.
        if (!layerShouldRunHere())
        {
            if (g_forceLog < 0)
                g_forceLog = (envVarA("VKBCHOOM_FORCE_LOG") == "1") ? 1 : 0;
            if (g_forceLog != 1)
                return;
        }

        // Always mirror to the debugger channel. This is the one output that
        // cannot fail for filesystem reasons -- Sysinternals DebugView (run
        // as admin, "Capture Global Win32" enabled) will show it even if
        // every path on the machine is read-only.
        OutputDebugStringA(("[vkbchoom] " + message + "\n").c_str());

        std::string path = fallbackLogDir() + "vkbchoom_load.log";

        HANDLE h = CreateFileA(path.c_str(),
                               FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr,
                               OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return;

        std::string line = timestampA() + "  pid=" + std::to_string(static_cast<unsigned long>(GetCurrentProcessId())) + "  "
                           + exeName() + "  " + message + "\r\n";

        DWORD written = 0;
        WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        CloseHandle(h);
    }

} // namespace vkbChoom

// Keep DllMain outside the namespace and with C linkage expectations intact.
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            // Deliberately the smallest possible amount of work. Anything
            // heavier here risks a loader-lock deadlock; all real init still
            // happens lazily in vkbChoom_GetInstanceProcAddr.
            // breadcrumb() self-gates: in a non-target process this whole
            // call is a no-op, so attaching to someone else's emulator
            // leaves no file, no log line and no debugger output.
            vkbChoom::breadcrumb(std::string("DLL_PROCESS_ATTACH  gate=")
                                 + (vkbChoom::layerShouldRunHere() ? "ACTIVE" : "inert")
                                 + "  target=" + vkbChoom::targetExecutableName()
                                 + "  dll=" + vkbChoom::modulePath()
                                 + "  exedir=" + vkbChoom::exeDir()
                                 + "  cwd=" + vkbChoom::currentDir());
            break;

        case DLL_PROCESS_DETACH:
            vkbChoom::breadcrumb("DLL_PROCESS_DETACH");
            break;

        default: break;
    }
    return TRUE;
}
