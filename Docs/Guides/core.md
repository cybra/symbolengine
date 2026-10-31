# Values, text, files and threads

The core module holds what an audio program needs before it makes a sound: values with JSON, XML documents, UTF-8 text, paths and the system's folders, files and settings written safely, checksums and gzip, tools for the audio thread, speaker layouts, and child processes with a link to them. The modules above it are built on it.

```cmake
target_link_libraries(my_app PRIVATE SymbolEngine::core)
```

```cpp
#include <symbolengine/core.h>

using namespace symbolengine;
using namespace std::chrono_literals;
```

`core.h` brings in everything except gzip and mapped files, which have headers of their own: `symbolengine/core/Gzip.h` and `symbolengine/core/MappedFile.h`. Classes are at the top of `symbolengine`, and groups of functions are in namespaces of their own: `json`, `xml`, `text`, `paths`, `files`, `gzip` and `system`.

A function that can fail for reasons outside the program, such as a missing file, throws `std::filesystem::filesystem_error`, or, given a `std::error_code`, sets it instead, as `std::filesystem` does. Reading data that may not be there, such as a member of a parsed value, never throws.

## Values and JSON

A `Value` holds what JSON holds: null, true or false, a number, text, an array or an object. A whole number stays a 64-bit integer, so sample positions stay exact, and an `Object` keeps its members in the order they were added.

A missing member or item reads as null, so a path into data that may not be there is safe. `stringOr`, `intOr`, `numberOr` and `boolOr` give the fallback for anything missing or of another kind:

```cpp
const auto project = json::parse(files::read("take.json"));
const auto path = project["sources"][0]["path"].stringOr("");
const auto rate = project["rate"].intOr(48000);
```

`asString`, `asInt`, `asNumber`, `asBool`, `asArray` and `asObject` are for a value that must be of that kind, and throw `TypeError` if it isn't. `ifArray`, `ifObject` and `ifString` give a pointer, or `nullptr`, which suits walking over what may be there:

```cpp
if (const auto* sources = project["sources"].ifArray())
    for (const auto& source : *sources)
        open(source["path"].stringOr(""));

if (const auto* members = project.ifObject())
    for (const auto& member : *members)
        show(member.name(), member.value);
```

Values are built with `Object` and `Array`. A `Value`'s own `operator[]` only reads, so changes go through `asObject()` and `asArray()`. An `Object`'s `operator[]` adds a missing member at the end, and `erase` removes one:

```cpp
Value take = Object{{"name", "First"}, {"rate", 48000}, {"gains", Array{0.5, 1.0}}};
take.asObject()["frames"] = 1024;
take.asObject()["gains"].asArray().push_back(0.25);
```

`json::write` gives compact text, or indented text with `json::Layout::pretty`. Numbers are written as browsers write them, in the fewest digits that read back as the same number, so a page and the native side read each other's numbers exactly:

```cpp
files::writeSafely("take.json", json::write(take, json::Layout::pretty));
```

`json::parse` throws `json::ParseError` when text isn't JSON, saying why and where, and `json::tryParse` gives nothing instead:

```cpp
try
{
    use(json::parse(text));
}
catch (const json::ParseError& error)
{
    report(error.reason(), error.line(), error.column());
}

if (const auto value = json::tryParse(text))
    use(*value);
```

## XML

`xml` reads and writes documents of data, such as lists and session files. An `Element` has a name, its attributes in order, the elements inside it and its text:

```cpp
xml::Element session("session");
session.setAttribute("version", "2");

auto& track = session.add(xml::Element("track"));
track.setAttribute("name", "Vocal");
track.setText("take.wav");

files::writeSafely("session.xml", xml::write(session));
```

Reading gives the root element. `child` finds the first element of a name, or gives `nullptr`, and `children` gives them all:

```cpp
const auto root = xml::parse(files::read("session.xml"));

if (const auto* first = root.child("track"))
    open(first->text(), first->attributeOr("name", "Untitled"));

for (const auto& element : root.children())
    show(element.name());
```

