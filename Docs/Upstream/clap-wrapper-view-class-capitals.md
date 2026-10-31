# clap-wrapper: AUv2 view classes lose their capitals

Where it goes: a new issue on [free-audio/clap-wrapper](https://github.com/free-audio/clap-wrapper/issues), with the change as a pull request. A search finds nothing on this; `main` still has it.

## Report

**The AUv2 build helper replaces every capital but A in a view class's name**

`src/detail/auv2/build-helper/build-helper.cpp` makes each plugin's Objective-C view class a name from its codes and names, keeping what is safe in one:

```cpp
if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'A') || (c >= '0' && c <= '9'))
```

`c <= 'A'` is meant to be `c <= 'Z'`, so every capital but A becomes `_`. A plugin called Filter by SymbolEngine, subtype `Filt`, gets

```
wrapAsAUV2_inst_cocoaUI__ilter_aufx__ilt__ymbol_ngine__y_n
```

So two plugins whose names and codes differ only in their capitals, Gain and Rain from one maker, have one class name between them. In a host that loads both, the Objective-C runtime keeps one of the two classes, and one plugin's view comes from the other's code.

The fix:

```diff
-        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'A') || (c >= '0' && c <= '9'))
+        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
```

after which that plugin's class is `wrapAsAUV2_inst_cocoaUI_Filter_aufx_Filt_SymbolEngine_SyEn`, read with `nm -U` from the built component.
