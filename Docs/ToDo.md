# ToDo

## Where things stand

- **Foundations.** The repository, the CMake build and continuous integration on Mac, Windows and Linux are in place, with a contributing guide and issue and pull request templates. The core module has its first parts: values with JSON, XML documents, UTF-8 text, paths and the system's folders, files written safely, settings files, SHA-256, and speaker layouts.
- **Audio files.** The files module reads and writes WAV, including RF64 past 4 GB with its broadcast extension and other chunks kept, AIFF and AIFF-C, and FLAC, Ogg Vorbis and Opus with their tags, a stretch at a time, and writes a recording from an audio device's thread, flushed every second. It reads MP3 gaplessly, and M4A, AAC and CAF through the systems' own decoders on Mac and Windows.
- **Web pages.** The ui module shows pages in the system's own web view on Mac and Windows, served from files built into the program, with calls and events between page and native code, in a program's own windows or a plugin host's. Plugin editors are web pages built on it, and the Filter example has one.
- **Programs.** The ui module runs the message loop, with calls to its thread from any other and timers, and the app module makes a desktop program of an executable on Mac and Windows: the system's requests to quit, to open files and to come back, one instance at a time with a second handing its arguments to the first, and the application bundle or the Windows program built by `symbolengine_add_app`. The Hello example is one.
- **Plugins.** The plugin module builds CLAP, VST3 and Audio Unit plugins from a class written once, and keeps a VST3 class ID and Audio Unit codes a plugin was released with. Buses take any standard layout from mono to 7.1.4 in every format, Dolby's 9.1.4 and 9.1.6 in VST3 and Audio Units, and the ITU 9.1.6 in CLAP and VST3, through SymbolEngine's fork of clap-wrapper. The Gain and Filter examples pass CLAP's validator, Steinberg's, pluginval at its strictest level and auval. A plugin released before keeps the state hosts saved of it and its parameter numbers, as Symbol Studio's four plugins do, checked against a Live set saved with their earlier builds.
- **Devices.** The devices module's lessons are written into the architecture, every problem audio software has met with Core Audio, Windows Audio and ASIO and in the layer above them, each with its test. The layer above the systems is built and passes its tests against a scripted fake system: device lists, saved setups found again, each stream's control thread and events, and a timeline that counts every lost frame and says every dropout, stall and loss. Core Audio lists a Mac's devices and plays through them, and Windows Audio lists a PC's endpoints, plays through them and records from them, its input losing nothing to a thread held up for 50 ms. A player plays files through them with a read-ahead, at the device's rate whatever the file's. A Mac's input is refused when the user hasn't allowed the microphone, where Core Audio would give silence. ASIO is built with the developer's SDK and passes its tests against fake drivers, and records from the owner's Focusrite through its own driver. Symbol Studio plays and records through all of it, an hour's take on Windows within 1 part per million of the clock. The rest of each system's section comes next.
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
- [x] An Audio Unit's state saved straight after the host sets a value has that value, with no render between, as before it is initialized, initialized with nothing rendered, after the host stops and while it plays, never leaving a block silent (`Tests/AudioUnitHost/State.cpp`).
- [x] Dolby's 9.1.4 and 9.1.6 in VST3 and Audio Units, their wide pair passed between a plugin and the fork's wrappers by an extension of the fork's own, since CLAP has no numbers for it; a CLAP host is never given them.
- [ ] Dolby's 9.1.x in AAX, when AAX is built: the fork's AAX wrapper reads 5.1's surround pair as the side pair, where VST3 and Audio Units read it as the rear pair, which it has to agree with first.
- [x] Parameters hosts see and record from 0 to 1 on a linear or ratio scale, shown in their own units, as plugins already released with such parameters have them.
- [x] Editors that tick up to 120 times a second, and the plugin's own functions changing parameters in gestures the host records; core opens web addresses in the browser.
- [x] Its earlier versions' saved state and parameter numbers kept, an Audio Unit's state under the key its earlier versions used, checked against a Live set saved with them.
- [x] Example: a gain, passing every validator on every system.
- [x] Example: a filter with an editor and a level meter.
- [ ] Example: a meter.
## Phase 2: Core, files and the app

