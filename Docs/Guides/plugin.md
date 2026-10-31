# Building plugins

The plugin module builds one C++ class as a CLAP plugin, a VST3 plugin and an Audio Unit v2. You write the class once, with its parameters, state, buses and processing, and on a Mac and Windows an editor as a web page. Every format is built through CLAP, so a fix lands in all of them at once.

It is taken in with FetchContent, since it builds plugins with CMake functions of its own:

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyGain VERSION 1.0.0 LANGUAGES CXX)

# An Audio Unit is built from Objective-C++ as well.
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

include(FetchContent)
FetchContent_Declare(SymbolEngine
    GIT_REPOSITORY https://github.com/cybra/symbolengine.git
    GIT_TAG        main)
FetchContent_MakeAvailable(SymbolEngine)

symbolengine_add_plugin(MyGain
    SOURCES              MyGain.cpp
    OUTPUT_NAME          "My Gain"
    BUNDLE_IDENTIFIER    com.example.mygain
    FORMATS              CLAP VST3 AUV2
    AU_SUBTYPE           MyGn
    AU_MANUFACTURER_CODE Exmp
    AU_MANUFACTURER_NAME Example
    AU_NAME              "My Gain")
```

[`Examples/Gain`](../../Examples/Gain/Gain.cpp) is a whole plugin in 50 lines, and [`Examples/Filter`](../../Examples/Filter/Filter.cpp) adds an editor [page](../../Examples/Filter/web/editor.js) with knobs, mode buttons and a level meter. Everything below is about what they do, and what a larger plugin does beyond them.

## A first plugin

A plugin is a class derived from `Plugin`, with a static `describe` function, listed in `SYMBOLENGINE_PLUGINS` in one of its source files. This is `MyGain.cpp`:

```cpp
#include <symbolengine/plugin.h>

#include <cmath>

using namespace symbolengine;
using namespace symbolengine::plugin;

class MyGain final : public Plugin
{
public:
    static Description describe()
    {
        return {.id = "com.example.mygain",
                .name = "My Gain",
                .vendor = "Example",
                .version = "1.0.0",
                .features = {"audio-effect", "stereo"}};
    }

    MyGain() : gain(addParameter(decibels(1, "Gain", -60.0, 24.0, 0.0))) {}

    void process(Block& block) override
    {
        const auto factor = static_cast<float>(std::pow(10.0, gain.get() / 20.0));
        const auto in = block.input();
        const auto out = block.output();

        for (std::size_t channel = 0; channel < out.size(); ++channel)
            for (std::uint32_t i = 0; i < block.frames(); ++i)
                out[channel][i] = in[channel][i] * factor;
    }

private:
    ParameterValue gain;
};

SYMBOLENGINE_PLUGINS(MyGain)
```

`describe` tells hosts about the plugin before they make one. Its `id` never changes once the plugin is released, as hosts save sessions against it. `features` are CLAP's feature names, and the VST3 build's categories are worked out from them. `version` is what the class reports and `VERSION` in `symbolengine_add_plugin`, the project's if not given, is what its bundles report, so keep them alike. One binary can offer several classes, as `SYMBOLENGINE_PLUGINS(Meter, MeterRack)`.

Build it:

```sh
cmake -B build
cmake --build build --target MyGain_all
```

That leaves each format in `build/Plugins/MyGain`: `My Gain.clap`, `My Gain.vst3` and, on a Mac, `My Gain.component`. Nothing is installed, so copy them where a host looks, such as `~/Library/Audio/Plug-Ins/VST3` or `~/Library/Audio/Plug-Ins/Components` on a Mac. `MyGain_clap`, `MyGain_vst3` and `MyGain_auv2` build one format alone, and `MyGain_impl` is the static library holding your code, which `LIBRARIES` in `symbolengine_add_plugin` links anything else into. On a Mac each bundle is signed whole after every build, ad hoc unless `SYMBOLENGINE_CODESIGN_IDENTITY` names a Developer ID Application.

A release goes to Apple's notary service, for which that identity's signature has a secure timestamp and the hardened runtime, as it does here without being asked. Build with the identity set, then submit each bundle and staple the answer to it:

```sh
cmake -B build -DSYMBOLENGINE_CODESIGN_IDENTITY="Developer ID Application: My Company (TEAMID)"
cmake --build build --target MyGain_all
ditto -c -k --keepParent "build/Plugins/MyGain/My Gain.vst3" MyGain.zip
xcrun notarytool submit MyGain.zip --keychain-profile my-profile --wait
xcrun stapler staple "build/Plugins/MyGain/My Gain.vst3"
```

## Threads and blocks

A plugin has two threads that matter. `process` and `reset` belong to the audio thread, and everything else a host calls belongs to the message thread. `process` must not allocate, lock or wait, so `prepare` is the place to allocate what it needs:

```cpp
void prepare(double sampleRate, std::uint32_t maximumFrames) override
{
    rate = sampleRate;
    scratch.assign(maximumFrames, 0.0f);
    reset();
}

