# Upstream reports

Bugs found in the libraries and tools SymbolEngine builds on, each written up for the project's own tracker, with what SymbolEngine does about it meanwhile. Each report opens by saying where it goes, and its body is ready to paste there.

| Project | Report | Where it goes | Filed | SymbolEngine meanwhile |
|---|---|---|---|---|
| libvorbis | [The 5.1 encoder leaks the LFE's residue](libvorbis-51-residue-leak.md) | A comment on Xiph's GitLab issue #2357 | Yes | Builds a copy of `vorbisenc.c` that frees it |
| opusfile | [Opening a chained file can lose a link's tags](opusfile-chained-tags-leak.md) | A new issue on Xiph's GitLab | No | Builds a copy of `opusfile.c` that frees them |
| CHOC | [Two plugins in one process can share a window class](choc-window-classes.md) | A new issue on GitHub | No | `cmake/PatchChoc.cmake` makes the fix |
| CHOC | [WebView2 failing to start isn't reported](choc-webview2-start.md) | A new issue on GitHub | No | `cmake/PatchChoc.cmake` makes the fix |
| CHOC | [choc_ObjectiveCHelpers.h doesn't include <chrono>](choc-chrono.md) | A new issue or pull request on GitHub | No | `cmake/PatchChoc.cmake` makes the fix |
| clap-wrapper | [AUv2 view classes lose their capitals](clap-wrapper-view-class-capitals.md) | A new issue and pull request on GitHub | No | SymbolEngine's fork has the fix |
| clap-validator | [A plugin whose only parameter is a switch fails param-set-events](clap-validator-lone-switch.md) | A new issue on GitHub | Yes, as #42 | Its test plugins have a second parameter |
| clap-validator | [param-conversions divides by zero on a plugin with no parameters](clap-validator-no-parameters.md) | A comment on pull request #40, which fixes it | Yes | Its test plugins have parameters |

[clap-validator-test-plugin.c](clap-validator-test-plugin.c) is the plain-C plugin both clap-validator reports reproduce with.
