#include <symbolengine/ui/Dialogs.h>

#include <choc/gui/choc_MessageLoop.h>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <commctrl.h>
#include <shobjidl.h>

#include <string>
#include <vector>

// Task dialogs are in version 6 of the common controls, which a program asks for in its manifest.
#if defined(_MSC_VER)
    #pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace symbolengine::ui
{

namespace
{
// The numbers the task dialog gives its buttons, clear of those of its own, such as IDCANCEL.
constexpr int firstButton = 1000;

std::wstring wide(const std::string& text)
{
    if (text.empty())
        return {};
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

using TaskDialogFunction = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

// TaskDialogIndirect, found where the program's common controls are, or nothing in a program without version 6.
TaskDialogFunction taskDialog()
{
    static const auto found = [] {
        const auto controls = LoadLibraryW(L"comctl32.dll");
        return controls != nullptr ? reinterpret_cast<TaskDialogFunction>(
                                         reinterpret_cast<void*>(GetProcAddress(controls, "TaskDialogIndirect")))
                                   : nullptr;
    }();
    return found;
}

std::size_t askNow(const Question& question, HWND owner)
{
    const auto title = wide(question.title);
    const auto message = wide(question.message);
    const auto cancel = question.cancel.value_or(0);

    if (const auto show = taskDialog())
    {
        std::vector<std::wstring> labels;
        for (const auto& button : question.buttons)
            labels.push_back(wide(button));

        std::vector<TASKDIALOG_BUTTON> buttons;
        for (std::size_t i = 0; i < labels.size(); ++i)
            buttons.push_back({firstButton + static_cast<int>(i), labels[i].c_str()});

        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof config;
        config.hwndParent = owner;
        config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | (question.cancel.has_value() ? TDF_ALLOW_DIALOG_CANCELLATION : 0);
        config.pszWindowTitle = title.c_str();
        config.pszContent = message.c_str();
        config.pButtons = buttons.data();
        config.cButtons = static_cast<UINT>(buttons.size());
        config.nDefaultButton = firstButton;

        if (question.kind == Question::Kind::warning)
            config.pszMainIcon = TD_WARNING_ICON;
        else if (question.kind == Question::Kind::information)
            config.pszMainIcon = TD_INFORMATION_ICON;
        else if (const auto icon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_QUESTION)))
        {
            config.hMainIcon = icon;
            config.dwFlags |= TDF_USE_HICON_MAIN;
        }

        int pressed = 0;
        if (SUCCEEDED(show(&config, &pressed, nullptr, nullptr)))
        {
            const auto index = pressed - firstButton;
            return index >= 0 && static_cast<std::size_t>(index) < question.buttons.size() ? static_cast<std::size_t>(index) : cancel;
        }
    }

    // Without task dialogs: OK for the first button, Cancel for the one that cancels.
    const auto type = (question.buttons.size() > 1 ? MB_OKCANCEL : MB_OK) |
                      (question.kind == Question::Kind::warning ? MB_ICONWARNING
                       : question.kind == Question::Kind::question ? MB_ICONQUESTION : MB_ICONINFORMATION);
    return MessageBoxW(owner, message.c_str(), title.c_str(), static_cast<UINT>(type)) == IDOK ? 0 : cancel;
}

std::optional<std::filesystem::path> chooseNow(const FileChoice& choice, HWND owner)
{
    const auto saving = choice.mode == FileChoice::Mode::save;
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(saving ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                saving ? IID_IFileSaveDialog : IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog))))
        return std::nullopt;

    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM;
    if (saving)
        options |= FOS_OVERWRITEPROMPT;
    else
        options |= FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST;
    if (choice.mode == FileChoice::Mode::openFolder)
        options |= FOS_PICKFOLDERS;
    dialog->SetOptions(options);
    dialog->SetTitle(wide(choice.title).c_str());

    const auto folder = saving && choice.start.has_filename() ? choice.start.parent_path() : choice.start;
    IShellItem* start = nullptr;
    if (!folder.empty() && SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_IShellItem,
                                                                  reinterpret_cast<void**>(&start))))
    {
        dialog->SetFolder(start);
        start->Release();
    }
    if (saving && choice.start.has_filename())
        dialog->SetFileName(choice.start.filename().c_str());

    // The kinds of file offered, as Windows' filter spells them: *.wav;*.aiff.
    std::wstring name = wide(choice.kindName.empty() ? std::string("Files") : choice.kindName), spec;
    for (auto extension : choice.extensions)
    {
        if (!extension.empty() && extension.front() == '.')
            extension.erase(0, 1);
        spec += (spec.empty() ? L"*." : L";*.") + wide(extension);
    }
    if (!spec.empty() && choice.mode != FileChoice::Mode::openFolder)
    {
        const COMDLG_FILTERSPEC filter{name.c_str(), spec.c_str()};
        dialog->SetFileTypes(1, &filter);
        if (saving)
        {
            auto first = choice.extensions.front();
            if (!first.empty() && first.front() == '.')
                first.erase(0, 1);
            dialog->SetDefaultExtension(wide(first).c_str());
        }
    }

    std::optional<std::filesystem::path> result;
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item)))
    {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
            result = std::filesystem::path(path);
            CoTaskMemFree(path);
        }
        item->Release();
    }

    dialog->Release();
    return result;
}
}

void ask(const Question& question, void* parent, std::function<void(std::size_t chosen)> answered)
{
    choc::messageloop::postMessage([question, owner = static_cast<HWND>(parent), answered = std::move(answered)] {
        answered(askNow(question, owner));
    });
}

void chooseFile(const FileChoice& choice, void* parent, std::function<void(std::optional<std::filesystem::path> chosen)> chosen)
{
    choc::messageloop::postMessage([choice, owner = static_cast<HWND>(parent), chosen = std::move(chosen)] {
        // The dialogs are COM objects, which need the message thread's COM single-threaded, as a window's usually is
        // already; it is started here if not, and stopped again after.
        const auto started = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto result = chooseNow(choice, owner);
        if (SUCCEEDED(started))
            CoUninitialize();
        chosen(std::move(result));
    });
}

}
