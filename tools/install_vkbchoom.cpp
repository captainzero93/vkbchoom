// install_vkbchoom.cpp
//
// The whole install-and-diagnose tool, as one exe. Copy it into the game
// folder next to smaa_layer.dll and vkbchoom.json, and double-click it.
//
//   install_vkbchoom.exe              register, verify, then check everything
//   install_vkbchoom.exe -uninstall   remove the registration
//   install_vkbchoom.exe -status      report registration state only
//   install_vkbchoom.exe -check       run all checks, change nothing
//   install_vkbchoom.exe -debug on    turn diagnostic logging on  (restart Steam)
//   install_vkbchoom.exe -debug off   turn diagnostic logging off (restart Steam)
//
// No admin rights needed -- everything is written under HKEY_CURRENT_USER.
// No PowerShell, no execution policy, no launch options, and no environment
// variables required for normal use.
//
// CHANGES FROM THE OLD VERSION
//
// 1. It no longer TOGGLES. The old build registered if unregistered and
//    unregistered if registered, so running it a second time to make sure it
//    took would silently turn the layer back off. Mid-debugging-session that
//    is a trap, not a convenience. Registering is now idempotent, with an
//    explicit -uninstall.
//
// 2. It verifies the write by reading the value back, instead of assuming
//    that RegSetValueEx returning ERROR_SUCCESS means the loader will see it.
//
// 3. It rewrites library_path in vkbchoom.json to an absolute path, removing
//    one more thing that has to be right.
//
// 4. It no longer prints the ENABLE_VKBCHOOM launch-options instructions.
//    Those went with the enable_environment block in vkbchoom.json, which has
//    been removed -- the layer now loads unconditionally once registered.
//
// 5. It runs the loader-side checks that used to need separate scripts:
//    loader settings file, competing registrations, DXVK presence, DLL
//    architecture, Mark-of-the-Web, a vulkan-1.dll shadowing the system
//    loader, and the VK_LOADER_* suppressor variables. Every one of those
//    fails silently at runtime, which is exactly why none of them ever
//    surfaced as an error.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// console output
// ---------------------------------------------------------------------------

static HANDLE g_console      = nullptr;
static WORD   g_defaultAttrs = 0x07;
static int    g_problems     = 0;

static void initConsole()
{
    g_console = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (g_console && GetConsoleScreenBufferInfo(g_console, &info))
        g_defaultAttrs = info.wAttributes;
}

static void colour(WORD attrs)
{
    if (g_console)
        SetConsoleTextAttribute(g_console, attrs);
}

static void resetColour()
{
    colour(g_defaultAttrs);
}

