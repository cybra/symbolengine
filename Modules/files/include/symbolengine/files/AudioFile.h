#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

/** Reading and writing audio files: WAV, including RF64 and BW64 for files past 4 GB, AIFF and AIFF-C, FLAC, Ogg Vorbis
    and Opus. A file is read and written a stretch at a time, so files of any length take little memory, and samples
    are floats, whole numbers divided by 2 to the power of one less than their bits, so a 16-bit sample reads as itself
    divided by 32768 and writes back as the same number. Channels come in WAV's order whatever the format keeps them
    in: Ogg Vorbis' and Opus' own order is turned to it as they are read and written. */
namespace symbolengine
{

/** A chunk of a WAV or AIFF file kept as it is, such as a WAV's bext, iXML, cue or LIST chunk: its four-character id
    and its bytes, without the pad byte that follows a chunk of odd length. */
struct AudioChunk
{
    std::string id{};
    std::string bytes{};

    bool operator==(const AudioChunk&) const = default;
};

/** What an audio file holds. */
struct AudioInfo
{
    std::string format{};          // "WAV", "AIFF", "FLAC", "Ogg Vorbis", "Opus", "MP3", "M4A", "AAC" or "CAF", as
                                   // people name them
    double sampleRate = 0.0;       // an Opus file's is 48 kHz, which it always plays at
    int channels = 0;
    std::uint64_t frames = 0;      // the length, in samples of each channel
    int bits = 0;                  // as stored: 8, 16, 24, 32 or 64, or 0 for the formats that keep none, such as
                                   // Ogg Vorbis, Opus, MP3 and AAC
    bool floatingPoint = false;    // samples stored as floats rather than whole numbers

    /** The speakers a WAV's channels are for, as WAVE_FORMAT_EXTENSIBLE gives them (1 front left, 2 front right,
        4 front center, 8 low frequency and so on), or 0 when the file doesn't say. */
    std::uint32_t channelMask = 0;

    /** The file's other chunks, in the order they come: everything but those describing the samples themselves
        (a WAV's fmt, fact, data and ds64, an AIFF's COMM, SSND and FVER) and the padding a file is laid out with
        (JUNK, PAD and FLLR), and any over 64 MB. Written back as they are, they keep a recording's broadcast
        extension, iXML, cue points and the rest. */
    std::vector<AudioChunk> chunks{};

    /** A FLAC, Ogg Vorbis or Opus file's tags, its Vorbis comments: each a name, such as TITLE, ARTIST or DATE, and
        its value, in the order the file has them. */
    std::vector<std::pair<std::string, std::string>> tags{};
};

/** An audio file opened for reading. One thread at a time reads it. */
class AudioFileReader
{
public:
    virtual ~AudioFileReader() = default;

    AudioFileReader(const AudioFileReader&) = delete;
    AudioFileReader& operator=(const AudioFileReader&) = delete;

    const AudioInfo& info() const noexcept { return information; }

    /** Reads up to frames samples of each channel, from the frame from on, into one buffer per channel, each with
        room for that many. Returns how many it read: fewer at the end of the file, and 0 from the end on. A file cut
        short, as when its writing was stopped, reads up to its last whole frame. Throws
        std::filesystem::filesystem_error when the file can't be read, or, given a std::error_code, sets it instead and
        returns how many frames it read before. */
    std::size_t read(std::uint64_t from, float* const* channels, std::size_t frames);
    std::size_t read(std::uint64_t from, float* const* channels, std::size_t frames, std::error_code& error);

protected:
    AudioFileReader() = default;

    virtual std::size_t readFrames(std::uint64_t from, float* const* channels, std::size_t frames,
                                   std::error_code& error) = 0;

    AudioInfo information{};
    std::filesystem::path path{};
};

/** What a new audio file holds, as createAudio() writes it. */
struct AudioFileSettings
{
    std::string format = "WAV";    // "WAV", "AIFF", "FLAC", "Ogg Vorbis" or "Opus"
    double sampleRate = 48000.0;   // Opus takes any rate, and plays at 48 kHz
    int channels = 2;              // 1 or more; FLAC up to 8, Opus up to 255
    int bits = 24;                 // whole numbers in 8, 16, 24 or 32 bits, FLAC in 16 or 24; floats in 32 or 64
    bool floatingPoint = false;    // floats are written as AIFF-C in an AIFF file; Ogg Vorbis and Opus ignore both

    /** For a WAV, the speakers its channels are for, as in AudioInfo; 0 for the usual ones for its number of
        channels, which are none past eight. */
    std::uint32_t channelMask = 0;

