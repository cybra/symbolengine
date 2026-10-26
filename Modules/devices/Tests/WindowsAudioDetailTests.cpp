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
