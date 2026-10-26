#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace symbolengine
{

/** Who a plugin is, whatever its file is called or wherever it lies: its format, and the format's own full identity
    as text the format defines, such as a VST3 class ID's 16 bytes as 32 hexadecimal digits. */
struct PluginIdentity
{
    std::string format;
    std::string id;

    bool operator==(const PluginIdentity&) const = default;
};

enum class PluginKind
{
    effect,
    instrument,
    other,
};

/** What the host knows of a plugin before making it: who it is, where its code lies and what it says it is. */
struct PluginDescription
{
    PluginIdentity identity;
    std::filesystem::path location;
    std::string name;
    std::string maker;
    std::string version;
    PluginKind kind = PluginKind::effect;
};

/** Why something asked of a plugin failed: in words for people, with the step it failed at and the format's own code. */
struct PluginError
{
    std::string message;
    std::string step;
    std::int64_t code = 0;

    explicit operator bool() const { return !message.empty(); }
};

/** How many channels a plugin's main buses have. */
struct PluginLayout
{
    int inputs = 2;
    int outputs = 2;

    bool operator==(const PluginLayout&) const = default;
};

/** How a plugin is to run: its rate, the longest block it is given, and whether it renders offline, where it may take
    as long as it needs. */
struct PluginSetup
{
    double rate = 48000.0;
    int maximumFrames = 512;
    bool offline = false;

    bool operator==(const PluginSetup&) const = default;
};

/** One block for a plugin's process call: a buffer for each channel of its main buses, as its layout says. */
struct PluginBlock
{
    const float* const* inputs = nullptr;
    float* const* outputs = nullptr;
    int inputChannels = 0;
    int outputChannels = 0;
    int frames = 0;
};

/** Which state is asked for: a document's, kept with a project or a saved group; a preset's; or one for a copy made to
    render through. Formats that keep them apart, as an Audio Unit's state for documents, give the one asked for. */
enum class PluginStateUse
{
    document,
    preset,
    copy,
};

/** What a plugin says has changed, as a set of flags. */
enum class PluginNotice : std::uint32_t
{
    /** Its latency or tail. */
    latency = 1u << 0,

    /** Its buses, or how many channels they have. */
    buses = 1u << 1,

    /** Its parameters: which there are, their names, ranges or flags. */
    parameters = 1u << 2,

    /** Its parameters' values, changed together, as by a preset chosen in its own window. */
    values = 1u << 3,

    /** Its state, outside its parameters, so the document holding it should be saved. */
    state = 1u << 4,

    /** It asks to be made again. */
    reload = 1u << 5,
};

constexpr PluginNotice operator|(PluginNotice a, PluginNotice b)
{
    return static_cast<PluginNotice>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

constexpr bool has(PluginNotice set, PluginNotice one)
{
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(one)) != 0;
}

/** What a plugin tells the host of. Called by a format from any thread at any moment, from inside the host's own call
    into the plugin among them, so it only records what it is told and returns at once. */
class PluginFormatClient
{
public:
    virtual void notice(PluginNotice what) noexcept = 0;

    virtual ~PluginFormatClient() = default;
};

/** One plugin as its format holds it, used by the host module alone. Every call is made on the main thread, one at a
    time, except startProcessing, stopProcessing and process, which come from one audio thread at a time while it is
    active. Destroying it takes it apart in its format's order, as far as it was made, on the main thread. */
class PluginFormatInstance
{
public:
    virtual ~PluginFormatInstance() = default;

    /** Asks for a layout of the main buses while it is inactive, its other buses switched off, giving what it settled
        on, which may be another layout or none. */
    virtual PluginLayout arrange(PluginLayout wanted) = 0;

    /** Sets it up and activates it, or gives false with the error set. */
    virtual bool activate(const PluginSetup& setup, PluginError& error) = 0;
    virtual void deactivate() = 0;

    /** Its latency and tail in frames, read while it is active. */
    virtual int latency() = 0;
    virtual int tail() = 0;

    /** Clears what it holds of audio already played, keeping its settings; while active and not processing. */
    virtual void reset() = 0;

    /** Around a run of process calls, on the audio thread, or on the main thread once no audio thread is calling. */
    virtual void startProcessing() noexcept = 0;
    virtual void stopProcessing() noexcept = 0;

    /** Processes a block on the audio thread, giving false if it failed, after which its output isn't heard. */
    virtual bool process(const PluginBlock& block) noexcept = 0;

    /** Hands it the values set since it last processed, while it is active and not processing. */
    virtual void flush() = 0;

    /** Its state for the use, or nothing with the error set; and giving it back, while it is inactive or active. */
    virtual std::optional<std::string> state(PluginStateUse use, PluginError& error) = 0;
    virtual bool setState(std::string_view state, PluginStateUse use, PluginError& error) = 0;
};

/** One plugin format: VST3, Audio Units, CLAP, or the fake one tests use. */
class PluginFormat
{
public:
    virtual ~PluginFormat() = default;

    virtual std::string name() const = 0;

    /** Makes a plugin on the main thread, its notices told to the client from then on. done is called once on the main
        thread with the plugin or an error, before make returns or after turns of the run loop, as a format that makes
        plugins asynchronously does. */
    virtual void make(const PluginDescription& plugin, PluginFormatClient& client,
                      std::function<void(std::unique_ptr<PluginFormatInstance>, PluginError)> done) = 0;
};

}
