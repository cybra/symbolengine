#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/** The system's own dialogs: questions with buttons of the program's choosing, and choosers for opening and saving
    files. Each is shown once the call has returned, and answers on the message thread when it closes. */
namespace symbolengine::ui
{

/** A question, as a Mac's alert or a Windows task dialog asks it. */
struct Question
{
    enum class Kind
    {
        information,
        question,
        warning
    };

    std::string title{};
    std::string message{};
    Kind kind = Kind::question;

    /** The buttons, named for what they do, such as "Save" and "Cancel", the first the one Return presses. */
    std::vector<std::string> buttons{"OK"};

    /** The button Escape presses, as closing the dialog does on Windows; none can be chosen without a button pressed. */
    std::optional<std::size_t> cancel{};
};

/** Asks the question and gives the number of the button chosen, counting from 0. With a parent window, an NSWindow, or an
    NSView inside one, on a Mac and an HWND on Windows, it is asked in front of that window, as a sheet on a Mac;
    without one, in front of the program. */
void ask(const Question& question, void* parent, std::function<void(std::size_t chosen)> answered);

/** A file to open or save, or a folder to open, as the system's own chooser asks for it. */
struct FileChoice
{
    enum class Mode
    {
        openFile,
        openFolder,
        save
    };

    Mode mode = Mode::openFile;
    std::string title{};

    /** The folder it starts in; for saving, the file it offers, in the folder it starts in. Empty for wherever the
        system starts. */
    std::filesystem::path start{};

    /** The extensions of the files it offers, such as ".wav", and what they are called together, such as "Audio
        files", which Windows shows; none for files of any kind. A file saved without one of them is given the first. */
    std::vector<std::string> extensions{};
    std::string kindName{};
};

/** Asks for a file or a folder and gives it, or nothing if none was chosen. Saving over a file asks first. The parent
    window is as ask() takes it, for Windows; on a Mac the chooser is in front of the program. */
void chooseFile(const FileChoice& choice, void* parent, std::function<void(std::optional<std::filesystem::path> chosen)> chosen);

}
