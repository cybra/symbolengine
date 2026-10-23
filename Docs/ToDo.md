# ToDo

## Where things stand

- **Planning.** The vision and architecture are written. The name is SymbolEngine, symbolengine.dev is registered, the license is MIT, and its home is the `symbol-engine` organization on GitHub, public from its first commit.
- **First product on it:** a new Symbol Studio plugin, built on the framework from its first line, so the plugin module is proven by a real product from the start.

## Open questions

1. **Contributions.** A Developer Certificate of Origin sign-off on each commit, without a contributor agreement, is the recommendation, so contributions come in under the same license they go out under.
2. **Support.** Whether to accept sponsorship, such as GitHub Sponsors, while everything stays free.

## Phase 0: Foundations

- [x] License: MIT. Home: the `symbol-engine` organization on GitHub.
- [ ] Repository with the license, a contributing guide, a code of conduct, issue and pull request templates, and the public roadmap.
- [ ] CMake build that a project takes in with `FetchContent` or `find_package`, each module a target; C++20; warnings as errors in continuous integration.
- [ ] Continuous integration on Mac, Windows and Linux on every push.
- [ ] Documentation site generated from the guides and the headers.

## Phase 1: Plugins

- [ ] The plugin interface, shaped like CLAP: parameters with permanent IDs, state, buses, latency, processing, and the editor as a web page with the typed bridge.
- [ ] CLAP, VST3, AU v2, AU v3, AAX (with the developer's SDK) and standalone through clap-wrapper.
- [ ] A plugin's own VST3 class ID and Audio Unit codes, its earlier versions' saved state and its parameter numbers kept ([moving an existing plugin](02_architecture.md#moving-an-existing-plugin)).
- [ ] Examples: a gain, a filter with an editor, a meter; every validator passing on every system.
- [ ] Symbol Studio's first plugin built on it.

## Phase 2: Core, files and the app

- [ ] core: text, paths, values and JSON, the message loop, timers, threads and FIFOs, logging, settings.
- [ ] files: WAV with BWF and iXML, AIFF, FLAC, Ogg Vorbis, MP3 reading, the systems' decoders for AAC, M4A and CAF, streaming readers.
- [ ] ui and app: windows, native menus, dialogs, file choosers, drag and drop, clipboard, keyboard, one instance, worker processes.

## Phase 3: Devices

- [ ] Core Audio, Windows Audio shared and exclusive, ASIO with the developer's SDK, ALSA, PipeWire and JACK.
- [ ] The recording lessons in the architecture's [devices](02_architecture.md#devices) section built in, and long-take tests on real devices.

## Phase 4: Hosting

- [ ] VST3, AU v2 and v3, and CLAP hosting, with scanning in worker processes and everything in the architecture's [hosting](02_architecture.md#hosting) section.
- [ ] A test library of free plugins scanned, made, played, saved and restored in continuous integration, and the owner's full plugin library checked locally.

## Phase 5: 1.0

- [ ] Every module documented with a guide and an example; the 1.0 release announced to the audio developer community.
