// Two copies of a plugin's binary in one process, each with its editor open, as any two plugins built on this code are in a
// host: they share no Objective-C class and no window class, and one being unloaded leaves the other working.

#include <clap/clap.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <CoreFoundation/CoreFoundation.h>
    #include <dlfcn.h>
    #include <objc/runtime.h>
#endif

namespace
{
#if defined(_WIN32)
const char* const windowApi = CLAP_WINDOW_API_WIN32;
#else
const char* const windowApi = CLAP_WINDOW_API_COCOA;
#endif

int failures = 0;

void fail(const std::string& what)
{
    std::fprintf(stderr, "%s\n", what.c_str());
    ++failures;
}

void runLoopFor(std::chrono::milliseconds length)
{
#if defined(_WIN32)
    const auto until = std::chrono::steady_clock::now() + length;
    while (std::chrono::steady_clock::now() < until)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#else
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, std::chrono::duration<double>(length).count(), false);
#endif
}

clap_host host{};

struct Copy
{
    std::filesystem::path file;
#if defined(_WIN32)
    HMODULE library = nullptr;
    HWND parent = nullptr;
#else
    void* library = nullptr;
#endif
    const clap_plugin_entry* entry = nullptr;
    const clap_plugin* plugin = nullptr;
    const clap_plugin_gui* gui = nullptr;

    bool open(const std::filesystem::path& from, const std::string& name)
    {
        file = std::filesystem::temp_directory_path() / name;
        std::filesystem::copy_file(from, file, std::filesystem::copy_options::overwrite_existing);
#if defined(_WIN32)
        library = LoadLibraryW(file.c_str());
        if (library == nullptr)
            return false;
        entry =
            reinterpret_cast<const clap_plugin_entry*>(reinterpret_cast<void*>(GetProcAddress(library, "clap_entry")));
#else
        library = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr)
            return false;
        entry = static_cast<const clap_plugin_entry*>(dlsym(library, "clap_entry"));
#endif
        if (entry == nullptr || !entry->init(file.string().c_str()))
            return false;

        const auto* factory = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (factory == nullptr || factory->get_plugin_count(factory) == 0)
            return false;

        plugin = factory->create_plugin(factory, &host, factory->get_plugin_descriptor(factory, 0)->id);
        if (plugin == nullptr || !plugin->init(plugin))
            return false;

        gui = static_cast<const clap_plugin_gui*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
        if (gui == nullptr || !gui->create(plugin, windowApi, false))
            return false;

#if defined(_WIN32)
        parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 320, 200, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        clap_window window{};
        window.api = windowApi;
        window.win32 = parent;
        if (!gui->set_parent(plugin, &window))
            return false;
#endif
        return true;
    }

    void close()
    {
        gui->destroy(plugin);
        plugin->destroy(plugin);
        plugin = nullptr;
#if defined(_WIN32)
        DestroyWindow(parent);
#endif
        entry->deinit();
#if defined(_WIN32)
        FreeLibrary(library);
#else
        dlclose(library);
#endif
    }
};

#if defined(_WIN32)
HMODULE moduleOfClass(HWND window)
{
    return reinterpret_cast<HMODULE>(GetClassLongPtrW(window, GCLP_HMODULE));
}

BOOL CALLBACK noteMessageWindow(HWND window, LPARAM found)
{
    char title[16] = {};
    GetWindowTextA(window, title, sizeof title);
    if (std::strcmp(title, "choc") == 0)
        reinterpret_cast<std::set<HMODULE>*>(found)->insert(moduleOfClass(window));
    return TRUE;
}

void checkClasses(const Copy& one, const Copy& other)
{
    if (moduleOfClass(GetWindow(one.parent, GW_CHILD)) != one.library ||
        moduleOfClass(GetWindow(other.parent, GW_CHILD)) != other.library)
        fail("a web view's window class isn't its own plugin's");

    std::set<HMODULE> messageWindows;
    EnumThreadWindows(GetCurrentThreadId(), noteMessageWindow, reinterpret_cast<LPARAM>(&messageWindows));
    if (messageWindows.count(one.library) != 1 || messageWindows.count(other.library) != 1)
        fail("the message windows' classes aren't their own plugins'");
}
#else
std::set<std::string> classesOf(const clap_plugin_entry* entry)
{
    Dl_info info{};
    dladdr(entry, &info);
    std::set<std::string> found;
    unsigned count = 0;
    const char** names = objc_copyClassNamesForImage(info.dli_fname, &count);
    for (unsigned i = 0; i < count; ++i)
        found.insert(names[i]);
    free(names);
    return found;
}

void checkClasses(const Copy& one, const Copy& other)
{
    const auto theirs = classesOf(other.entry);
    for (const auto& name : classesOf(one.entry))
        if (theirs.contains(name))
            fail("both copies have the Objective-C class " + name);
}
#endif
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "Give it a CLAP binary\n");
        return 2;
    }

    host.clap_version = CLAP_VERSION;
    host.name = "two copies";
    host.vendor = "SymbolEngine";
    host.url = "";
    host.version = "1";
    host.get_extension = [](const clap_host*, const char*) -> const void* { return nullptr; };
    host.request_restart = [](const clap_host*) {};
    host.request_process = [](const clap_host*) {};
    host.request_callback = [](const clap_host*) {};

    const auto stem =
        "symbolengine-two-copies-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    Copy one, other;
    if (!one.open(argv[1], stem + "-one.clap") || !other.open(argv[1], stem + "-other.clap"))
    {
        std::fprintf(stderr, "A copy didn't open\n");
        return 1;
    }

    runLoopFor(std::chrono::seconds(2));
    checkClasses(one, other);

    // The other's windows and timers carry on, which on Windows calls whatever procedure their classes have.
    one.close();
    runLoopFor(std::chrono::seconds(1));
    other.close();
    runLoopFor(std::chrono::milliseconds(200));

    std::error_code ignored;
    std::filesystem::remove(one.file, ignored);
    std::filesystem::remove(other.file, ignored);
    std::printf("%s\n", failures == 0 ? "Two copies share nothing" : "Two copies share something");
    return failures == 0 ? 0 : 1;
}
