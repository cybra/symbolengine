#pragma once

#include <symbolengine/core/Value.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/** Plugins written once and built for every format. A plugin is a class derived from Plugin, with a static describe()
    function, listed in SYMBOLENGINE_PLUGINS in one of its source files and built with symbolengine_add_plugin in
    CMake. */
namespace symbolengine::plugin
{

/** What hosts are told about a plugin before they make one. */
struct Description
{
    std::string id{};   // never changes once released: hosts save sessions against it, such as
                        // "com.example.gain"
    std::string name{};
    std::string vendor{};
    std::string version{};   // such as "1.2.0"
    std::string description{};
    std::string url{};
    std::string manualUrl{};
    std::string supportUrl{};
    std::vector<std::string> features{};   // CLAP's feature names, such as "audio-effect", "analyzer" and "stereo"

    /** The class ID hosts know the plugin's VST3 build by, for a plugin already released with one of its own: 32
        hexadecimal digits as Steinberg's validator prints them, such as "ABCDEF0153796D6247616E6750726F21". Empty for
        one made from the id, which never changes either. */
    std::string vst3ClassId{};
};

/** One of a plugin's parameters, the settings a host can save, show and automate. */
struct Parameter
{
    std::uint32_t id = 0;   // never changes once released: hosts record automation against it
    std::string name{};
    std::string group{};   // where the host lists it, with groups inside groups parted by '/', such as "Band 1"
    double minimum = 0.0;
    double maximum = 1.0;
    double defaultValue = 0.0;
    bool automatable = true;
    bool stepped = false;   // whole numbers only, as choices and switches are
    bool hidden = false;
    bool bypass = false;   // the plugin's own bypass, which hosts show as theirs: stepped, 0 or 1

    /** The value as text, such as "-6.0 dB"; without it, the number to two decimal places. */
    std::function<std::string(double)> toText{};

    /** Text typed into a host as a value; without it, the number it starts with. */
    std::function<std::optional<double>(std::string_view)> fromText{};
};

/** A parameter's value, read from any thread, the audio thread included. */
class ParameterValue
{
public:
    ParameterValue() = default;

    std::uint32_t id() const noexcept { return parameterId; }
    double get() const noexcept { return slot != nullptr ? slot->load(std::memory_order_relaxed) : 0.0; }

private:
    friend class Plugin;

    ParameterValue(std::uint32_t id, const std::atomic<double>* value) noexcept : parameterId(id), slot(value) {}

    std::uint32_t parameterId = 0;
    const std::atomic<double>* slot = nullptr;
};

/** A plugin's audio inputs or outputs; the first is its main one. */
struct Bus
{
    std::string name{};
    std::uint32_t channels = 2;
};

struct Buses
{
    std::vector<Bus> inputs{Bus{"Input", 2}};
    std::vector<Bus> outputs{Bus{"Output", 2}};
};

/** The audio a plugin processes in one call: each bus's channels, frames() samples long. An output may be the same
    memory as the input of the same number, so a plugin reads a sample before writing the one at the same place. */
class Block
{
public:
    Block(std::uint32_t frames, std::span<const std::span<const float* const>> inputs,
          std::span<const std::span<float* const>> outputs) noexcept
        : count(frames), ins(inputs), outs(outputs)
    {
    }

    std::uint32_t frames() const noexcept { return count; }
    std::size_t inputBuses() const noexcept { return ins.size(); }
    std::size_t outputBuses() const noexcept { return outs.size(); }

    /** A bus's channels, or none if there is no such bus. */
    std::span<const float* const> input(std::size_t bus = 0) const noexcept
    {
        return bus < ins.size() ? ins[bus] : std::span<const float* const>();
    }

