# Upstream reports

Bugs found in the libraries and tools SymbolEngine builds on, each written up for the project's own tracker, with what SymbolEngine does about it meanwhile. Each report opens by saying where it goes, and its body is ready to paste there.

| Project | Report | Where it goes | Filed | SymbolEngine meanwhile |
|---|---|---|---|---|
| libvorbis | [The 5.1 encoder leaks the LFE's residue](libvorbis-51-residue-leak.md) | A comment on Xiph's GitLab issue #2357 | Yes | Builds a copy of `vorbisenc.c` that frees it |
| clap-validator | [A plugin whose only parameter is a switch fails param-set-events](clap-validator-lone-switch.md) | A new issue on GitHub | Not yet | Its test plugins have a second parameter |
| clap-validator | [param-conversions divides by zero on a plugin with no parameters](clap-validator-no-parameters.md) | A comment on pull request #40, which fixes it | Not yet | Its test plugins have parameters |

[clap-validator-test-plugin.c](clap-validator-test-plugin.c) is the plain-C plugin both clap-validator reports reproduce with.