An element's text is all the text directly inside it, joined, so a document that mixes text and elements as running prose, as XHTML does, can be read but its text no longer interleaves with its elements. A document's own entities are refused, so a document can't make itself grow without bound. `xml::parse` throws `xml::ParseError` with the line and column, `xml::tryParse` gives nothing instead, and `xml::Element` throws `std::invalid_argument` for a name XML doesn't allow.

## Text

Text is UTF-8 in `std::string`, and `text` holds the helpers. `split` and `trim` give `std::string_view`s into what they were given, so that text has to outlive them:

```cpp
const auto pieces = text::split(line, ",");
const auto joined = text::join(pieces, " and ");
const auto name = text::trim(pieces.front());
```

`replaceAll`, `toLowerAscii`, `toUpperAscii` and `equalsIgnoringCaseAscii` do what they say, the last three on A to Z alone. Text from outside the program may be broken: `isValidUtf8` says whether it is, and `replaceInvalidUtf8` puts U+FFFD in place of each broken sequence, as browsers do.

Numbers written for files and other programs belong with these functions, which come out the same on every system and in every locale, where `printf` and `std::stod` follow the locale:

```cpp
const auto shown = text::formatNumber(0.1);       // "0.1"
const auto fixed = text::formatFixed(-6.04, 1);   // "-6.0"
const auto rate = text::parseNumber("48000");     // nothing for anything but a number, spaces included
```

## Paths and folders

Paths are `std::filesystem::path`. Go to and from UTF-8 with `paths::fromUtf8` and `paths::toUtf8`, as on Windows a path's constructor from a `std::string` and its `string()` use the system's code page, which garbles any name outside it:

```cpp
const auto song = paths::systemFolder(paths::Folder::music) / paths::fromUtf8(nameFromPage);
const auto shown = paths::toUtf8(song.filename());
const auto beside = paths::executableFile().parent_path();
```

`paths::Folder` names the folders a system gives each user: `home`, `documents`, `music`, `desktop`, `downloads`, `appData` for what follows the user, `localAppData` for what stays on this computer, `cache` for files that can be made again, and `temp`. Any but home and temp may not exist yet.

The arguments `main` is given on Windows are in the system's code page. A desktop program gets them as UTF-8 from `Application::arguments` in the app module.

## Files and settings

`files::read` gives a whole file's bytes. `files::writeSafely` replaces a file so that it holds either what it held or the new contents whole, even if the program or the computer stops partway, which makes it the way to save a user's work. It makes any missing folders on the way, an existing file keeps its permissions, and a symbolic link is followed:

```cpp
std::error_code error;
files::writeSafely(file, contents, error);

if (error)
    report(error.message());
```

`SettingsFile` keeps named settings in a JSON file, written with `writeSafely`. A missing file starts empty. A file that reads as something other than settings is moved aside, its name ending in `.unreadable`, and the settings start empty, so the next save doesn't destroy what the user had. A file the system wouldn't read may hold good settings: it is left as it is, the settings start empty, `readProblem()` says why, and saving refuses, so a passing failure costs this run's changes and never the file. Any thread may use it except the audio thread:

```cpp
SettingsFile settings(paths::systemFolder(paths::Folder::appData) / "My App" / "settings.json");
settings.set("rate", 48000);
settings.set("window", Object{{"x", 10}, {"y", 20}});

const auto rate = settings.get("rate").intOr(48000);
const auto x = settings.get("window")["x"].intOr(0);
```

`get` gives a copy, so a reference into it lasts only for that statement. Settings are saved when the object is let go of, ignoring errors, so call `save`, with or without a `std::error_code`, to know that it worked.

## Checksums, gzip and long files

`Sha256` takes bytes in pieces of any size, as a download arrives. `Sha256::of` takes bytes in memory, and `Sha256::ofFile` reads a file a block at a time, so a file of any size never sits in memory whole:

