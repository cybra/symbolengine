# CHOC: WebView2 failing to start isn't reported

Where it goes: a new issue on [Tracktion/choc](https://github.com/Tracktion/choc/issues). A search finds nothing on this. The code is unchanged at 8af031c.

## Report

**`loadedOK()` says true on Windows when there is no WebView2 runtime**

`WebView::Pimpl::initialise` ignores what `CreateCoreWebView2EnvironmentWithOptions` returns:

```cpp
eventHandler = new EventHandler (*this);
createCoreWebView2EnvironmentWithOptions (nullptr, userDataFolder.c_str(), nullptr, eventHandler);
return true;
```

Without a runtime that call fails, so `loadedOK()` says true for a view that will stay blank, and scripts given to `evaluateJavascript` are dropped with nothing to say why. A failure that only arrives in the completion handlers has the same effect, as they return `E_FAIL` with no way for the program to hear of it.

The first part is a small change:

```diff
         eventHandler = new EventHandler (*this);
-        createCoreWebView2EnvironmentWithOptions (nullptr, userDataFolder.c_str(), nullptr, eventHandler);
-        return true;
+        return createCoreWebView2EnvironmentWithOptions (nullptr, userDataFolder.c_str(), nullptr, eventHandler) == S_OK;
```

The second wants an option called from those handlers, such as `std::function<void()> webviewFailed`.

To check it without uninstalling anything, set `WEBVIEW2_BROWSER_EXECUTABLE_FOLDER` to an empty folder before making the view.
