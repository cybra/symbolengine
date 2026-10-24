# ToDo

## Where things stand

- **Foundations.** The repository, the CMake build and continuous integration on Mac, Windows and Linux are in place, with a contributing guide and issue and pull request templates. The core module has its first parts: values with JSON, XML documents, UTF-8 text, paths and the system's folders, files written safely, settings files, SHA-256, and speaker layouts.
- **Web pages.** The ui module shows pages in the system's own web view on Mac and Windows, served from files built into the program, with calls and events between page and native code. Plugin editors are web pages built on it, and the Filter example has one.
- **Plugins.** The plugin module builds CLAP, VST3 and Audio Unit plugins from a class written once, and keeps a VST3 class ID and Audio Unit codes a plugin was released with. Buses take any standard layout from mono to 7.1.4 in every format, Dolby's 9.1.4 and 9.1.6 in VST3 and Audio Units, and the ITU 9.1.6 in CLAP and VST3, through SymbolEngine's fork of clap-wrapper. The Gain and Filter examples pass CLAP's validator, Steinberg's, pluginval at its strictest level and auval. A plugin released before keeps the state hosts saved of it and its parameter numbers, as Symbol Studio's four plugins do, checked against a Live set saved with their earlier builds.
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
- [x] The editor as a web page, on Mac and Windows, with parameters, gestures the host records as automation, and the plugin's own functions and events.
- [x] CLAP, with CLAP's validator in continuous integration on every system.
- [x] VST3 and AU v2 through clap-wrapper, checked by Steinberg's validator, pluginval and auval.
- [ ] AU v3, AAX (with the developer's SDK) and standalone through clap-wrapper.
- [x] A plugin's own VST3 class ID and Audio Unit codes kept ([moving an existing plugin](02_architecture.md#moving-an-existing-plugin)).
- [x] Editors resized by the page and the host within their limits, their size remembered per instance and for new editors; snapshots for scripted checks on a Mac; pages read live from disk in debug builds.
- [ ] Snapshots on Windows, and Space reaching the host's transport while an editor has the keyboard, checked in Live on Windows; it does on a Mac.
- [x] Buses in any standard layout from mono to 7.1.4 and the ITU 9.1.6, or the plugin's own, through CLAP's surround and configurable audio ports and clap-wrapper's VST3 speaker arrangements, with Steinberg's validator checking exactly which arrangements a plugin takes.
- [x] Audio Unit layouts through SymbolEngine's fork of clap-wrapper: the layouts a plugin takes offered to the host up to Atmos 7.1.4, Logic's channel orders among them, and each channel handed to the plugin as its speaker, checked by auval and by a host in the tests.
- [ ] Audio Unit layouts checked in Logic itself.
- [x] Dolby's 9.1.4 and 9.1.6 in VST3 and Audio Units, their wide pair passed between a plugin and the fork's wrappers by an extension of the fork's own, since CLAP has no numbers for it; a CLAP host is never given them.
- [ ] Dolby's 9.1.x in AAX, when AAX is built: the fork's AAX wrapper reads 5.1's surround pair as the side pair, where VST3 and Audio Units read it as the rear pair, which it has to agree with first.
- [x] Parameters hosts see and record from 0 to 1 on a linear or ratio scale, shown in their own units, as plugins already released with such parameters have them.
- [x] Editors that tick up to 120 times a second, and the plugin's own functions changing parameters in gestures the host records; core opens web addresses in the browser.
- [x] Its earlier versions' saved state and parameter numbers kept, an Audio Unit's state under the key its earlier versions used, checked against a Live set saved with them.
- [x] Example: a gain, passing every validator on every system.
- [x] Example: a filter with an editor and a level meter.
- [ ] Example: a meter.
## Phase 2: Core, files and the app

- [x] core: values and JSON, XML documents, UTF-8 text, paths and the system's folders, files written safely, settings files, SHA-256, speaker layouts.
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
