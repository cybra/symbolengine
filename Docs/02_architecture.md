# Architecture

## Modules

Each module is a CMake target usable on its own, with its own tests, guide and example, and depends only on the modules under it.

| Module | What it does | Built on |
|---|---|---|
| core | UTF-8 text helpers over `std::string`, paths over `std::filesystem`, values with JSON, the message loop and timers, threads, spin locks, lock-free FIFOs, logging, settings files written safely | C++20, CHOC |
| files | Reading and writing audio files: WAV with BWF and iXML, AIFF, FLAC, Ogg Vorbis; reading MP3; AAC, M4A and CAF through the system's own decoders; streaming readers for files of any length | CHOC, AudioToolbox, Media Foundation |
| devices | Audio and MIDI devices: Core Audio, Windows Audio in shared and exclusive mode, ASIO with the developer's own SDK, ALSA, PipeWire and JACK; device lists and changes, latency in and out, inputs and outputs run apart or together | System APIs, CHOC's device layer where it fits |
| ui | The system's web view (WKWebView, WebView2, WebKitGTK) with a typed bridge: native functions the page calls and awaits, events the native side sends, and resources served from memory; windows, native menus, dialogs, file choosers, drag and drop, the clipboard, keyboard handling | CHOC's web view, AppKit, Win32, GTK |
| app | A desktop application around it: one instance or many, worker processes with a message channel, opening files from the system, updates' hooks | core, ui |
| plugin | Plugins written once against a CLAP-shaped C++ interface (parameters, state, buses, latency, processing, an editor as a web page) and built as CLAP, VST3, AU v2 and v3, AAX and standalone | CLAP, clap-wrapper, VST3 SDK, AudioUnitSDK, the developer's AAX SDK |
| host | Hosting other makers' plugins: VST3, AU v2 and v3, CLAP; scanning in worker processes; a plugin made in the background or at once, on the message thread or out of process for AU v3; buses, state, parameters, latency, offline rendering, and its editor in a window | VST3 SDK, AudioToolbox, CLAP |

## Threads and real time

Three kinds of thread, named in every interface: the message thread, which owns windows, web views and plugin editors; the audio thread, which never allocates, locks or waits, and is checked for it in debug builds; and worker threads for everything slow. Every callback says which thread it is on. Nothing is a global singleton the user didn't make; a program creates its message loop, devices and hosts and passes them where they are needed.

## Interfaces

A plugin's editor and an app's screens are web pages in the system's web view, loaded from resources built into the binary; meters and spectrograms drawn this way run at 60 frames a second. The bridge is typed both ways, its native functions and events declared once and checked on both sides. A native drawing surface for interfaces that can't be web pages is a later question, not a starting point.

## Plugins

A plugin is written once against one interface shaped like CLAP, and clap-wrapper turns it into every other format, so a fix lands in every format at once. Parameters are declared once, with their IDs kept forever, and drive automation, the editor and saved state. The validators (CLAP's, Steinberg's, Apple's auval, and pluginval run as a separate tool) run on every example in continuous integration on every system.

## Moving an existing plugin

A plugin that already ships keeps what hosts and its users have of it. It states its own identity, the VST3 class ID and the Audio Unit type, subtype and manufacturer codes it was released with, which clap-wrapper passes to each format, so a DAW session finds it. It receives the state its earlier versions saved, exactly as the host stored it, so it can open presets and sessions with their settings, and it keeps the parameter numbers hosts recorded automation against.

## Hosting

Plugins are scanned in worker processes, one plugin at a time per worker, so a plugin that crashes or hangs fails alone; a worker that loses its link to the host ends at once. What was found and what failed are kept, so a scan looks only at what is new, and a scan cut short carries on next time. A plugin is made with stereo in and out where it takes them, and one that describes its buses only once an arrangement is set on them is given stereo first, as a DAW does. A plugin with no audio out is set aside with a reason instead of silencing what plays through it. Its editor opens in a window kept above the app's, which steps back while the app asks a question, and Space reaches the app's transport even when the plugin's view takes every key, unless text is being typed. Each of these comes from a real plugin and has a test.

## Devices

Lessons from recording on real devices are built in: an input runs apart from the output when the two can't share a pace; a shared-mode Windows input asks for a long enough buffer and passes on what built up after a late turn; a take records the time the computer slept or the device dropped out, and says so; latency in and out is measured, not only reported, so recordings line up.

## Testing

Unit tests for every module, the plugin validators on every example, and integration tests that open real devices where the runner has them. Continuous integration builds and tests on Mac, Windows and Linux on every push. Symbol Studio's own checks, run against each release, are the proof that the parts work together in a real product.

## Dependencies

Only permissive licenses: MIT, ISC, BSD, zlib, Boost and Apache 2.0, each fetched by CMake and pinned by checksum. SDKs under other terms, AAX and ASIO, are supplied by the developer and never included.
