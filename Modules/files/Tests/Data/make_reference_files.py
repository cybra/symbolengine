#!/usr/bin/env python3
"""Makes the reference files the files module's tests read, with encoders other than SymbolEngine's own: ffmpeg for
most, the Mac's afconvert for Apple's own AIFF and its 64-bit float AIFF-C, and the flac tool for a tagged FLAC. The
lossless ones hold the pattern of samples the tests work out for themselves (pattern::whole in the tests), so every
sample read can be checked exactly; the Ogg Vorbis and Opus ones hold a tone of its own in each channel (pattern::tone),
so a channel out of place or a sample out of time shows. Run on a Mac with ffmpeg and flac installed, from anywhere:
python3 make_reference_files.py"""

import math

import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FRAMES = 1000


def whole(frame, channel, bits):
    """The pattern: the lowest and highest values first, then values spread over the whole range."""
    if frame == 0:
        return -(1 << (bits - 1))
    if frame == 1:
        return (1 << (bits - 1)) - 1
    mixed = ((frame * 7919 + channel * 104729) * 2654435761) % (1 << 32)
    return (mixed >> (32 - bits)) - (1 << (bits - 1))


def floating(frame, channel):
    """Floats, some past full scale, which float files keep."""
    return whole(frame, channel, 24) / 8388608.0 * 1.5


def tone(frame, channel, rate):
    """Half full scale, at a frequency of its own in each channel; the fourth, the LFE of 5.1 and more, low enough to
    keep."""
    frequency = 55.0 if channel == 3 else 220.0 * (channel + 1)
    return 0.5 * math.sin(2.0 * math.pi * frequency * frame / rate)


def raw(bits, channels, kind, rate=48000, frames=FRAMES):
    out = bytearray()
    for frame in range(frames):
        for channel in range(channels):
            if kind == "tone":
                out += struct.pack("<f", tone(frame, channel, rate))
            elif kind == "same tone":
                out += struct.pack("<f", tone(frame, 0, rate))
            elif kind == "f32":
                out += struct.pack("<f", floating(frame, channel))
            elif kind == "f64":
                out += struct.pack("<d", floating(frame, channel))
            elif kind == "u8":
                out += struct.pack("<B", whole(frame, channel, 8) + 128)
            else:
                out += whole(frame, channel, bits).to_bytes(bits // 8, "little", signed=True)
    return bytes(out)


INPUT = {"s8": ("s8", 8), "u8": ("u8", 8), "s16": ("s16le", 16), "s24": ("s24le", 24), "s32": ("s32le", 32),
         "f32": ("f32le", 32), "f64": ("f64le", 64), "tone": ("f32le", 32), "same tone": ("f32le", 32)}


def ffmpeg(name, kind, channels, rate, codec, extra=(), layout=None, frames=FRAMES):
    raw_format, bits = INPUT[kind]
    with tempfile.NamedTemporaryFile(suffix=".raw", delete=False) as source:
        source.write(raw(bits, channels, kind, rate, frames))
    try:
        command = ["ffmpeg", "-loglevel", "error", "-y", "-f", raw_format, "-ar", str(rate), "-ac", str(channels)]
        if layout:
            command += ["-channel_layout", layout]
        command += ["-i", source.name, "-c:a", codec, "-fflags", "+bitexact", "-flags:a", "+bitexact", *extra,
                    os.path.join(HERE, name)]
        subprocess.run(command, check=True)
    finally:
        os.unlink(source.name)


def afconvert(name, source, file_format, data_format):
    subprocess.run(["afconvert", "-f", file_format, "-d", data_format, os.path.join(HERE, source), os.path.join(HERE, name)],
                   check=True)


def flac_tool(name, source, tags):
    command = ["flac", "--silent", "--force", "--padding=4096", "-o", os.path.join(HERE, name)]
    for tag in tags:
        command += ["-T", tag]
    subprocess.run(command + [os.path.join(HERE, source)], check=True)


def main():
    ffmpeg("pcm16-stereo-44100.wav", "s16", 2, 44100, "pcm_s16le")
    ffmpeg("pcm24-mono-48000.wav", "s24", 1, 48000, "pcm_s24le")
    ffmpeg("pcm32-stereo-96000.wav", "s32", 2, 96000, "pcm_s32le")
    ffmpeg("unsigned8-mono-22050.wav", "u8", 1, 22050, "pcm_u8")
    ffmpeg("float32-stereo-48000.wav", "f32", 2, 48000, "pcm_f32le")
    ffmpeg("float64-mono-48000.wav", "f64", 1, 48000, "pcm_f64le")
    ffmpeg("pcm24-5.1-48000.wav", "s24", 6, 48000, "pcm_s24le", layout="5.1")
    ffmpeg("rf64-pcm16-stereo-48000.wav", "s16", 2, 48000, "pcm_s16le", ["-rf64", "always"])
    ffmpeg("bwf-pcm24-stereo-48000.wav", "s24", 2, 48000, "pcm_s24le",
           ["-write_bext", "1", "-metadata", "description=A take for the tests",
            "-metadata", "originator=SymbolEngine", "-metadata", "originator_reference=REF-0001",
            "-metadata", "origination_date=2026-10-23", "-metadata", "origination_time=21:45:30",
            "-metadata", "time_reference=172800000", "-metadata", "coding_history=A=PCM,F=48000,W=24,M=stereo"])
    ffmpeg("adpcm-mono-44100.wav", "s16", 1, 44100, "adpcm_ms")
    ffmpeg("pcm16-stereo-44100.aiff", "s16", 2, 44100, "pcm_s16be")
    ffmpeg("pcm24-mono-48000.aiff", "s24", 1, 48000, "pcm_s24be")
    ffmpeg("pcm8-mono-22050.aiff", "s8", 1, 22050, "pcm_s8")
    ffmpeg("float32-stereo-48000.aifc", "f32", 2, 48000, "pcm_f32be", ["-f", "aiff"])
    ffmpeg("sowt16-stereo-44100.aifc", "s16", 2, 44100, "pcm_s16le", ["-f", "aiff"])
    afconvert("float64-mono-48000.aifc", "float64-mono-48000.wav", "AIFC", "BEF64")
    afconvert("pcm24-stereo-48000-apple.aiff", "bwf-pcm24-stereo-48000.wav", "AIFF", "BEI24")
    ffmpeg("pcm16-stereo-44100.flac", "s16", 2, 44100, "flac")
    ffmpeg("pcm24-5.1-48000.flac", "s24", 6, 48000, "flac", layout="5.1")
    flac_tool("tagged-pcm24-mono-48000.flac", "pcm24-mono-48000.wav", ["TITLE=A take for the tests", "ARTIST=SymbolEngine", "DATE=2026"])
    tags = ["-metadata", "title=A take for the tests", "-metadata", "artist=SymbolEngine"]
    ffmpeg("tones-stereo-48000.opus", "tone", 2, 48000, "libopus", ["-b:a", "160k", *tags], frames=24000)
    ffmpeg("tones-5.1-48000.opus", "tone", 6, 48000, "libopus", ["-b:a", "320k", "-mapping_family", "1"], layout="5.1", frames=24000)
    # ffmpeg's own Vorbis encoder, the only one it has, writes stereo alone, and codes a second channel badly unless it
    # carries what the first does, so both carry the first channel's tone.
    ffmpeg("tone-stereo-44100.ogg", "same tone", 2, 44100, "vorbis", ["-strict", "-2", "-q:a", "6", *tags], frames=22050)


if __name__ == "__main__":
    sys.exit(main())
