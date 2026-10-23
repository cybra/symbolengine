#pragma once

#include <symbolengine/core/Value.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
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

class Plugin;

namespace detail
{
/** What a plugin can ask of the host it is running in. */
struct HostLink
{
    virtual ~HostLink() = default;
    virtual void latencyChanged() = 0;
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

    /** The parameters' values by id, and setting them back from it, for plugins whose saveState() writes more. */
    Object saveParameters() const;
    void loadParameters(const Value& saved);

private:
    friend struct detail::Access;

    std::vector<Parameter> definitions;
    std::deque<std::atomic<double>> values;   // a deque, so each value stays where its ParameterValue points
    std::unordered_map<std::uint32_t, std::size_t> indexOf;
    detail::HostLink* host = nullptr;
};

/** A plugin a binary offers: its description, and how to make one. */
struct Entry
{
    Description description;
    std::function<std::unique_ptr<Plugin>()> make;
};

/** The plugins this binary offers, in order, as SYMBOLENGINE_PLUGINS lists them. */
const std::vector<Entry>& registered();

template <typename... Kinds>
std::vector<Entry> entriesFor()
{
    return {Entry{Kinds::describe(), [] { return std::unique_ptr<Plugin>(std::make_unique<Kinds>()); }}...};
}

}

/** Lists the plugin classes a binary offers, in one source file of the plugin's own, such as
    SYMBOLENGINE_PLUGINS(Gain) or SYMBOLENGINE_PLUGINS(Meter, MeterRack). */
#define SYMBOLENGINE_PLUGINS(...)                                                                                      \
    const std::vector<symbolengine::plugin::Entry>& symbolengine::plugin::registered()                                 \
    {                                                                                                                  \
        static const auto entries = symbolengine::plugin::entriesFor<__VA_ARGS__>();                                   \
        return entries;                                                                                                \
    }
