# ToDo

## Where things stand

- **Foundations.** The repository, the CMake build and continuous integration on Mac, Windows and Linux are in place, with a contributing guide and issue and pull request templates. The core module has its first parts: values with JSON, UTF-8 text, paths and the system's folders, files written safely, and settings files.
- **Web pages.** The ui module shows pages in the system's own web view on Mac and Windows, served from files built into the program, with calls and events between page and native code; plugin editors are built on it next.
- **Plugins.** The plugin module builds CLAP, VST3 and Audio Unit plugins from a class written once, and keeps a VST3 class ID and Audio Unit codes a plugin was released with. The Gain example passes CLAP's validator, Steinberg's, pluginval at its strictest level and auval. The editor as a web page comes next.
- **The first release** comes when Symbol Studio runs on SymbolEngine alone, and the phases below follow what it needs, plugins first and hosting other makers' plugins last.
- **Home.** The name is SymbolEngine, symbolengine.dev is registered, the license is MIT, and the code is public at github.com/cybra/symbolengine. Contributions are signed off under the Developer Certificate of Origin.

## Open questions

1. **Support.** Whether to accept sponsorship, such as GitHub Sponsors, while everything stays free.

## Phase 0: Foundations

- [x] License: MIT. Code at github.com/cybra/symbolengine.
- [x] A contributing guide with the sign-off, and issue and pull request templates.
- [ ] GitHub's private vulnerability reporting and a short security policy pointing to it, nearer the first release.
- [x] CMake build that a project takes in with `FetchContent` or `find_package`, each module a target; C++20; warnings as errors in continuous integration.
- [x] Continuous integration on Mac, Windows and Linux on every push, with sanitizers on Linux and a project outside SymbolEngine building against it.
- [ ] Documentation site generated from the guides and the headers.

## Phase 1: Plugins

- [x] The plugin interface, shaped like CLAP: parameters with permanent IDs, state, buses, latency, tail, and processing split where parameters change.
- [ ] The editor as a web page with the typed bridge.
- [x] CLAP, with CLAP's validator in continuous integration on every system.
- [x] VST3 and AU v2 through clap-wrapper, checked by Steinberg's validator, pluginval and auval.
- [ ] AU v3, AAX (with the developer's SDK) and standalone through clap-wrapper.
- [x] A plugin's own VST3 class ID and Audio Unit codes kept ([moving an existing plugin](02_architecture.md#moving-an-existing-plugin)).
- [ ] Its earlier versions' saved state and parameter numbers kept, checked against sessions saved with them.
- [x] Example: a gain, passing every validator on every system.
- [ ] Examples: a filter with an editor, a meter.
- [ ] Symbol Studio's first plugin built on it.

## Phase 2: Core, files and the app

- [x] core: values and JSON, UTF-8 text, paths and the system's folders, files written safely, settings files.
- [ ] core: the message loop, timers, threads and FIFOs, logging, child processes.
- [ ] files: WAV with BWF and iXML, AIFF, FLAC, Ogg Vorbis, MP3 reading, the systems' decoders for AAC, M4A and CAF, streaming readers.
- [x] ui: the web view on Mac and Windows, with pages served from files built into the program, and calls and events between page and native code.
- [ ] ui and app: windows, native menus, dialogs, file choosers, drag and drop, clipboard, keyboard, one instance, worker processes; Linux's web view.

## Phase 3: Devices

- [ ] Core Audio, Windows Audio shared and exclusive, and ASIO with the developer's SDK; ALSA, PipeWire and JACK after the first release.
- [ ] The recording lessons in the architecture's [devices](02_architecture.md#devices) section built in, and long-take tests on real devices.

## Phase 4: Hosting

- [ ] VST3, AU v2 and v3, and CLAP hosting, with scanning in worker processes and everything in the architecture's [hosting](02_architecture.md#hosting) section.
- [ ] A test library of free plugins scanned, made, played, saved and restored in continuous integration, and the owner's full plugin library checked locally.

## Phase 5: 1.0

- [ ] Every module documented with a guide and an example; the 1.0 release announced to the audio developer community.
- [ ] A code of conduct, once others take part in the issues and discussions.
