#include "ProcessChannel.h"

#include "symbolengine/core/Paths.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

// After windows.h, which it needs; version 2 takes the memory counters from kernel32, with no library of its own.
#ifndef PSAPI_VERSION
    #define PSAPI_VERSION 2
#endif
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

namespace symbolengine
{

namespace
{

std::error_code lastError()
{
    return {static_cast<int>(GetLastError()), std::system_category()};
}

std::wstring wide(std::string_view text)
{
    return paths::fromUtf8(text).wstring();
}

HANDLE handleFrom(void* handle)
{
    return static_cast<HANDLE>(handle);
}

void closeHandle(void*& handle)
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
        CloseHandle(handleFrom(handle));

    handle = nullptr;
}

// One argument as the C runtime and CommandLineToArgvW read it back: quoted when it holds a space, a tab, a quote or
// nothing, with the backslashes before a quote, and at the end of a quoted argument, doubled.
void appendArgument(std::wstring& line, const std::wstring& argument)
{
    if (!line.empty())
        line += L' ';

    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    {
        line += argument;
        return;
    }

    line += L'"';

    for (auto at = argument.begin();; ++at)
    {
        std::size_t backslashes = 0;

        while (at != argument.end() && *at == L'\\')
        {
            ++at;
            ++backslashes;
        }

        if (at == argument.end())
        {
            line.append(backslashes * 2, L'\\');
            break;
        }

        if (*at == L'"')
            line.append(backslashes * 2 + 1, L'\\');
        else
            line.append(backslashes, L'\\');

        line += *at;
    }

    line += L'"';
}

bool sameName(std::wstring_view a, std::wstring_view b)
{
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

std::wstring_view nameOf(std::wstring_view entry)
{
    // Variables naming a drive's folder start with "=", which is part of their name.
    const auto equals = entry.find(L'=', 1);
    return entry.substr(0, equals);
}

// The job every child is in: the system ends them all when this program ends, however it ends, and a crash in one
// never waits on the system's error dialog.
HANDLE childrenJob()
{
    static const HANDLE job = [] {
        const HANDLE made = CreateJobObjectW(nullptr, nullptr);

        if (made != nullptr)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
            SetInformationJobObject(made, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        }

        return made;
    }();

    return job;
}

// The exit code a child is ended with by kill(), to tell it from one that ended by itself.
constexpr UINT killedCode = 0x5E1F0000;

}

ProcessLink::Native::~Native()
{
    closeHandle(readHandle);
    closeHandle(writeHandle);
    closeHandle(parent);
    closeHandle(readerThread);
}

bool ProcessLink::Native::write(const char* data, std::size_t size)
{
    // Counted while writing, so shutting down closes the channel only once no write is under way.
    ++writers;
    writerThread = GetCurrentThreadId();

    while (size > 0 && !shut)
    {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1 << 20));
        DWORD written = 0;

        if (!WriteFile(handleFrom(writeHandle), data, chunk, &written, nullptr) || written == 0)
            break;

        data += written;
        size -= written;
    }

    writerThread = 0;
    --writers;
    return size == 0 && !shut;
}

bool ProcessLink::Native::read(char* data, std::size_t size)
{
    while (size > 0)
    {
        if (shut)
            return false;

        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1 << 20));
        DWORD got = 0;

        if (!ReadFile(handleFrom(readHandle), data, chunk, &got, nullptr) || got == 0)
            return false;

        data += got;
        size -= got;
    }

    return true;
}

void ProcessLink::Native::beginReading()
{
    HANDLE thread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    readerThread = thread;
    reading = true;
}

void ProcessLink::Native::endReading()
{
    reading = false;
}

void ProcessLink::Native::shutDown()
{
    if (shut.exchange(true))
        return;

    // A read waiting in the system is cancelled until the reading thread has seen it: it may have been about to read
    // when the first cancel came.
    const bool onReader = readerThread != nullptr && GetThreadId(handleFrom(readerThread)) == GetCurrentThreadId();

    for (int tries = 0; !onReader && reading && tries < 400; ++tries)
    {
        CancelSynchronousIo(handleFrom(readerThread));
        Sleep(5);
    }

    // A write waiting on a child that isn't reading is cancelled too. Closing this side's writing end is what tells the
    // other end, once no write is using it.
    for (int tries = 0; writers > 0 && tries < 400; ++tries)
    {
        Sleep(5);
    }

    if (writers == 0)
        closeHandle(writeHandle);
}

bool ProcessLink::Native::watchesParent() const
{
    return parent != nullptr;
}

bool ProcessLink::Native::waitForParent(const std::atomic<bool>& stop)
{
    while (!stop)
        if (WaitForSingleObject(handleFrom(parent), 200) != WAIT_TIMEOUT)
            return true;

    return false;
}