static void head(const std::string& text)
{
    std::cout << "\n";
    colour(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    std::cout << "== " << text << "\n";
    resetColour();
}

static void ok(const std::string& text)
{
    colour(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::cout << "  [ ok ] ";
    resetColour();
    std::cout << text << "\n";
}

static void warn(const std::string& text)
{
    colour(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::cout << "  [warn] ";
    resetColour();
    std::cout << text << "\n";
}

static void bad(const std::string& text)
{
    ++g_problems;
    colour(FOREGROUND_RED | FOREGROUND_INTENSITY);
    std::cout << "  [ !! ] ";
    resetColour();
    std::cout << text << "\n";
}

static void note(const std::string& text)
{
    colour(FOREGROUND_INTENSITY);
    std::cout << "         " << text << "\n";
    resetColour();
}

// ---------------------------------------------------------------------------
// paths and files
// ---------------------------------------------------------------------------

static const char* IMPLICIT_LAYERS  = "Software\\Khronos\\Vulkan\\ImplicitLayers";
static const char* LOADER_SETTINGS  = "Software\\Khronos\\Vulkan\\LoaderSettings";
static const char* USER_ENVIRONMENT = "Environment";
static const char* MACHINE_ENVIRONMENT =
    "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";

static std::string getExeDir()
{
    char  path[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (len == 0 || len == MAX_PATH)
    {
        std::cerr << "ERROR: could not determine this exe's own path.\n";
        std::exit(1);
    }
    std::string full(path, len);
    size_t      slash = full.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string() : full.substr(0, slash);
}

static bool fileExists(const std::string& path)
{
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string envVar(const char* name)
{
    std::vector<char> buf(32767);
    DWORD             n = GetEnvironmentVariableA(name, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size())
        return std::string();
    return std::string(buf.data(), n);
}

static std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::string();
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string fileVersionOf(const std::string& path)
{
    DWORD dummy = 0;
    DWORD size  = GetFileVersionInfoSizeA(path.c_str(), &dummy);
    if (size == 0)
        return std::string();

    std::vector<char> buf(size);
    if (!GetFileVersionInfoA(path.c_str(), 0, size, buf.data()))
        return std::string();

    VS_FIXEDFILEINFO* info = nullptr;
    UINT              len  = 0;
    if (!VerQueryValueA(buf.data(), "\\", reinterpret_cast<LPVOID*>(&info), &len) || info == nullptr)
        return std::string();

    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "."
           + std::to_string(HIWORD(info->dwFileVersionLS)) + "." + std::to_string(LOWORD(info->dwFileVersionLS));
}

// Reads the PE header's Machine field. 0x8664 = x64, 0x014c = x86.
static bool peMachine(const std::string& path, uint16_t& machine)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;

    char mz[2] = {0, 0};
    f.read(mz, 2);
    if (mz[0] != 'M' || mz[1] != 'Z')
        return false;

    f.seekg(0x3C, std::ios::beg);
    int32_t peOffset = 0;
    f.read(reinterpret_cast<char*>(&peOffset), 4);
    if (!f || peOffset <= 0)
        return false;

    f.seekg(peOffset, std::ios::beg);
    char sig[4] = {0, 0, 0, 0};
    f.read(sig, 4);
    if (sig[0] != 'P' || sig[1] != 'E')
        return false;

    f.read(reinterpret_cast<char*>(&machine), 2);
    return !f.fail();
}

// Mark-of-the-Web lives in an alternate data stream. If it opens, the file
// came from a download, and some policies will refuse to load it.
static bool hasMarkOfTheWeb(const std::string& path)
{
    std::string stream = path + ":Zone.Identifier";
    HANDLE      h      = CreateFileA(stream.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h);
    return true;
}

static bool clearMarkOfTheWeb(const std::string& path)
{
    std::string stream = path + ":Zone.Identifier";
    return DeleteFileA(stream.c_str()) != 0;
}

// ---------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------

struct RegValue
{
    std::string name;
    DWORD       type  = 0;
    DWORD       dword = 0;
};

static std::vector<RegValue> enumerateValues(HKEY root, const char* subkey)
{
    std::vector<RegValue> out;

    HKEY key;
    if (RegOpenKeyExA(root, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return out;

    for (DWORD index = 0;; ++index)
    {
        std::vector<char> name(16384);
        DWORD             nameLen = static_cast<DWORD>(name.size());
        DWORD             type    = 0;
        DWORD             data    = 0;
        DWORD             dataLen = sizeof(data);

        LSTATUS result =
            RegEnumValueA(key, index, name.data(), &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(&data), &dataLen);

        if (result == ERROR_NO_MORE_ITEMS)
            break;
        if (result != ERROR_SUCCESS && result != ERROR_MORE_DATA)
            break;

        RegValue v;
        v.name  = std::string(name.data(), nameLen);
        v.type  = type;
        v.dword = (type == REG_DWORD) ? data : 0;
        out.push_back(v);
    }

    RegCloseKey(key);
    return out;
}

static std::string readRegString(HKEY root, const char* subkey, const char* name)
{
    HKEY key;
    if (RegOpenKeyExA(root, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return std::string();

    std::vector<char> buf(32767);
    DWORD             size = static_cast<DWORD>(buf.size());
    DWORD             type = 0;

    LSTATUS result = RegQueryValueExA(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &size);
    RegCloseKey(key);

    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return std::string();

    while (size > 0 && buf[size - 1] == '\0')
        --size;
    return std::string(buf.data(), size);
}

// -1 = not present. Otherwise the DWORD: 0 means enabled, anything else means
// the loader treats the layer as disabled.
static long queryLayerState(const std::string& jsonPath)
{
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, IMPLICIT_LAYERS, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return -1;

    DWORD   value  = 0;
    DWORD   size   = sizeof(value);
    DWORD   type   = 0;
    LSTATUS result = RegQueryValueExA(key, jsonPath.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);

    if (result != ERROR_SUCCESS)
        return -1;
    if (type != REG_DWORD)
        return 1; // wrong type; the loader will not read it as enabled
    return static_cast<long>(value);
}

// ---------------------------------------------------------------------------
// manifest
// ---------------------------------------------------------------------------

// Rewrites "library_path" to an absolute, JSON-escaped path. Deliberately a
// targeted string edit rather than a parse-and-reserialise: it preserves the
// rest of the file byte for byte, so nothing can be reformatted or lost.
static bool setAbsoluteLibraryPath(const std::string& jsonPath, const std::string& dllPath, std::string& error)
{
    std::string text = readFile(jsonPath);
    if (text.empty())
    {
        error = "could not read the manifest";
        return false;
    }

    size_t keyPos = text.find("\"library_path\"");
    if (keyPos == std::string::npos)
    {
        error = "no \"library_path\" key in the manifest";
        return false;
    }

    size_t colon = text.find(':', keyPos);
    if (colon == std::string::npos)
    {
        error = "malformed library_path entry";
        return false;
    }

    size_t openQuote = text.find('"', colon);
    if (openQuote == std::string::npos)
    {
        error = "malformed library_path value";
        return false;
    }

    size_t closeQuote = openQuote + 1;
    while (closeQuote < text.size())
    {
        if (text[closeQuote] == '\\')
        {
            closeQuote += 2;
            continue;
        }
        if (text[closeQuote] == '"')
            break;
        ++closeQuote;
    }
    if (closeQuote >= text.size())
    {
        error = "unterminated library_path string";
        return false;
    }

    std::string escaped;
    for (char c : dllPath)
    {
        if (c == '\\')
            escaped += "\\\\";
        else
            escaped += c;
    }

    if (text.substr(openQuote + 1, closeQuote - openQuote - 1) == escaped)
        return true; // already absolute and correct

    text = text.substr(0, openQuote + 1) + escaped + text.substr(closeQuote);

    std::ofstream out(jsonPath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        error = "could not open the manifest for writing (read-only folder?)";
        return false;
    }
    out << text;
    out.close();

    if (!out.good())
    {
        error = "write to the manifest failed";
        return false;
    }
    return true;
}

// Pulls the library_path value back out, unescaped.
static bool getLibraryPath(const std::string& text, std::string& value)
{
    size_t keyPos = text.find("\"library_path\"");
    if (keyPos == std::string::npos)
        return false;

    size_t colon = text.find(':', keyPos);
    if (colon == std::string::npos)
        return false;

    size_t openQuote = text.find('"', colon);
    if (openQuote == std::string::npos)
        return false;

    size_t closeQuote = openQuote + 1;
    while (closeQuote < text.size())
    {
        if (text[closeQuote] == '\\')
        {
            closeQuote += 2;
            continue;
        }
        if (text[closeQuote] == '"')
            break;
        ++closeQuote;
    }
    if (closeQuote >= text.size())
        return false;

    std::string raw = text.substr(openQuote + 1, closeQuote - openQuote - 1);

    value.clear();
    for (size_t i = 0; i < raw.size(); ++i)
    {
        if (raw[i] == '\\' && i + 1 < raw.size())
        {
            value += raw[i + 1];
            ++i;
        }
        else
        {
            value += raw[i];
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// checks
// ---------------------------------------------------------------------------

static void checkLoaderSettings()
{
    head("Loader settings file");

    // Vulkan loader 1.3.234+ honours a settings file written by Vulkan
    // Configurator and by some SDK and vendor-tool installs. When one exists
    // it takes complete control of layer selection and ignores the
    // ImplicitLayers keys entirely -- silently. Precisely the sort of thing a
    // fresh driver or SDK install leaves behind, which makes it the first
    // thing to rule out after a Windows reinstall.
    bool found = false;

    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
    {
        const char* rootName = (root == HKEY_LOCAL_MACHINE) ? "HKLM" : "HKCU";
        for (const RegValue& v : enumerateValues(root, LOADER_SETTINGS))
        {
            found = true;
            bad(std::string(rootName) + " loader settings file: " + v.name);
        }
    }

    if (!found)
    {
        ok("none registered -- normal implicit layer discovery is in effect");
        return;
    }

    note("While that exists, this layer's ImplicitLayers registration is ignored");
    note("and it will never load, with no error printed anywhere.");
    note("FIX: open Vulkan Configurator and switch it to 'Vulkan Applications'");
    note("     (system-controlled) mode, or delete the registry value above.");
}

static void checkRegistrations(const std::string& jsonPath)
{
    head("Implicit layer registrations");

    struct
    {
        HKEY        root;
        const char* name;
    } hives[] = {{HKEY_CURRENT_USER, "HKCU"}, {HKEY_LOCAL_MACHINE, "HKLM"}};

    bool sawOurs = false;

    for (auto& hive : hives)
    {
        std::vector<RegValue> values = enumerateValues(hive.root, IMPLICIT_LAYERS);
        if (values.empty())
        {
            note(std::string(hive.name) + ": no entries");
            continue;
        }

        for (const RegValue& v : values)
        {
            bool        isOurs  = (v.name == jsonPath);
            bool        present = fileExists(v.name);
            bool        enabled = (v.type == REG_DWORD && v.dword == 0);
            std::string label   = std::string(hive.name) + "  " + v.name;

            if (isOurs)
                sawOurs = true;

            if (!present)
                bad(label + "   -> manifest file MISSING");
            else if (!enabled)
                bad(label + "   -> DISABLED (the value must be 0)");
            else if (isOurs)
                ok(label + "   -> enabled  (this one)");
            else
                ok(label + "   -> enabled");

            // A leftover registration pointing at an older copy of this layer
            // will load alongside, or instead of, the current one.
            if (!isOurs && v.name.find("vkbchoom") != std::string::npos)
                warn("     ^ another vkbchoom manifest, not the one next to this exe");
        }
    }

    if (!sawOurs)
        bad("this layer is not registered -- run this exe with no arguments");
}

static void checkManifest(const std::string& jsonPath, const std::string& exeDir)
{
    head("Manifest");

    if (!fileExists(jsonPath))
    {
        bad("vkbchoom.json not found next to this exe:");
        note(jsonPath);
        return;
    }

    std::string text = readFile(jsonPath);

    if (text.find("\"GLOBAL\"") != std::string::npos)
        ok("type is GLOBAL (correct for an implicit layer)");
    else
        bad("type is not GLOBAL -- implicit layers must be GLOBAL");

    if (text.find("\"enable_environment\"") != std::string::npos)
    {
        bad("enable_environment is still present in this manifest");
        note("The loader will not even LoadLibrary the layer unless that variable is");
        note("set in THE GAME'S OWN environment. It used to arrive via Steam launch");
        note("options -- and Steam keeps those in");
        note("Steam\\userdata\\<id>\\config\\localconfig.vdf, which is local to the");
        note("machine and is NOT restored by a clean Windows + Steam reinstall.");
        note("FIX: use the vkbchoom.json shipped with this build, which has no");
        note("     enable_environment block.");
    }
    else
    {
        ok("no enable_environment -- loads with no launch options needed");
    }

    if (text.find("\"disable_environment\"") != std::string::npos)
        ok("disable_environment present (required for implicit layers)");
    else
        bad("disable_environment MISSING -- the loader may reject this manifest");

    std::string libraryPath;
    if (!getLibraryPath(text, libraryPath))
    {
        bad("could not read library_path out of the manifest");
        return;
    }

    bool        absolute = libraryPath.size() > 2 && libraryPath[1] == ':';
    std::string resolved = absolute ? libraryPath : (exeDir + "\\" + libraryPath);

    if (fileExists(resolved))
        ok(std::string("library_path resolves") + (absolute ? " (absolute): " : " (relative): ") + resolved);
    else
        bad("library_path does NOT resolve: " + libraryPath + "  ->  " + resolved);
}

static void checkLayerDll(const std::string& dllPath)
{
    head("Layer DLL");

    if (!fileExists(dllPath))
    {
        bad("smaa_layer.dll not found next to this exe:");
        note(dllPath);
        note("Build it, then copy builddir\\src\\smaa_layer.dll here.");
        return;
    }

    uint16_t machine = 0;
    if (!peMachine(dllPath, machine))
        warn("could not read the PE header -- is the file complete?");
    else if (machine == 0x8664)
        ok("64-bit (x64) -- matches mgsvtpp.exe");
    else if (machine == 0x014c)
        bad("32-bit (x86) -- a 64-bit game CANNOT load this. Rebuild for x64.");
    else if (machine == 0xAA64)
        bad("ARM64 -- wrong architecture for this machine");
    else
        warn("unrecognised PE machine type");

    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExA(dllPath.c_str(), GetFileExInfoStandard, &data))
    {
        SYSTEMTIME st;
        FILETIME   local;
        FileTimeToLocalFileTime(&data.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &st);

        char buf[160];
        std::snprintf(buf,
                      sizeof(buf),
                      "built %02u/%02u/%02u %02u:%02u,  %lu bytes",
                      st.wDay,
                      st.wMonth,
                      static_cast<unsigned>(st.wYear % 100),
                      st.wHour,
                      st.wMinute,
                      static_cast<unsigned long>(data.nFileSizeLow));
        note(buf);
    }

    if (hasMarkOfTheWeb(dllPath))
    {
        warn("the DLL carries Mark-of-the-Web (it came from a download)");
        if (clearMarkOfTheWeb(dllPath))
            note("cleared it automatically");
        else
            note("could not clear it -- right-click > Properties > Unblock");
    }
    else
    {
        ok("no Mark-of-the-Web");
    }
}

static void checkDxvk(const std::string& exeDir)
{
    head("DXVK");

    // If DXVK is not actually present, MGSV runs on native D3D11, never
    // creates a Vulkan instance at all, and no Vulkan layer of any kind can
    // load. That sits one step earlier in the chain than anything else here,
    // and a game reinstall or file-verify during a Windows reinstall is
    // exactly how those DLLs go missing.
    std::string d3d11 = exeDir + "\\d3d11.dll";
    std::string dxgi  = exeDir + "\\dxgi.dll";

    if (fileExists(d3d11))
    {
        std::string version = fileVersionOf(d3d11);
        ok("d3d11.dll present" + (version.empty() ? std::string() : "  (version " + version + ")"));
    }
    else
    {
        bad("d3d11.dll NOT present in this folder");
        note("Without it the game uses native D3D11, never touches Vulkan, and no");
        note("layer can possibly load. Reinstall DXVK into the game folder.");
    }

    if (fileExists(dxgi))
    {
        ok("dxgi.dll present");
    }
    else
    {
        note("dxgi.dll not present -- often fine for DXVK 2.x, which usually only");
        note("needs d3d11.dll, but check how your DXVK build expects to be installed");
    }

    // DXVK writes its own logs next to the exe when DXVK_LOG_LEVEL is set.
    // Their presence proves DXVK loaded on the last run.
    if (fileExists(exeDir + "\\mgsvtpp_d3d11.log") || fileExists(exeDir + "\\mgsvtpp_dxgi.log"))
    {
        ok("DXVK logs from a previous run are here -- DXVK is loading");
    }
    else
    {
        note("no DXVK logs here yet; they only appear with DXVK_LOG_LEVEL set, so");
        note("use -debug on, restart Steam, and launch once to produce them");
    }
}

static void checkLoaderShadowing(const std::string& exeDir)
{
    head("Vulkan loader");

    // Windows resolves imports from the application directory before
    // System32, and vulkan-1.dll is not a KnownDLL. A copy sitting in the
    // game folder -- from an older DXVK bundle, a mod pack, or a previous
    // experiment -- means the game uses THAT loader while vulkaninfo in a
    // terminal keeps using the healthy System32 one. That difference on its
    // own accounts for "vulkaninfo loads the layer, the game does not".
    std::string shadow = exeDir + "\\vulkan-1.dll";

    if (fileExists(shadow))
    {
        bad("vulkan-1.dll is present in the game folder:");
        note(shadow);
        std::string version = fileVersionOf(shadow);
        if (!version.empty())
            note("version " + version);
        note("The game will load this loader instead of the system one.");
        note("FIX: rename it to vulkan-1.dll.bak and relaunch.");
    }
    else
    {
        ok("nothing shadowing the system loader in this folder");
    }

    char sysDir[MAX_PATH];
    UINT n = GetSystemDirectoryA(sysDir, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        std::string sysLoader = std::string(sysDir, n) + "\\vulkan-1.dll";
        if (fileExists(sysLoader))
        {
            std::string version = fileVersionOf(sysLoader);
            ok("system loader present" + (version.empty() ? std::string() : "  (version " + version + ")"));
        }
        else
        {
            bad("no vulkan-1.dll in System32 -- reinstall your GPU driver");
        }
    }
}

static void checkEnvironment()
{
    head("Environment variables");

    struct
    {
        const char* name;
        bool        fatal;
    } names[] = {
        {"VK_LOADER_LAYERS_DISABLE", true},
        {"DISABLE_VKBCHOOM", true},
        {"VK_LOADER_LAYERS_ENABLE", false},
        {"VK_LOADER_LAYERS_ALLOW", false},
        {"VK_INSTANCE_LAYERS", false},
        {"VK_LAYER_PATH", false},
        {"VK_LOADER_DEBUG", false},
        {"VKBCHOOM_LOG_LEVEL", false},
        {"VKBCHOOM_LOG_FILE", false},
        {"ENABLE_VKBCHOOM", false},
        {"DXVK_LOG_LEVEL", false},
    };

    bool anything = false;

    for (auto& entry : names)
    {
        struct
        {
            const char* scope;
            std::string value;
        } scopes[] = {
            {"process", envVar(entry.name)},
            {"user", readRegString(HKEY_CURRENT_USER, USER_ENVIRONMENT, entry.name)},
            {"machine", readRegString(HKEY_LOCAL_MACHINE, MACHINE_ENVIRONMENT, entry.name)},
        };

        for (auto& s : scopes)
        {
            if (s.value.empty())
                continue;

            anything = true;
            std::string line = std::string(entry.name) + " = \"" + s.value + "\"   (" + s.scope + ")";

            if (entry.fatal)
                bad(line + "   <- this switches the layer OFF");
            else
                warn(line);

            // A trailing space is the classic Steam launch-options mistake:
            // "set VAR=layer && ..." puts the space inside the value, and the
            // loader then rejects it as an unrecognised setting.
            if (s.value.back() == ' ')
                bad("     ^ that value ends in a space, so the loader ignores it");
        }
    }

    if (!anything)
        ok("nothing set that would suppress or alter layer loading");
}

static void showBreadcrumb()
{
    head("Last launch");

    std::string local = envVar("LOCALAPPDATA");
    if (local.empty())
    {
        warn("LOCALAPPDATA is not set, cannot find the breadcrumb log");
        return;
    }

    std::string path = local + "\\vkbchoom\\vkbchoom_load.log";
    if (!fileExists(path))
    {
        warn("no breadcrumb log yet at:");
        note(path);
        note("Expected if nothing has been launched since building. Start the game");
        note("once, then run install_vkbchoom.exe -check again.");
        return;
    }

    ok(path);

    std::ifstream            in(path);
    std::vector<std::string> lines;
    std::string              line;
    while (std::getline(in, line))
        lines.push_back(line);

    size_t start = lines.size() > 12 ? lines.size() - 12 : 0;
    for (size_t i = start; i < lines.size(); ++i)
        note(lines[i]);

    bool sawGame = false;
    for (const std::string& l : lines)
    {
        if (l.find("mgsvtpp") != std::string::npos)
            sawGame = true;
    }

    std::cout << "\n";
    if (sawGame)
    {
        ok("mgsvtpp.exe appears above -- the DLL IS loading into the game");
        note("So this is not a discovery problem. Read smaa_layer.log next to");
        note("smaa_layer.dll for what happened after that.");
    }
    else
    {
        warn("mgsvtpp.exe does not appear above -- the DLL is not reaching the game");
        note("Still a discovery problem. Fix anything flagged above, then see the");
        note("DebugView steps at the end.");
    }
}

static void printDebugViewHelp()
{
    head("If it still will not load");
    note("The Vulkan loader explains its own decisions, but on Windows it writes");
    note("them to stderr and OutputDebugString -- and mgsvtpp.exe is a windowed");
    note("process with no console. That is why every dead end so far has been");
    note("silent: the explanations were going to a channel nothing was reading.");
    note("");
    note("  1. install_vkbchoom.exe -debug on");
    note("  2. Fully exit Steam (tray icon > Exit), then start it again");
    note("  3. Run Sysinternals DbgView.exe AS ADMINISTRATOR");
    note("  4. Capture menu: tick 'Capture Win32' and 'Capture Global Win32'");
    note("  5. Launch MGSV, then Ctrl+F for: vkbchoom");
    note("");
    note("The loader prints a line for every manifest it finds, every one it");
    note("skips, and the reason why.");
}

// ---------------------------------------------------------------------------
// debug environment variables
// ---------------------------------------------------------------------------

static bool writeUserEnv(const char* name, const char* value)
{
    HKEY    key;
    LSTATUS open = RegCreateKeyExA(
        HKEY_CURRENT_USER, USER_ENVIRONMENT, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (open != ERROR_SUCCESS)
        return false;

    LSTATUS result;
    if (value == nullptr)
    {
        result = RegDeleteValueA(key, name);
        if (result == ERROR_FILE_NOT_FOUND)
            result = ERROR_SUCCESS;
    }
    else
    {
        result = RegSetValueExA(
            key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value), static_cast<DWORD>(std::strlen(value) + 1));
    }

    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

static int setDebugMode(bool on)
{
    // User-scope variables rather than Steam launch options. Steam on Windows
    // has no VAR=value syntax -- that is a Linux/Proton thing. The Windows
    // equivalent is cmd /c "set X=y&& %command%", which has two sharp edges:
    // a space before the && ends up inside the value, and Steam substitutes
    // %command% already quoted, so nested quotes can silently drop everything
    // after the first one. User-scope variables have neither problem: Steam
    // inherits them at startup and every game it launches inherits from Steam.
    struct
    {
        const char* name;
        const char* value;
    } vars[] = {
        {"VK_LOADER_DEBUG", "layer"},
        {"VKBCHOOM_LOG_LEVEL", "trace"},
        {"DXVK_LOG_LEVEL", "info"},
    };

    bool allOk = true;
    for (auto& v : vars)
        allOk = writeUserEnv(v.name, on ? v.value : nullptr) && allOk;

    DWORD_PTR unused = 0;
    SendMessageTimeoutA(
        HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>("Environment"), SMTO_ABORTIFHUNG, 5000, &unused);

    head(on ? "Debug logging ON" : "Debug logging OFF");

    if (!allOk)
    {
        bad("could not write one or more values to HKCU\\Environment");
        return 1;
    }

    if (on)
    {
        ok("VK_LOADER_DEBUG=layer       loader's own layer-discovery commentary");
        ok("VKBCHOOM_LOG_LEVEL=trace    full layer verbosity");
        ok("DXVK_LOG_LEVEL=info         DXVK logs next to the game exe");
    }
    else
    {
        ok("all three cleared");
    }

    std::cout << "\n";
    note("Now fully exit Steam -- tray icon > Exit, not just closing the window --");
    note("and start it again. Steam only reads the environment when it starts, so a");
    note("running Steam will not pass these on to the game.");
    return 0;
}

// ---------------------------------------------------------------------------
// register / unregister
// ---------------------------------------------------------------------------

static void reportState(const std::string& jsonPath)
{
    long state = queryLayerState(jsonPath);

    if (state < 0)
        bad("NOT registered");
    else if (state == 0)
        ok("registered and ENABLED");
    else
        bad("registered but DISABLED (value = " + std::to_string(state) + ", must be 0)");

    note("manifest: " + jsonPath);
}

static int doUnregister(const std::string& jsonPath)
{
    head("Unregistering");

    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, IMPLICIT_LAYERS, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
    {
        warn("no ImplicitLayers key exists, nothing to remove");
        return 0;
    }

    LSTATUS result = RegDeleteValueA(key, jsonPath.c_str());
    RegCloseKey(key);

    if (result == ERROR_SUCCESS)
        ok("unregistered -- SMAA is now off");
    else
        warn("was not registered, nothing to remove");

    reportState(jsonPath);
    return 0;
}

static int doRegister(const std::string& jsonPath, const std::string& dllPath)
{
    head("Registering");

    if (!fileExists(jsonPath))
    {
        bad("vkbchoom.json not found next to this exe:");
        note(jsonPath);
        note("Copy install_vkbchoom.exe, vkbchoom.json, vkbchoom.conf and");
        note("smaa_layer.dll into the same folder (your MGS_TPP install folder)");
        note("and run it from there.");
        return 1;
    }

    if (!fileExists(dllPath))
    {
        // A hard error, not a warning. Registering a manifest whose DLL is
        // absent gives you a layer the loader silently skips -- the exact
        // "no error, no log, nothing happens" state this is meant to escape.
        bad("smaa_layer.dll not found next to this exe:");
        note(dllPath);
        note("Build it first, then copy builddir\\src\\smaa_layer.dll here.");
        return 1;
    }

    std::string error;
    if (setAbsoluteLibraryPath(jsonPath, dllPath, error))
    {
        ok("library_path points at the absolute path of the DLL");
    }
    else
    {
        warn("could not rewrite library_path: " + error);
        note("The relative path should still work -- this just removes a variable.");
    }

    HKEY    key;
    LSTATUS open = RegCreateKeyExA(
        HKEY_CURRENT_USER, IMPLICIT_LAYERS, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (open != ERROR_SUCCESS)
    {
        bad("could not open or create the registry key (error " + std::to_string(open) + ")");
        return 1;
    }

    DWORD   zero  = 0;
    LSTATUS write = RegSetValueExA(key, jsonPath.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
    RegCloseKey(key);

    if (write != ERROR_SUCCESS)
    {
        bad("could not write the registry value (error " + std::to_string(write) + ")");
        return 1;
    }

    if (queryLayerState(jsonPath) != 0)
    {
        bad("wrote the value, but the read-back did not return 0");
        return 1;
    }

    ok("registered, and verified by reading it back");
    return 0;
}

// ---------------------------------------------------------------------------

static void runAllChecks(const std::string& exeDir, const std::string& jsonPath, const std::string& dllPath)
{
    checkLoaderSettings();
    checkRegistrations(jsonPath);
    checkManifest(jsonPath, exeDir);
    checkLayerDll(dllPath);
    checkDxvk(exeDir);
    checkLoaderShadowing(exeDir);
    checkEnvironment();
    showBreadcrumb();
}

static void printSummary()
{
    std::cout << "\n";
    if (g_problems == 0)
    {
        colour(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        std::cout << "  Nothing flagged.\n";
        resetColour();
        std::cout << "  Launch MGSV from Steam normally -- no launch options and no\n";
        std::cout << "  environment variables are needed. \n";
    }
    else
    {
        colour(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cout << "  " << g_problems << " problem" << (g_problems == 1 ? "" : "s") << " flagged above.\n";
        resetColour();
        std::cout << "  Work through the [ !! ] lines from the top down.\n";
        printDebugViewHelp();
    }
}

static void printUsage()
{
    std::cout << "\n";
    std::cout << "  install_vkbchoom.exe                register, verify, and check everything\n";
    std::cout << "  install_vkbchoom.exe -uninstall     remove the registration\n";
    std::cout << "  install_vkbchoom.exe -status        report registration state only\n";
    std::cout << "  install_vkbchoom.exe -check         run all checks, change nothing\n";
    std::cout << "  install_vkbchoom.exe -debug on|off  diagnostic logging (restart Steam after)\n";
    std::cout << "\n";
}

static int run(int argc, char** argv)
{
    std::string exeDir   = getExeDir();
    std::string jsonPath = exeDir + "\\vkbchoom.json";
    std::string dllPath  = exeDir + "\\smaa_layer.dll";

    std::string arg  = (argc > 1) ? std::string(argv[1]) : std::string();
    std::string arg2 = (argc > 2) ? std::string(argv[2]) : std::string();

    // Accept -x, /x and --x alike.
    while (!arg.empty() && (arg[0] == '-' || arg[0] == '/'))
        arg.erase(0, 1);

    std::cout << "\n";
    colour(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    std::cout << "  vkbchoom -- SMAA layer for MGSV via DXVK\n";
    resetColour();
    note("running from: " + exeDir);

    if (arg == "help" || arg == "?" || arg == "h")
    {
        printUsage();
        return 0;
    }

    if (arg == "debug")
    {
        if (arg2 == "on")
            return setDebugMode(true);
        if (arg2 == "off")
            return setDebugMode(false);
        bad("say either  -debug on  or  -debug off");
        return 1;
    }

    if (arg == "status")
    {
        head("Status");
        reportState(jsonPath);
        note(std::string("dll:      ") + dllPath + (fileExists(dllPath) ? "   (present)" : "   (MISSING)"));
        return 0;
    }

    if (arg == "check" || arg == "diagnose")
    {
        runAllChecks(exeDir, jsonPath, dllPath);
        printSummary();
        return g_problems == 0 ? 0 : 1;
    }

    if (arg == "uninstall")
    {
        doUnregister(jsonPath);
        return 0;
    }

    if (!arg.empty())
    {
        bad("unrecognised argument: " + arg);
        printUsage();
        return 1;
    }

    // Default, and the only thing most people need: register, then check.
    int result = doRegister(jsonPath, dllPath);
    if (result != 0)
    {
        printSummary();
        return result;
    }

    runAllChecks(exeDir, jsonPath, dllPath);
    printSummary();
    return 0;
}

int main(int argc, char** argv)
{
    initConsole();
    int result = run(argc, argv);
    std::cout << "\nPress Enter to close this window...";
    std::cin.get();
    resetColour();
    return result;
}
