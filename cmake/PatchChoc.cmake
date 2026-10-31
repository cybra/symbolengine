# cmake -DFILE=<choc_WebView.h> -P PatchChoc.cmake
#
# Changes CHOC, run in its folder. In its web view:
#  - a userDataFolder option, so a program or plugin chooses where WebView2 keeps its data instead of a folder named
#    after the program that loaded it;
#  - the keyboard passed on to the page when the web view's window is given it, as a WKWebView takes it on a Mac,
#    so keys reach the page as soon as its window is active, without a click inside it first;
#  - a background color option, which WebView2 shows where the page hasn't drawn, as while its window grows, instead
#    of white;
#  - on a Mac and on Windows, the program's functions bound before the first page starts loading, rather than just
#    after, when the page could already have been made without them, and waited for them forever;
#  - WebView2 failing to start, at once or later, said rather than left blank (Docs/Upstream/choc-webview2-start.md);
#  - a page that behaves as a program's page, not a browser's, unless it is for development: no reload, zoom or
#    browser menu on Windows, and nothing on a Mac's context menu that leaves the page;
#  - a page whose renderer has died loaded again;
#  - on a Mac, the editing keys taken only while the page has the keyboard;
#  - part of a file served when a page asks for it, which an audio or video element does.
# Elsewhere, on Windows, plugins in one process never sharing a window class (Docs/Upstream/choc-window-classes.md), and
# <chrono> where a newer standard library no longer brings it along (Docs/Upstream/choc-chrono.md).
# Each change looks for the text it replaces and stops the build if CHOC has changed it, so a new version of CHOC is
# never patched wrongly, and is skipped if it has been made already.

file(READ "${FILE}" source)

macro(replace_once done old new)
    string(FIND "${source}" "${done}" made)
    if(made EQUAL -1)
        string(FIND "${source}" "${old}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "PatchChoc.cmake: CHOC has changed; this text is gone: ${old}")
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

replace_once("struct Part" [=[            std::vector<uint8_t> data;
            std::string mimeType;
        };
]=] [=[            std::vector<uint8_t> data;
            std::string mimeType;

            /// What a Range header asks for of a file of this size, by the rules browsers keep to: one range, and
            /// anything else in the header means the whole file.
            struct Part
            {
                int status = 200;
                size_t first = 0, count = 0;
                std::string contentRange;
            };

            static Part partAsked (const std::string& header, size_t size)
            {
                Part whole { 200, 0, size, {} };

                const auto digits = [] (const std::string& text, size_t& number)
                {
                    number = 0;

                    for (auto c : text)
                    {
                        if (c < '0' || c > '9')
                            return false;

                        number = number > (size_t) -1 / 20 ? (size_t) -1 / 2 : number * 10 + (size_t) (c - '0');
                    }

                    return ! text.empty();
                };

                const auto dash = header.find ('-');

                if (header.rfind ("bytes=", 0) != 0 || dash == std::string::npos || header.find (',') != std::string::npos)
                    return whole;

                const auto from = header.substr (6, dash - 6), to = header.substr (dash + 1);
                size_t first = 0, last = 0;

                if (from.empty())
                {
                    if (! digits (to, last))
                        return whole;

                    if (last == 0 || size == 0)
                        return { 416, 0, 0, "bytes */" + std::to_string (size) };

                    first = size - std::min (last, size);
                    last = size - 1;
                }
                else
                {
                    if (! digits (from, first))
                        return whole;

                    if (first >= size)
                        return { 416, 0, 0, "bytes */" + std::to_string (size) };

                    if (to.empty())
                        last = size - 1;
                    else if (! digits (to, last) || last < first)
                        return whole;

                    last = std::min (last, size - 1);
                }

                return { 206, first, last - first + 1,
                         "bytes " + std::to_string (first) + "-" + std::to_string (last) + "/" + std::to_string (size) };
            }
        };
]=])