void reset() override { std::fill(scratch.begin(), scratch.end(), 0.0f); }
```

`reset` clears anything held from earlier audio, as when the host jumps. `release` says processing has stopped until `prepare` is called again.

The framework splits each block where the host changes a parameter, so `process` always sees the values for the samples it is given, and automation is exact to the sample. So `frames()` can be any number up to `maximumFrames`. An output may be the same memory as the input of the same number, so read a sample before writing the one at the same place. Denormal numbers are flushed to zero while `process` runs. An exception thrown in a plugin's code fails the host's call and never crashes the host.

A plugin whose output is late, or which goes on sounding after its input stops, says so:

```cpp
std::uint32_t latency() const override { return lookahead; }
std::uint32_t tail() const override { return static_cast<std::uint32_t>(rate * 2.0); }
```

The host asks for `latency` after `prepare`. When it changes, call `latencyChanged()` on the message thread, and the host asks again. `tail` may be `UINT32_MAX` for a plugin that sounds forever. `renderingOffline()` says whether the host is bouncing or exporting, where the plugin may take as long as it needs.

## Parameters

Parameters are added in the constructor, and each `addParameter` gives back the `ParameterValue` that reads it, from any thread, the audio thread included:

```cpp
MyFilter()
    : cutoff(addParameter(number(1, "Cutoff", 20.0, 20000.0, 1000.0, "Hz", 0))),
      mode(addParameter(choice(2, "Mode", {"Low Pass", "High Pass"}, 0))),
      off(addParameter(bypass(3)))
{
}
```

An id must be new, and it never changes once the plugin is released, as hosts record automation against it. Keep ids below 2^31, as a VST3 parameter's ID has no top bit. `addParameter` throws for an id given twice or a default outside its range. `parameterValue(id)` also reads a value by its id, from any thread.

| Function | What it makes |
|---|---|
| `decibels(id, name, minimum, maximum, defaultValue)` | A level, shown as "-6.0 dB" and typed with or without "dB" |
| `number(id, name, minimum, maximum, defaultValue, unit, decimals)` | A number with a unit, shown as "250 Hz" |
| `choice(id, name, choices, defaultIndex)` | One of several choices, by its index from 0, shown and typed by its name |
| `toggle(id, name, defaultOn)` | A switch, 1 for on and 0 for off, shown as "On" or "Off" |
| `bypass(id)` | The plugin's own bypass, which hosts show as theirs. Read it in `process` and pass the input through yourself |
| `scaled(id, name, scale, defaultUnits, toText, fromText)` | A value hosts see from 0 to 1, shown and typed in its units |

Anything else is a `Parameter` of your own. `group` says where hosts list it, with groups inside groups parted by `/`, and `automatable`, `stepped` and `hidden` say what they sound like. Without `toText` a value shows as a number to two decimal places, and without `fromText` typed text reads as the number it starts with:

```cpp
addParameter(Parameter{.id = 4,
                       .name = "Mix",
                       .group = "Output",
                       .minimum = 0.0,
                       .maximum = 100.0,
                       .defaultValue = 100.0,
                       .toText = [](double value) { return text::formatFixed(value, 0) + " %"; }});
```

`text::formatFixed` is in `<symbolengine/core/Text.h>`.

A parameter from `number` spreads its units evenly over its travel. `scaled` spreads them by a `Scale`, either `Scale::linear` or `Scale::ratio`, where each doubling takes the same travel, as frequency and Q are heard. Hosts see it from 0 to 1, so read its units through the scale:

```cpp
static inline const Scale frequencies = Scale::ratio(20.0, 20000.0);