std::unique_ptr<ProcessLink> ProcessLink::toParent(std::error_code& error)
{
    wchar_t value[128] = {};
    const auto length = GetEnvironmentVariableW(L"SYMBOLENGINE_LINK", value, 128);
    unsigned long long handles[3] = {};

    if (length == 0 || length >= 128)
    {
        error = std::make_error_code(std::errc::no_such_file_or_directory);
        return nullptr;
    }

    DWORD flags = 0;

    if (swscanf_s(value, L"%llu;%llu;%llu", &handles[0], &handles[1], &handles[2]) != 3 ||
        std::any_of(std::begin(handles), std::end(handles), [&flags](unsigned long long handle) {
            return handle == 0 || !GetHandleInformation(reinterpret_cast<HANDLE>(handle), &flags);
        }))
    {
        error = std::make_error_code(std::errc::invalid_argument);
        return nullptr;
    }

    // Kept from any program this one starts, whose holding the channel open would hide this one's end.
    _wputenv_s(L"SYMBOLENGINE_LINK", L"");
    auto native = std::make_unique<Native>();
    native->readHandle = reinterpret_cast<HANDLE>(handles[0]);
    native->writeHandle = reinterpret_cast<HANDLE>(handles[1]);
    native->parent = reinterpret_cast<HANDLE>(handles[2]);

    for (const auto handle : handles)
        SetHandleInformation(reinterpret_cast<HANDLE>(handle), HANDLE_FLAG_INHERIT, 0);

    error.clear();
    return std::make_unique<ProcessLink>(std::move(native));
}

struct ChildProcess::Native
{
    HANDLE process = nullptr;
    DWORD id = 0;
    bool killing = false;
};

ChildProcess::ChildProcess(std::unique_ptr<Native> made) : native(std::move(made)) {}

ChildProcess::~ChildProcess()
{
    if (theLink != nullptr)
        theLink->close();

    if (!outlives)
        kill();

    CloseHandle(native->process);
}

std::unique_ptr<ChildProcess> ChildProcess::start(const std::filesystem::path& program, const Options& options,
                                                  std::error_code& error)
{

    error.clear();

    // Every handle the child is to have: standard input, output and error, and its ends of the link, inheritable
    // only by it, through the list given with it.
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    void* input = nullptr;
    void* output = nullptr;
    void* childRead = nullptr;
    void* childWrite = nullptr;
    void* parentRead = nullptr;
    void* parentWrite = nullptr;
    void* parentForChild = nullptr;

    const auto cleanUp = [&] {
        for (auto* handle : {&input, &output, &childRead, &childWrite, &parentRead, &parentWrite, &parentForChild})
            closeHandle(*handle);
    };

    input =
        CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);

    if (!options.outputFile.empty())
    {
        if (options.outputFile.has_parent_path())
            std::filesystem::create_directories(options.outputFile.parent_path(), error);

        if (!error)
            output = CreateFileW(options.outputFile.c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inheritable, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    else
    {
        output = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0,
                             nullptr);
    }

    if (error || input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE)
    {
        if (!error)
            error = lastError();

        if (input == INVALID_HANDLE_VALUE)
            input = nullptr;

        if (output == INVALID_HANDLE_VALUE)
            output = nullptr;

        cleanUp();
        return nullptr;
    }

    std::vector<HANDLE> inherited{handleFrom(input), handleFrom(output)};

    if (options.link)
    {
        HANDLE readEnd = nullptr, writeEnd = nullptr, thisProcess = nullptr;

        // The child reads what this program writes, and this program reads what the child writes.
        const bool made = CreatePipe(&readEnd, &writeEnd, nullptr, 1 << 16);
        childRead = readEnd;
        parentWrite = writeEnd;
        const bool madeBack = made && CreatePipe(&readEnd, &writeEnd, nullptr, 1 << 16);

        if (madeBack)
        {
            parentRead = readEnd;
            childWrite = writeEnd;
        }

        // A handle to this program the child can wait on, to end when it does.
        const bool duplicated = madeBack && DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
                                                            GetCurrentProcess(), &thisProcess, SYNCHRONIZE, TRUE, 0);
        parentForChild = thisProcess;

        if (!duplicated || !SetHandleInformation(handleFrom(childRead), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ||
            !SetHandleInformation(handleFrom(childWrite), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
        {
            error = lastError();
            cleanUp();
            return nullptr;
        }

        inherited.insert(inherited.end(), {handleFrom(childRead), handleFrom(childWrite), thisProcess});
    }

    // The environment: this program's, less what is taken out and the link this program may have been given, with
    // what is set, sorted by name as Windows keeps it.
    std::vector<std::wstring> environment;
    std::vector<std::pair<std::wstring, std::wstring>> set;

    for (const auto& [name, value] : options.environment)
        set.emplace_back(wide(name), wide(value));

    if (options.link)
        set.emplace_back(L"SYMBOLENGINE_LINK", std::to_wstring(reinterpret_cast<std::uintptr_t>(childRead)) + L";" +
                                                   std::to_wstring(reinterpret_cast<std::uintptr_t>(childWrite)) +
                                                   L";" +
                                                   std::to_wstring(reinterpret_cast<std::uintptr_t>(parentForChild)));

    if (const auto block = GetEnvironmentStringsW())
    {
        for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1)
        {
            const auto name = nameOf(entry);
            bool keep = !sameName(name, L"SYMBOLENGINE_LINK");

            for (const auto& removed : options.removeFromEnvironment)
                keep = keep && !sameName(name, wide(removed));

            for (const auto& [setName, value] : set)
                keep = keep && !sameName(name, setName);

            if (keep)
                environment.emplace_back(entry);
        }

        FreeEnvironmentStringsW(block);
    }

    for (const auto& [name, value] : set)
        environment.push_back(name + L"=" + value);

    std::sort(environment.begin(), environment.end(), [](const std::wstring& a, const std::wstring& b) {
        const auto x = nameOf(a), y = nameOf(b);
        return CompareStringOrdinal(x.data(), static_cast<int>(x.size()), y.data(), static_cast<int>(y.size()), TRUE) ==
               CSTR_LESS_THAN;
    });

    std::wstring environmentBlock;

    for (const auto& entry : environment)
    {
        environmentBlock += entry;
        environmentBlock += L'\0';
    }

    environmentBlock += L'\0';

    std::wstring commandLine;
    appendArgument(commandLine, program.wstring());

    for (const auto& argument : options.arguments)
        appendArgument(commandLine, wide(argument));

    SIZE_T listSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &listSize);
    std::vector<unsigned char> listBytes(listSize);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(listBytes.data());
    std::sort(inherited.begin(), inherited.end());
    inherited.erase(std::unique(inherited.begin(), inherited.end()), inherited.end());

    if (!InitializeProcThreadAttributeList(list, 1, 0, &listSize) ||
        !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                   inherited.size() * sizeof(HANDLE), nullptr, nullptr))
    {
        error = lastError();
        cleanUp();
        return nullptr;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = handleFrom(input);
    startup.StartupInfo.hStdOutput = handleFrom(output);
    startup.StartupInfo.hStdError = handleFrom(output);
    startup.lpAttributeList = list;

    // Made suspended, so it is in the job before it runs anything.
    PROCESS_INFORMATION made{};
    const auto started =
        CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                       CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                       environmentBlock.data(), nullptr, &startup.StartupInfo, &made);

    if (!started)
        error = lastError();

    DeleteProcThreadAttributeList(list);
    closeHandle(input);
    closeHandle(output);
    closeHandle(childRead);
    closeHandle(childWrite);
    closeHandle(parentForChild);

    if (!started)
    {
        cleanUp();
        return nullptr;
    }

    if (const auto job = childrenJob(); job != nullptr && !options.outlives)
        AssignProcessToJobObject(job, made.hProcess);

    ResumeThread(made.hThread);
    CloseHandle(made.hThread);

    auto held = std::make_unique<Native>();
    held->process = made.hProcess;
    held->id = made.dwProcessId;
    std::unique_ptr<ChildProcess> child(new ChildProcess(std::move(held)));
    child->outlives = options.outlives;

    if (options.link)
    {
        auto end = std::make_unique<ProcessLink::Native>();
        end->readHandle = parentRead;
        end->writeHandle = parentWrite;
        parentRead = parentWrite = nullptr;
        child->theLink = std::make_unique<ProcessLink>(std::move(end));
    }

    return child;
}

