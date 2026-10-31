// What a Windows endpoint is, read from the properties Windows 11 stored for the owner's AirPods and USB interface.

#include "WindowsAudioDetail.h"

#include <catch2/catch_test_macros.hpp>

using namespace symbolengine;
using windowsAudio::detail::kindOf;

TEST_CASE("A Bluetooth headset's hands-free endpoint is known by its enumerator, whatever Windows names it")
{
    // Windows 11's names for AirPods: the microphone with no word of its profile, the stereo output.
    const auto microphone = kindOf("BTHHFENUM", "Headset (AirPods Pro - Find My)");
    CHECK(microphone.handsFree);
    CHECK(microphone.transport == AudioTransport::bluetooth);

    const auto stereo = kindOf("BTHENUM", "Headphones (AirPods Pro - Find My)");
    CHECK_FALSE(stereo.handsFree);
    CHECK(stereo.transport == AudioTransport::bluetooth);

    // Windows 10's, with the profile in the name, and an enumerator in lower case.
    CHECK(kindOf("", "Headset (WH-1000XM4 Hands-Free AG Audio)").handsFree);
    CHECK(kindOf("bthhfenum", "Headset").handsFree);
}

TEST_CASE("A USB interface's endpoints are USB, by the class driver or its maker's own, and others are unknown")
{
    CHECK(kindOf("USB", "Microphone (USB Audio Device)").transport == AudioTransport::usb);
    CHECK(kindOf("FOCUSRITEUSB", "Analogue 1 + 2 (Focusrite USB Audio)").transport == AudioTransport::usb);
    CHECK_FALSE(kindOf("FOCUSRITEUSB", "Analogue 1 + 2 (Focusrite USB Audio)").handsFree);
    CHECK(kindOf("HDAUDIO", "DELL S2725QS (NVIDIA High Definition Audio)").transport == AudioTransport::unknown);
    CHECK(kindOf("", "Speakers").transport == AudioTransport::unknown);
}

TEST_CASE("A short run of errors a buffer call can recover from doesn't end a stream, and anything else does at once")
{
    using windowsAudio::detail::ErrorRun;
    constexpr std::int32_t ok = 0;
    constexpr auto bufferError = static_cast<std::int32_t>(0x88890018u);
    constexpr auto resetInProgress = static_cast<std::int32_t>(0x8889000Bu);
    constexpr auto tooLarge = static_cast<std::int32_t>(0x88890006u);
    constexpr auto invalidated = static_cast<std::int32_t>(0x88890004u);

    ErrorRun run;
    CHECK_FALSE(run.ends(ok));
    CHECK_FALSE(run.ends(bufferError));
    CHECK_FALSE(run.ends(resetInProgress));
    CHECK_FALSE(run.ends(tooLarge));
    CHECK_FALSE(run.ends(ok));

    // A run that goes on is no longer one it can recover from, and one good call starts the count again.
    int heard = 0;
    while (!run.ends(bufferError) && heard < 1000)
        ++heard;
    CHECK(heard == ErrorRun::mostInARow);
    CHECK_FALSE(run.ends(ok));
    CHECK_FALSE(run.ends(bufferError));

    CHECK(run.ends(invalidated));
    ErrorRun fresh;
    CHECK(fresh.ends(static_cast<std::int32_t>(0x88890010u)));   // the audio service has stopped
}

TEST_CASE("A session disconnected for its format is opened again, and for any other reason is lost")
{
    using namespace windowsAudio::detail;
    CHECK(reopensAfterDisconnect(disconnectReasonFormatChanged));
    CHECK_FALSE(reopensAfterDisconnect(0));   // the device removed
    CHECK_FALSE(reopensAfterDisconnect(5));   // another program taking it for itself
}