MyFilter()
    : cutoff(addParameter(scaled(1, "Cutoff", frequencies, 1000.0, [](double hertz) { return text::formatFixed(hertz, 0) + " Hz"; })))
{
}

void process(Block&) override
{
    const double hertz = frequencies.toUnits(cutoff.get());
    tune(hertz);
}
```

## State

A host keeps a plugin's state with a session or a preset. By default that is the parameters' values, as JSON, so a plugin with only parameters has nothing to do. A plugin that holds more overrides both functions, and `saveParameters` and `loadParameters` keep the values in it:

```cpp
std::string saveState() const override
{
    return json::write(Object{{"parameters", saveParameters()}, {"impulse", impulseName}});
}

bool loadState(std::string_view state) override
{
    const auto saved = json::tryParse(state);
    if (!saved || !saved->isObject())
        return false;

    loadParameters((*saved)["parameters"]);
    impulseName = (*saved)["impulse"].stringOr("");
    return true;
}
```

`json::write` and `json::tryParse` are in `<symbolengine/core/Json.h>`. `loadState` says whether it could read the state. A parameter the saved state doesn't have, such as one added in a later version, takes its default, so loading the same state always gives the same sound.

Both run on the message thread, so anything `process` reads that `loadState` changes has to reach the audio thread without a lock. When something outside the parameters changes, call `stateChanged()` on the message thread, so the host knows its project has changed and asks to save it.

## Buses and layouts

A plugin has stereo in and stereo out unless `buses` says otherwise, and takes only the layouts `buses` gave unless `accepts` says otherwise. `buses` is asked once, after the plugin is made. `accepts` is asked on the message thread before a host changes a layout, with one layout for each bus. This one takes any standard layout from mono to 9.1.6, its output matching its input:

```cpp
bool accepts(const Layouts& layouts) const override
{
    const auto standard = ChannelLayout::standard();
    return layouts.inputs[0] == layouts.outputs[0] &&
           std::find(standard.begin(), standard.end(), layouts.outputs[0]) != standard.end();
}
```

`layouts()` gives each bus's layout now. It changes only while the plugin isn't prepared, so read it in `prepare`, and from there it holds for every block:

```cpp
void prepare(double, std::uint32_t maximumFrames) override
{
    const auto& output = layouts().outputs[0];

    delays.assign(output.channels(), std::vector<float>(maximumFrames));
    lfeChannel = output.channelOf(Speaker::lfe);
}
```

Channels come in the same order in every format, the order of the speakers' numbers. Dolby's 9.1.4 and 9.1.6 reach a plugin through VST3 and Audio Units only, as CLAP has no numbers for their wide pair.

The first bus in each list is the main one, and a second input is how a plugin has a sidechain. A host can give a bus no channels, if `accepts` lets it, and `block.input(1)` is empty then:

```cpp
Buses buses() const override
{
    return {.inputs = {Bus{"Input", ChannelLayout::stereo()}, Bus{"Sidechain", ChannelLayout::stereo()}},
            .outputs = {Bus{"Output", ChannelLayout::stereo()}}};
}

bool accepts(const Layouts& layouts) const override
{
    const auto& sidechain = layouts.inputs[1];
    return layouts.inputs[0] == ChannelLayout::stereo() && layouts.outputs[0] == ChannelLayout::stereo() &&
           (sidechain == ChannelLayout::stereo() || sidechain.channels() == 0);
}
```

## An editor as a web page

A plugin's editor is a web page in the host's window, on a Mac and Windows. On Linux a plugin builds and runs without its editor. The page's files go into the plugin's own library, `<target>_impl`, from a folder of your source:

```cmake
symbolengine_add_files(MyFilter_impl FOLDER web FUNCTION myEditorFiles)
```

Declare the function it defines, and return it from `editor`, which the host asks for when it opens the editor. The size is in points on a Mac and in pixels at 100% on Windows:

```cpp
std::optional<std::string_view> myEditorFiles(std::string_view path);

class MyFilter final : public Plugin
{
public:
    std::optional<Editor> editor() const override
    {
        return Editor{.width = 420, .height = 260, .files = myEditorFiles};
    }