    std::span<float* const> output(std::size_t bus = 0) const noexcept
    {
        return bus < outs.size() ? outs[bus] : std::span<float* const>();
    }

private:
    std::uint32_t count;
    std::span<const std::span<const float* const>> ins;
    std::span<const std::span<float* const>> outs;
};

/** A plugin's editor: a web page shown in the host's window for the plugin, on Mac and Windows. Its files come from
    symbolengine_add_files, starting with "/index.html", which loads symbolengine.js and plugin.js:

        <script src="symbolengine.js"></script>
        <script src="plugin.js"></script>

        const parameters = await plugin.parameters();
        const gain = parameters.get(1);       // by id: gain.name, gain.value, gain.text, gain.minimum, ...
        gain.onChange(() => show(gain.text));  // whenever the value changes, from the page, the host or a preset
        await gain.change(-6);                 // a change the host records as automation; or begin(), set(), end()
                                               // around a drag */
struct Editor
{
    std::uint32_t width = 600;   // in points on a Mac and in pixels at 100% on Windows
    std::uint32_t height = 400;
    bool resizable = false;
    std::uint32_t minimumWidth = 200;
    std::uint32_t minimumHeight = 120;
    std::uint32_t maximumWidth = 0;   // 0 for no limit
    std::uint32_t maximumHeight = 0;

    /** For a resizable editor: a size the user gives it is kept, for this plugin instance and, in sizeFile, for new
        ones, once the resizing has rested for 0.6 s. An editor opens at its instance's size, or the size kept in the
        file, or width and height, held to the limits and to the screen it opens on. */
    bool remember = true;
    std::filesystem::path sizeFile{};   // empty for <app data>/<vendor>/<plugin name>/Editor.json
    std::function<std::optional<std::string_view>(std::string_view path)> files{};

    /** Windows only: the folder the editor's web view keeps its data in. Empty for AppData\Local\<vendor>\<plugin
        name>\WebView. On every system the page's stored data is the plugin's own, apart from other plugins'. */
    std::filesystem::path dataFolder{};
};

/** The plugin's side of its open editor, on the message thread. */
class EditorLink
{
public:
    virtual ~EditorLink() = default;

    /** Adds a function the page calls with symbolengine.call, given its arguments as an array. */
    virtual void expose(std::string name, std::function<Value(const Value& arguments)> function) = 0;

    /** Sends the page an event, which it receives with symbolengine.on. */
    virtual void send(std::string_view event, const Value& data) = 0;

    /** Saves a picture of the editor as it is drawn now to a PNG, then says whether it was written; on a Mac only
        for now. An editor saves one by itself when SYMBOLENGINE_EDITOR_SNAPSHOT names a PNG, two seconds after it
        opens and a second after running SYMBOLENGINE_EDITOR_SCRIPT in the page, and prints whether it was written. */
    virtual void snapshot(const std::filesystem::path& png, std::function<void(bool written)> done) = 0;
};

class Plugin;

namespace detail
{
/** What a plugin can ask of the host it is running in. */
struct HostLink
{
    virtual ~HostLink() = default;
    virtual void latencyChanged() = 0;
    virtual void editorSizeChanged() = 0;
};

struct Access;
}

/** A plugin. Its constructor adds its parameters; hosts then call the rest on the threads each says.

    The framework splits each block where the host changes a parameter, so process() always sees the values for the
    samples it is given, and automation is exact to the sample. Denormal numbers are flushed to zero while it runs. */
class Plugin
{
public:
    Plugin();
    virtual ~Plugin();

    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    /** The audio inputs and outputs, asked once after the plugin is made. Stereo in and out unless overridden. */
    virtual Buses buses() const;

    /** Message thread: processing is about to start at this sample rate, in blocks of up to maximumFrames. The place
        to allocate what process() needs. */
    virtual void prepare(double sampleRate, std::uint32_t maximumFrames);

    /** Message thread: processing has stopped until prepare() is called again. */
    virtual void release();

    /** Message thread: how many samples late the output is, asked after prepare(). */
    virtual std::uint32_t latency() const;

    /** Message thread: how many samples the plugin goes on sounding after its input falls silent, as a reverb does;
        UINT32_MAX for ever. */
    virtual std::uint32_t tail() const;

    /** Message thread: the plugin's state as bytes, for the host to keep with a session or a preset. By default the
        parameters' values, as JSON. */
    virtual std::string saveState() const;

    /** Message thread: state the host kept, from this plugin or an earlier version of it, exactly as saveState() gave
        it. Says whether it could be read. By default the parameters' values, as saveState() writes them. */
    virtual bool loadState(std::string_view state);

    /** Audio thread: processes a block. It must not allocate, lock or wait. */
    virtual void process(Block& block) = 0;

    /** Audio thread: clears anything held from earlier audio, such as delay lines, as when the host jumps. */
    virtual void reset();