std::int64_t ChildProcess::id() const
{
    return native->id;
}

std::int64_t ChildProcess::memoryInUse() const
{
    if (WaitForSingleObject(native->process, 0) == WAIT_OBJECT_0)
        return 0;
    PROCESS_MEMORY_COUNTERS counters{};
    if (!GetProcessMemoryInfo(native->process, &counters, sizeof counters))
        return 0;
    return static_cast<std::int64_t>(counters.WorkingSetSize);
}

std::optional<ProcessEnding> ChildProcess::ending()
{
    return wait(std::chrono::milliseconds(0));
}

std::optional<ProcessEnding> ChildProcess::wait(std::chrono::milliseconds limit)
{
    if (ended.has_value())
        return ended;

    const auto milliseconds =
        static_cast<DWORD>(std::clamp<std::chrono::milliseconds::rep>(limit.count(), 0, INFINITE - 1));

    if (WaitForSingleObject(native->process, milliseconds) != WAIT_OBJECT_0)
        return std::nullopt;

    DWORD code = 0;
    GetExitCodeProcess(native->process, &code);

    if (native->killing && code == killedCode)
        ended = ProcessEnding{ProcessEnding::Kind::exited, code, true};
    else if (code >= 0xC0000000u)
        ended = ProcessEnding{ProcessEnding::Kind::exception, code, false};
    else
        ended = ProcessEnding{ProcessEnding::Kind::exited, code, false};

    return ended;
}

bool ChildProcess::kill(std::chrono::milliseconds limit)
{
    if (ending().has_value())
        return true;

    native->killing = true;
    TerminateProcess(native->process, killedCode);
    return wait(limit).has_value();
}

ProcessLink* ChildProcess::link()
{
    return theLink.get();
}

}
