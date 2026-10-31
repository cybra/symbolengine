# CHOC: two plugins in one process can share a window class

Where it goes: a new issue on [Tracktion/choc](https://github.com/Tracktion/choc/issues). A search finds #52, on two web views in one program, which is why `WindowClass` adds the tick count to its name; nothing on two modules. The code is unchanged at 8af031c.

## Report

**On Windows, window classes are registered under the executable, with names two DLLs can share**

`MessageWindow` in `choc_MessageLoop.h` and `WindowClass` in `choc_DesktopWindow.h` register their classes like this:

```cpp
className = "choc_" + std::to_string (rand());
wc.hInstance = GetModuleHandleA (nullptr);
```

```cpp
name += std::to_wstring (static_cast<uint32_t> (GetTickCount()));
moduleHandle = GetModuleHandle (nullptr);
```

Both go wrong when the code is in a DLL, as it is in a plugin. `GetModuleHandle (nullptr)` is the host's executable, so all plugins register in one namespace. Under the static runtime each DLL has its own `rand()`, whose first answer is always the same, so a second plugin's name matches the first's, `RegisterClassExA` fails unchecked, and `CreateWindowA` makes its window with the first plugin's class and procedure. `GetTickCount` has the same problem within its 16 ms resolution. Once the first plugin is unloaded, that procedure is gone, and the second plugin's next message calls into nothing.

The fix is to name the class after the code registering it and register it under the module holding that code:

```diff
-        className = "choc_" + std::to_string (rand());
+        static std::atomic<unsigned> made { 0 };
+        className = "choc_" + std::to_string (reinterpret_cast<uintptr_t> (&windowProc)) + "_" + std::to_string (++made);
+
+        GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
+                            reinterpret_cast<LPCSTR> (&windowProc), &moduleHandle);

-        wc.hInstance = GetModuleHandleA (nullptr);
+        wc.hInstance = moduleHandle;
```

with `UnregisterClassA (className.c_str(), moduleHandle)` in the destructor, and the same in `WindowClass`.

To check it: load two copies of a DLL using CHOC's message loop or `choc::ui::WebView`, and read `GetClassLongPtrW (window, GCLP_HMODULE)` from each's window. Before the change it is the executable's handle for both; after, each DLL's own.
