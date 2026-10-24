# cmake -DFILE=<choc_WebView.h> -P PatchChoc.cmake
#
# Changes CHOC's web view in two ways, on Windows:
#  - a userDataFolder option, so a program or plugin chooses where WebView2 keeps its data instead of a folder named
#    after the program that loaded it;
#  - the keyboard passed on to the page when the web view's window is given it, as a WKWebView takes it on a Mac,
#    so keys reach the page as soon as its window is active, without a click inside it first.
# Each change looks for the text it replaces and stops the build if CHOC has changed it, so a new version of CHOC is
# never patched wrongly, and is skipped if it has been made already.

file(READ "${FILE}" source)

macro(replace_once done old new)
    string(FIND "${source}" "${done}" made)
    if(made EQUAL -1)
        string(FIND "${source}" "${old}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "PatchChoc.cmake: CHOC's web view has changed; this text is gone: ${old}")
        endif()
        string(REPLACE "${old}" "${new}" source "${source}")
    endif()
endmacro()

replace_once("std::string userDataFolder;" [[        std::string customSchemeURI;
]] [[        std::string customSchemeURI;

        /// Windows only: the folder WebView2 keeps its data in, as UTF-8. Leave this
        /// empty for the default, a folder named after the program in AppData.
        std::string userDataFolder;
]])

replace_once("options.userDataFolder.empty()"
    [[if (auto userDataFolder = getUserDataFolder(); ! userDataFolder.empty())]]
    [[if (auto userDataFolder = options.userDataFolder.empty() ? getUserDataFolder() : createUTF16StringFromUTF8 (options.userDataFolder); ! userDataFolder.empty())]])

replace_once("COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);" [[        if (msg == WM_SHOWWINDOW)
            if (auto w = getPimpl (h))
                w->setVisible (wp != 0);
]] [[        if (msg == WM_SHOWWINDOW)
            if (auto w = getPimpl (h))
                w->setVisible (wp != 0);

        if (msg == WM_SETFOCUS)
            if (auto w = getPimpl (h); w != nullptr && w->coreWebViewController)
                w->coreWebViewController->MoveFocus (COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
]])

file(WRITE "${FILE}" "${source}")
