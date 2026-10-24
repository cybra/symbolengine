# Upstream reports

Bugs found in the libraries SymbolEngine builds on, each written up for the library's own tracker, with what SymbolEngine does about it meanwhile. Each report opens by saying where it goes, and its body is ready to paste there.

| Library | Report | Where it goes | Filed | SymbolEngine meanwhile |
|---|---|---|---|---|
| libvorbis | [The 5.1 encoder leaks the LFE's residue](libvorbis-51-residue-leak.md) | A comment on Xiph's GitLab issue #2357 | Not yet | Builds a copy of `vorbisenc.c` that frees it |