replace_once("webviewFailed" [=[        std::function<void(choc::ui::WebView&)> webviewIsReady;
]=] [=[        std::function<void(choc::ui::WebView&)> webviewIsReady;

        /// Windows only: called if WebView2 turns out not to be able to start, which it may not know until
        /// after this object has been made, or its browser process later ends.
        std::function<void()> webviewFailed;

        /// Called if the process drawing the page has died, just before the page is loaded again.
        std::function<void()> webviewRendererLost;
]=])

replace_once("ownerPimpl.startFailed" [=[        HRESULT STDMETHODCALLTYPE Invoke (HRESULT, ICoreWebView2Environment* env) override
        {
            if (env == nullptr || deletionCheckerRef->deleted)
                return E_FAIL;
]=] [=[        HRESULT STDMETHODCALLTYPE Invoke (HRESULT, ICoreWebView2Environment* env) override
        {
            if (deletionCheckerRef->deleted)
                return E_FAIL;

            if (env == nullptr)
            {
                ownerPimpl.startFailed();
                return E_FAIL;
            }
]=])

replace_once("if (controller == nullptr) { ownerPimpl" [=[        HRESULT STDMETHODCALLTYPE Invoke (HRESULT, ICoreWebView2Controller* controller) override
        {
            if (controller == nullptr || deletionCheckerRef->deleted)
                return E_FAIL;
]=] [=[        HRESULT STDMETHODCALLTYPE Invoke (HRESULT, ICoreWebView2Controller* controller) override
        {
            if (deletionCheckerRef->deleted)
                return E_FAIL;

            if (controller == nullptr) { ownerPimpl.startFailed(); return E_FAIL; }
]=])

replace_once("void startFailed()" [=[    void webviewControllerCreationComplete (ICoreWebView2Controller* controller, ICoreWebView2* view)
    {
]=] [=[    void startFailed()
    {
        if (options.webviewFailed)
            options.webviewFailed();
    }

    void webviewControllerCreationComplete (ICoreWebView2Controller* controller, ICoreWebView2* view)
    {
]=])

replace_once("ICoreWebView2Settings3" [=[    static IID getIID() { return { 0xee9a0f68, 0xf46c, 0x4e32, { 0xac, 0x23, 0xef, 0x8c, 0xac, 0x22, 0x4d, 0x2a } }; }
};
]=] [=[    static IID getIID() { return { 0xee9a0f68, 0xf46c, 0x4e32, { 0xac, 0x23, 0xef, 0x8c, 0xac, 0x22, 0x4d, 0x2a } }; }
};

MIDL_INTERFACE("fdb5ab74-af33-4854-84f0-0a631deb5eba")
ICoreWebView2Settings3 : public ICoreWebView2Settings2
{
public:
    virtual HRESULT STDMETHODCALLTYPE get_AreBrowserAcceleratorKeysEnabled(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AreBrowserAcceleratorKeysEnabled(BOOL) = 0;

    static IID getIID() { return { 0xfdb5ab74, 0xaf33, 0x4854, { 0x84, 0xf0, 0x0a, 0x63, 0x1d, 0xeb, 0x5e, 0xba } }; }
};

MIDL_INTERFACE("8155a9a4-1474-4a86-8cae-151b0fa6b8ca")
ICoreWebView2ProcessFailedEventArgs : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE get_ProcessFailedKind(int*) = 0;
};

MIDL_INTERFACE("79e0aea4-990b-42d9-aa1d-0fcc2e5bc7f1")
ICoreWebView2ProcessFailedEventHandler : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) = 0;
};
]=])

replace_once("put_AreBrowserAcceleratorKeysEnabled (" [=[            settings->put_AreDevToolsEnabled (options.enableDebugMode);
]=] [=[            settings->put_AreDevToolsEnabled (options.enableDebugMode);

            // Unless it's for development, this is a program's page, not a browser's: no reload key, no zooming.
            settings->put_AreDefaultContextMenusEnabled (options.enableDebugMode);
            settings->put_IsZoomControlEnabled (FALSE);
            settings->put_IsStatusBarEnabled (FALSE);

            COMPtr<ICoreWebView2Settings3> settings3;

            if (settings->QueryInterface (ICoreWebView2Settings3::getIID(), (void**) settings3.getAddress()) == S_OK
                  && settings3 != nullptr)
                settings3->put_AreBrowserAcceleratorKeysEnabled (options.enableDebugMode);
]=])

replace_once("struct ProcessFailedHandler" [=[    void addRequestFilters (ICoreWebView2* view)
    {
]=] [=[    // A page whose renderer has died is loaded again; without its browser process the view has gone for good.
    struct ProcessFailedHandler  : public ICoreWebView2ProcessFailedEventHandler
    {
        ProcessFailedHandler (Pimpl& p) : ownerPimpl (p), deletionCheckerRef (p.deletionChecker) {}
        virtual ~ProcessFailedHandler() {}

        HRESULT STDMETHODCALLTYPE QueryInterface (REFIID, LPVOID*) override   { return E_NOINTERFACE; }
        ULONG STDMETHODCALLTYPE AddRef() override     { return ++refCount; }
        ULONG STDMETHODCALLTYPE Release() override    { auto newCount = --refCount; if (newCount == 0) delete this; return newCount; }

        HRESULT STDMETHODCALLTYPE Invoke (ICoreWebView2* view, ICoreWebView2ProcessFailedEventArgs* args) override
        {
            enum { browserProcessExited = 0, renderProcessExited = 1 };

            int kind = -1;

            if (deletionCheckerRef->deleted || args == nullptr || args->get_ProcessFailedKind (std::addressof (kind)) != S_OK)
                return S_OK;

            if (kind == browserProcessExited)
            {
                ownerPimpl.startFailed();
            }
            else if (kind == renderProcessExited && view != nullptr)
            {
                if (ownerPimpl.options.webviewRendererLost)
                    ownerPimpl.options.webviewRendererLost();

                view->Reload();
            }

            return S_OK;
        }

        Pimpl& ownerPimpl;
        std::shared_ptr<DeletionChecker> deletionCheckerRef;
        std::atomic<ULONG> refCount { 0 };
    };

    void addProcessFailedHandler (ICoreWebView2* view)
    {
        COMPtr<ProcessFailedHandler> handler (new ProcessFailedHandler (*this));
        EventRegistrationToken token;
        view->add_ProcessFailed (static_cast<ICoreWebView2ProcessFailedEventHandler*> (handler.object), std::addressof (token));
    }

    void addRequestFilters (ICoreWebView2* view)
    {
]=])

replace_once("addProcessFailedHandler (coreWebView)" [=[        // Bound before the first page loads (Windows), so it has the functions from the start.
]=] [=[        addProcessFailedHandler (coreWebView);

        // Bound before the first page loads (Windows), so it has the functions from the start.
]=])

replace_once("webViewWebContentProcessDidTerminate" [=[            class_addMethod (delegateClass, sel_registerName ("webView:startURLSchemeTask:"),
]=] [=[            class_addMethod (delegateClass, sel_registerName ("webViewWebContentProcessDidTerminate:"),
                             (IMP) (+[](id self, SEL, id wkwebview)
                             {
                                 if (auto p = getPimpl (self); p != nullptr && p->options->webviewRendererLost)
                                     p->options->webviewRendererLost();

                                 objc::call<id> (wkwebview, "reload");
                             }),
                             "v@:@");

            class_addMethod (delegateClass, sel_registerName ("webView:startURLSchemeTask:"),
]=])

replace_once("willOpenMenu:withEvent:" [=[            class_addMethod (webviewClass, sel_registerName ("performKeyEquivalent:"),
]=] [=[            class_addMethod (webviewClass, sel_registerName ("willOpenMenu:withEvent:"),
                            (IMP) (+[](id self, SEL, id menu, id event)
                            {
                                if (auto p = getPimpl (self); p != nullptr && ! p->options->enableDebugMode)
                                    removeWaysOfLeaving (menu);

                                choc::objc::callSuper<void> (self, "willOpenMenu:withEvent:", menu, event);
                            }), "v@:@@");

            class_addMethod (webviewClass, sel_registerName ("performKeyEquivalent:"),
]=])

replace_once("static void removeWaysOfLeaving" [=[    struct WebviewClass
]=] [=[    // What a context menu offers that would take a program's page away from it.
    static void removeWaysOfLeaving (id menu)
    {
        static constexpr const char* leaving[] = { "WKMenuItemIdentifierReload", "WKMenuItemIdentifierGoBack", "WKMenuItemIdentifierGoForward",
                                                   "WKMenuItemIdentifierOpenLink", "WKMenuItemIdentifierOpenLinkInNewWindow",
                                                   "WKMenuItemIdentifierOpenFrameInNewWindow", "WKMenuItemIdentifierOpenImageInNewWindow",
                                                   "WKMenuItemIdentifierOpenMediaInNewWindow", "WKMenuItemIdentifierDownloadImage",
                                                   "WKMenuItemIdentifierDownloadLinkedFile", "WKMenuItemIdentifierDownloadMedia" };

        for (auto i = objc::call<long> (menu, "numberOfItems"); --i >= 0;)
        {
            auto identifier = objc::getString (objc::call<id> (objc::call<id> (menu, "itemAtIndex:", i), "identifier"));

            for (auto name : leaving)
                if (identifier == name)
                    objc::call<void> (menu, "removeItemAtIndex:", i);
        }
    }

    struct WebviewClass
]=])

replace_once("whichever view has the keyboard" [=[        if (! options->enableDefaultClipboardKeyShortcutsInSafari)
            return false;
]=] [=[        if (! options->enableDefaultClipboardKeyShortcutsInSafari)
            return false;

        // A key equivalent is offered to every view in the window, whichever view has the keyboard.
        id firstResponder = objc::call<id> (objc::call<id> (self, "window"), "firstResponder");

        if (firstResponder == nullptr
             || ! objc::call<BOOL> (firstResponder, "isKindOfClass:", objc_getClass ("NSView"))
             || ! objc::call<BOOL> (firstResponder, "isDescendantOf:", self))
            return false;
]=])

replace_once("partAsked (objc::getString" [=[                const auto& [bytes, mimeType] = *resource;

                auto contentLength = std::to_string (bytes.size());
                id headerKeys[]    = { getNSString ("Content-Length"), getNSString ("Content-Type"), getNSString ("Cache-Control"), getNSString ("Access-Control-Allow-Origin") };
                id headerObjects[] = { getNSString (contentLength),    getNSString (mimeType),       getNSString ("no-store") ,     getNSString ("*") };

                id headerFields = callClass<id> ("NSDictionary", "dictionaryWithObjects:forKeys:count:",
                                                 headerObjects, headerKeys, sizeof (headerObjects) / sizeof (id));

                call<void> (task, "didReceiveResponse:", makeResponse (200, headerFields));

                id data = callClass<id> ("NSData", "dataWithBytes:length:", bytes.data(), bytes.size());
                call<void> (task, "didReceiveData:", data);
]=] [=[                const auto& [bytes, mimeType] = *resource;

                auto part = Options::Resource::partAsked (objc::getString (call<id> (call<id> (task, "request"), "valueForHTTPHeaderField:", getNSString ("Range"))), bytes.size());

                auto contentLength = std::to_string (part.count);
                id headerKeys[]    = { getNSString ("Content-Length"), getNSString ("Content-Type"), getNSString ("Cache-Control"), getNSString ("Access-Control-Allow-Origin"), getNSString ("Accept-Ranges"), getNSString ("Content-Range") };
                id headerObjects[] = { getNSString (contentLength),    getNSString (mimeType),       getNSString ("no-store") ,     getNSString ("*"),                           getNSString ("bytes"),         getNSString (part.contentRange) };

                id headerFields = callClass<id> ("NSDictionary", "dictionaryWithObjects:forKeys:count:",
                                                 headerObjects, headerKeys, (unsigned long) (part.contentRange.empty() ? 5 : 6));

                call<void> (task, "didReceiveResponse:", makeResponse ((long) part.status, headerFields));

                if (part.count > 0)
                {
                    id data = callClass<id> ("NSData", "dataWithBytes:length:", bytes.data() + part.first, part.count);
                    call<void> (task, "didReceiveData:", data);
                }
]=])

replace_once("L\"Partial Content\"" [=[            if (auto resource = fetchResourceOrPageHTML (createUTF8FromUTF16 (uri.uri)))
            {
                COMPtr<IStream> stream;
]=] [=[            if (auto resource = fetchResourceOrPageHTML (createUTF8FromUTF16 (uri.uri)))
            {
                std::string rangeAsked;

                {
                    COMPtr<ICoreWebView2HttpRequestHeaders> requestHeaders;
                    URIStringPtr value;

                    if (request->get_Headers (requestHeaders.getAddress()) == S_OK && requestHeaders != nullptr
                         && requestHeaders->GetHeader (L"Range", std::addressof (value.uri)) == S_OK)
                        rangeAsked = createUTF8FromUTF16 (value.uri);
                }

                const auto part = Options::Resource::partAsked (rangeAsked, resource->data.size());
                const auto reason = part.status == 206 ? L"Partial Content" : part.status == 416 ? L"Range Not Satisfiable" : L"OK";

                COMPtr<IStream> stream;
]=])

replace_once("resource->data.data() + part.first" [=[                        stream.object = fn (reinterpret_cast<const BYTE*> (resource->data.data()),
                                            static_cast<UINT> (resource->data.size()));
]=] [=[                        stream.object = fn (reinterpret_cast<const BYTE*> (resource->data.data() + part.first),
                                            static_cast<UINT> (part.count));
]=])

replace_once("Accept-Ranges: bytes" [=[                headers.emplace_back ("Access-Control-Allow-Origin: *");

                if (! options.customUserAgent.empty())
]=] [=[                headers.emplace_back ("Access-Control-Allow-Origin: *");
                headers.emplace_back ("Accept-Ranges: bytes");

                if (! part.contentRange.empty())
                    headers.emplace_back ("Content-Range: " + part.contentRange);

                if (! options.customUserAgent.empty())
]=])

replace_once("CreateWebResourceResponse (stream, part.status" [=[CreateWebResourceResponse (stream, 200, L"OK", headerString.c_str()]=] [=[CreateWebResourceResponse (stream, part.status, reason, headerString.c_str()]=])

file(WRITE "${FILE}" "${source}")

# On Windows each plugin's copy of this code in one process has a window class of its own, named after the copy and
# registered under the module holding it, so one being unloaded can't leave another's windows with its procedure.
file(READ "choc/gui/choc_MessageLoop.h" source)

replace_once("#include <atomic>" [=[#define Rectangle Rectangle_renamed_to_avoid_name_collisions
]=] [=[#include <atomic>
#define Rectangle Rectangle_renamed_to_avoid_name_collisions
]=])

file(WRITE "choc/gui/choc_MessageLoop.h" "${source}")

file(READ "choc/gui/choc_DesktopWindow.h" source)

replace_once("#include <atomic>" [=[#define Rectangle Rectangle_renamed_to_avoid_name_collisions
]=] [=[#include <atomic>
#define Rectangle Rectangle_renamed_to_avoid_name_collisions
]=])

file(WRITE "choc/gui/choc_DesktopWindow.h" "${source}")

file(READ "choc/platform/choc_ObjectiveCHelpers.h" source)

replace_once("#include <chrono>" [=[#include <memory>
#include <objc/runtime.h>
]=] [=[#include <chrono>
#include <memory>
#include <objc/runtime.h>
]=])

file(WRITE "choc/platform/choc_ObjectiveCHelpers.h" "${source}")
