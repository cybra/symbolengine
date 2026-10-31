# CHOC: choc_ObjectiveCHelpers.h doesn't include <chrono>

Where it goes: a new issue on [Tracktion/choc](https://github.com/Tracktion/choc/issues), or a one-line pull request. A search finds nothing on this. The code is unchanged at 8af031c.

## Report

**`choc_ObjectiveCHelpers.h` uses `std::chrono` without including it**

`createDelegateClass` calls `std::chrono::high_resolution_clock::now()`, and the file relies on another header to bring `<chrono>` along. The libc++ that comes with LLVM 23 no longer does, so the header doesn't compile by itself there:

```
choc/platform/choc_ObjectiveCHelpers.h: error: no member named 'chrono' in namespace 'std'
```

The fix:

```diff
+#include <chrono>
 #include <memory>
 #include <objc/runtime.h>
```
