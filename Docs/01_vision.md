# Vision

## Why it exists

Most of what a framework for audio software needs is now free for closed-source products: Steinberg moved the VST3 SDK to MIT in October 2025, CLAP and its wrapper to VST3, AU and AAX are MIT, Apple's Audio Unit SDK is Apache 2.0, and CHOC, a library of web views, devices and audio files, is ISC. SymbolEngine puts them together into one framework that builds plugins, hosts other makers' plugins, runs audio devices well enough to record on, and makes full desktop apps, with nothing to pay and nothing to register.

It is given to the audio developer community, free.

## Who it is for

- Plugin makers, from one person to a company, who want every format from one codebase without a license fee or a revenue limit.
- Makers of audio applications: editors, recorders, DAWs, hosts and tools, which need devices, plugin hosting, files and a desktop app around them.
- Web developers coming to audio, whose interfaces are HTML, CSS and JavaScript.
- Symbol Studio, its first user, whose needs set the order of the work.

## What it promises

- **Free for anything.** One permissive license, no revenue limits, no seats, no splash screen and no registration, for closed and open source alike.
- **Complete for audio.** Plugins in CLAP, VST3, AU and AAX, and standalone; hosting other makers' VST3, AU and CLAP plugins, scanned safely in worker processes; audio devices on every system, ASIO included; audio files; desktop apps with windows, menus, dialogs and drag and drop. It does audio, and leaves general-purpose GUI widgets to the web.
- **Proven, never theoretical.** A part is done when Symbol Studio runs on it and passes its checks, and when the plugin validators pass every example. Problems found in real plugins and real devices are fixed in the framework, with a test that would catch them again.
- **Modern C++.** C++20 and its standard library for strings, files and threads instead of a framework's own copies, no global singletons the user doesn't create, and modules usable on their own.
- **Easy to move to.** A plugin that already ships keeps the IDs hosts know it by, receives the state its earlier versions saved and keeps the parameter numbers its automation was recorded against, so its users' sessions and presets open as they did.
- **Interfaces as web pages.** Plugin editors and app screens are HTML, CSS and JavaScript in the system's own web view, with a typed bridge to the native side.
- **Documented.** Every module has a guide and an example that builds, and the reference is generated from the headers.

## How it is run

It is meant to be good enough that people choose it for itself, and it is given back to the community that audio software is built by. It is maintained for the long run, not released and left. It is updated as the systems, plugin formats and SDKs move, with releases on a published schedule and notes for every change. Collaborators are invited from the start: the roadmap, the issues and the discussion are public, contributing is documented, and people who contribute well become maintainers. It stays free, and no part of it is ever moved behind a paid tier.

## License

SymbolEngine's license is MIT. It is a product of Cyberhost LLC, which holds its copyright; the license and each file's notice name Cyberhost LLC. Everything it depends on is under a permissive license: MIT, ISC, BSD, zlib, Boost or Apache 2.0. SDKs under other terms, Avid's AAX SDK and Steinberg's ASIO SDK, are never included: a developer who wants them supplies them, as each SDK's own license requires. The framework is written from Apple's, Microsoft's and Steinberg's documentation and the SDKs themselves.

## Platforms

Mac (Apple Silicon and Intel) and Windows first, with Linux as a first-class platform for the plugins and apps that want it, its continuous integration running from the start, since public repositories get GitHub's own Mac, Windows and Linux runners free.

## The name

SymbolEngine, one word. Its home is [symbolengine.dev](https://symbolengine.dev), and its code is at [github.com/symbol-engine](https://github.com/symbol-engine).