- [x] core: values and JSON, XML documents, UTF-8 text, paths and the system's folders, files written safely, settings files, SHA-256, speaker layouts, a lock-free fifo, a guard against denormal numbers, and a resampler.
- [x] ui: the message loop, calls to the message thread from any other, and timers.
- [ ] core: threads and FIFOs, logging, child processes.
- [x] files: WAV, with RF64 and BW64 past 4 GB and WAVE_FORMAT_EXTENSIBLE, its broadcast extension, iXML and other chunks kept, and AIFF and AIFF-C, read and written a stretch at a time, checked against ffmpeg's and Apple's files.
- [x] files: FLAC, Ogg Vorbis and Opus, with their tags, through Xiph.Org's libraries built into the module, checked against ffmpeg's and the flac tool's files and read back by them.
- [x] files: MP3, read gaplessly where LAME's or Xing's information says where the audio lies, and M4A, AAC and CAF through the systems' decoders on Mac and Windows, checked against LAME's, Apple's and ffmpeg's files.
- [x] files: a recording written from an audio device's thread, its WAV or AIFF header rewritten and put on disk every second so a take survives a crash, a WAV passing 4 GB as RF64 in place, and each stretch given as the file holds it.
- [ ] core's JSON tests on a PC's Visual Studio Build Tools, whose preprocessor takes the tests' raw strings inside Catch2's macros as escapes; GitHub's Visual Studio builds them.
- [ ] files: M4A, AAC and CAF on Linux.
- [ ] files: on GitHub's Windows runner, reading AAC in ADTS through Media Foundation sometimes never returns; each test has ten minutes, so a hang fails by name, as it has twice more. It never hung in 150 runs on the owner's PC, the slowest 8.7 s, so it can't be found there: give the system decoders a bound of their own, the source reader asynchronous and each read waited on for a set time, so a decoder that stops answering is an error in a test and in a program, never a hang.
- [ ] files: libvorbis' own `vorbisenc.c` again once Xiph.Org takes the fix for its 5.1 encoder's leak ([the report](Upstream/libvorbis-51-residue-leak.md)).
- [x] ui: the web view on Mac and Windows, with pages served from files built into the program, and calls and events between page and native code.
- [x] ui: a program's own windows holding a page, sized alike on every screen, their places held to a screen, see-through until the page draws; functions a page calls that answer later; WebView2 given the keyboard with its window.
- [x] ui: the system's own dialogs, questions with buttons of the program's naming and choosers for files and folders.
- [x] ui: the Mac's menu bar, with shortcuts, ticks and submenus; windows maximized and back, keeping where they were, with Windows 11 title bar colors.
- [x] app: a desktop program on Mac and Windows, with its arguments as UTF-8, the system's requests to quit, to open files and to come back, one instance at a time with a second handing over its arguments, the Dock icon, the recent documents, and `symbolengine_add_app` building the bundle and the Windows program with its version information; the Hello example.
- [ ] ui and app: drag and drop, clipboard, keyboard, worker processes; Linux's web view and message loop.

## Phase 3: Devices