    void process(Block&) override {}
};
```

`/index.html` is the page, and it loads two scripts that every editor is served without them being in your folder:

```html
<!doctype html>
<meta charset="utf-8">
<script src="symbolengine.js"></script>
<script src="plugin.js"></script>
<script src="editor.js" defer></script>
```

`plugin.parameters()` gives the parameters as a map from each one's id. A parameter holds its `name`, `group`, `minimum`, `maximum`, `default`, `value` and `text`, and a stepped one with up to 64 steps lists its `steps` for a menu or buttons, each with its `value` and `text`. Your handler hears every change, whether the page, the host's automation or a preset made it:

```js
(async () => {
    const parameters = await plugin.parameters();
    const gain = parameters.get(1);

    gain.onChange(() => show(gain.text));
    show(gain.text);

    // A drag is one gesture, so the host records it as one stretch of automation.
    slider.addEventListener("pointerdown", () => gain.begin());
    slider.addEventListener("input", () => gain.set(Number(slider.value)));
    slider.addEventListener("pointerup", () => gain.end());

    // A click is a gesture of its own.
    resetButton.addEventListener("click", () => gain.change(gain.default));
})();
```

For knobs and sliders, `share` gives a value as a share of its range and `valueAt` turns one back. `textFor` asks what a value would show as.

The plugin's side of the open editor is an `EditorLink`, on the message thread. `editorOpened` is the place to expose the plugin's own functions to the page, which it calls with `symbolengine.call`, and `editorClosed` says the editor has gone. A function that changes a parameter does it in a gesture, as the page does, so the host records it:

```cpp
void editorOpened(EditorLink& link) override
{
    link.expose("flatten", [&link](const Value&) -> Value {
        link.beginGesture(1);
        const auto held = link.setParameter(1, 0.0);
        link.endGesture(1);
        return held;
    });
}
```

`editorTick` is called 30 times a second while the editor is open, or as `ticksPerSecond` says from 1 to 120. It is the place to send the page what changes on its own, such as levels the audio thread has measured, which it hears with `symbolengine.on`. Changes to parameters reach the page without it. Keep what the audio thread measures in atomics, so neither side waits, as [`Examples/Filter`](../../Examples/Filter/Filter.cpp) does for its meter:

```cpp
void editorTick(EditorLink& link) override
{
    Array levels;
    for (auto& peak : peaks)
        levels.push_back(static_cast<double>(peak.exchange(0.0f, std::memory_order_relaxed)));
    link.send("levels", std::move(levels));
}
```

An editor is resizable if it says so, within its limits:

```cpp
return Editor{.width = 420,
              .height = 260,
              .resizable = true,
              .minimumWidth = 360,
              .minimumHeight = 220,
              .maximumWidth = 1200,
              .maximumHeight = 800,
              .files = myEditorFiles};