    /** Chunks to write as they are, before the samples, such as those AudioInfo gave of another file of the same
        format. Ids that describe the samples or lay the file out are left out. */
    std::vector<AudioChunk> chunks{};

    /** For FLAC, Ogg Vorbis and Opus, tags to write as Vorbis comments, as AudioInfo gives them. */
    std::vector<std::pair<std::string, std::string>> tags{};

    double quality = 0.4;          // Ogg Vorbis' quality, from -0.1 to 1 as libvorbis takes it; 0.4 is about 128 kbps in stereo
    int bitRate = 128000;          // Opus' bit rate, in bits a second for all its channels together
    int compression = 5;           // FLAC's compression, from 0, the quickest, to 8, the smallest
};

/** An audio file being written. Samples are written in order, and finish() completes the file; a writer let go of
    before finish() completes it too, ignoring anything that goes wrong. One thread at a time writes it. */
class AudioFileWriter
{
public:
    virtual ~AudioFileWriter() = default;

    AudioFileWriter(const AudioFileWriter&) = delete;
    AudioFileWriter& operator=(const AudioFileWriter&) = delete;

    const AudioFileSettings& settings() const noexcept { return written; }

    /** The samples written so far, of each channel. */
    std::uint64_t frames() const noexcept { return count; }

    /** Adds frames samples of each channel, from one buffer per channel, to the end of the file. Whole-number formats
        take a sample as the nearest whole number to it times 2 to the power of one less than their bits, held within
        the range they have, so a sample of 1 writes as their highest value and anything past it is clipped; floats
        are written as they are. Throws std::filesystem::filesystem_error when the file can't be written, or, given a
        std::error_code, sets it instead. */
    void write(const float* const* channels, std::size_t frames);
    void write(const float* const* channels, std::size_t frames, std::error_code& error);

    /** Writes the file's header for its final length and closes it. A WAV past 4 GB becomes RF64. Throws or sets the
        std::error_code given as write() does; an AIFF can't pass 4 GB, and fails with AudioFileError::tooLong. */
    void finish();
    void finish(std::error_code& error);

protected:
    AudioFileWriter() = default;

    virtual void writeFrames(const float* const* channels, std::size_t frames, std::error_code& error) = 0;
    virtual void finishFile(std::error_code& error) = 0;

    AudioFileSettings written{};
    std::filesystem::path path{};
    std::uint64_t count = 0;
    bool finished = false;
};

/** Why an audio file couldn't be read or written, apart from the system's own reasons, as std::error_code values of
    files::audioFileCategory(). */
enum class AudioFileError
{
    notAudio = 1,     // not a file of any format read here
    unsupported,      // a format read here holding its samples in a way it doesn't read, such as compressed
    damaged,          // a file whose own description of itself doesn't hold together
    badSettings,      // settings createAudio() can't write, such as 12 bits or no channels
    tooLong           // more than its format can hold
};

namespace files
{

/** The category of AudioFileError's codes, whose messages can be shown as they are. */
const std::error_category& audioFileCategory() noexcept;

/** Opens an audio file for reading, by what it holds rather than its name. Throws
    std::filesystem::filesystem_error when it can't be opened, or, given a std::error_code, sets it instead and
    returns nullptr; for a file that isn't audio it reads, the error is an AudioFileError.

    WAV, AIFF, FLAC, Ogg Vorbis, Opus and MP3 read the same everywhere. An MP3 with LAME's or Xing's information in
    its first frame reads without the samples its encoder and decoder add at each end, as gapless players read it. M4A
    and other MPEG-4 audio, AAC in ADTS and CAF are read by the system's own decoders: on a Mac all three, on Windows
    the first two, through Media Foundation, and on Linux none, which is AudioFileError::unsupported. */
std::unique_ptr<AudioFileReader> openAudio(const std::filesystem::path& file);
std::unique_ptr<AudioFileReader> openAudio(const std::filesystem::path& file, std::error_code& error);

/** Creates an audio file, replacing any file of that name, to write with the settings given. Throws as openAudio()
    does, or, given a std::error_code, sets it and returns nullptr. */
std::unique_ptr<AudioFileWriter> createAudio(const std::filesystem::path& file, const AudioFileSettings& settings);
std::unique_ptr<AudioFileWriter> createAudio(const std::filesystem::path& file, const AudioFileSettings& settings,
                                             std::error_code& error);

}

std::error_code make_error_code(AudioFileError error) noexcept;

}

namespace std
{
template <>
struct is_error_code_enum<symbolengine::AudioFileError> : true_type
{
};
}