    /** Message thread: the plugin's editor, or nothing for one without, asked when the host opens it. */
    virtual std::optional<Editor> editor() const;

    /** Message thread: the editor has opened; the place to expose the plugin's own functions to the page. */
    virtual void editorOpened(EditorLink& editor);

    /** Message thread, about 30 times a second while the editor is open: the place to send the page what changes on
        its own, such as levels the audio thread has measured. Changes to the parameters reach the page without it. */
    virtual void editorTick(EditorLink& editor);

    /** Message thread: the editor has closed. */
    virtual void editorClosed();

    /** Message thread: the user, the host or the page has resized the editor, to a size in points; the place to keep
        it in the plugin's own state, so a session opens the editor at it. */
    virtual void editorResized(std::uint32_t width, std::uint32_t height);

    /** The parameters, in the order they were added. */
    const std::vector<Parameter>& parameters() const noexcept { return definitions; }

    /** A parameter, or nullptr if there is none with this id. */
    const Parameter* findParameter(std::uint32_t id) const noexcept;

    /** A parameter's value now, from any thread; 0 if there is no such parameter. */
    double parameterValue(std::uint32_t id) const noexcept;

protected:
    /** Adds a parameter, in the constructor only. Its id must be new; it starts at its default value. */
    ParameterValue addParameter(Parameter parameter);

    /** Message thread: tells the host the latency has changed, so it asks latency() again. */
    void latencyChanged();

    /** Message thread: the size in points this instance's editor opens at, such as one its saved state held; an
        editor that is open asks the host to resize it. */
    void setEditorSize(std::uint32_t width, std::uint32_t height);

    /** The parameters' values by id, and setting them back from it, for plugins whose saveState() writes more. */
    Object saveParameters() const;
    void loadParameters(const Value& saved);

private:
    friend struct detail::Access;

    std::vector<Parameter> definitions;
    std::deque<std::atomic<double>> values;   // a deque, so each value stays where its ParameterValue points
    std::unordered_map<std::uint32_t, std::size_t> indexOf;
    detail::HostLink* host = nullptr;
    std::uint32_t editorWidth = 0, editorHeight = 0;   // 0 until this instance's editor has a size of its own
};

/** A plugin a binary offers: its description, and how to make one. */
struct Entry
{
    Description description;
    std::function<std::unique_ptr<Plugin>()> make;
};

/** The plugins this binary offers, in order, as SYMBOLENGINE_PLUGINS lists them. */
const std::vector<Entry>& registered();

namespace detail
{
/** The CLAP entry point's functions, which SYMBOLENGINE_PLUGINS passes on to. */
bool clapInit(const char* path) noexcept;
void clapDeinit() noexcept;
const void* clapFactory(const char* id) noexcept;
}

template <typename... Kinds>
std::vector<Entry> entriesFor()
{
    return {Entry{Kinds::describe(), [] { return std::unique_ptr<Plugin>(std::make_unique<Kinds>()); }}...};
}

}

/** Lists the plugin classes a binary offers, in one source file of the plugin's own, such as
    SYMBOLENGINE_PLUGINS(Gain) or SYMBOLENGINE_PLUGINS(Meter, MeterRack). */
// The entry point's functions are defined here, in the plugin's own code, so a linker that reads each library once,
// in order, as GNU ld does, takes the plugin's code first and the framework it uses after it.
#define SYMBOLENGINE_PLUGINS(...)                                                                                      \
    const std::vector<symbolengine::plugin::Entry>& symbolengine::plugin::registered()                                 \
    {                                                                                                                  \
        static const auto entries = symbolengine::plugin::entriesFor<__VA_ARGS__>();                                   \
        return entries;                                                                                                \
    }                                                                                                                  \
    extern "C" bool symbolengine_clap_init(const char* path)                                                           \
    {                                                                                                                  \
        return symbolengine::plugin::detail::clapInit(path);                                                           \
    }                                                                                                                  \
    extern "C" void symbolengine_clap_deinit(void)                                                                     \
    {                                                                                                                  \
        symbolengine::plugin::detail::clapDeinit();                                                                    \
    }                                                                                                                  \
    extern "C" const void* symbolengine_clap_get_factory(const char* id)                                               \
    {                                                                                                                  \
        return symbolengine::plugin::detail::clapFactory(id);                                                          \
    }