```

The page gets a grip in its bottom right corner, as some hosts don't let a plugin's window be resized from its edges. Restyle or hide it through the class `symbolengine-grip`, and ask for a size yourself with `plugin.resize(width, height)`. A size the user gives is kept for the instance and, in `sizeFile`, for new ones, unless `remember` is off. To keep it with a session, save what `editorResized` is told in your state and give it back to `setEditorSize` from `loadState`. The page's stored data is the plugin's own, apart from other plugins'.

Two things help while you work on a page. `SYMBOLENGINE_LIVE_FILES=1` in the environment of a debug build reads the page's files from their folder, so a change shows on a reload with no build. On a Mac, `SYMBOLENGINE_EDITOR_SNAPSHOT` naming a PNG has an editor save a picture of itself, after running any script in `SYMBOLENGINE_EDITOR_SCRIPT`, for checks run from a script.

## What each format needs

| Format | Built | What it needs | What hosts know it by |
|---|---|---|---|
| CLAP | Always, on every system | Nothing more | `id` in `describe` |
| VST3 | `FORMATS VST3` | Nothing more | A class ID, made from `id` unless `vst3ClassId` gives one |
| Audio Unit v2 | `FORMATS AUV2`, on a Mac only | `AU_SUBTYPE`, `AU_MANUFACTURER_CODE`, and `enable_language(OBJC OBJCXX)` in your project | Its type, subtype and manufacturer codes |

The VST3 SDK and Apple's AudioUnitSDK are fetched, pinned by checksum, only when a format needs them. `VST3_SDK_ROOT` and `AUDIOUNIT_SDK_ROOT` name copies of your own instead. A product carries their notices, which the [README](../../README.md#what-a-product-includes) lists.

**VST3 class ID.** A new plugin leaves `vst3ClassId` empty, and its class ID is made from `id`. A plugin with one of its own gives it as 32 hexadecimal digits, as Steinberg's validator prints it. An ID written wrongly leaves the binary offering no plugins at all, so the mistake shows at once.

**Audio Unit codes.** Each is four characters, checked as the project configures:

- **`AU_TYPE`** is `aufx` for an effect, the default, `aumf` for an effect that takes notes, or `aumu` for an instrument. `Block` holds audio alone and the `Plugin` class has no way to receive notes, so an effect's `aufx` is what fits.
- **`AU_SUBTYPE`** is the plugin's own among its maker's.
- **`AU_MANUFACTURER_CODE`** is the maker's, with at least one capital.

`AU_MANUFACTURER_NAME` and `AU_NAME` are how hosts list it, here as "Example: My Gain". Each is `OUTPUT_NAME` if not given.

An Audio Unit's codes are its identity in every host and session, as a VST3 plugin's class ID is, so neither may change once the plugin is released.

## Checking it with validators

`symbolengine_validate_plugin` adds every validator that applies as a test. Turn it on before taking SymbolEngine in:

```cmake
enable_testing()
set(SYMBOLENGINE_PLUGIN_VALIDATION ON)   # before FetchContent_MakeAvailable
```

Then, after `symbolengine_add_plugin`:

```cmake
symbolengine_validate_plugin(MyGain)
```

And run them:

```sh
ctest --test-dir build -R MyGain
```

- **Always**, the plugin is loaded from its CLAP binary as a host does, made, run and destroyed, and a VST3 build goes through Steinberg's validator, built from the same SDK as the plugin. On a Mac each bundle's signature is checked, that its CLAP binary makes nothing visible but its entry point, that the Audio Unit's Info.plist names it, and that its view's Objective-C class is its own.
- **`SYMBOLENGINE_CLAP_VALIDATOR`** naming CLAP's validator runs it. Version 0.4.1 divides by zero on a plugin with no parameters, and can fail a plugin whose only parameter is a switch.
- **`SYMBOLENGINE_PLUGINVAL`** naming pluginval runs it on the VST3 build at its strictest level. `SYMBOLENGINE_PLUGINVAL_EDITORS` also opens editors, which puts windows on the screen.
- **`SYMBOLENGINE_AUVAL`** turned on runs Apple's auval. It installs the Audio Unit for the user running the tests, so it is meant for continuous integration.

Two arguments check more. `VST3_CLASS_ID` checks that Steinberg's validator finds the VST3 build under that ID. `VST3_ARRANGEMENTS` checks that, of the speaker arrangements the validator asks for, the plugin takes exactly these, as the validator names them, here for a plugin that takes mono or stereo with its output matching its input:

```cmake
symbolengine_validate_plugin(MyGain
    VST3_CLASS_ID ABCDEF0153796D6247616E6750726F21
    VST3_ARRANGEMENTS Mono>Mono Stereo>Stereo)
```

A build with the sanitizers makes the CLAP build alone, and only the loading test runs, as a validator can't load a plugin built with them. `symbolengine_set_warnings(MyGain_impl)` holds your code to the warnings SymbolEngine holds its own to.

## Moving a released plugin

A plugin that already ships keeps what hosts and its users have of it: its identity, the state they saved, and the parameter numbers they automated. Keep all three and a session saved with an earlier build opens with the plugin found, its settings in place and its automation playing as before.

**Identity.** State the class ID and codes the plugin was released with. Steinberg's validator prints a VST3 build's class ID on its `cid =` line, and `auval -a` lists each Audio Unit's type, subtype and manufacturer codes. The class ID goes in `describe`:

```cpp
static Description describe()
{
    return {.id = "com.example.mygain",
            .name = "My Gain",
            .vendor = "Example",
            .version = "2.0.0",
            .features = {"audio-effect", "stereo"},
            .vst3ClassId = "ABCDEF0153796D6247616E6750726F21"};
}
```

The codes go in CMake, with the bundle identifier and file name it was released with, so an update lands on the same files:

```cmake
symbolengine_add_plugin(MyGain
    SOURCES              MyGain.cpp
    OUTPUT_NAME          "My Gain"
    BUNDLE_IDENTIFIER    com.example.mygain
    VERSION              2.0.0
    FORMATS              CLAP VST3 AUV2
    AU_TYPE              aufx
    AU_SUBTYPE           MyGn
    AU_MANUFACTURER_CODE Exmp
    AU_MANUFACTURER_NAME Example
    AU_NAME              "My Gain"
    AU_STATE_KEY         myGainState)