```cpp
const auto digest = Sha256::hex(Sha256::ofFile(download));

Sha256 hash;
hash.update(firstPiece);
hash.update(secondPiece);
const auto same = Sha256::hex(hash.finish());
```

`gzip` works on whole files held in memory. `decompress` checks the file's checksum and length, and gives nothing for anything else, for one cut short or changed, or for one holding more than its limit, 1 GB unless you give one, as a few megabytes can unpack to gigabytes:

```cpp
const auto packed = gzip::compress(files::read("session.json"));

if (const auto bytes = gzip::decompress(packed))
    use(*bytes);
```

`MappedFile` reads a long file through memory, in parts or from several threads at once. The system brings in what is read and lets it go again when memory is short:

```cpp
std::error_code error;
const auto mapped = MappedFile::open(file, error);

if (mapped != nullptr)
    scan(static_cast<const char*>(mapped->data()), mapped->size());
```

It is read only, and for files nothing else is writing: whatever changes a file's length while it is held can end the program when a part no longer there is read. An empty file holds nothing, at a null pointer.

## The audio thread

An audio thread never allocates, locks or waits, and these three are made for it.

`LockFreeFifo` passes items from a thread that writes to a thread that reads, neither ever waiting on the other. Make it before the audio starts, as it allocates its room then and holds exactly that many items:

```cpp
LockFreeFifo<float> fifo(48000);

// On the audio thread: as many as there is room for, and how many that was.
const auto written = fifo.write(samples, frames);

// On another thread.
float block[512];
const auto got = fifo.read(block, 512);
```

One thread writes at a time and one reads at a time, which a debug build checks. `push` and `pop` move one item, `skip` passes over items without reading them, and `readable` and `writable` say how many wait and how many there is room for. Items are copied in and out, so use a kind of item that copies without allocating, such as a number or a small struct.

`NoDenormals` flushes denormal numbers to zero on its thread for as long as it lives. A filter or reverb decaying toward silence passes through them, and on x86 each one can cost a hundred times an ordinary number, enough to overload an audio thread:

```cpp
void processBlock(float* const* channels, std::size_t frames)
{
    const NoDenormals guard;
    runFilters(channels, frames);
}
```

`Resampler` takes audio from one rate to another a stretch at a time, a 1 kHz tone coming out within -110 dB of the ideal. It allocates as it is made and never again, so `process` runs on an audio thread too:

```cpp
Resampler resampler(2, 48000.0 / 44100.0);

const auto done = resampler.process(input, inputFrames, output, outputFrames);
```

`process` takes input and gives output until either runs out, and `done.consumed` and `done.produced` say how far each went, so the next call carries on from there. The constructor's third argument is the most input frames you give in one call, 8192 unless you say. A sample only comes out once `lookahead()` frames of input past its instant have gone in, so at the end of a file give it that many frames of silence to get the last of it out. `setRatio` changes the ratio by small amounts, as correcting the drift between two clocks does, and `reset` starts again from silence.

## Speaker layouts

A `ChannelLayout` says which speaker each channel is for, in order. Each standard layout from mono to 9.1.6 has a function, and `standard()` lists them all:

```cpp
const auto layout = ChannelLayout::surround51();
const auto name = layout.name();                   // "5.1"
const auto lfe = layout.channelOf(Speaker::lfe);   // channel 3, or nothing if the layout has no such speaker
```

`ChannelLayout::fromMask` reads a channel mask, such as a WAV's, and gives nothing for a bit with no speaker. Mono's one channel is `Speaker::center`.

## Child processes

`ChildProcess` starts another program, given by its full path, and holds it for as long as it may run. The child gets exactly the arguments given, as UTF-8, and your program's environment with the changes you ask for. It inherits nothing else: no open files, sockets or handles, and every signal at its default. Its standard output and error go to a file or nowhere, never a pipe, so a child that writes a great deal never waits on your program:

