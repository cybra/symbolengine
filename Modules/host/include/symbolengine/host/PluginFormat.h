#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace symbolengine
{

/** Who a plugin is, whatever its file is called or wherever it lies: its format, and the format's own full identity
    as text the format defines, such as a VST3 class ID's 16 bytes as 32 hexadecimal digits. */
struct PluginIdentity
{
    std::string format;
    std::string id;

    /** An Audio Unit's identity: its type, subtype and manufacturer codes as raw values, each as 8 hexadecimal digits
        joined by hyphens, such as "61756678-53706b72-5379456e" for aufx Spkr SyEn, so codes holding spaces, zeros or
        bytes past ASCII are kept exactly. */
    static PluginIdentity audioUnit(std::uint32_t type, std::uint32_t subtype, std::uint32_t manufacturer);

    bool operator==(const PluginIdentity&) const = default;

    bool operator<(const PluginIdentity& other) const
    {
        return format != other.format ? format < other.format : id < other.id;
    }
};

enum class PluginKind
{
    effect,
    instrument,
    other,
};

/** How many channels a plugin's main buses have. */
struct PluginLayout
{
    int inputs = 2;
    int outputs = 2;

    bool operator==(const PluginLayout&) const = default;
};

/** What the host knows of a plugin before making it: who it is, where its code lies and what it says it is, and what
    a scan found it does, so a program knows it without making it. */
struct PluginDescription
{
    PluginIdentity identity{};
    std::filesystem::path location{};
    std::string name{};
    std::string maker{};
    std::string version{};
    PluginKind kind = PluginKind::effect;

    /** The layouts of its main buses it took when a scan made it, none for a plugin no scan made; one with no outputs
        gives no audio out. */
    std::vector<PluginLayout> layouts{};

    /** Whether it has an editor of its own. */
    bool editor = false;

    /** The version of its format's SDK it says it was built with, where it says. */
    std::string sdk{};

    bool operator==(const PluginDescription&) const = default;
};

/** What says a plugin's file has changed since it was scanned: its binary's size and modification time, the binary
    inside a bundle rather than the bundle, since an installer can replace one and leave the other's time; the version
    it declares; and the modification time of a manifest that describes it without its code running, such as a VST3's
    moduleinfo.json, 0 where it has none. Times are in nanoseconds since 1970 on the system clock, and two stamps are
    only compared for a difference, never for which is newer, so an older build put back counts as a change. */
struct PluginStamp
{
    std::int64_t size = 0;
    std::int64_t modified = 0;
    std::string version{};
    std::int64_t manifestModified = 0;

    bool operator==(const PluginStamp&) const = default;
};

/** What a format reads of a plugin file without loading its code: its stamp, and the architectures its binaries are
    built for, as their headers say, such as "arm64" and "x86_64". */
struct PluginFileFacts
{
    PluginStamp stamp{};
    std::vector<std::string> architectures{};
};

/** A folder plugins are installed in, and whether it is the user's own rather than the system's, as the user's own
    plugin folder is, or one the user added. Of two copies of a plugin at one version, the user's is loaded. */
struct PluginFolder
{
    std::filesystem::path path{};
    bool user = false;
};

/** Which of two plugin versions is the newer, as a negative number, 0 or a positive number like strcmp's: compared
    piece by piece, the runs of digits as numbers and the rest as text, so "1.10" is newer than "1.9" and "2.0" the
    same as "2.0.0". */
int compareVersions(std::string_view a, std::string_view b);

/** Why something asked of a plugin failed: in words for people, with the step it failed at and the format's own code. */
struct PluginError
{
    std::string message;
    std::string step;
    std::int64_t code = 0;

    explicit operator bool() const { return !message.empty(); }
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

/** One of a plugin's parameters, as it describes it. Values run from 0 to 1, as every format gives them. */
struct PluginParameter
{
    std::uint32_t id = 0;
    std::string name;
    std::string units;
    double defaultValue = 0.0;

    /** 0 for a continuous parameter, else how many steps it has less one, as VST3 counts them. */
    int steps = 0;

    bool automatable = true;
    bool readOnly = false;
    bool hidden = false;
    bool bypass = false;
    bool programChange = false;

    bool operator==(const PluginParameter&) const = default;
};

/** A parameter's new value, at a frame of the block it comes with. */
struct PluginParameterChange
{
    std::uint32_t id = 0;
    int offset = 0;
    double value = 0.0;
};

/** Where the program is in time for a block: whether it plays or records; the position of the block's first frame in
    its timeline, in frames at the plugin's rate, which holds still while it is stopped; the device's time for that
    frame in the steady clock's nanoseconds, 0 where it isn't known; and its tempo in beats a minute and its meter,
    0 where it has none. A discontinuity, as a jump or a new take starts with, clears what the plugin holds of the audio
    before it; a loop returning to its start is none, so tails carry over it. */
struct PluginTime
{
    bool playing = false;
    bool recording = false;
    bool discontinuity = false;
    std::int64_t position = 0;
    std::int64_t systemNanoseconds = 0;
    double tempo = 0.0;
    int numerator = 0;
    int denominator = 0;
};

/** The most parameter changes a block carries, which a format makes room for when a plugin is activated. */
constexpr int maximumBlockChanges = 4096;

/** One block for a plugin's process call: a buffer for each channel of its main buses, as its layout says; the
    parameters changed for it, in order of their frames, at most maximumBlockChanges; room for the values its
    processing reports, as a meter's, the last of each parameter's, where the format says how many it put; and where
    the program is in time, which the host always gives. */
struct PluginBlock
{
    const float* const* inputs = nullptr;
    float* const* outputs = nullptr;
    int inputChannels = 0;
    int outputChannels = 0;
    int frames = 0;
    const PluginParameterChange* changes = nullptr;
    int changeCount = 0;
    PluginParameterChange* reported = nullptr;
    int reportedRoom = 0;
    int reportedCount = 0;
    const PluginTime* time = nullptr;
};

/** A parameter the plugin moved itself: the begin and end of a gesture in its own window around the values it set,
    or a value its processing reports, as a meter's. */
struct PluginEdit
{
    enum class Kind
    {
        begin,
        value,
        end,
        reported,
    };

    Kind kind = Kind::value;
    std::uint32_t id = 0;
    double value = 0.0;

    bool operator==(const PluginEdit&) const = default;
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

    /** It asks to be reset from the main thread, as an Audio Unit playing in real time does at a discontinuity, since
        it can't be reset on the audio thread. */
    reset = 1u << 6,

    /** It asks for its editor to be opened, or brought in front of the others if it is open. */
    editor = 1u << 7,
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

    /** The plugin's own edit of a parameter, from its window or elsewhere: a gesture's begin, value or end. */
    virtual void edit(const PluginEdit& edit) noexcept = 0;

    virtual ~PluginFormatClient() = default;
};

/** The size of a plugin's editor: in points on a Mac and in pixels on Windows, as the formats measure them. */
struct PluginEditorSize
{
    int width = 0;
    int height = 0;

    bool operator==(const PluginEditorSize&) const = default;
};

/** A key pressed or let go of while a plugin's editor has the keyboard, as a format offers it to the editor: its
    character, 0 for a key without one; the key, where it is one of those named; and the modifier keys held, command
    being Command on a Mac and Control on Windows. */
struct PluginKey
{
    enum class Named
    {
        none,
        backspace,
        tab,
        enter,
        escape,
        space,
        left,
        up,
        right,
        down,
        home,
        end,
        pageUp,
        pageDown,
        insert,
        forwardDelete,
        f1,
        f2,
        f3,
        f4,
        f5,
        f6,
        f7,
        f8,
        f9,
        f10,
        f11,
        f12,
    };

    char32_t character = 0;
    Named named = Named::none;
    bool shift = false;
    bool alt = false;
    bool command = false;

    bool operator==(const PluginKey&) const = default;
};

/** The native view a plugin's editor is open in, as the program holds it, which sizes itself and the window holding it
    as the plugin asks. Called on the main thread. */
class PluginEditorFrame
{
public:
    /** Sizes the view the editor is in to the size, and the window holding it to fit. */
    virtual void place(PluginEditorSize size) = 0;

    /** The scale of the screen the view is on, for a format whose editors are sized in pixels: its DPI over 96 on
        Windows. */
    virtual double scale() const { return 1.0; }

    virtual ~PluginEditorFrame() = default;
};

/** A plugin's editor open in a native view of the program's, as its format holds it, used by the host module alone.
    Every call is made on the main thread. Destroying it closes the editor in its format's order while the view it is in
    is still whole. */
class PluginFormatEditor
{
public:
    virtual ~PluginFormatEditor() = default;

    /** Its size now. */
    virtual PluginEditorSize size() = 0;

    /** Whether the user may resize it. */
    virtual bool resizable() = 0;

    /** The size nearest the one given that it takes, for a window the user is dragging to that size. */
    virtual PluginEditorSize constrain(PluginEditorSize wanted) = 0;

    /** Tells it the view it is in has been resized to the size, as by the user's drag. */
    virtual void resized(PluginEditorSize size) = 0;

    /** Tells it the view it is in is on a screen of another scale, once the user's move there is over, for a format
        whose editors are sized in pixels. */
    virtual void rescaled(double scale) { static_cast<void>(scale); }

    /** Offers it a key, for a format whose editors are offered keys before the system's own windows get them, as
        VST3's are on Windows: whether it took it. */
    virtual bool keyDown(const PluginKey& key)
    {
        static_cast<void>(key);
        return false;
    }

    virtual bool keyUp(const PluginKey& key)
    {
        static_cast<void>(key);
        return false;
    }
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

    /** Around a run of process calls, on the audio thread, or on the main thread once no audio thread is calling. Like
        process, these let an exception from the plugin's own code through, which the host catches. */
    virtual void startProcessing() = 0;
    virtual void stopProcessing() = 0;

    /** Processes a block on the audio thread, giving false if it failed, after which its output isn't heard, and
        putting the values its processing reports in the block's room for them. */
    virtual bool process(PluginBlock& block) = 0;

    /** Why it can never be called again, once its format knows, as for an Audio Unit whose instance the system says is
        gone, which the host then fails with this reason; nothing while it can. Read on the audio thread after a block
        it failed, so it is text that lasts, never made there. */
    virtual const char* stopped() const { return nullptr; }

    /** Hands it parameters' new values while it is active and not processing, so its processing has them as it would
        after a block, as before its state is taken. */
    virtual void flush(const PluginParameterChange* changes, int count) = 0;

    /** Its parameters, read on the main thread once its layout is settled, and again when it says they changed. */
    virtual std::vector<PluginParameter> parameters() = 0;

    /** A parameter's value as the plugin has it, setting the value the plugin shows, which its processing hears only
        through a change, and a value as text. */
    virtual double parameterValue(std::uint32_t id) = 0;
    virtual void showParameterValue(std::uint32_t id, double value) = 0;
    virtual std::string parameterText(std::uint32_t id, double value) = 0;

    /** Its state for the use, or nothing with the error set; and giving it back, while it is inactive or active. */
    virtual std::optional<std::string> state(PluginStateUse use, PluginError& error) = 0;
    virtual bool setState(std::string_view state, PluginStateUse use, PluginError& error) = 0;

    /** Whether it has an editor that can be opened on this system, asked without opening one: false where the
        format's editors aren't hosted here. */
    virtual bool hasEditor() { return false; }

    /** Opens its editor in the native view, an NSView on a Mac or an HWND on Windows, already in its window, which
        the frame sizes to the editor before the editor is put in it; or nothing, with the error set. At most one is
        open at a time. */
    virtual std::unique_ptr<PluginFormatEditor> openEditor(void* view, PluginEditorFrame& frame, PluginError& error)
    {
        static_cast<void>(view);
        static_cast<void>(frame);
        error = {"It has no editor.", "openEditor", 0};
        return nullptr;
    }
};

/** One plugin format: VST3, Audio Units, CLAP, or the fake one tests use. */
class PluginFormat
{
public:
    virtual ~PluginFormat() = default;

    virtual std::string name() const = 0;

    /** The folders this format's plugins are installed in on this system, the system's before the user's. */
    virtual std::vector<PluginFolder> folders() const = 0;

    /** Whether a path found in a folder is one of this format's plugin files, taken whole and never walked into. */
    virtual bool isPluginFile(const std::filesystem::path& path) const = 0;

    /** A plugin file's stamp and architectures, read without loading its code, or nothing with the error set when it
        holds no binary to read. From any thread. */
    virtual std::optional<PluginFileFacts> examine(const std::filesystem::path& file, PluginError& error) const = 0;

    /** The plugins a file of the format holds, each as it declares itself, without making any, in the order the file
        gives them; called on the main thread, loading the file's code where the format has to, so a scan calls it in
        a worker. Nothing, with the error set, if the file can't be read or its code won't load. */
    virtual std::vector<PluginDescription> pluginsIn(const std::filesystem::path& file, PluginError& error) = 0;

    /** Whether the system can make the plugin now, for a format whose plugins come from the system's registry, which
        can lag behind the folders: false for an Audio Unit installed but not yet in it. */
    virtual bool registered(const PluginDescription&) const { return true; }

    /** Makes a plugin on the main thread, its notices told to the client from then on. done is called once on the main
        thread with the plugin or an error, before make returns or after turns of the run loop, as a format that makes
        plugins asynchronously does. */
    virtual void make(const PluginDescription& plugin, PluginFormatClient& client,
                      std::function<void(std::unique_ptr<PluginFormatInstance>, PluginError)> done) = 0;
};

}