```

**Saved state.** `loadState` receives what earlier versions saved, exactly as the host stored it. A VST3 host's bytes arrive as they are. An Audio Unit keeps its state in a dictionary, so if earlier versions kept theirs under a key of their own, name that key with `AU_STATE_KEY`. To find it, run `plutil -p` on an `.aupreset` a released build saved, and look beside the standard keys, `type`, `subtype`, `manufacturer`, `version`, `data` and `name`. A plugin that kept its state in `data` leaves `AU_STATE_KEY` out, and state under `data` still opens when it is set.

The bytes are in your earlier versions' format, which only your code can read, so `loadState` reads that as well as whatever you write now. `loadParameters` takes values by id, which is a short way to hand over what you read:

```cpp
bool loadState(std::string_view state) override
{
    if (const auto saved = json::tryParse(state); saved && saved->isObject())
    {
        loadParameters((*saved)["parameters"]);
        return true;
    }

    // Your own reader for what earlier versions wrote.
    const auto earlier = readEarlierState(state);
    if (!earlier)
        return false;

    loadParameters(Object{{"1", earlier->gain}, {"2", earlier->mode}});
    return true;
}
```

`AU_STATE_KEY` is also where the plugin saves. To let earlier versions open new sessions too, override `saveState` to go on writing the format they read.

**Parameters.** Give each parameter the id its earlier versions gave it, as hosts recorded automation against that number. Then make the same value mean the same thing. A VST3 host records a position from 0 to 1, and `number` and `decibels` spread their units evenly over it, so a parameter earlier versions spread another way needs `scaled`, and so does one they showed hosts from 0 to 1. For a curve neither `Scale` draws, write a `Parameter` from 0 to 1 with its own `toText` and `fromText`, and turn it into units in `process`.

**Checking it.** `symbolengine_validate_plugin(MyGain VST3_CLASS_ID ABCDEF0153796D6247616E6750726F21)` proves the validator finds it under its old ID, as [`Tests/IdentityPlugin`](../../Tests/IdentityPlugin/IdentityPlugin.cpp) does. The test that counts is a session and a preset saved with the released build, in each format, opened with the new one. Keep those files, and open them again after every change that touches state or parameters.

## What's in the reference

| Where | What it gives you |
|---|---|
| `Plugin.h` | `Plugin`, `Description`, `Parameter`, `ParameterValue`, `Bus`, `Buses`, `Layouts`, `Block`, `Editor`, `EditorLink`, `SYMBOLENGINE_PLUGINS` |
| `Parameters.h` | `decibels`, `number`, `choice`, `toggle`, `bypass`, `scaled`, `Scale` |
| `<symbolengine/plugin.h>` | Both of those |
| `<symbolengine/core/ChannelLayout.h>` | `ChannelLayout` and `Speaker`, from the core module |
| `Modules/plugin/web/plugin.js` | The page's side of a plugin's parameters |
| `cmake/SymbolEnginePlugins.cmake` | `symbolengine_add_plugin`, with every argument described |
| `cmake/SymbolEngineValidation.cmake` | `symbolengine_validate_plugin` |
| `cmake/SymbolEngineResources.cmake` | `symbolengine_add_files` |

The [architecture's plugins section](../02_architecture.md#plugins) says how each part is built and why, and its [section on moving a plugin](../02_architecture.md#moving-an-existing-plugin) what a released plugin keeps. The [ui guide](ui.md#talking-to-the-page) has more on what a page and its native side can say to each other.