```cpp
ChildProcess::Options options;
options.arguments = {"--render", "take one.wav"};
options.environment = {{"RENDER_QUALITY", "high"}};
options.outputFile = logFile;

std::error_code error;
const auto child = ChildProcess::start(program, options, error);

if (child == nullptr)
{
    report(error.message());
    return;
}

const auto ending = child->wait(30s);

if (!ending)
    child->kill();
else if (ending->kind == ProcessEnding::Kind::exited)
    report(ending->code);
```

`wait` gives how the child ended, or nothing if it still runs, and `ending` asks without waiting. A child never outlives what started it: letting go of the object kills it if it still runs, and on Windows every child is in a job the system ends with your program. `Options::outlives` is for a child that should go on, such as one that opens a new version of your program once it has gone.

For messages both ways, ask for a link. Each message arrives whole and in order, any bytes up to `ProcessLink::maximumMessage`, 64 MB. Set both callbacks before `start`. They run on the link's own threads, so hand long work elsewhere, and never destroy the link from inside them:

```cpp
options.link = true;
const auto child = ChildProcess::start(program, options, error);

child->link()->onMessage([&inbox](std::string message) { inbox.add(std::move(message)); });
child->link()->onLost([&inbox] { inbox.close(); });
child->link()->start();
child->link()->send("render");
```

The child opens its end with `ProcessLink::toParent`. A child that is to end with your program calls `system::endNow` from `onLost`, which ends a process at once, running nothing more of it:

```cpp
int main()
{
    std::error_code error;
    const auto link = ProcessLink::toParent(error);

    if (link == nullptr)
        return 1;

    link->onMessage([&link](std::string message) { link->send("done: " + message); });
    link->onLost([] { system::endNow(1); });
    link->start();

    return runUntilAsked();
}
```

A link is lost the moment the other end closes it or its process ends, when a message can't be read, or when nothing is heard for five of this side's heartbeats, one a second unless `ProcessLink::Timing` says otherwise. The beats are counted from the first thing heard, and by the beat rather than the clock, so a program slow to start isn't taken for gone and a computer asleep loses none.

## The system

`system` holds what the operating system can say and do for a program:

```cpp
const auto folder = system::environmentVariable("MY_APP_FOLDER");   // nothing if it isn't set
const auto opened = system::openInBrowser("https://symbolengine.dev");
```

`openInBrowser` opens only http and https addresses, so text from a page or a file can't open a file or another program through it. `architecture` names what the program's code runs as, the same on every system: `arm64`, `x86_64`, `arm` or `x86`. `computerStarted` says when the computer last started.

## What's in the reference

| Header | What it gives you |
|---|---|
| `Value.h`, `Json.h` | `Value`, `Object`, `Array`, and `json::parse` and `json::write` |
| `Xml.h` | `xml::Element`, `xml::parse` and `xml::write` |
| `Text.h` | UTF-8 checks, splitting and joining, and numbers that read the same in every locale |
| `Paths.h` | Paths as UTF-8, `paths::systemFolder` and `paths::executableFile` |
| `Files.h`, `SettingsFile.h` | `files::read`, `files::writeSafely` and `SettingsFile` |
| `Sha256.h` | `Sha256` |
| `Gzip.h`, `MappedFile.h` | `gzip::compress`, `gzip::decompress` and `MappedFile`, each included by name |
| `LockFreeFifo.h`, `NoDenormals.h`, `Resampler.h` | What the audio thread uses |
| `ChannelLayout.h` | `Speaker` and `ChannelLayout` |
| `ChildProcess.h`, `System.h` | `ChildProcess`, `ProcessLink` and `system::endNow`, with what else `system` holds |

The architecture's [layout and conventions](../02_architecture.md#layout-and-conventions) and [child processes](../02_architecture.md#child-processes) sections say how these are built and why.
