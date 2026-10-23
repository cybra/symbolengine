# ToDo

## Where things stand

- **Foundations.** The repository, the CMake build and continuous integration on Mac, Windows and Linux are in place, with a contributing guide and issue and pull request templates. The core module has its first parts: values with JSON, UTF-8 text, paths and the system's folders, files written safely, and settings files.
- **Plugins.** The plugin module builds CLAP plugins from a class written once, and the Gain example passes CLAP's validator on Mac, Windows and Linux. VST3, Audio Units and standalone come next, through clap-wrapper, then the editor as a web page.
- **The first release** comes when Symbol Studio runs on SymbolEngine alone, and the phases below follow what it needs, plugins first and hosting other makers' plugins last.
- **Home.** The name is SymbolEngine, symbolengine.dev is registered, the license is MIT, and the code is public at github.com/cybra/symbolengine. Contributions are signed off under the Developer Certificate of Origin.

## Open questions

1. **Support.** Whether to accept sponsorship, such as GitHub Sponsors, while everything stays free.
2. **Security reports.** Should GitHub's private vulnerability reporting be switched on, so a security problem can be reported privately instead of in a public issue? Recommended: yes, with a short security policy pointing to it. Until then, the repository has no security policy.

## Phase 0: Foundations

- [x] License: MIT. Code at github.com/cybra/symbolengine.
- [x] A contributing guide with the sign-off, and issue and pull request templates.
- [ ] A security policy (open question 2).
- [x] CMake build that a project takes in with `FetchContent` or `find_package`, each module a target; C++20; warnings as errors in continuous integration.
- [x] Continuous integration on Mac, Windows and Linux on every push, with sanitizers on Linux and a project outside SymbolEngine building against it.
- [ ] Documentation site generated from the guides and the headers.

## Phase 1: Plugins

- [x] The plugin interface, shaped like CLAP: parameters with permanent IDs, state, buses, latency, tail, and processing split where parameters change.
- [ ] The editor as a web page with the typed bridge.
- [x] CLAP, with CLAP's validator in continuous integration on every system.
- [ ] VST3, AU v2, AU v3, AAX (with the developer's SDK) and standalone through clap-wrapper.
- [ ] A plugin's own VST3 class ID and Audio Unit codes, its earlier versions' saved state and its parameter numbers kept ([moving an existing plugin](02_architecture.md#moving-an-existing-plugin)).
- [x] Example: a gain.
- [ ] Examples: a filter with an editor, a meter; every validator passing on every system.
- [ ] Symbol Studio's first plugin built on it.

## Phase 2: Core, files and the app

- [x] core: values and JSON, UTF-8 text, paths and the system's folders, files written safely, settings files.
- [ ] core: the message loop, timers, threads and FIFOs, logging, child processes.
- [ ] files: WAV with BWF and iXML, AIFF, FLAC, Ogg Vorbis, MP3 reading, the systems' decoders for AAC, M4A and CAF, streaming readers.
- [ ] ui and app: windows, native menus, dialogs, file choosers, drag and drop, clipboard, keyboard, one instance, worker processes.

## Phase 3: Devices

- [ ] Core Audio, Windows Audio shared and exclusive, and ASIO with the developer's SDK; ALSA, PipeWire and JACK after the first release.
- [ ] The recording lessons in the architecture's [devices](02_architecture.md#devices) section built in, and long-take tests on real devices.

## Phase 4: Hosting

- [ ] VST3, AU v2 and v3, and CLAP hosting, with scanning in worker processes and everything in the architecture's [hosting](02_architecture.md#hosting) section.
- [ ] A test library of free plugins scanned, made, played, saved and restored in continuous integration, and the owner's full plugin library checked locally.

## Phase 5: 1.0

- [ ] Every module documented with a guide and an example; the 1.0 release announced to the audio developer community.
- [ ] A code of conduct, once others take part in the issues and discussions.
