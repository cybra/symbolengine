# Reading and writing audio files

The files module reads and writes audio files a stretch at a time, so a file of any length takes little memory: WAV, with RF64 past 4 GB, AIFF and AIFF-C, FLAC, Ogg Vorbis and Opus. It also reads MP3, and M4A, AAC and CAF through the system's own decoders. It brings the core module with it.

```cmake
target_link_libraries(my_app PRIVATE SymbolEngine::files)
```

[`Examples/AudioConvert`](../../Examples/AudioConvert/AudioConvert.cpp) is a whole program that converts a file to the format its new name asks for, and runs as a test by converting a tone of its own to every format.

```cpp
#include <symbolengine/files.h>

using namespace symbolengine;
using namespace std::chrono_literals;
```

Samples are floats, one buffer per channel. A whole number reads as itself divided by 2 to the power of one less than its bits, so a 16-bit sample is divided by 32768, and writes back as the same number. Float files keep their floats as they are, past full scale too. Channels come in WAV's order whatever order the format keeps them in.

## Reading a file

`files::openAudio` goes by what a file holds, never its name, and `info` says what that is:

```cpp
const auto reader = files::openAudio(file);
const auto& info = reader->info();

std::printf("%s, %g Hz, %d channels, %llu frames\n", info.format.c_str(), info.sampleRate, info.channels,
            static_cast<unsigned long long>(info.frames));
```

`read` takes the frame to start from and one buffer per channel, and gives how many frames it read: fewer at the end of the file, and 0 from the end on. It reads from anywhere, in any order:

```cpp
std::vector<float> left(4096), right(4096);
float* channels[] = {left.data(), right.data()};
std::uint64_t at = 0;

while (const auto got = reader->read(at, channels, 4096))
{
    use(left, right, got);
    at += got;
}
```

Go by what `read` gives. `info.frames` is the decoder's estimate for AAC in ADTS and for WMA on Windows until the file has been read to its end, and a file cut short, as when its writing was stopped, reads up to its last whole frame. One thread at a time uses a reader, so open one for each thread that reads. A read may take as long as the disk does, so keep it off the audio thread: the devices module's `AudioPlayer` reads ahead on a thread of its own to play a file through a device.

`info.bits` is 0 for the formats that keep none, such as Ogg Vorbis, Opus, MP3 and AAC. An Opus file's `info.sampleRate` is always 48 kHz, which is what it plays at.

## Writing a file

`files::createAudio` makes a file with the settings given, replacing any of that name. The folder has to exist. `write` adds samples to the end, and `finish` writes the header for the final length and closes the file:

```cpp
AudioFileSettings settings;
settings.format = "FLAC";
settings.sampleRate = 44100.0;
settings.channels = 2;
settings.bits = 16;

const auto writer = files::createAudio(file, settings);

const float* channels[] = {left.data(), right.data()};
writer->write(channels, frames);
writer->finish();
```

Call `finish` yourself, as that's where you hear of anything going wrong. A writer let go of without it completes the file too, ignoring errors.

A whole-number format scales a sample back the way it was read and holds it within its range, so a sample of 1 writes as its highest value and anything past that is clipped. Each format holds this much, and what `createAudio` can't write is refused with `AudioFileError::badSettings`:

| `format` | Samples | What else it takes |
|---|---|---|
| `"WAV"` | Whole numbers in 8, 16, 24 or 32 bits, floats in 32 or 64 | `channelMask`, `chunks`. Past 4 GB it becomes RF64 |
| `"AIFF"` | The same; floats make it AIFF-C | `chunks`. It can't pass 4 GB, and fails with `AudioFileError::tooLong` |
| `"FLAC"` | Whole numbers in 16 or 24 bits, up to 8 channels, at a whole-number rate up to 65535, or a multiple of 10 up to 655350 | `compression` from 0 to 8, 5 unless you say; `tags` |
| `"Ogg Vorbis"` | Up to 255 channels, at up to 200 kHz; ignores `bits` and `floatingPoint` | `quality` from -0.1 to 1, 0.4 unless you say, about 128 kbps in stereo; `tags` |
| `"Opus"` | Up to 8 channels, at up to 384 kHz; ignores `bits` and `floatingPoint` | `bitRate` for all channels together, 128000 unless you say; `tags` |

An Opus file made from another rate plays at 48 kHz with its length in time kept, so 44100 frames written at 44.1 kHz read back as about 48000.

## When something goes wrong

Each call throws `std::filesystem::filesystem_error`, or, given a `std::error_code`, sets it instead. Besides the system's own reasons, the code can be an `AudioFileError`, and `message` gives words that can be shown as they are:

```cpp
std::error_code error;
const auto reader = files::openAudio(file, error);

if (error == AudioFileError::notAudio)
    offerAnotherFile();
else if (error)
    report(error.message());
```

- **`notAudio`** is a file of no format read here.
- **`unsupported`** is a format read here holding its samples in a way it doesn't read, such as M4A on Linux.
- **`damaged`** is a file whose own description of itself doesn't hold together.
- **`badSettings`** is settings `createAudio` can't write, such as 12 bits or no channels.
- **`tooLong`** is more than the format can hold.

