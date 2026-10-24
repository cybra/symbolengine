# cmake -DFILE=<choc_WebView.h> -P PatchChoc.cmake
#
# Changes CHOC's web view in four ways, the first three on Windows:
#  - a userDataFolder option, so a program or plugin chooses where WebView2 keeps its data instead of a folder named
#    after the program that loaded it;
#  - the keyboard passed on to the page when the web view's window is given it, as a WKWebView takes it on a Mac,
#    so keys reach the page as soon as its window is active, without a click inside it first;
#  - a background color option, which WebView2 shows where the page hasn't drawn, as while its window grows, instead
#    of white;
#  - on a Mac and on Windows, the program's functions bound before the first page starts loading, rather than just
#    after, when the page could already have been made without them, and waited for them forever.
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

replace_once("windowsBackgroundColor" [[        bool transparentBackground = false;
]] [[        bool transparentBackground = false;

        /// Windows only: the color WebView2 shows where the page hasn't drawn, as
        /// 0xRRGGBB, when the background isn't transparent.
        std::optional<uint32_t> windowsBackgroundColor;
]])

replace_once("options.windowsBackgroundColor.has_value()" [[        if (options.transparentBackground)
        {
            COMPtr<ICoreWebView2Controller2> controller2;

            if (controller->QueryInterface (ICoreWebView2Controller2::getIID(), (void**) controller2.getAddress()) == S_OK
                  && controller2 != nullptr)
            {
                controller2->put_DefaultBackgroundColor ({ 0, 0, 0, 0 });
            }
        }
]] [[        if (options.transparentBackground || options.windowsBackgroundColor.has_value())
        {
            COMPtr<ICoreWebView2Controller2> controller2;

            if (controller->QueryInterface (ICoreWebView2Controller2::getIID(), (void**) controller2.getAddress()) == S_OK
                  && controller2 != nullptr)
            {
                const auto c = options.windowsBackgroundColor.value_or (0);

                if (options.transparentBackground)
                    controller2->put_DefaultBackgroundColor ({ 0, 0, 0, 0 });
                else
                    controller2->put_DefaultBackgroundColor ({ 255, (BYTE) (c >> 16), (BYTE) (c >> 8), (BYTE) c });
            }
        }
]])

replace_once("Bound before the first page loads (Mac)" [[        if (options->fetchResource)
            navigate ({});

        CHOC_AUTORELEASE_END

        if (options->webviewIsReady)
            options->webviewIsReady (owner);

        return true;
]] [[        CHOC_AUTORELEASE_END

        // Bound before the first page loads (Mac), so it has the functions from the start.
        if (options->webviewIsReady)
            options->webviewIsReady (owner);

        if (options->fetchResource)
        {
            CHOC_AUTORELEASE_BEGIN
            navigate ({});
            CHOC_AUTORELEASE_END
        }

        return true;
]])

replace_once("Bound before the first page loads (Windows)" [[        if (options.fetchResource)
            navigate ({});

        COMPtr<ICoreWebView2Settings> settings;
]] [[        COMPtr<ICoreWebView2Settings> settings;
]])

replace_once("Bound before the first page loads (Windows)" [[        resizeContentToFit();

        if (options.webviewIsReady)
            options.webviewIsReady (owner);
    }
]] [[        resizeContentToFit();

        // Bound before the first page loads (Windows), so it has the functions from the start.
        if (options.webviewIsReady)
            options.webviewIsReady (owner);

        if (options.fetchResource)
            navigate ({});
    }
]])

file(WRITE "${FILE}" "${source}")