- [x] First the lessons, then the code: the problems audio software has met with devices, read from other frameworks' and products' histories of fixes, the system makers' documentation and Symbol Studio's own long takes, each written into the architecture's [devices](02_architecture.md#devices) section with the test that fails when the module doesn't do it.
- [x] The system layer, `AudioSystem`, and `FakeAudioSystem`, the scripted fake that takes its place in the tests: devices that call back when a test says or on threads of their own, fail, hang or change on cue, and send the system's notifications from any thread; the devices section's tests written against it first, failing.
- [x] Above the layer, written once: device lists read without opening a device and identity (`AudioDevices`), setups saved and resolved direction by direction (`AudioSetup`), each stream's control thread with its events, the callback's contract, and the timeline with its dropouts and watchdog, sleep, and a device changing or lost under a stream (`AudioStream`); core's `LockFreeFifo` and `NoDenormals`.
- [ ] An input and an output on two clocks, the drift measured and corrected on the playback side.
- [ ] Streams that follow the system's default output, moving when it changes while an input never moves during a take.
- [ ] A recorder that captures real hardware's sequences of calls and notifications for the fake to replay, such as a Bluetooth headset switching profiles.
- [x] Core Audio's first part: devices listed from their properties alone, with each one's channels across its streams, its rates within the ranges it gives and its block sizes; streams in and out taking each channel from the stream that holds it, a rate or block size set, waited for and read back, the inputs' streams switched off for a stream that only plays, host times converted by the machine's timebase, and the device's changes, its loss, its overloads, the system's notices and the computer's sleep passed on; checked on a Mac with its five devices listed and silence through its built-in speakers at the device's own rate.
- [x] The microphone's permission on a Mac: `microphoneAccess()` and `askForMicrophone()`, an input not allowed refused at its open with an error of its own, since Core Audio gives it silence, and one not yet asked asked as it starts, a no ending the stream. Two seconds from the owner's Fireface's Input 1 came in at 48 kHz, continuous, with no dropouts and its rate within half a percent of its host times; the MacBook's own microphone gave exact silence with its lid closed, where Apple cuts it off.
- [ ] Core Audio's rest: the process's input mute shown, an input giving exact silence for seconds said, an idle output stopped, Multi-Output devices and the user's aggregates, and audio workgroups for worker threads.
- [x] Windows Audio's first part: endpoints listed from their properties without a client made, a shared-mode stream at the system's mix rate and never another, an input's buffer as long as its thread may be held up, every packet taken on each wake with its flags and device position, the raw stream asked for when recording, exclusive mode trying the formats a device may take with the alignment Windows asks for and never shared mode in its place, the session's ends passed on with their reasons, the multimedia scheduler's Pro Audio, the system's notices and the computer's sleep, and COM kept to the process's multithreaded apartment so no caller's is touched; checked on a PC with its four endpoints listed, silence through a monitor's speakers at the device's rate on a Pro Audio thread, and a USB interface's input recorded with its thread held up 50 ms every two seconds losing nothing, and 400 ms losing 0.21 s, said as one dropout where it fell.
- [ ] Windows Audio's rest, checked on a PC: exclusive mode, the raw stream, sleep, unplugging during a take, another program taking the device for its own, and a Bluetooth headset's hands-free endpoints.
- [x] ASIO with the developer's SDK, given as `SYMBOLENGINE_ASIO_SDK`: drivers listed from the registry unread, one loaded when a stream opens it and a second refused, every call made from one thread of its own in ASIO's order, a fault or a call that never returns let go of and said, the buffer switch reading that half's input, calling the program and writing that half's output, resets and resyncs opening the stream again at the size the driver now prefers, rates set and read back with the channels read again at them, a rate of 0 the clock lost, every sample type converted exactly, and a size refused tried once more at the preferred one. Its 16 tests against fake drivers pass on the owner's PC and GitHub's, six breakages each caught by its test, output written to the other half among them, and on the PC's own drivers listing loaded none, the Fireface's leftover entry, whose COM class isn't registered, listed as not loadable, and two seconds from the Focusrite USB ASIO driver's Inputs 1 and 2 came in at 44.1 kHz in blocks of 256, 549 frames of input latency, continuous, live and within half a percent of its rate.
- [x] ASIO's rest: the driver's control panel through `AudioStream::showDeviceSettings()`, opened from the driver's thread without the program waiting, a reset asked while it is open held until it closes and the stream then opened again once at what was chosen; a reset asked as every open makes its buffers answered once, never in a loop, and one asked as the stream stops reopening nothing; a driver calling back after its buffers are let go of reaching nothing; what drivers write into laid before a guard page, so one writing 8 KB past it faults and is let go of; and Symbol Studio's input kept to the output's own driver on ASIO. 20 tests against fake drivers, and the panel's and the guard page's each fail against the layer without them.
- [ ] ALSA, PipeWire and JACK, after the first release.
- [x] Files played through a device with a read-ahead, as the devices section's [playing files](02_architecture.md#playing-files) says (`AudioPlayer`), at the device's rate through core's `Resampler`, a windowed sinc within -110 dB of the ideal at 1 kHz.
- [ ] A loopback device of SymbolEngine's own for the Mac runner, a Core Audio server plug-in installed for the run.
- [ ] What no source settles, measured on real devices before the module depends on it ([measured before it is relied on](02_architecture.md#measured-before-it-is-relied-on)).
- [ ] Long-take tests on real devices: overnight takes on a Mac and on Windows through a USB interface with the output on another device, each checked sample by sample against the interface's clock, with sleep, unplugging and another program changing the rate on the way.

## Phase 4: Hosting

- [ ] First the lessons, then the code: the problems hosts have met with other makers' plugins, read from other hosts' fix histories and from Symbol Studio's own scans of a large plugin library, each written into the architecture's [hosting](02_architecture.md#hosting) section and each a test.
- [ ] VST3, AU v2 and v3, and CLAP hosting, with scanning in worker processes and everything in the architecture's [hosting](02_architecture.md#hosting) section.
- [ ] A test library of free plugins scanned, made, played, saved and restored in continuous integration, and the owner's full plugin library checked locally.

## Phase 5: 1.0

- [ ] Every module documented with a guide and an example; the 1.0 release announced to the audio developer community.
- [ ] A code of conduct, once others take part in the issues and discussions.