## What each system reads

WAV, AIFF, FLAC, Ogg Vorbis, Opus and MP3 read the same everywhere. An MP3 with LAME's or Xing's information in its first frame reads without the samples its encoder and decoder add at each end, as gapless players read it. The system's own decoders read the rest:

- **On a Mac**, AudioToolbox reads M4A and other MPEG-4 audio, AAC in ADTS and CAF, a WAV or AIFF-C whose samples are coded in a way of their own, such as ADPCM or µ-law, and whatever else it knows, such as AC-3, AMR and Sun's AU.
- **On Windows**, Media Foundation reads M4A, other MPEG-4 audio, AAC in ADTS and WMA. Windows' N editions, which lack it, find those files unsupported.
- **On Linux**, those files are unsupported.

`files::readableExtensions` lists what this system reads, each with its dot and in lower case, for an open dialog:

```cpp
const auto extensions = files::readableExtensions();
```

## Chunks, tags and speakers

A WAV's or AIFF's other chunks, such as bext, iXML, cue and LIST, come in `info.chunks` as they are, apart from those describing the samples, the padding, and any over 64 MB. Give them to a new file of the same format and a recording's details survive an edit. FLAC, Ogg Vorbis and Opus keep tags instead, each a name and its value:

```cpp
AudioFileSettings settings;
settings.format = info.format;
settings.sampleRate = info.sampleRate;
settings.channels = info.channels;
settings.bits = info.bits;
settings.floatingPoint = info.floatingPoint;
settings.chunks = info.chunks;
settings.tags = info.tags;
settings.tags.emplace_back("TITLE", "First Take");
```

A WAV's broadcast extension reads and writes field by field. Text is cut at each field's size when written, and `timeReference`, the first sample's place in the day in samples since midnight, is what recorders and editors line recordings up by:

```cpp
BroadcastExtension extension;
extension.description = "Scene 4, take 2";
extension.originator = "My Recorder";
extension.timeReference = 48000 * 3600;

AudioFileSettings settings;
settings.chunks = {files::bextChunk(extension)};
```

`files::broadcastExtension` reads one back from a file's chunks, or gives nothing when it has none:

```cpp
if (const auto found = files::broadcastExtension(reader->info().chunks))
    show(found->description, found->timeReference);
```

`info.channelMask` gives the speakers a WAV's channels are for, or 0 when the file doesn't say, and `ChannelLayout`, from `symbolengine/core/ChannelLayout.h`, reads it:

```cpp
const auto speakers = ChannelLayout::fromMask(info.channelMask);
```

## Recording from an audio thread

`BackgroundAudioWriter` writes a file from a thread that mustn't wait, such as an audio device's. It takes the writer, and room for as many frames as the disk may fall behind by:

```cpp
BackgroundAudioWriter recording(files::createAudio(file, settings), 2 * 48000);
```

On the audio thread, `write` copies samples into its buffer and returns at once, never locking, waiting or allocating. It refuses a stretch whole when there isn't room for it, or once writing the file has gone wrong, so the recording can say what it lost:

```cpp
if (!recording.write(channels, frames))
    lostFrames += frames;
```

A thread of the writer's own writes them to the file. It flushes the file every second, or the interval given, so a WAV or AIFF recording stopped by a crash reads to within that of where it stopped. A FLAC, Ogg Vorbis or Opus file is only whole once finished, so a recording that has to survive a crash belongs in a WAV or AIFF.

Once the audio thread has stopped adding samples, `finish` writes what is still waiting and closes the file, throwing the first thing that went wrong. `failed` says at any time whether something has, and `framesWritten` and `framesLost` how many frames reached the file and how many it took and couldn't write. Letting go of the object finishes it too, ignoring errors:

```cpp
recording.finish();
```

To draw or measure the recording as it goes, give it a function. It is called on the writer's thread with each stretch once it is in the file, as the file holds it: whole numbers as they read back, floats as they are, and Ogg Vorbis and Opus as they were written:

```cpp
BackgroundAudioWriter recording(files::createAudio(file, settings), 2 * 48000, 1s,
                                [&overview](const float* const* channels, std::size_t frames) {
                                    overview.add(channels, frames);
                                });
```

A program writing a WAV or AIFF on an ordinary thread gets the same safety with `flush`, which writes the header for the samples so far and has the system put the file on disk:

```cpp
writer->flush();
```

## What's in the reference

| Header | What it gives you |
|---|---|
| `AudioFile.h` | `files::openAudio`, `files::createAudio`, `files::readableExtensions`, `AudioFileReader`, `AudioFileWriter`, `AudioInfo`, `AudioFileSettings` and `AudioFileError` |
| `BackgroundAudioWriter.h` | Writing a file from a thread that mustn't wait |
| `BroadcastWave.h` | `BroadcastExtension`, `files::broadcastExtension` and `files::bextChunk` |

The [architecture's audio files section](../02_architecture.md#audio-files) says how each format is read and written, and which other programs' files it is checked against.
